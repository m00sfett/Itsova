#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace glimply
{

// One capture event: a single trigger, possibly with several files (groups, multiple rectangles).
struct HistoryEntry
{
    QString id;
    QDateTime timestamp;
    QString targetId;
    QString targetName;
    QString triggerKind; // "manual", "cli", "mcp", "editor", ...
    QStringList files;
};

// Rolling list of recent captures, newest first, persisted as JSON. Interval captures are never
// recorded. Removing entries never touches the image files themselves.
class HistoryStore
{
public:
    static constexpr int DefaultLimit = 20;
    static constexpr int MinLimit = 5;
    static constexpr int MaxLimit = 100;

    using ExistsFunction = std::function<bool(const QString &)>;

    explicit HistoryStore(QString filePath = defaultFilePath(), int limit = DefaultLimit);

    // ~/.local/state/glimply/history.json
    [[nodiscard]] static QString defaultFilePath();
    [[nodiscard]] QString filePath() const;

    // Reads the file; a missing file is an empty history, a corrupt one is reported and ignored.
    bool load(QString *error = nullptr);
    // Writes atomically with mode 0600.
    bool save(QString *error = nullptr) const;

    [[nodiscard]] int limit() const;
    // Clamps to MinLimit..MaxLimit and drops the oldest entries beyond it. Returns true on change.
    bool setLimit(int limit);

    // Prepends an entry (a fresh id and the current time are filled in when empty). Returns false
    // and records nothing for interval captures or an entry without files.
    bool record(HistoryEntry entry);
    // Drops files that no longer exist, and entries left without files. Returns true on change.
    bool prune(const ExistsFunction &exists = {});
    // Removes one file from every entry (and entries left empty). Returns true on change.
    bool removeFile(const QString &path);
    bool removeEntry(const QString &id);
    void clear();

    [[nodiscard]] const QList<HistoryEntry> &entries() const;
    [[nodiscard]] std::optional<HistoryEntry> entry(const QString &id) const;
    [[nodiscard]] bool containsFile(const QString &path) const;

    // JSON for D-Bus and the CLI. thumbnailFor returns the cached thumbnail path or an empty string.
    [[nodiscard]] QJsonArray toJson(const std::function<QString(const QString &)> &thumbnailFor = {}) const;
    // Inverse of toJson (thumbnails are ignored); used by the CLI to read the D-Bus reply.
    [[nodiscard]] static QList<HistoryEntry> fromJson(const QJsonArray &array);

private:
    void trim();

    QString filePath_;
    int limit_;
    QList<HistoryEntry> entries_;
};

} // namespace glimply
