#pragma once

#include "imageoutput.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

#include <optional>

class QIODevice;

namespace glimply
{

struct CliCaptureFile
{
    QString targetId;
    QString path;
    int width = 0;
    int height = 0;
    QString format;
    qint64 bytes = 0;
};

struct CliCaptureRequest
{
    QString target;
    std::optional<ImageFormat> format;
    std::optional<int> quality;
    std::optional<int> pngCompression;
    std::optional<bool> lossless;
    std::optional<bool> cursor;
    int delaySeconds = 0;
    QString outPath;
    bool toStdout = false;
    bool clipboard = false;
    bool noSave = false;
    QString triggerKind;
};

struct CliCaptureResult
{
    bool ok = false;
    int exitCode = 1; // 0 = OK, 1 = error, 2 = bad arguments, 3 = target not found / unavailable
    QString targetId;
    QString displayName;
    QString timestamp;
    QList<CliCaptureFile> files;
    QString stdoutFile;
    bool tempStdoutFile = false;
    QString error;
};

struct CliResolveResult
{
    bool ok = false;
    QString targetId;
    QString kind;
    QString name;
    QString error;
};

struct CliSimpleResult
{
    bool ok = false;
    int exitCode = 1;
    QJsonObject data;
    QString error;
};

[[nodiscard]] QJsonObject captureRequestToJson(const CliCaptureRequest &request);
[[nodiscard]] CliCaptureRequest captureRequestFromJson(const QJsonObject &obj, bool *ok = nullptr);
[[nodiscard]] QJsonObject captureResultToJson(const CliCaptureResult &result);
[[nodiscard]] CliCaptureResult captureResultFromJson(const QJsonObject &obj);

class CliBackend
{
public:
    virtual ~CliBackend() = default;

    virtual CliCaptureResult capture(const CliCaptureRequest &request) = 0;
    virtual CliSimpleResult listTargets(QJsonArray *outArray) = 0;
    virtual CliSimpleResult listMonitors(QJsonArray *outArray) = 0;
    virtual CliSimpleResult listWindows(QJsonArray *outArray) = 0;
    virtual CliResolveResult resolveTarget(const QString &query) = 0;
    virtual CliSimpleResult addAreaInteractive() = 0;
    virtual CliSimpleResult addAreaRect(const QString &name, const QString &connector, const QRect &rect) = 0;
    virtual CliSimpleResult editArea(const QString &targetId) = 0;
    virtual CliSimpleResult renameTarget(const QString &targetId, const QString &newName) = 0;
    virtual CliSimpleResult removeTarget(const QString &targetId) = 0;
    virtual CliSimpleResult startInterval(const QString &targetId, int seconds, int maxCaptures, int maxMinutes,
                                          bool onlyWhenChanged) = 0;
    virtual CliSimpleResult stopInterval(const QString &targetId) = 0;
    virtual CliSimpleResult stopAllIntervals() = 0;
    virtual CliSimpleResult intervalStatus(QJsonArray *outArray) = 0;
    virtual CliSimpleResult openOverlay(const QString &mode, int delaySeconds) = 0;
    // Capture history, newest first (JSON as returned by D-Bus History()).
    virtual CliSimpleResult history(QJsonArray *outArray)
    {
        Q_UNUSED(outArray);
        return {false, 1, {}, QStringLiteral("history is not supported by this backend")};
    }
};

class DBusCliBackend final : public CliBackend
{
public:
    CliCaptureResult capture(const CliCaptureRequest &request) override;
    CliSimpleResult listTargets(QJsonArray *outArray) override;
    CliSimpleResult listMonitors(QJsonArray *outArray) override;
    CliSimpleResult listWindows(QJsonArray *outArray) override;
    CliResolveResult resolveTarget(const QString &query) override;
    CliSimpleResult addAreaInteractive() override;
    CliSimpleResult addAreaRect(const QString &name, const QString &connector, const QRect &rect) override;
    CliSimpleResult editArea(const QString &targetId) override;
    CliSimpleResult renameTarget(const QString &targetId, const QString &newName) override;
    CliSimpleResult removeTarget(const QString &targetId) override;
    CliSimpleResult startInterval(const QString &targetId, int seconds, int maxCaptures, int maxMinutes,
                                  bool onlyWhenChanged) override;
    CliSimpleResult stopInterval(const QString &targetId) override;
    CliSimpleResult stopAllIntervals() override;
    CliSimpleResult intervalStatus(QJsonArray *outArray) override;
    CliSimpleResult openOverlay(const QString &mode, int delaySeconds) override;
    CliSimpleResult history(QJsonArray *outArray) override;
};

[[nodiscard]] bool isCliSubcommand(const QString &arg);
[[nodiscard]] int runCli(const QStringList &arguments, CliBackend &backend, QIODevice &out, QIODevice &err);

} // namespace glimply
