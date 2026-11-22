#pragma once

#include <QImage>
#include <QString>

namespace glimply
{

struct CaptureResult
{
    QImage image;
    qreal scale = 0;
    QString error;
};

class CaptureService
{
public:
    // Blocks until KWin has supplied one native frame or an error. Call from a worker thread.
    [[nodiscard]] static CaptureResult captureScreen(const QString &connector, bool includeCursor = false);
    // A single window by its KWin handle (UUID), also when it is covered or minimized.
    [[nodiscard]] static CaptureResult captureWindow(const QString &handle, bool includeCursor = false);
};

} // namespace glimply
