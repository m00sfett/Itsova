#pragma once

#include <QDateTime>
#include <QImage>
#include <QRect>
#include <QString>

namespace glimply {

[[nodiscard]] QImage cropLogicalArea(const QImage &nativeFrame,
                                       const QRect &logicalArea,
                                       qreal scale,
                                       QString *error = nullptr);

[[nodiscard]] QString sanitizeFilenameToken(const QString &token);

// Expands {date}, {time} and {area} and returns one flat, safe PNG file name.
[[nodiscard]] QString filenameFor(const QString &filenameTemplate, const QString &areaToken,
                                  const QDateTime &timestamp);

// Writes image atomically to directory/baseFilename (appending -2, -3, ... before the extension if
// the file already exists). Never overwrites existing files.
[[nodiscard]] bool writeUniquePngFile(const QString &directory,
                                      const QString &baseFilename,
                                      const QImage &image,
                                      QString *writtenPath = nullptr,
                                      QString *error = nullptr);

class OutputWriter {
public:
    explicit OutputWriter(
        QString directory,
        QString filenameTemplate = QStringLiteral("{date}_{time}_{area}.png"));

    [[nodiscard]] bool writeImage(const QImage &image,
                                  const QString &areaToken,
                                  const QDateTime &timestamp,
                                  QString *writtenPath = nullptr,
                                  QString *error = nullptr) const;

private:
    QString directory_;
    QString filenameTemplate_;
};

} // namespace glimply
