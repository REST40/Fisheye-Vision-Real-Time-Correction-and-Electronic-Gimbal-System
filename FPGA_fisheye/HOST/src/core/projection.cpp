#include "projection.h"
#include "npz_writer.h"
#include <QDir>
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
namespace df {
static constexpr double pi = 3.14159265358979323846;
static double rad(double a) {
    return a * pi / 180;
}
cv::Matx33d rotation(double yaw, double pitch, double roll) {
    double y = rad(yaw), p = rad(pitch), r = rad(roll), cy = cos(y), sy = sin(y), cp = cos(p),
           sp = sin(p), cr = cos(r), sr = sin(r);
    return cv::Matx33d(cy, 0, sy, 0, 1, 0, -sy, 0, cy) *
           cv::Matx33d(1, 0, 0, 0, cp, -sp, 0, sp, cp) *
           cv::Matx33d(cr, -sr, 0, sr, cr, 0, 0, 0, 1);
}
cv::Mat panoramaRays(cv::Size size) {
    cv::Mat out(size, CV_64FC3);
    cv::parallel_for_(cv::Range(0, size.height), [&](const cv::Range &range) {
        for (int y = range.start; y < range.end; ++y) {
            double lat = ((y + .5) / size.height - .5) * pi;
            auto *row = out.ptr<cv::Vec3d>(y);
            for (int x = 0; x < size.width; ++x) {
                double lon = ((x + .5) / size.width - .5) * 2 * pi;
                row[x] = {cos(lat) * sin(lon), sin(lat), cos(lat) * cos(lon)};
            }
        }
    });
    return out;
}
cv::Mat perspectiveRays(cv::Size size, const View &view) {
    if (!std::isfinite(view.yaw) || !std::isfinite(view.pitch) || !std::isfinite(view.hfov) ||
        view.hfov < 20 || view.hfov > 140 || fabs(view.pitch) > 90)
        throw std::runtime_error("云台角度超出范围");
    cv::Mat out(size, CV_64FC3);
    double f = size.width / (2 * tan(rad(view.hfov / 2)));
    auto R = rotation(view.yaw, view.pitch);
    cv::parallel_for_(cv::Range(0, size.height), [&](const cv::Range &range) {
        for (int y = range.start; y < range.end; ++y) {
            auto *row = out.ptr<cv::Vec3d>(y);
            for (int x = 0; x < size.width; ++x) {
                cv::Vec3d ray((x - (size.width - 1) / 2.) / f, (y - (size.height - 1) / 2.) / f, 1);
                row[x] = R * (ray / cv::norm(ray));
            }
        }
    });
    return out;
}
cv::Mat sphereRays(cv::Size size, const View &view, cv::Mat &domain) {
    cv::Mat out(size, CV_64FC3);
    domain = cv::Mat::zeros(size, CV_8U);
    double f = std::min(size.width, size.height) / (2 * tan(rad(15))), distance = 4.5;
    auto R = rotation(view.yaw, view.pitch);
    cv::parallel_for_(cv::Range(0, size.height), [&](const cv::Range &range) {
        for (int y = range.start; y < range.end; ++y) {
            auto *row = out.ptr<cv::Vec3d>(y);
            auto *mask = domain.ptr<uchar>(y);
            for (int x = 0; x < size.width; ++x) {
                double dx = (x - (size.width - 1) / 2.) / f, dy = (y - (size.height - 1) / 2.) / f,
                       a = dx * dx + dy * dy + 1,
                       disc = distance * distance - a * (distance * distance - 1),
                       t = (distance - sqrt(std::max(0., disc))) / a;
                cv::Vec3d point(t * dx, t * dy, distance - t);
                row[x] = R * (point / cv::norm(point));
                mask[x] = disc >= 0 ? 255 : 0;
            }
        }
    });
    return out;
}
cv::Mat WarpPair::render(const cv::Mat &front, const cv::Mat &rear) const {
    cv::Mat a, b, result;
    cv::remap(front, a, fixed[0], subpixel[0], cv::INTER_LINEAR, cv::BORDER_CONSTANT);
    cv::remap(rear, b, fixed[1], subpixel[1], cv::INTER_LINEAR, cv::BORDER_CONSTANT);
    cv::blendLinear(a, b, weight, rearWeight, result);
    cv::Mat masked = cv::Mat::zeros(result.size(), result.type());
    result.copyTo(masked, mask);
    return masked;
}
void WarpPair::save(const QString &file, const QJsonObject &metadata) const {
    NpzWriter writer(file);
    for (int i = 0; i < 2; ++i) {
        QString prefix = i == 0 ? "front" : "rear";
        writer.array((prefix + "_map_x").toUtf8().constData(), x[i]);
        writer.array((prefix + "_map_y").toUtf8().constData(), y[i]);
        writer.array((prefix + "_valid").toUtf8().constData(), valid[i], true);
    }
    writer.array("front_weight", weight);
    writer.array("output_valid", mask, true);
    writer.metadata(metadata);
    writer.finish();
}
Projection::Projection(const QString &parameters, cv::Size panorama, cv::Size gimbal,
                       cv::Size sphere)
    : panoramaSize(panorama), gimbalSize(gimbal), sphereSize(sphere) {
    params = readJson(parameters);
    if (params["model"].toString() !=
            "Mei_unified_omnidirectional_with_5_Brown_Conrady_coefficients" ||
        params["D_order"].toArray() != QJsonArray{"k1", "k2", "p1", "p2", "k3"})
        throw std::runtime_error("需要Mei与完整5项Brown模型参数");
    auto size = params["input_size"].toArray();
    inputSize = {size.at(0).toInt(), size.at(1).toInt()};
    if (inputSize.width <= 0 || inputSize.height <= 0)
        throw std::runtime_error("输入尺寸无效");
    auto all = params["lenses"].toObject();
    int index = 0;
    for (const auto &name : {"front", "rear"}) {
        auto item = all[name].toObject();
        auto K = item["K"].toArray(), D = item["D"].toArray();
        if (K.size() != 3 || D.size() != 5)
            throw std::runtime_error("K或D维度不正确");
        auto &lens = lenses[index++];
        for (int y = 0; y < 3; ++y) {
            if (K[y].toArray().size() != 3)
                throw std::runtime_error("K维度不正确");
            for (int x = 0; x < 3; ++x)
                lens.K(y, x) = K[y].toArray()[x].toDouble();
        }
        for (int i = 0; i < 5; ++i)
            lens.D[i] = D[i].toDouble();
        lens.xi = item["xi"].toDouble();
        if (lens.xi <= 0)
            throw std::runtime_error("xi无效");
    }
    rotations = {cv::Matx33d::eye(), rotation(180)};
}
WarpPair Projection::map(const cv::Mat &rays, const cv::Mat &domain) const {
    WarpPair out;
    cv::Size size = rays.size();
    for (int i = 0; i < 2; ++i) {
        out.x[i] = cv::Mat(size, CV_32F);
        out.y[i] = cv::Mat(size, CV_32F);
        out.valid[i] = cv::Mat(size, CV_8U);
    }
    out.weight = cv::Mat(size, CV_32F);
    out.mask = cv::Mat(size, CV_8U);
    double width = std::max(1e-6, 2 * sin(rad(alignment.blend / 2)));
    cv::parallel_for_(cv::Range(0, size.height), [&](const cv::Range &range) {
        for (int y = range.start; y < range.end; ++y) {
            for (int x = 0; x < size.width; ++x) {
                std::array<double, 2> axes;
                bool valid[2];
                auto world = rays.at<cv::Vec3d>(y, x);
                bool inside = domain.empty() || domain.at<uchar>(y, x) != 0;
                for (int i = 0; i < 2; ++i) {
                    const auto &lens = lenses[i];
                    auto local = rotations[i].t() * world;
                    axes[i] = local[2];
                    double denominator = local[2] + lens.xi * cv::norm(local),
                           u = local[0] / denominator, v = local[1] / denominator,
                           r2 = u * u + v * v,
                           radial =
                               1 + lens.D[0] * r2 + lens.D[1] * r2 * r2 + lens.D[4] * r2 * r2 * r2;
                    double xd = u * radial + 2 * lens.D[2] * u * v + lens.D[3] * (r2 + 2 * u * u),
                           yd = v * radial + lens.D[2] * (r2 + 2 * v * v) + 2 * lens.D[3] * u * v,
                           cx = lens.K(0, 2), cy = lens.K(1, 2), px = lens.K(0, 0) * xd + cx,
                           py = lens.K(1, 1) * yd + cy,
                           radius = std::min(
                               {cx, cy, inputSize.width - 1 - cx, inputSize.height - 1 - cy}),
                           limit = lens.xi > 1 ? -1 / lens.xi : -1;
                    valid[i] = inside && local[2] > limit + 1e-7 && std::isfinite(px) &&
                               std::isfinite(py) && px >= 0 && px < inputSize.width - 1 &&
                               py >= 0 && py < inputSize.height - 1 &&
                               pow(px - cx, 2) + pow(py - cy, 2) <= radius * radius;
                    out.x[i].at<float>(y, x) = valid[i] ? float(px) : -1;
                    out.y[i].at<float>(y, x) = valid[i] ? float(py) : -1;
                    out.valid[i].at<uchar>(y, x) = valid[i] ? 255 : 0;
                }
                double t = std::clamp(.5 + .5 * (axes[0] - axes[1]) / width, 0., 1.);
                t = t * t * (3 - 2 * t);
                double a = valid[0] ? (valid[1] ? t : 1) : 0,
                       b = valid[1] ? (valid[0] ? 1 - t : 1) : 0, total = a + b;
                out.weight.at<float>(y, x) = total > 1e-10 ? float(a / total) : 0;
                out.mask.at<uchar>(y, x) = inside && total > 1e-10 ? 255 : 0;
            }
        }
    });
    out.rearWeight = 1 - out.weight;
    for (int i = 0; i < 2; ++i)
        cv::convertMaps(out.x[i], out.y[i], out.fixed[i], out.subpixel[i], CV_16SC2);
    return out;
}
void Projection::setAlignment(const Alignment &value) {
    if (poseReady && value == alignment)
        return;
    if (value.blend < 1 || value.blend > 60 || !std::isfinite(value.blend))
        throw std::runtime_error("融合宽度无效");
    for (double a : value.rear)
        if (!std::isfinite(a))
            throw std::runtime_error("镜头对齐角度无效");
    alignment = value;
    rotations = {cv::Matx33d::eye(),
                 rotation(180) * rotation(value.rear[0], value.rear[1], value.rear[2])};
    panoramaMap = map(panoramaRays(panoramaSize));
    poseReady = true;
    viewReady = false;
}
void Projection::setView(const View &value) {
    if (viewReady && view == value)
        return;
    if (!poseReady)
        setAlignment(alignment);
    view = value;
    gimbalMap = map(perspectiveRays(gimbalSize, value));
    sphereMap = map(sphereRays(sphereSize, value, sphereDomain), sphereDomain);
    viewReady = true;
}
QString Projection::exportFrame(const QString &folder, const cv::Mat &front, const cv::Mat &rear,
                                const QJsonObject &source) const {
    QDir directory;
    if (QDir(folder).exists() || !directory.mkpath(folder))
        throw std::runtime_error("导出目录不可用");
    cv::Mat sphere;
    cv::cvtColor(sphereMap.render(front, rear), sphere, cv::COLOR_BGR2BGRA);
    std::vector<cv::Mat> channels;
    cv::split(sphere, channels);
    channels[3] = sphereDomain;
    cv::merge(channels, sphere);
    for (const auto &image :
         std::array<std::pair<QString, cv::Mat>, 3>{{{"panorama", panoramaMap.render(front, rear)},
                                                     {"sphere", sphere},
                                                     {"gimbal", gimbalMap.render(front, rear)}}})
        if (!cv::imwrite((folder + "/" + image.first + ".png").toStdString(), image.second))
            throw std::runtime_error("图像导出失败");
    QJsonArray matrices;
    for (const auto &r : rotations) {
        QJsonArray rows;
        for (int y = 0; y < 3; ++y)
            rows.append(QJsonArray{r(y, 0), r(y, 1), r(y, 2)});
        matrices.append(rows);
    }
    QJsonObject info{
        {"schema_version", 1},
        {"model", params["model"]},
        {"D_order", params["D_order"]},
        {"lens_parameters", params["lenses"]},
        {"input_size", QJsonArray{inputSize.width, inputSize.height}},
        {"panorama_size", QJsonArray{panoramaSize.width, panoramaSize.height}},
        {"gimbal_size", QJsonArray{gimbalSize.width, gimbalSize.height}},
        {"sphere_size", QJsonArray{sphereSize.width, sphereSize.height}},
        {"rear_nominal_yaw_degrees", 180},
        {"rear_offsets_degrees",
         QJsonArray{alignment.rear[0], alignment.rear[1], alignment.rear[2]}},
        {"blend_degrees", alignment.blend},
        {"yaw_degrees", view.yaw},
        {"pitch_degrees", view.pitch},
        {"hfov_degrees", view.hfov},
        {"vfov_degrees",
         atan(double(gimbalSize.height) / gimbalSize.width * tan(rad(view.hfov / 2))) * 360 / pi},
        {"lens_to_world_rotations", matrices},
        {"factory_conversion_certified", false},
        {"relative_rotation_factory_verified", false},
        {"depth_parallax_compensated", false},
        {"crop_policy", params["crop_policy"]},
        {"source", source},
        {"coordinate_axes", "X right, Y down, Z front; positive yaw right, pitch up"},
        {"sampling", "remap INTER_LINEAR + blendLinear; invalid pixels black"}};
    for (const auto &entry : std::array<std::pair<QString, const WarpPair *>, 3>{
             {{"panorama", &panoramaMap}, {"sphere", &sphereMap}, {"gimbal", &gimbalMap}}}) {
        auto metadata = info;
        metadata["projection"] = entry.first;
        entry.second->save(folder + "/" + entry.first + "_lut.npz", metadata);
    }
    writeJson(folder + "/simulation_parameters.json", info);
    return folder;
}
} // namespace df
