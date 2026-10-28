#pragma once
#include "types.h"
#include <opencv2/core.hpp>
namespace df {
cv::Matx33d rotation(double yaw = 0, double pitch = 0, double roll = 0);
cv::Mat panoramaRays(cv::Size size);
cv::Mat perspectiveRays(cv::Size size, const View &view);
cv::Mat sphereRays(cv::Size size, const View &view, cv::Mat &domain);
struct Lens {
    cv::Matx33d K;
    cv::Vec<double, 5> D;
    double xi = 0;
};
struct WarpPair {
    std::array<cv::Mat, 2> x, y, valid, fixed, subpixel;
    cv::Mat weight, rearWeight, mask;
    cv::Mat render(const cv::Mat &front, const cv::Mat &rear) const;
    void save(const QString &file, const QJsonObject &metadata) const;
};
class Projection {
  public:
    Projection(const QString &parameters, cv::Size panorama = {1920, 960},
               cv::Size gimbal = {640, 360}, cv::Size sphere = {360, 360});
    void setAlignment(const Alignment &value);
    void setView(const View &value);
    QString exportFrame(const QString &folder, const cv::Mat &front, const cv::Mat &rear,
                        const QJsonObject &source) const;
    WarpPair map(const cv::Mat &rays, const cv::Mat &domain = {}) const;
    cv::Size inputSize, panoramaSize, gimbalSize, sphereSize;
    WarpPair panoramaMap, gimbalMap, sphereMap;
    cv::Mat sphereDomain;
    View view;
    Alignment alignment;

  private:
    QJsonObject params;
    std::array<Lens, 2> lenses;
    std::array<cv::Matx33d, 2> rotations;
    bool poseReady = false, viewReady = false;
};
} // namespace df
