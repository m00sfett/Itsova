#include "thumbnailcache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QSaveFile>
#include <QStandardPaths>

namespace glimply
{

ThumbnailCache::ThumbnailCache(QString directory)
    : directory_(std::move(directory))
{
}

QString ThumbnailCache::defaultDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/glimply/thumbs");
}

QString ThumbnailCache::directory() const
{
    return directory_;
}

QString ThumbnailCache::thumbnailPath(const QString &sourcePath) const
{
    const QFileInfo info(sourcePath);
    if (sourcePath.isEmpty() || !info.exists()) {
        return {};
    }
    const QByteArray key = info.absoluteFilePath().toUtf8() + '\n' + QByteArray::number(info.size()) + '\n'
                           + QByteArray::number(info.lastModified().toMSecsSinceEpoch());
    const QString name = QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex());
    return directory_ + QLatin1Char('/') + name + QStringLiteral(".png");
}

QString ThumbnailCache::existingThumbnail(const QString &sourcePath) const
{
    const QString path = thumbnailPath(sourcePath);
    return !path.isEmpty() && QFileInfo::exists(path) ? path : QString();
}

QString ThumbnailCache::generate(const QString &sourcePath) const
{
    const QString target = thumbnailPath(sourcePath);
    if (target.isEmpty()) {
        return {};
    }
    if (QFileInfo::exists(target)) {
        return target;
    }
    QImageReader reader(sourcePath);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > MaxEdge || size.height() > MaxEdge)) {
        // Let the decoder scale where it can; this keeps large PNGs cheap.
        reader.setScaledSize(size.scaled(MaxEdge, MaxEdge, Qt::KeepAspectRatio));
    }
    QImage image = reader.read();
    if (image.isNull()) {
        return {};
    }
    if (image.width() > MaxEdge || image.height() > MaxEdge) {
        image = image.scaled(MaxEdge, MaxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (!QDir().mkpath(directory_)) {
        return {};
    }
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    if (!image.save(&file, "PNG") || !file.commit()) {
        return {};
    }
    QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return target;
}

void ThumbnailCache::sweep(const QStringList &keepSources) const
{
    QSet<QString> keep;
    for (const QString &source : keepSources) {
        const QString path = thumbnailPath(source);
        if (!path.isEmpty()) {
            keep.insert(QFileInfo(path).fileName());
        }
    }
    QDir dir(directory_);
    if (!dir.exists()) {
        return;
    }
    const QStringList names = dir.entryList({QStringLiteral("*.png")}, QDir::Files);
    for (const QString &name : names) {
        if (!keep.contains(name)) {
            dir.remove(name);
        }
    }
}

} // namespace glimply
