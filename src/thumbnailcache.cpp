#include "thumbnailcache.h"

#include "recordingplan.h"

#include <QCryptographicHash>

#include <algorithm>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
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

namespace
{

// The first frame of a recording (#30), decoded by ffmpeg; empty without ffmpeg.
QImage firstVideoFrame(const QString &sourcePath, int maxEdge)
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        return {};
    }
    QProcess process;
    process.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), sourcePath,
                           QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-vf"),
                           QStringLiteral("scale=%1:%1:force_original_aspect_ratio=decrease").arg(maxEdge),
                           QStringLiteral("-f"), QStringLiteral("image2pipe"), QStringLiteral("-c:v"),
                           QStringLiteral("png"), QStringLiteral("-")});
    if (!process.waitForFinished(15000) || process.exitCode() != 0) {
        process.kill();
        return {};
    }
    return QImage::fromData(process.readAllStandardOutput(), "PNG");
}

// A small play mark in the corner tells a recording from a screenshot.
void markAsVideo(QImage &image)
{
    if (image.isNull()) {
        return;
    }
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal d = std::max(14.0, std::min(image.width(), image.height()) * 0.22);
    const QRectF disc(image.width() - d - d * 0.25, image.height() - d - d * 0.25, d, d);
    painter.setPen(QPen(QColor(255, 255, 255, 220), std::max(1.0, d / 14)));
    painter.setBrush(QColor(0, 0, 0, 150));
    painter.drawEllipse(disc);
    QPainterPath triangle;
    const QPointF c = disc.center();
    triangle.moveTo(c.x() - d * 0.14, c.y() - d * 0.22);
    triangle.lineTo(c.x() + d * 0.24, c.y());
    triangle.lineTo(c.x() - d * 0.14, c.y() + d * 0.22);
    triangle.closeSubpath();
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.drawPath(triangle);
}

} // namespace

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
    const bool video = isRecordingFile(sourcePath);
    if (image.isNull() && video) {
        image = firstVideoFrame(sourcePath, MaxEdge);
    }
    if (image.isNull()) {
        return {};
    }
    if (image.width() > MaxEdge || image.height() > MaxEdge) {
        image = image.scaled(MaxEdge, MaxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (video) {
        markAsVideo(image);
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
