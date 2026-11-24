#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

namespace glimply
{

// Small PNG thumbnails of history files in ~/.cache/glimply/thumbs/. The name depends on path,
// size and modification time, so an overwritten file gets a fresh thumbnail.
class ThumbnailCache
{
public:
    static constexpr int MaxEdge = 256;

    explicit ThumbnailCache(QString directory = defaultDirectory());

    [[nodiscard]] static QString defaultDirectory();
    [[nodiscard]] QString directory() const;

    // Expected thumbnail path for a source file, or empty when the source does not exist.
    [[nodiscard]] QString thumbnailPath(const QString &sourcePath) const;
    // The thumbnail path if it already exists, otherwise empty.
    [[nodiscard]] QString existingThumbnail(const QString &sourcePath) const;
    // Decodes, scales and writes the thumbnail (safe to call from a worker thread). Returns the
    // thumbnail path, or empty on failure.
    [[nodiscard]] QString generate(const QString &sourcePath) const;
    // Deletes every thumbnail that is not expected for one of the given sources.
    void sweep(const QStringList &keepSources) const;

private:
    QString directory_;
};

} // namespace glimply
