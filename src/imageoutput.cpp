#include "imageoutput.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImageWriter>
#include <QTemporaryFile>

#include <fcntl.h>
#include <stdio.h>

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
                     const QString &regionToken,
                     const QDateTime &timestamp)
{
    QString filename = filenameTemplate;
    filename.replace(QStringLiteral("{date}"), timestamp.toString(QStringLiteral("yyyy-MM-dd")));
    filename.replace(QStringLiteral("{time}"), timestamp.toString(QStringLiteral("HH-mm-ss")));
    filename.replace(QStringLiteral("{region}"), sanitizeFilenameToken(regionToken));

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
    filename = std::move(safeFilename);

    if (!filename.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
        filename.append(QStringLiteral(".png"));
    }
    return filename;
}

} // namespace

QString filenameFor(const QString &filenameTemplate, const QString &regionToken, const QDateTime &timestamp)
{
    return makeFilenameImpl(filenameTemplate, regionToken, timestamp);
}

QImage cropLogicalRegion(const QImage &nativeFrame,
                         const QRect &logicalRegion,
                         qreal scale,
                         QString *error)
{
    if (nativeFrame.isNull()) {
        if (error) {
            *error = ImageOutputStrings::tr("The monitor capture is empty.");
        }
        return {};
    }

    if (logicalRegion.x() < 0 || logicalRegion.y() < 0 || logicalRegion.width() <= 0 ||
        logicalRegion.height() <= 0 || !std::isfinite(scale) || scale <= 0.0) {
        if (error) {
            *error = ImageOutputStrings::tr("The capture region or monitor scale is invalid.");
        }
        return {};
    }

    const qint64 logicalRight = static_cast<qint64>(logicalRegion.x()) + logicalRegion.width();
    const qint64 logicalBottom = static_cast<qint64>(logicalRegion.y()) + logicalRegion.height();
    if (logicalRight > std::numeric_limits<int>::max() ||
        logicalBottom > std::numeric_limits<int>::max()) {
        if (error) {
            *error = ImageOutputStrings::tr("The capture region is outside the monitor image.");
        }
        return {};
    }

    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    if (!scaledEdge(logicalRegion.x(), scale, &left) ||
        !scaledEdge(logicalRegion.y(), scale, &top) ||
        !scaledEdge(static_cast<int>(logicalRight), scale, &right) ||
        !scaledEdge(static_cast<int>(logicalBottom), scale, &bottom) || right <= left ||
        bottom <= top || right > nativeFrame.width() || bottom > nativeFrame.height()) {
        if (error) {
            *error = ImageOutputStrings::tr("The capture region is outside the monitor image.");
        }
        return {};
    }

    QImage cropped = nativeFrame.copy(left, top, right - left, bottom - top);
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

OutputWriter::OutputWriter(QString directory, QString filenameTemplate)
    : directory_(std::move(directory)),
      filenameTemplate_(filenameTemplate.isEmpty()
                            ? QStringLiteral("{date}_{time}_{region}.png")
                            : std::move(filenameTemplate))
{
}

bool OutputWriter::writeImage(const QImage &image,
                              const QString &regionToken,
                              const QDateTime &timestamp,
                              QString *writtenPath,
                              QString *error) const
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
    if (!timestamp.isValid()) {
        if (error) {
            *error = ImageOutputStrings::tr("The capture timestamp is invalid.");
        }
        return false;
    }

    if (directory_.isEmpty()) {
        if (error) {
            *error = ImageOutputStrings::tr("The capture directory is empty.");
        }
        return false;
    }

    if (!QDir(directory_).exists() && !QDir().mkpath(directory_)) {
        if (error) {
            *error = ImageOutputStrings::tr("Cannot create capture directory: %1").arg(directory_);
        }
        return false;
    }

    const QDir outputDirectory(directory_);
    const QString filename = filenameFor(filenameTemplate_, regionToken, timestamp);
    const qsizetype extensionPosition = filename.lastIndexOf(QLatin1Char('.'));
    const QString stem = filename.left(extensionPosition);
    const QString extension = filename.mid(extensionPosition);

    QTemporaryFile temporary(outputDirectory.filePath(QStringLiteral(".glimply-XXXXXX")));
    if (!temporary.open()) {
        if (error) {
            *error = ImageOutputStrings::tr("Cannot create a temporary capture file: %1")
                         .arg(temporary.errorString());
        }
        return false;
    }

    QImageWriter writer(&temporary, "png");
    if (!writer.write(image) || !temporary.flush()) {
        QString writeError = writer.errorString();
        if (writeError.isEmpty()) {
            writeError = temporary.errorString();
        }
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

    for (quint64 suffix = 1; suffix != 0; ++suffix) {
        const QString candidateName = suffix == 1
                                          ? filename
                                          : stem + QLatin1Char('-') + QString::number(suffix) + extension;
        const QString candidatePath = outputDirectory.absoluteFilePath(candidateName);
        const QByteArray finalPath = QFile::encodeName(candidatePath);
        if (::renameat2(AT_FDCWD, temporaryPath.constData(), AT_FDCWD, finalPath.constData(), RENAME_NOREPLACE) != 0) {
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

} // namespace glimply
