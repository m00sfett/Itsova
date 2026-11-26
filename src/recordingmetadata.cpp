#include "recordingmetadata.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

#include <cmath>

namespace glimply
{

namespace
{

const QString tagName = QStringLiteral("glimply");

QJsonArray rectJson(const QRect &rect)
{
    return {rect.x(), rect.y(), rect.width(), rect.height()};
}

QRect rectFrom(const QJsonValue &value)
{
    const QJsonArray a = value.toArray();
    return a.size() == 4 ? QRect(a.at(0).toInt(), a.at(1).toInt(), a.at(2).toInt(), a.at(3).toInt()) : QRect();
}

QString kindOf(const RecordingTarget &target)
{
    if (target.kind == RecordingTarget::Kind::Window) {
        return QStringLiteral("window");
    }
    if (target.targetId == allMonitorsTargetId()) {
        return QStringLiteral("all-monitors");
    }
    if (target.targetId.startsWith(QLatin1String("screen:"))) {
        return QStringLiteral("monitor");
    }
    return QStringLiteral("area");
}

// Logical → video pixels, rounded outwards so neighbouring monitors meet without a gap.
QRect toVideo(const QRect &logical, const QPoint &origin, qreal scale)
{
    const qreal x0 = (logical.left() - origin.x()) * scale;
    const qreal y0 = (logical.top() - origin.y()) * scale;
    const qreal x1 = (logical.left() + logical.width() - origin.x()) * scale;
    const qreal y1 = (logical.top() + logical.height() - origin.y()) * scale;
    return QRect(QPoint(int(std::lround(x0)), int(std::lround(y0))),
                 QPoint(int(std::lround(x1)) - 1, int(std::lround(y1)) - 1));
}

RecordingMonitorInfo monitorInfo(const ScreenInfo &screen, qreal scale)
{
    RecordingMonitorInfo info;
    info.connector = screen.connector;
    info.manufacturer = screen.manufacturer;
    info.model = screen.model;
    info.serial = screen.serialNumber;
    info.logical = screen.geometry;
    info.scale = scale;
    info.resolution = QSize(int(std::lround(screen.geometry.width() * scale)),
                            int(std::lround(screen.geometry.height() * scale)));
    return info;
}

} // namespace

RecordingMetadata buildRecordingMetadata(const RecordingTarget &target, const QList<ScreenInfo> &screens,
                                         const QList<qreal> &scales)
{
    RecordingMetadata meta;
    meta.kind = kindOf(target);
    meta.targetId = target.targetId;
    if (target.kind == RecordingTarget::Kind::Window) {
        // The window streams at the scale of the monitor it is on (its centre decides).
        const QPoint centre = target.region.center();
        for (int i = 0; i < screens.size(); ++i) {
            if (screens.at(i).geometry.contains(centre)) {
                const qreal scale = scales.value(i, 1.0);
                meta.streamScale = scale;
                meta.windowRect = target.region.translated(-screens.at(i).geometry.topLeft());
                meta.monitors.append(monitorInfo(screens.at(i), scale));
                break;
            }
        }
        return meta;
    }
    meta.streamScale = target.scale;
    meta.region = target.region;
    for (int i = 0; i < screens.size(); ++i) {
        const QRect part = screens.at(i).geometry.intersected(target.region);
        if (part.isEmpty()) {
            continue;
        }
        RecordingMonitorInfo info = monitorInfo(screens.at(i), scales.value(i, 1.0));
        info.video = toVideo(part, target.region.topLeft(), target.scale);
        meta.monitors.append(info);
    }
    return meta;
}

QString recordingMetadataJson(const RecordingMetadata &metadata)
{
    QJsonArray monitors;
    for (const RecordingMonitorInfo &m : metadata.monitors) {
        monitors.append(QJsonObject{
            {QStringLiteral("connector"), m.connector},
            {QStringLiteral("manufacturer"), m.manufacturer},
            {QStringLiteral("model"), m.model},
            {QStringLiteral("serial"), m.serial},
            {QStringLiteral("logical"), rectJson(m.logical)},
            {QStringLiteral("video"), rectJson(m.video)},
            {QStringLiteral("scale"), m.scale},
            {QStringLiteral("resolution"), QJsonArray{m.resolution.width(), m.resolution.height()}},
        });
    }
    QJsonObject root{
        {QStringLiteral("version"), metadata.version},
        {QStringLiteral("kind"), metadata.kind},
        {QStringLiteral("targetId"), metadata.targetId},
        {QStringLiteral("streamScale"), metadata.streamScale},
        {QStringLiteral("monitors"), monitors},
    };
    if (!metadata.region.isNull()) {
        root.insert(QStringLiteral("region"), rectJson(metadata.region));
    }
    if (!metadata.windowRect.isNull()) {
        root.insert(QStringLiteral("windowRect"), rectJson(metadata.windowRect));
    }
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

std::optional<RecordingMetadata> parseRecordingMetadata(const QString &json)
{
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }
    const QJsonObject root = doc.object();
    RecordingMetadata meta;
    meta.version = root.value(QStringLiteral("version")).toInt();
    if (meta.version < 1 || meta.version > RecordingMetadata::currentVersion) {
        return std::nullopt;
    }
    meta.kind = root.value(QStringLiteral("kind")).toString();
    meta.targetId = root.value(QStringLiteral("targetId")).toString();
    meta.streamScale = root.value(QStringLiteral("streamScale")).toDouble(1.0);
    meta.region = rectFrom(root.value(QStringLiteral("region")));
    meta.windowRect = rectFrom(root.value(QStringLiteral("windowRect")));
    for (const QJsonValue &value : root.value(QStringLiteral("monitors")).toArray()) {
        const QJsonObject o = value.toObject();
        RecordingMonitorInfo m;
        m.connector = o.value(QStringLiteral("connector")).toString();
        m.manufacturer = o.value(QStringLiteral("manufacturer")).toString();
        m.model = o.value(QStringLiteral("model")).toString();
        m.serial = o.value(QStringLiteral("serial")).toString();
        m.logical = rectFrom(o.value(QStringLiteral("logical")));
        m.video = rectFrom(o.value(QStringLiteral("video")));
        m.scale = o.value(QStringLiteral("scale")).toDouble(1.0);
        const QJsonArray resolution = o.value(QStringLiteral("resolution")).toArray();
        m.resolution = QSize(resolution.at(0).toInt(), resolution.at(1).toInt());
        meta.monitors.append(m);
    }
    return meta;
}

std::optional<RecordingMetadata> readRecordingMetadata(const QString &path)
{
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffprobe.isEmpty()) {
        return std::nullopt;
    }
    QProcess probe;
    probe.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                          QStringLiteral("format_tags=") + tagName, QStringLiteral("-of"),
                          QStringLiteral("default=nw=1:nk=1"), path});
    if (!probe.waitForFinished(30000) || probe.exitCode() != 0) {
        return std::nullopt;
    }
    const QString json = QString::fromUtf8(probe.readAllStandardOutput()).trimmed();
    return json.isEmpty() ? std::nullopt : parseRecordingMetadata(json);
}

QStringList recordingMetadataArguments(const QString &outputPath, const QString &json)
{
    if (json.isEmpty()) {
        return {};
    }
    const QString suffix = QFileInfo(outputPath).suffix().toLower();
    if (suffix == QLatin1String("mp4")) {
        // MP4 keeps only its standard tags unless told to write custom ones.
        return {QStringLiteral("-movflags"), QStringLiteral("+use_metadata_tags"), QStringLiteral("-metadata"),
                tagName + QLatin1Char('=') + json};
    }
    if (suffix == QLatin1String("webm") || suffix == QLatin1String("mkv")) {
        return {QStringLiteral("-metadata"), tagName + QLatin1Char('=') + json};
    }
    return {}; // GIF has no container tags
}

} // namespace glimply
