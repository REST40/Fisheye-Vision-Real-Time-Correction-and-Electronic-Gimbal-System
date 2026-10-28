#pragma once
#include <QJsonObject>
#include <QString>
namespace df {
QJsonObject probeVideo(const QString &file);
QJsonObject verifyVideoPair(const QString &directory);
} // namespace df
