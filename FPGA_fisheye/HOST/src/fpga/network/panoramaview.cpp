#include "panoramaview.h"
#include "core/projection.h"
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>
namespace {
constexpr double pi = 3.14159265358979323846;
}
void PanoramaView::maps(const cv::Mat &rays, cv::Mat &mx, cv::Mat &my) {
    mx.create(rays.size(), CV_32F);
    my.create(rays.size(), CV_32F);
    for (int y = 0; y < rays.rows; ++y)
        for (int x = 0; x < rays.cols; ++x) {
            const auto r = rays.at<cv::Vec3d>(y, x);
            const double lon = std::atan2(r[0], r[2]), lat = std::asin(std::clamp(r[1], -1., 1.));
            mx.at<float>(y, x) = float((lon / (2 * pi) + .5) * source.width() - .5);
            my.at<float>(y, x) = float(std::clamp((lat / pi + .5) * source.height() - .5, 0.,
                                                  double(source.height() - 1)));
        }
}
void PanoramaView::render(const QImage &panorama, const df::View &view, QImage &sphere,
                          QImage &perspective, QSize sphereSize, QSize perspectiveSize) {
    if (panorama.isNull()) {
        sphere = {};
        perspective = {};
        return;
    }
    const bool changed = !ready || source != panorama.size() || !(view == previous);
    if (changed || sphereSize != sphereResolution || perspectiveSize != viewResolution) {
        source = panorama.size();
        previous = view;
        cv::Mat x, y;
        if (changed || sphereSize != sphereResolution) {
            maps(df::sphereRays({sphereSize.width(), sphereSize.height()}, view, domain), x, y);
            // Cached fixed-point maps use the same bilinear interpolation,
            // with less per-frame work at presentation resolutions.
            cv::convertMaps(x, y, sphereX, sphereY, CV_16SC2);
            sphereResolution = sphereSize;
        }
        if (changed || perspectiveSize != viewResolution) {
            maps(df::perspectiveRays({perspectiveSize.width(), perspectiveSize.height()}, view), x, y);
            cv::convertMaps(x, y, viewX, viewY, CV_16SC2);
            viewResolution = perspectiveSize;
        }
        ready = true;
    }
    auto rgb = panorama.convertToFormat(QImage::Format_RGB888);
    cv::Mat input(rgb.height(), rgb.width(), CV_8UC3, rgb.bits(), rgb.bytesPerLine()), a, b;
    cv::remap(input, a, sphereX, sphereY, cv::INTER_LINEAR, cv::BORDER_WRAP);
    a.setTo(cv::Scalar(0, 0, 0), domain == 0);
    cv::remap(input, b, viewX, viewY, cv::INTER_LINEAR, cv::BORDER_WRAP);
    sphere = QImage(a.data, a.cols, a.rows, int(a.step), QImage::Format_RGB888).copy();
    perspective = QImage(b.data, b.cols, b.rows, int(b.step), QImage::Format_RGB888).copy();
}
