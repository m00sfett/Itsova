#include "imageoutput.h"

#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QPainter>
#include <QSaveFile>
#include <QTemporaryFile>

#include <fcntl.h>
#include <stdio.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <unistd.h>

namespace glimply {
namespace {

class ImageOutputStrings
{
    Q_DECLARE_TR_FUNCTIONS(ImageOutput)
};

constexpr std::array<QLatin1StringView, 10> knownImageExtensions{
    QLatin1StringView(".png"),
    QLatin1StringView(".jpeg"),
    QLatin1StringView(".jpg"),
    QLatin1StringView(".webp"),
    QLatin1StringView(".avif"),
    QLatin1StringView(".jxl"),
    QLatin1StringView(".tiff"),
    QLatin1StringView(".tif"),
    QLatin1StringView(".bmp"),
    QLatin1StringView(".qoi"),
};

int qtPngCompressionFromZlibLevel(int level)
{
    // Qt's PNG handler scales QImageWriter::compression (0..100) back down to zlib levels 0..9.
    constexpr std::array<int, 10> breakpoints{0, 16, 27, 38, 49, 60, 71, 80, 88, 100};
    return breakpoints[static_cast<std::size_t>(std::clamp(level, 0, 9))];
}

bool scaledEdge(int logicalEdge, qreal scale, int *nativeEdge)
{
    const qreal scaled = static_cast<qreal>(logicalEdge) * scale;
    if (!std::isfinite(scaled) || scaled < 0.0 ||
        scaled > static_cast<qreal>(std::numeric_limits<int>::max())) {
        return false;
    }

    *nativeEdge = static_cast<int>(std::round(scaled));
    return true;
}

QString makeFilenameImpl(const QString &filenameTemplate,
                         const QString &areaToken,
                         const QDateTime &timestamp,
                         ImageFormat format)
{
    QString filename = filenameTemplate;
    filename.replace(QStringLiteral("{date}"), timestamp.toString(QStringLiteral("yyyy-MM-dd")));
    filename.replace(QStringLiteral("{time}"), timestamp.toString(QStringLiteral("HH-mm-ss")));
    filename.replace(QStringLiteral("{area}"), sanitizeFilenameToken(areaToken));
    // Templates written before regions were renamed to areas keep working.
    filename.replace(QStringLiteral("{region}"), sanitizeFilenameToken(areaToken));
    filename.replace(QStringLiteral("{ext}"), canonicalExtension(format));

    // The template must still resolve to one flat filename, even if it contains
    // path separators or other characters that are invalid in common filesystems.
    QString safeFilename;
    safeFilename.reserve(filename.size());
    for (const QChar character : filename) {
        if (!character.isNull() && character.category() != QChar::Other_Control &&
            !QStringLiteral("/\\:*?\"<>|").contains(character)) {
            safeFilename.append(character);
        }
    }
    return replaceImageExtension(std::move(safeFilename), format);
}

bool renameNoReplaceUnsupported(int error)
{
    return error == EINVAL || error == ENOSYS || error == EOPNOTSUPP || error == ENOTSUP;
}

// Reserves finalPath exclusively, then moves temporaryPath over the reserved placeholder.
// Returns 0 on success, otherwise -1 with errno set (EEXIST when the name is taken).
int publishViaExclusiveReservation(const QByteArray &temporaryPath, const QByteArray &finalPath)
{
    const int fd = ::open(finalPath.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    ::close(fd);
    if (::rename(temporaryPath.constData(), finalPath.constData()) != 0) {
        const int renameError = errno;
        ::unlink(finalPath.constData());
        errno = renameError;
        return -1;
    }
    return 0;
}

} // namespace

QList<ImageFormat> availableCuratedFormats()
{
    QList<ImageFormat> formats;
    for (const ImageFormat format : {ImageFormat::Png, ImageFormat::Jpeg, ImageFormat::WebP,
                                     ImageFormat::Avif, ImageFormat::Jxl}) {
        if (format == ImageFormat::Png || isImageFormatSupported(format)) {
            formats.append(format);
        }
    }
    return formats;
}

bool isImageFormatSupported(ImageFormat format)
{
    const QList<QByteArray> supported = QImageWriter::supportedImageFormats();
    const QByteArray name = qtFormatName(format);
    for (const QByteArray &entry : supported) {
        if (entry.toLower() == name) {
            return true;
        }
    }
    return format == ImageFormat::Png;
}

QByteArray qtFormatName(ImageFormat format)
{
    switch (format) {
    case ImageFormat::Png:
        return QByteArrayLiteral("png");
    case ImageFormat::Jpeg:
        return QByteArrayLiteral("jpeg");
    case ImageFormat::WebP:
        return QByteArrayLiteral("webp");
    case ImageFormat::Avif:
        return QByteArrayLiteral("avif");
    case ImageFormat::Jxl:
        return QByteArrayLiteral("jxl");
    case ImageFormat::Tiff:
        return QByteArrayLiteral("tiff");
    case ImageFormat::Bmp:
        return QByteArrayLiteral("bmp");
    case ImageFormat::Qoi:
        return QByteArrayLiteral("qoi");
    }
    return QByteArrayLiteral("png");
}

QString canonicalExtension(ImageFormat format)
{
    switch (format) {
    case ImageFormat::Png:
        return QStringLiteral("png");
    case ImageFormat::Jpeg:
        return QStringLiteral("jpg");
    case ImageFormat::WebP:
        return QStringLiteral("webp");
    case ImageFormat::Avif:
        return QStringLiteral("avif");
    case ImageFormat::Jxl:
        return QStringLiteral("jxl");
    case ImageFormat::Tiff:
        return QStringLiteral("tiff");
    case ImageFormat::Bmp:
        return QStringLiteral("bmp");
    case ImageFormat::Qoi:
        return QStringLiteral("qoi");
    }
    return QStringLiteral("png");
}

QString formatConfigKey(ImageFormat format)
{
    return QString::fromLatin1(qtFormatName(format));
}

std::optional<ImageFormat> imageFormatFromKey(const QString &key)
{
    const QString norm = key.trimmed().toLower();
    if (norm == QLatin1String("png")) {
        return ImageFormat::Png;
    }
    if (norm == QLatin1String("jpg") || norm == QLatin1String("jpeg")) {
        return ImageFormat::Jpeg;
    }
    if (norm == QLatin1String("webp")) {
        return ImageFormat::WebP;
    }
    if (norm == QLatin1String("avif")) {
        return ImageFormat::Avif;
    }
    if (norm == QLatin1String("jxl")) {
        return ImageFormat::Jxl;
    }
    if (norm == QLatin1String("tif") || norm == QLatin1String("tiff")) {
        return ImageFormat::Tiff;
    }
    if (norm == QLatin1String("bmp")) {
        return ImageFormat::Bmp;
    }
    if (norm == QLatin1String("qoi")) {
        return ImageFormat::Qoi;
    }
    return std::nullopt;
}

std::optional<ImageFormat> imageFormatFromPath(const QString &filePath)
{
    const QString suffix = QFileInfo(filePath).suffix();
    return suffix.isEmpty() ? std::nullopt : imageFormatFromKey(suffix);
}

QString formatDisplayName(ImageFormat format)
{
    switch (format) {
    case ImageFormat::Png:
        return ImageOutputStrings::tr("PNG (lossless)");
    case ImageFormat::Jpeg:
        return ImageOutputStrings::tr("JPEG");
    case ImageFormat::WebP:
        return ImageOutputStrings::tr("WebP");
    case ImageFormat::Avif:
        return ImageOutputStrings::tr("AVIF");
    case ImageFormat::Jxl:
        return ImageOutputStrings::tr("JPEG XL");
    case ImageFormat::Tiff:
        return ImageOutputStrings::tr("TIFF");
    case ImageFormat::Bmp:
        return ImageOutputStrings::tr("BMP");
    case ImageFormat::Qoi:
        return ImageOutputStrings::tr("QOI");
    }
    return ImageOutputStrings::tr("PNG (lossless)");
}

bool formatSupportsAlpha(ImageFormat format)
{
    return format != ImageFormat::Jpeg && format != ImageFormat::Bmp;
}

bool formatSupportsQuality(ImageFormat format)
{
    return format == ImageFormat::Jpeg || format == ImageFormat::WebP
        || format == ImageFormat::Avif || format == ImageFormat::Jxl;
}

bool formatSupportsLosslessToggle(ImageFormat format)
{
    return format == ImageFormat::WebP;
}

QString replaceImageExtension(const QString &filename, ImageFormat format)
{
    QString out = filename;
    for (const QLatin1StringView ext : knownImageExtensions) {
        if (out.endsWith(ext, Qt::CaseInsensitive)) {
            out.chop(ext.size());
            break;
        }
    }
    const QString suffix = QLatin1Char('.') + canonicalExtension(format);
    if (!out.endsWith(suffix, Qt::CaseInsensitive)) {
        out.append(suffix);
    }
    return out;
}

QString filenameFor(const QString &filenameTemplate, const QString &areaToken, const QDateTime &timestamp,
                    ImageFormat format)
{
    return makeFilenameImpl(filenameTemplate, areaToken, timestamp, format);
}

QImage prepareImageForExport(const QImage &image, const ExportOptions &options)
{
    if (image.isNull()) {
        return {};
    }
    if (!formatSupportsAlpha(options.format)) {
        QImage flattened(image.size(), QImage::Format_RGB32);
        flattened.setDevicePixelRatio(1.0);
        flattened.fill(QColor(0, 0, 0));
        QPainter painter(&flattened);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.drawImage(0, 0, image);
        return flattened;
    }
    const QImage::Format targetFmt = image.hasAlphaChannel()
                                         ? QImage::Format_ARGB32_Premultiplied
                                         : QImage::Format_RGB32;
    QImage clean(image.size(), targetFmt);
    clean.setDevicePixelRatio(1.0);
    QPainter painter(&clean);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(0, 0, image);
    return clean;
}

bool encodeImageToDevice(QIODevice *device, const QImage &image, const ExportOptions &options, QString *error)
{
    if (!device) {
        if (error) {
            *error = ImageOutputStrings::tr("No output device was provided.");
        }
        return false;
    }
    if (image.isNull()) {
        if (error) {
            *error = ImageOutputStrings::tr("Cannot save an empty image.");
        }
        return false;
    }

    const QImage prepared = prepareImageForExport(image, options);
    QImageWriter writer(device, qtFormatName(options.format));
    if (options.format == ImageFormat::Png) {
        if (options.pngCompression >= 0) {
            writer.setCompression(qtPngCompressionFromZlibLevel(options.pngCompression));
        }
    } else if (options.format == ImageFormat::WebP) {
        writer.setQuality(options.lossless ? 100 : std::clamp(options.quality, 1, 99));
    } else if (formatSupportsQuality(options.format)) {
        writer.setQuality(std::clamp(options.quality, 1, 100));
    }

    if (!writer.write(prepared)) {
        if (error) {
            *error = writer.errorString().isEmpty()
                         ? ImageOutputStrings::tr("Could not encode image.")
                         : writer.errorString();
        }
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

qint64 estimateEncodedSize(const QImage &image, const ExportOptions &options, QString *error)
{
    if (image.isNull()) {
        if (error) {
            *error = ImageOutputStrings::tr("Cannot estimate size of an empty image.");
        }
        return -1;
    }
    QBuffer buffer;
    if (!buffer.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = buffer.errorString();
        }
        return -1;
    }
    if (!encodeImageToDevice(&buffer, image, options, error)) {
        return -1;
    }
    return buffer.size();
}

bool writeImageToFile(const QString &targetPath, const QImage &image, const ExportOptions &options, QString *error)
{
    if (image.isNull()) {
        if (error) {
            *error = ImageOutputStrings::tr("Cannot save an empty image.");
        }
        return false;
    }
    const QFileInfo info(targetPath);
    if (!info.dir().exists() && !QDir().mkpath(info.absolutePath())) {
        if (error) {
            *error = ImageOutputStrings::tr("Cannot create capture directory: %1").arg(info.absolutePath());
        }
        return false;
    }
    const bool existed = QFile::exists(targetPath);
    QSaveFile file(targetPath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    if (!existed) {
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup
                            | QFileDevice::ReadOther);
    }
    QString encodeErr;
    if (!encodeImageToDevice(&file, image, options, &encodeErr) || !file.commit()) {
        if (error) {
            *error = !encodeErr.isEmpty() ? encodeErr : file.errorString();
        }
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

QImage cropLogicalArea(const QImage &nativeFrame,
                         const QRect &logicalArea,
                         qreal scale,
                         QString *error,
                         const QMargins &nativeNudge)
{
    if (nativeFrame.isNull()) {
        if (error) {
            *error = ImageOutputStrings::tr("The monitor capture is empty.");
        }
        return {};
    }

    if (logicalArea.x() < 0 || logicalArea.y() < 0 || logicalArea.width() <= 0 ||
        logicalArea.height() <= 0 || !std::isfinite(scale) || scale <= 0.0) {
        if (error) {
            *error = ImageOutputStrings::tr("The capture area or monitor scale is invalid.");
        }
        return {};
    }

    const qint64 logicalRight = static_cast<qint64>(logicalArea.x()) + logicalArea.width();
    const qint64 logicalBottom = static_cast<qint64>(logicalArea.y()) + logicalArea.height();
    if (logicalRight > std::numeric_limits<int>::max() ||
        logicalBottom > std::numeric_limits<int>::max()) {
        if (error) {
            *error = ImageOutputStrings::tr("The capture area is outside the monitor image.");
        }
        return {};
    }

    // One source of truth for the native edges, shared with the loupe (#101).
    QRect native = nativeAreaFor(logicalArea, scale);
    if (!native.isEmpty() && !nativeNudge.isNull()) {
        native = native.adjusted(nativeNudge.left(), nativeNudge.top(), nativeNudge.right(), nativeNudge.bottom());
    }
    if (native.isEmpty() || native.left() < 0 || native.top() < 0 || native.right() >= nativeFrame.width()
        || native.bottom() >= nativeFrame.height()) {
        if (error) {
            *error = ImageOutputStrings::tr("The capture area is outside the monitor image.");
        }
        return {};
    }

    QImage cropped = nativeFrame.copy(native);
    if (cropped.isNull()) {
        if (error) {
            *error = ImageOutputStrings::tr("Could not allocate the cropped image.");
        }
        return {};
    }
    if (error) {
        error->clear();
    }
    return cropped;
}

QRect nativeAreaFor(const QRect &logicalArea, qreal scale)
{
    if (!std::isfinite(scale) || scale <= 0.0 || logicalArea.width() <= 0 || logicalArea.height() <= 0) {
        return {};
    }
    const qint64 right = static_cast<qint64>(logicalArea.x()) + logicalArea.width();
    const qint64 bottom = static_cast<qint64>(logicalArea.y()) + logicalArea.height();
    if (right > std::numeric_limits<int>::max() || bottom > std::numeric_limits<int>::max()) {
        return {};
    }
    int left = 0;
    int top = 0;
    int nativeRight = 0;
    int nativeBottom = 0;
    if (!scaledEdge(logicalArea.x(), scale, &left) || !scaledEdge(logicalArea.y(), scale, &top)
        || !scaledEdge(static_cast<int>(right), scale, &nativeRight)
        || !scaledEdge(static_cast<int>(bottom), scale, &nativeBottom) || nativeRight <= left || nativeBottom <= top) {
        return {};
    }
    return QRect(left, top, nativeRight - left, nativeBottom - top);
}

QString sanitizeFilenameToken(const QString &token)
{
    QString sanitized;
    sanitized.reserve(token.size());
    for (const QChar character : token) {
        if (character.isSpace()) {
            sanitized.append(QLatin1Char('-'));
        } else if (!character.isNull() && character.category() != QChar::Other_Control &&
                   !QStringLiteral("/\\:*?\"<>|").contains(character)) {
            sanitized.append(character);
        }
    }
    return sanitized;
}

bool writeUniqueImageFile(const QString &directory,
                          const QString &baseFilename,
                          const QImage &image,
                          const ExportOptions &options,
                          QString *writtenPath,
                          QString *error)
{
    if (writtenPath) {
        writtenPath->clear();
    }
    if (image.isNull()) {
        if (error) {
            *error = ImageOutputStrings::tr("Cannot save an empty image.");
        }
        return false;
    }
    if (directory.isEmpty()) {
        if (error) {
            *error = ImageOutputStrings::tr("The capture directory is empty.");
        }
        return false;
    }
    if (!QDir(directory).exists() && !QDir().mkpath(directory)) {
        if (error) {
            *error = ImageOutputStrings::tr("Cannot create capture directory: %1").arg(directory);
        }
        return false;
    }

    const QString filename = replaceImageExtension(baseFilename, options.format);
    const qsizetype extensionPosition = filename.lastIndexOf(QLatin1Char('.'));
    const QString stem = filename.left(extensionPosition);
    const QString extension = filename.mid(extensionPosition);

    const QDir outputDirectory(directory);
    QTemporaryFile temporary(outputDirectory.filePath(QStringLiteral(".glimply-XXXXXX")));
    if (!temporary.open()) {
        if (error) {
            *error = ImageOutputStrings::tr("Cannot create a temporary capture file: %1")
                         .arg(temporary.errorString());
        }
        return false;
    }

    QString encodeError;
    if (!encodeImageToDevice(&temporary, image, options, &encodeError) || !temporary.flush()) {
        const QString writeError = !encodeError.isEmpty() ? encodeError : temporary.errorString();
        if (error) {
            *error = ImageOutputStrings::tr("Cannot write temporary capture file: %1")
                         .arg(writeError);
        }
        return false;
    }

    if (::fsync(temporary.handle()) != 0) {
        if (error) {
            *error = ImageOutputStrings::tr("Cannot finish temporary capture file: %1")
                         .arg(QString::fromLocal8Bit(std::strerror(errno)));
        }
        return false;
    }
    // QTemporaryFile creates 0600; captures are ordinary user files.
    if (!temporary.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup
                                  | QFileDevice::ReadOther)) {
        if (error) {
            *error = ImageOutputStrings::tr("Cannot set permissions of temporary capture file: %1")
                         .arg(temporary.errorString());
        }
        return false;
    }
    const QByteArray temporaryPath = QFile::encodeName(temporary.fileName());
    temporary.close();

    // Some filesystems (FUSE mounts, older NFS/CIFS) reject RENAME_NOREPLACE. They fall back to
    // reserving the name with O_EXCL and then replacing only that reserved placeholder.
    bool useExclusiveReservation = false;
    for (quint64 suffix = 1; suffix != 0; ++suffix) {
        const QString candidateName = suffix == 1
                                          ? filename
                                          : stem + QLatin1Char('-') + QString::number(suffix) + extension;
        const QString candidatePath = outputDirectory.absoluteFilePath(candidateName);
        const QByteArray finalPath = QFile::encodeName(candidatePath);
        int published = -1;
        if (!useExclusiveReservation) {
            published = ::renameat2(AT_FDCWD, temporaryPath.constData(), AT_FDCWD, finalPath.constData(),
                                    RENAME_NOREPLACE);
            if (published != 0 && renameNoReplaceUnsupported(errno)) {
                useExclusiveReservation = true;
            }
        }
        if (useExclusiveReservation) {
            published = publishViaExclusiveReservation(temporaryPath, finalPath);
        }
        if (published != 0) {
            if (errno == EEXIST) {
                continue;
            }
            if (error) {
                *error = ImageOutputStrings::tr("Cannot publish capture file %1: %2")
                             .arg(candidatePath, QString::fromLocal8Bit(std::strerror(errno)));
            }
            return false;
        }

        temporary.setAutoRemove(false);
        if (writtenPath) {
            *writtenPath = candidatePath;
        }
        if (error) {
            error->clear();
        }
        return true;
    }

    if (error) {
        *error = ImageOutputStrings::tr("No available filename remains for this capture.");
    }
    return false;
}

bool writeUniquePngFile(const QString &directory,
                        const QString &baseFilename,
                        const QImage &image,
                        QString *writtenPath,
                        QString *error)
{
    ExportOptions png;
    png.format = ImageFormat::Png;
    return writeUniqueImageFile(directory, baseFilename, image, png, writtenPath, error);
}

OutputWriter::OutputWriter(QString directory, QString filenameTemplate, ExportOptions defaultOptions)
    : directory_(std::move(directory)),
      filenameTemplate_(filenameTemplate.isEmpty()
                            ? QStringLiteral("{date}_{time}_{area}.png")
                            : std::move(filenameTemplate)),
      defaultOptions_(defaultOptions)
{
}

bool OutputWriter::writeImage(const QImage &image,
                              const QString &areaToken,
                              const QDateTime &timestamp,
                              QString *writtenPath,
                              QString *error) const
{
    return writeImage(image, areaToken, timestamp, defaultOptions_, writtenPath, error);
}

bool OutputWriter::writeImage(const QImage &image,
                              const QString &areaToken,
                              const QDateTime &timestamp,
                              const ExportOptions &options,
                              QString *writtenPath,
                              QString *error) const
{
    if (writtenPath) {
        writtenPath->clear();
    }
    if (!timestamp.isValid()) {
        if (error) {
            *error = ImageOutputStrings::tr("The capture timestamp is invalid.");
        }
        return false;
    }
    return writeUniqueImageFile(directory_, filenameFor(filenameTemplate_, areaToken, timestamp, options.format),
                                image, options, writtenPath, error);
}

} // namespace glimply
