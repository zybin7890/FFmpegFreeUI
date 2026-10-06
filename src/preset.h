// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <stdexcept>

namespace fui {
struct Command { QString program; QStringList args; QString label; };
struct Plan { QList<Command> steps; double duration = 0; };
struct Error : std::runtime_error {
    explicit Error(const QString &s) : std::runtime_error(s.toUtf8().constData()) {}
};
QJsonArray schema();
QJsonArray builtins();
QJsonObject defaults();
QJsonObject loadPreset(const QString &path);
void saveJson(const QString &path, const QJsonObject &object);
QStringList tokenize(const QString &text);
QString displayCommand(const Command &command);
QString resolveTool(const QString &name, const QString &configured = {});
double probeDuration(const QString &input, const QString &ffprobe);
Plan compilePreset(const QJsonObject &preset, const QString &input, const QString &output,
                   const QString &ffmpeg, double duration = 0, const QString &passLog = {});
QString outputPath(const QJsonObject &preset, const QString &input, const QString &directory,
                   const QStringList &reserved = {});
}
