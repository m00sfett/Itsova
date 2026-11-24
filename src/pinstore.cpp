#include "pinstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

namespace glimply
{
namespace
{

constexpr int formatVersion = 1;

QString indexPath(const QString &directory)
{
    return directory + QStringLiteral("/pins.json");
}

bool validId(const QString &id)
{
    // Ids become file names: only accept what QUuid produces.
    return !id.isEmpty() && !QUuid::fromString(id).isNull();
}

} // namespace

PinStore::PinStore(QString directory)
    : directory_(std::move(directory))
{
}

QString PinStore::defaultDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/glimply/pins");
}

QString PinStore::directory() const
{
    return directory_;
}

QString PinStore::imagePath(const QString &id) const
{
    return directory_ + QLatin1Char('/') + id + QStringLiteral(".png");
}

bool PinStore::load(QString *error)
{
    pins_.clear();
    hidden_ = false;
    QFile file(indexPath(directory_));
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) {
            *error = parseError.error != QJsonParseError::NoError ? parseError.errorString()
                                                                   : QStringLiteral("not a JSON object");
        }
        return false;
    }
    const QJsonObject root = doc.object();
    hidden_ = root.value(QStringLiteral("hidden")).toBool(false);
    for (const QJsonValue &value : root.value(QStringLiteral("pins")).toArray()) {
        const QJsonObject obj = value.toObject();
        PinRecord record;
        record.id = obj.value(QStringLiteral("id")).toString();
        if (!validId(record.id) || !QFileInfo::exists(imagePath(record.id))) {
            continue; // an image that is gone cannot be shown again
        }
        record.connector = obj.value(QStringLiteral("connector")).toString();
        record.offset = QPoint(obj.value(QStringLiteral("x")).toInt(), obj.value(QStringLiteral("y")).toInt());
        record.zoom = std::clamp(obj.value(QStringLiteral("zoom")).toDouble(1.0), pinZoomSteps().first(),
                                 pinZoomSteps().last());
        record.opacity = clampPinOpacity(obj.value(QStringLiteral("opacity")).toDouble(1.0));
        record.created = QDateTime::fromString(obj.value(QStringLiteral("created")).toString(), Qt::ISODate);
        pins_.append(record);
    }
    return true;
}

bool PinStore::save(QString *error) const
{
    if (!QDir().mkpath(directory_)) {
        if (error) {
            *error = QStringLiteral("cannot create %1").arg(directory_);
        }
        return false;
    }
    QJsonArray array;
    for (const PinRecord &record : pins_) {
        array.append(QJsonObject{
            {QStringLiteral("id"), record.id},
            {QStringLiteral("connector"), record.connector},
            {QStringLiteral("x"), record.offset.x()},
            {QStringLiteral("y"), record.offset.y()},
            {QStringLiteral("zoom"), record.zoom},
            {QStringLiteral("opacity"), record.opacity},
            {QStringLiteral("created"), record.created.toString(Qt::ISODate)},
        });
    }
    const QJsonObject root{
        {QStringLiteral("version"), formatVersion},
        {QStringLiteral("hidden"), hidden_},
        {QStringLiteral("pins"), array},
    };
    const QString path = indexPath(directory_);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

std::optional<PinRecord> PinStore::add(const QImage &image, const QString &connector, const QPoint &offset,
                                       QString *error)
{
    if (image.isNull()) {
        if (error) {
            *error = QStringLiteral("empty image");
        }
        return std::nullopt;
    }
    if (!QDir().mkpath(directory_)) {
        if (error) {
            *error = QStringLiteral("cannot create %1").arg(directory_);
        }
        return std::nullopt;
    }
    PinRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.connector = connector;
    record.offset = offset;
    record.created = QDateTime::currentDateTime();
    const QString path = imagePath(record.id);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return std::nullopt;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    pins_.append(record);
    return record;
}

bool PinStore::update(const PinRecord &record)
{
    for (PinRecord &existing : pins_) {
        if (existing.id == record.id) {
            existing = record;
            existing.zoom = std::clamp(existing.zoom, pinZoomSteps().first(), pinZoomSteps().last());
            existing.opacity = clampPinOpacity(existing.opacity);
            return true;
        }
    }
    return false;
}

bool PinStore::remove(const QString &id)
{
    const qsizetype removed = pins_.removeIf([&id](const PinRecord &record) { return record.id == id; });
    if (removed == 0) {
        return false;
    }
    if (validId(id)) {
        QFile::remove(imagePath(id));
    }
    return true;
}

std::optional<PinRecord> PinStore::pin(const QString &id) const
{
    for (const PinRecord &record : pins_) {
        if (record.id == id) {
            return record;
        }
    }
    return std::nullopt;
}

const QList<PinRecord> &PinStore::pins() const
{
    return pins_;
}

bool PinStore::hidden() const
{
    return hidden_;
}

void PinStore::setHidden(bool hidden)
{
    hidden_ = hidden;
}

const QList<double> &pinZoomSteps()
{
    static const QList<double> steps{0.25, 0.33, 0.5, 0.67, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
    return steps;
}

double nextPinZoom(double current, int direction)
{
    const QList<double> &steps = pinZoomSteps();
    if (direction > 0) {
        for (double step : steps) {
            if (step > current + 1e-6) {
                return step;
            }
        }
        return steps.last();
    }
    if (direction < 0) {
        for (auto it = steps.crbegin(); it != steps.crend(); ++it) {
            if (*it < current - 1e-6) {
                return *it;
            }
        }
        return steps.first();
    }
    return std::clamp(current, steps.first(), steps.last());
}

double clampPinOpacity(double opacity)
{
    if (!std::isfinite(opacity)) {
        return 1.0;
    }
    return std::round(std::clamp(opacity, 0.2, 1.0) * 10.0) / 10.0;
}

double nextPinOpacity(double current, int direction)
{
    return clampPinOpacity(clampPinOpacity(current) + (direction > 0 ? 0.1 : direction < 0 ? -0.1 : 0.0));
}

QPoint resolvePinPosition(const PinRecord &record, const QList<PinOutput> &outputs, const QSize &size)
{
    if (outputs.isEmpty()) {
        return record.offset;
    }
    const PinOutput *target = &outputs.first();
    for (const PinOutput &output : outputs) {
        if (output.connector == record.connector) {
            target = &output;
            break;
        }
    }
    const QRect geo = target->geometry;
    // Keep at least part of the pin on the output so it can always be grabbed again.
    constexpr int keepVisible = 32;
    const int minX = geo.left() - std::max(0, size.width() - keepVisible);
    const int maxX = geo.right() - keepVisible;
    const int minY = geo.top();
    const int maxY = geo.bottom() - keepVisible;
    const QPoint wanted = geo.topLeft() + record.offset;
    return {std::clamp(wanted.x(), std::min(minX, maxX), std::max(minX, maxX)),
            std::clamp(wanted.y(), std::min(minY, maxY), std::max(minY, maxY))};
}

std::optional<std::pair<QString, QPoint>> pinOutputAt(const QPoint &globalTopLeft, const QList<PinOutput> &outputs)
{
    if (outputs.isEmpty()) {
        return std::nullopt;
    }
    const PinOutput *best = nullptr;
    qint64 bestDistance = std::numeric_limits<qint64>::max();
    for (const PinOutput &output : outputs) {
        if (output.geometry.contains(globalTopLeft)) {
            best = &output;
            break;
        }
        const int dx = std::max({output.geometry.left() - globalTopLeft.x(), 0, globalTopLeft.x() - output.geometry.right()});
        const int dy = std::max({output.geometry.top() - globalTopLeft.y(), 0, globalTopLeft.y() - output.geometry.bottom()});
        const qint64 distance = qint64(dx) * dx + qint64(dy) * dy;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = &output;
        }
    }
    return std::make_pair(best->connector, globalTopLeft - best->geometry.topLeft());
}

} // namespace glimply
