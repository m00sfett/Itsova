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
