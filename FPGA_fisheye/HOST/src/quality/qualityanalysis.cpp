#include "qualityanalysis.h"
#include "fpga/network/mediawire.h"
#include "ps_reference/reference_id.h"
extern "C" {
#include "ps_reference/fptz_lut_generate.h"
}
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPainter>
#include <QSaveFile>
#include <QtEndian>
#include <cmath>
#include <mutex>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
namespace Quality {
static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static void check(const Cancel &cancel) {
    if (cancel && cancel())
        throw std::runtime_error("操作已取消");
}
QString referenceId() {
    return QStringLiteral(QUALITY_REFERENCE_SHA256);
}
void splitPair(const QByteArray &pair, QByteArray &front, QByteArray &rear) {
    require(MediaWire::validInput(pair), "双路 JPEG 数据格式不正确");
    auto b = reinterpret_cast<const uchar *>(pair.constData());
    int n = int(qFromBigEndian<quint32>(b));
    front = pair.mid(64, n);
    rear = pair.mid(64 + ((n + 127) & ~63), int(qFromBigEndian<quint32>(b + 4)));
}
static cv::Mat luminance(const QImage &image) {
    QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    cv::Mat out(rgb.height(), rgb.width(), CV_64F);
    for (int y = 0; y < rgb.height(); ++y) {
        auto p = rgb.constScanLine(y);
        auto d = out.ptr<double>(y);
        for (int x = 0; x < rgb.width(); ++x)
            d[x] = .299 * p[3 * x] + .587 * p[3 * x + 1] + .114 * p[3 * x + 2];
    }
    return out;
}
Scores compare(const QImage &reference, const QImage &actual, const cv::Mat &mask) {
    require(!reference.isNull() && reference.size() == actual.size(), "图片为空或尺寸不匹配");
    require(mask.type() == CV_8UC1 && mask.cols == reference.width() &&
                mask.rows == reference.height(),
            "有效区域尺寸不匹配");
    Scores s;
    s.pixels = cv::countNonZero(mask);
    require(s.pixels > 0, "没有有效像素");
    s.coverage = double(s.pixels) / mask.total();
    cv::Mat a = luminance(reference), b = luminance(actual), diff;
    cv::subtract(a, b, diff);
    diff = diff.mul(diff);
    double mse = cv::mean(diff, mask)[0];
    s.infinite = mse == 0;
    if (!s.infinite)
        s.psnr = 10 * std::log10(255. * 255. / mse);
    cv::Mat valid;
    cv::erode(mask, valid, cv::Mat::ones(11, 11, CV_8U), {-1, -1}, 1, cv::BORDER_CONSTANT,
              cv::Scalar(0));
    s.windows = cv::countNonZero(valid);
    require(s.windows > 0, "没有完整有效的 11×11 SSIM 窗口");
    cv::Mat ma, mb, aa, bb, ab;
    auto blur = [](const cv::Mat &in, cv::Mat &out) {
        cv::GaussianBlur(in, out, {11, 11}, 1.5, 1.5, cv::BORDER_CONSTANT);
    };
    blur(a, ma);
    blur(b, mb);
    blur(a.mul(a), aa);
    blur(b.mul(b), bb);
    blur(a.mul(b), ab);
    cv::Mat ma2 = ma.mul(ma), mb2 = mb.mul(mb), mamb = ma.mul(mb);
    cv::Mat numerator = (2 * mamb + 6.5025).mul(2 * (ab - mamb) + 58.5225);
    cv::Mat denominator = (ma2 + mb2 + 6.5025).mul(aa - ma2 + bb - mb2 + 58.5225), map;
    cv::divide(numerator, denominator, map);
    s.ssim = cv::mean(map, valid)[0];
    return s;
}
QImage renderReference(const QByteArray &pair, cv::Mat &mask, const Cancel &cancel) {
    // The production C generator has a shared ray cache: serialize all reference jobs.
    static std::mutex generatorMutex;
    std::lock_guard<std::mutex> lock(generatorMutex);
    check(cancel);
    QByteArray front, rear;
    splitPair(pair, front, rear);
    QImage lenses[] = {QImage::fromData(front, "JPEG").convertToFormat(QImage::Format_RGB888),
                       QImage::fromData(rear, "JPEG").convertToFormat(QImage::Format_RGB888)};
    for (auto &lens : lenses)
        require(lens.size() == QSize(1328, 1328), "输入 JPEG 解码失败或尺寸错误");
    QByteArray lut(int(FPTZ_LUT_BYTES), 0);
    FptzLutGen gen;
    require(fptz_lut_gen_begin_panorama(&gen, reinterpret_cast<uint8_t *>(lut.data()),
                                        fptz_scene_mask_front_bits, fptz_scene_mask_rear_bits) == 0,
            "参考映射初始化失败");
    int status;
    do {
        check(cancel);
        status = fptz_lut_gen_rows(&gen, 8);
    } while (!status);
    require(status == 1, "参考映射生成失败");
    QImage result(2656, 1328, QImage::Format_RGB888);
    mask = cv::Mat::zeros(1328, 2656, CV_8U);
    auto bytes = reinterpret_cast<const uchar *>(lut.constData());
    auto coord = [&](int plane, int i) {
        return qFromLittleEndian<qint32>(bytes + plane * FPTZ_LUT_PLANE_BYTES + i * 4);
    };
    for (int y = 0; y < 1328; ++y) {
        check(cancel);
        auto row = result.scanLine(y);
        for (int x = 0; x < 2656; ++x) {
            int i = y * 2656 + x, w = qFromLittleEndian<quint16>(bytes + FPTZ_LUT_W_OFFSET + i * 2);
            int pixel[2][3] = {};
            bool valid = w <= 256;
            for (int lens = 0; lens < 2; ++lens) {
                if ((lens == 0 && w == 0) || (lens == 1 && w == 256))
                    continue;
                int qx = coord(lens * 2, i), qy = coord(lens * 2 + 1, i);
                if (qx < 0 || qy < 0 || qx >= int(FPTZ_Q6_X_LIMIT) || qy >= int(FPTZ_Q6_Y_LIMIT)) {
                    valid = false;
                    continue;
                }
                int ix = qx >> 6, iy = qy >> 6, fx = qx & 63, fy = qy & 63;
                auto p = lenses[lens].constScanLine(iy) + 3 * ix;
                auto next = lenses[lens].constScanLine(iy + 1) + 3 * ix;
                for (int c = 0; c < 3; ++c)
                    pixel[lens][c] = ((p[c] * (64 - fx) + p[c + 3] * fx) * (64 - fy) +
                                      (next[c] * (64 - fx) + next[c + 3] * fx) * fy) >>
                                     12;
            }
            mask.at<uchar>(y, x) = valid ? 255 : 0;
            for (int c = 0; c < 3; ++c)
                row[3 * x + c] =
                    valid ? uchar((pixel[0][c] * w + pixel[1][c] * (256 - w)) >> 8) : 0;
        }
    }
    return result;
}
Result analyze(const QByteArray &pair, const QByteArray &output, quint32 session, quint32 epoch,
               quint32 frame, qint64 timestamp, const Cancel &cancel) {
    require(session && epoch && frame, "缺少有效帧身份");
    Result r;
    r.session = session;
    r.epoch = epoch;
    r.frame = frame;
    r.cameraTimestamp = timestamp;
    r.capturedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    splitPair(pair, r.front, r.rear);
    r.output = output;
    r.fpga = QImage::fromData(output, "JPEG");
    require(r.fpga.size() == QSize(2656, 1328), "FPGA 回图 JPEG 解码失败或尺寸错误");
    cv::Mat mask;
    r.reference = renderReference(pair, mask, cancel);
    check(cancel);
    r.scores = compare(r.reference, r.fpga, mask);
    check(cancel);
    r.referenceId = referenceId();
    return r;
}
static QByteArray digest(const QByteArray &data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}
static void write(const QString &path, const QByteArray &data) {
    QSaveFile f(path);
    require(f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit(),
            "保存文件失败");
}
QImage inputImage(const QByteArray &front, const QByteArray &rear) {
    const auto left = QImage::fromData(front, "JPEG");
    const auto right = QImage::fromData(rear, "JPEG");
    require(left.size() == QSize(1328, 1328) && right.size() == left.size(),
            "同帧输入 JPEG 解码失败或尺寸错误");
    QImage image(2656, 1328, QImage::Format_RGB888);
    QPainter painter(&image);
    painter.drawImage(0, 0, left);
    painter.drawImage(1328, 0, right);
    return image;
}
void save(const Result &r, const QString &directory) {
    require(!r.reference.isNull() && r.scores.pixels > 0, "没有可保存的分析结果");
    require(QDir().mkpath(directory), "无法创建结果目录");
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    require(r.reference.save(&buffer, "PNG"), "参考图保存失败");
    QByteArray inputPng;
    QBuffer inputBuffer(&inputPng);
    inputBuffer.open(QIODevice::WriteOnly);
    require(inputImage(r.front, r.rear).save(&inputBuffer, "PNG"), "原始鱼眼图保存失败");
    QMap<QString, QByteArray> files = {{"front.jpg", r.front},
                                       {"rear.jpg", r.rear},
                                       {"fpga.jpg", r.output},
                                       {"fisheye.png", inputPng},
                                       {"reference.png", png}};
    QJsonObject hashes;
    for (auto it = files.begin(); it != files.end(); ++it) {
        write(directory + "/" + it.key(), it.value());
        hashes[it.key()] = QString::fromLatin1(digest(it.value()));
    }
    QJsonObject root{{"schema", 1},
                     {"algorithm", "bt601-full-ssim11-sigma1.5-v1"},
                     {"session", double(r.session)},
                     {"epoch", double(r.epoch)},
                     {"frame", double(r.frame)},
                     {"camera_timestamp_ms", QString::number(r.cameraTimestamp)},
                     {"captured_at", r.capturedAt},
                     {"reference_sha256", r.referenceId},
                     {"board_config_verified", false},
                     {"width", 2656},
                     {"height", 1328},
                     {"psnr_db", r.scores.infinite ? QJsonValue() : QJsonValue(r.scores.psnr)},
                     {"psnr_status", r.scores.infinite ? "infinite" : "finite"},
                     {"ssim", r.scores.ssim},
                     {"valid_pixels", r.scores.pixels},
                     {"ssim_windows", r.scores.windows},
                     {"coverage", r.scores.coverage},
                     {"files_sha256", hashes},
                     {"scope", "Same-frame software reference versus board JPEG; includes "
                               "codec/color differences; not pure corrector error"}};
    write(directory + "/metrics.csv",
          QString("metric,value,unit,status,samples,source\nPSNR,%1,dB,%2,%3,software_reference_vs_"
                  "board_JPEG\nSSIM,%4,,valid,%5,software_reference_vs_board_JPEG\n")
              .arg(r.scores.infinite ? QString() : QString::number(r.scores.psnr, 'g', 17),
                   r.scores.infinite ? "infinite" : "finite")
              .arg(r.scores.pixels)
              .arg(r.scores.ssim, 0, 'g', 17)
              .arg(r.scores.windows)
              .toUtf8());
    write(directory + "/quality.json", QJsonDocument(root).toJson()); // manifest committed last
}
Result load(const QString &directory, const Cancel &cancel) {
    auto read = [&](QString name) {
        check(cancel);
        QFile f(directory + "/" + name);
        require(f.open(QIODevice::ReadOnly) && f.size() < 64 * 1024 * 1024, "结果文件缺失或过大");
        return f.readAll();
    };
    auto root = QJsonDocument::fromJson(read("quality.json")).object();
    require(root["schema"].toInt() == 1 &&
                root["algorithm"].toString() == "bt601-full-ssim11-sigma1.5-v1",
            "不支持的结果格式");
    Result r;
    auto identity = [&](QString k) {
        double v = root[k].toDouble();
        require(v >= 1 && v <= 4294967295. && std::floor(v) == v, "无效帧身份");
        return quint32(v);
    };
    r.session = identity("session");
    r.epoch = identity("epoch");
    r.frame = identity("frame");
    r.referenceId = root["reference_sha256"].toString();
    r.capturedAt = root["captured_at"].toString();
    r.cameraTimestamp = root["camera_timestamp_ms"].toString().toLongLong();
    auto verified = [&](QString name) {
        auto data = read(name);
        require(QString::fromLatin1(digest(data)) ==
                    root["files_sha256"].toObject()[name].toString(),
                "文件校验失败，素材可能不匹配");
        return data;
    };
    r.front = verified("front.jpg");
    r.rear = verified("rear.jpg");
    r.output = verified("fpga.jpg");
    // Derived preview is optional so existing schema-1 result folders still open.
    if (root["files_sha256"].toObject().contains("fisheye.png"))
        verified("fisheye.png");
    auto png = verified("reference.png");
    r.reference = QImage::fromData(png, "PNG").convertToFormat(QImage::Format_RGB888);
    r.fpga = QImage::fromData(r.output, "JPEG");
    require(r.reference.size() == QSize(2656, 1328) && r.fpga.size() == r.reference.size(),
            "结果图片尺寸错误");
    r.scores.infinite = root["psnr_status"].toString() == "infinite";
    require(r.scores.infinite ||
                (root["psnr_status"].toString() == "finite" && root["psnr_db"].isDouble()),
            "PSNR 字段错误");
    r.scores.psnr = root["psnr_db"].toDouble();
    r.scores.ssim = root["ssim"].toDouble(-2);
    r.scores.pixels = root["valid_pixels"].toInt();
    r.scores.windows = root["ssim_windows"].toInt();
    r.scores.coverage = root["coverage"].toDouble();
    require(r.scores.pixels > 0 && r.scores.pixels <= 2656 * 1328 && r.scores.windows > 0 &&
                r.scores.windows <= 2646 * 1318 && r.scores.ssim >= -1.000001 &&
                r.scores.ssim <= 1.000001 &&
                std::abs(r.scores.coverage - double(r.scores.pixels) / (2656 * 1328)) < 1e-9,
            "结果统计字段错误");
    return r;
}
} // namespace Quality
