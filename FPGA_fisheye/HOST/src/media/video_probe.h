#pragma once
#include <QJsonObject>
#include <QString>
#include <array>
namespace df {
std::array<QString, 2> pairedVideoFiles(const QString &directory);
QJsonObject probeVideo(const QString &file);
QJsonObject verifyVideoPair(const QString &directory);
} // namespace df
