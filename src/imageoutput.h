#pragma once

#include <QDateTime>
#include <QImage>
#include <QRect>
#include <QString>

namespace glimply {

[[nodiscard]] QImage cropLogicalRegion(const QImage &nativeFrame,
                                       const QRect &logicalRegion,
                                       qreal scale,
                                       QString *error = nullptr);

[[nodiscard]] QString sanitizeFilenameToken(const QString &token);

// Expands {date}, {time} and {region} and returns one flat, safe PNG file name.
[[nodiscard]] QString filenameFor(const QString &filenameTemplate, const QString &regionToken,
                                  const QDateTime &timestamp);

class OutputWriter {
public:
    explicit OutputWriter(
        QString directory,
        QString filenameTemplate = QStringLiteral("{date}_{time}_{region}.png"));

    [[nodiscard]] bool writeImage(const QImage &image,
                                  const QString &regionToken,
                                  const QDateTime &timestamp,
                                  QString *writtenPath = nullptr,
                                  QString *error = nullptr) const;

private:
    QString directory_;
    QString filenameTemplate_;
};

} // namespace glimply
