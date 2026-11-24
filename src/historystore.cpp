#include "historystore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace glimply
{
namespace
{

constexpr int formatVersion = 1;

QJsonObject entryToJson(const HistoryEntry &entry)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), entry.id);
    obj.insert(QStringLiteral("timestamp"), entry.timestamp.toString(Qt::ISODateWithMs));
    obj.insert(QStringLiteral("targetId"), entry.targetId);
    obj.insert(QStringLiteral("targetName"), entry.targetName);
    obj.insert(QStringLiteral("triggerKind"), entry.triggerKind);
    return obj;
}

std::optional<HistoryEntry> entryFromJson(const QJsonObject &obj)
{
    HistoryEntry entry;
    entry.id = obj.value(QStringLiteral("id")).toString();
    entry.timestamp = QDateTime::fromString(obj.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
    entry.targetId = obj.value(QStringLiteral("targetId")).toString();
    entry.targetName = obj.value(QStringLiteral("targetName")).toString();
    entry.triggerKind = obj.value(QStringLiteral("triggerKind")).toString();
    const QJsonArray files = obj.value(QStringLiteral("files")).toArray();
    for (const QJsonValue &value : files) {
        // Files are plain strings on disk and objects with a thumbnail in the D-Bus reply.
        const QString path = value.isObject() ? value.toObject().value(QStringLiteral("path")).toString()
                                              : value.toString();
        if (!path.isEmpty() && !entry.files.contains(path)) {
            entry.files.append(path);
        }
    }
    if (entry.id.isEmpty() || entry.files.isEmpty()) {
        return std::nullopt;
    }
    return entry;
}

} // namespace

HistoryStore::HistoryStore(QString filePath, int limit)
    : filePath_(std::move(filePath))
    , limit_(std::clamp(limit, MinLimit, MaxLimit))
{
}

QString HistoryStore::defaultFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)
           + QStringLiteral("/glimply/history.json");
}

QString HistoryStore::filePath() const
{
    return filePath_;
}

bool HistoryStore::load(QString *error)
{
    entries_.clear();
    QFile file(filePath_);
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
    const QJsonArray array = doc.object().value(QStringLiteral("entries")).toArray();
    for (const QJsonValue &value : array) {
        if (std::optional<HistoryEntry> entry = entryFromJson(value.toObject())) {
            entries_.append(*entry);
        }
    }
    trim();
    return true;
}

bool HistoryStore::save(QString *error) const
{
    const QFileInfo info(filePath_);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error) {
            *error = QStringLiteral("cannot create %1").arg(info.absolutePath());
        }
        return false;
    }
    QJsonArray array;
    for (const HistoryEntry &entry : entries_) {
        QJsonObject obj = entryToJson(entry);
        obj.insert(QStringLiteral("files"), QJsonArray::fromStringList(entry.files));
        array.append(obj);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), formatVersion);
    root.insert(QStringLiteral("entries"), array);

    QSaveFile file(filePath_);
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
    QFile::setPermissions(filePath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

int HistoryStore::limit() const
{
    return limit_;
}

bool HistoryStore::setLimit(int limit)
{
    const int clamped = std::clamp(limit, MinLimit, MaxLimit);
    const qsizetype before = entries_.size();
    const bool limitChanged = clamped != limit_;
    limit_ = clamped;
    trim();
    return limitChanged || entries_.size() != before;
}

bool HistoryStore::record(HistoryEntry entry)
{
    if (entry.triggerKind == QStringLiteral("interval")) {
        return false;
    }
    QStringList files;
    for (const QString &path : std::as_const(entry.files)) {
        if (!path.isEmpty() && !files.contains(path)) {
            files.append(path);
        }
    }
    if (files.isEmpty()) {
        return false;
    }
    entry.files = files;
    if (entry.id.isEmpty()) {
        entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!entry.timestamp.isValid()) {
        entry.timestamp = QDateTime::currentDateTime();
    }
    // A file written again (for example an editor save over the same path) moves to the top.
    for (const QString &path : std::as_const(files)) {
        removeFile(path);
    }
    entries_.prepend(entry);
    trim();
    return true;
}

bool HistoryStore::prune(const ExistsFunction &exists)
{
    const ExistsFunction check = exists ? exists : [](const QString &path) { return QFileInfo::exists(path); };
    bool changed = false;
    for (HistoryEntry &entry : entries_) {
        changed |= entry.files.removeIf([&check](const QString &path) { return !check(path); }) > 0;
    }
    changed |= entries_.removeIf([](const HistoryEntry &entry) { return entry.files.isEmpty(); }) > 0;
    return changed;
}

bool HistoryStore::removeFile(const QString &path)
{
    bool changed = false;
    for (HistoryEntry &entry : entries_) {
        changed |= entry.files.removeAll(path) > 0;
    }
    changed |= entries_.removeIf([](const HistoryEntry &entry) { return entry.files.isEmpty(); }) > 0;
    return changed;
}

bool HistoryStore::removeEntry(const QString &id)
{
    return entries_.removeIf([&id](const HistoryEntry &entry) { return entry.id == id; }) > 0;
}

void HistoryStore::clear()
{
    entries_.clear();
}

const QList<HistoryEntry> &HistoryStore::entries() const
{
    return entries_;
}

std::optional<HistoryEntry> HistoryStore::entry(const QString &id) const
{
    for (const HistoryEntry &entry : entries_) {
        if (entry.id == id) {
            return entry;
        }
    }
    return std::nullopt;
}

bool HistoryStore::containsFile(const QString &path) const
{
    if (path.isEmpty()) {
        return false;
    }
    return std::any_of(entries_.cbegin(), entries_.cend(),
                       [&path](const HistoryEntry &entry) { return entry.files.contains(path); });
}

QJsonArray HistoryStore::toJson(const std::function<QString(const QString &)> &thumbnailFor) const
{
    QJsonArray array;
    for (const HistoryEntry &entry : entries_) {
        QJsonObject obj = entryToJson(entry);
        QJsonArray files;
        for (const QString &path : entry.files) {
            QJsonObject file;
            file.insert(QStringLiteral("path"), path);
            file.insert(QStringLiteral("name"), QFileInfo(path).fileName());
            file.insert(QStringLiteral("thumbnail"), thumbnailFor ? thumbnailFor(path) : QString());
            files.append(file);
        }
        obj.insert(QStringLiteral("files"), files);
        array.append(obj);
    }
    return array;
}

QList<HistoryEntry> HistoryStore::fromJson(const QJsonArray &array)
{
    QList<HistoryEntry> result;
    for (const QJsonValue &value : array) {
        if (std::optional<HistoryEntry> entry = entryFromJson(value.toObject())) {
            result.append(*entry);
        }
    }
    return result;
}

void HistoryStore::trim()
{
    while (entries_.size() > limit_) {
        entries_.removeLast();
    }
}

} // namespace glimply
