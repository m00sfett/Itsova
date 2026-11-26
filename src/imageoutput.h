#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QList>
#include <QMargins>
#include <QRect>
#include <QString>

#include <optional>

class QIODevice;

namespace glimply {

enum class ImageFormat {
    Png,
    Jpeg,
    WebP,
    Avif,
    Jxl,
    Tiff,
    Bmp,
    Qoi,
};

struct ExportOptions {
    ImageFormat format = ImageFormat::Png;
    int quality = 90;         // 1..100 for lossy formats (1..99 when lossy WebP)
    int pngCompression = -1;  // -1 = Qt default zlib level (6); 0..9 explicit zlib level
    bool lossless = false;    // WebP lossless toggle

    bool operator==(const ExportOptions &) const = default;
};

// Curated user-facing formats available at runtime (always starts with PNG).
[[nodiscard]] QList<ImageFormat> availableCuratedFormats();
[[nodiscard]] bool isImageFormatSupported(ImageFormat format);
[[nodiscard]] QByteArray qtFormatName(ImageFormat format);
[[nodiscard]] QString canonicalExtension(ImageFormat format);
[[nodiscard]] QString formatConfigKey(ImageFormat format);
[[nodiscard]] std::optional<ImageFormat> imageFormatFromKey(const QString &key);
[[nodiscard]] std::optional<ImageFormat> imageFormatFromPath(const QString &filePath);
[[nodiscard]] QString formatDisplayName(ImageFormat format);
[[nodiscard]] bool formatSupportsAlpha(ImageFormat format);
[[nodiscard]] bool formatSupportsQuality(ImageFormat format);
[[nodiscard]] bool formatSupportsLosslessToggle(ImageFormat format);
[[nodiscard]] QString replaceImageExtension(const QString &filename, ImageFormat format);

// The native pixels a logical area covers on a monitor of this scale (#101): every edge rounds to
// the nearest native pixel boundary. cropLogicalArea() cuts exactly these pixels, and the loupe
// draws the same boundary. Empty when an edge is out of range.
[[nodiscard]] QRect nativeAreaFor(const QRect &logicalArea, qreal scale);

// nativeNudge (#102) moves the edges by whole native pixels on top of the logical area.
[[nodiscard]] QImage cropLogicalArea(const QImage &nativeFrame,
                                       const QRect &logicalArea,
                                       qreal scale,
                                       QString *error = nullptr,
                                       const QMargins &nativeNudge = QMargins());

[[nodiscard]] QString sanitizeFilenameToken(const QString &token);

// Expands {date}, {time}, {area}, and {ext}, aligning the suffix to format's canonical extension.
[[nodiscard]] QString filenameFor(const QString &filenameTemplate, const QString &areaToken,
                                  const QDateTime &timestamp, ImageFormat format = ImageFormat::Png);

// Strips metadata and flattens transparent areas onto solid black for non-alpha formats.
[[nodiscard]] QImage prepareImageForExport(const QImage &image, const ExportOptions &options);
[[nodiscard]] bool encodeImageToDevice(QIODevice *device,
                                         const QImage &image,
                                         const ExportOptions &options,
                                         QString *error = nullptr);
[[nodiscard]] qint64 estimateEncodedSize(const QImage &image,
                                         const ExportOptions &options,
                                         QString *error = nullptr);
[[nodiscard]] bool writeImageToFile(const QString &targetPath,
                                      const QImage &image,
                                      const ExportOptions &options,
                                      QString *error = nullptr);

// Writes image atomically to directory/baseFilename (appending -2, -3, ... before the extension if
// the file already exists). Never overwrites existing files.
[[nodiscard]] bool writeUniqueImageFile(const QString &directory,
                                        const QString &baseFilename,
                                        const QImage &image,
                                        const ExportOptions &options = {},
                                        QString *writtenPath = nullptr,
                                        QString *error = nullptr);
[[nodiscard]] bool writeUniquePngFile(const QString &directory,
                                      const QString &baseFilename,
                                      const QImage &image,
                                      QString *writtenPath = nullptr,
                                      QString *error = nullptr);

class OutputWriter {
public:
    explicit OutputWriter(
        QString directory,
        QString filenameTemplate = QStringLiteral("{date}_{time}_{area}.png"),
        ExportOptions defaultOptions = {});

    [[nodiscard]] bool writeImage(const QImage &image,
                                  const QString &areaToken,
                                  const QDateTime &timestamp,
                                  QString *writtenPath = nullptr,
                                  QString *error = nullptr) const;
    [[nodiscard]] bool writeImage(const QImage &image,
                                  const QString &areaToken,
                                  const QDateTime &timestamp,
                                  const ExportOptions &options,
                                  QString *writtenPath = nullptr,
                                  QString *error = nullptr) const;

private:
    QString directory_;
    QString filenameTemplate_;
    ExportOptions defaultOptions_;
};

} // namespace glimply
