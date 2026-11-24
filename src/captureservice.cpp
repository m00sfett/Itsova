#include "captureservice.h"

#include "latencytrace.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDeadlineTimer>
#include <QVariantMap>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <poll.h>
#include <unistd.h>
#include <utility>

namespace glimply
{
namespace
{

constexpr int captureTimeoutMs = 5'000;
constexpr int replyCheckIntervalMs = 20;
constexpr qsizetype maxFrameBytes = 256 * 1024 * 1024;

class CaptureText
{
    Q_DECLARE_TR_FUNCTIONS(CaptureService)
};

CaptureResult failure(QString error)
{
    return {QImage(), 0, std::move(error)};
}

// subject names what was captured, e.g. "monitor DP-1" or "the window".
QString describeError(const QDBusError &error, const QString &subject)
{
    if (error.name() == QStringLiteral("org.kde.KWin.ScreenShot2.Error.NoAuthorized")
        || error.type() == QDBusError::AccessDenied) {
        return CaptureText::tr("KWin denied permission to capture the screen. Install glimply with its desktop entry "
                               "to grant access to the KDE screenshot service.");
    }
    if (error.type() == QDBusError::NoReply || error.type() == QDBusError::Timeout
        || error.type() == QDBusError::TimedOut) {
        return CaptureText::tr("KWin timed out while capturing %1.").arg(subject);
    }
    if (error.name() == QStringLiteral("org.kde.KWin.ScreenShot2.Error.InvalidWindow")) {
        return CaptureText::tr("The window is no longer open.");
    }
    return CaptureText::tr("KWin could not capture %1 (%2).").arg(subject, error.name());
}

struct RawFrame
{
    int width;
    int height;
    qsizetype stride;
    qsizetype bytes;
    QImage::Format format;
    qreal scale;
};

// connector empty: the frame is not tied to a monitor (window captures).
std::optional<RawFrame> parseMetadata(const QVariantMap &data, const QString &connector)
{
    const QVariant widthValue = data.value(QStringLiteral("width"));
    const QVariant heightValue = data.value(QStringLiteral("height"));
    const QVariant strideValue = data.value(QStringLiteral("stride"));
    const QVariant formatValue = data.value(QStringLiteral("format"));
    const QVariant scaleValue = data.value(QStringLiteral("scale"));
    const auto screen = data.constFind(QStringLiteral("screen"));
    if (data.value(QStringLiteral("type")).toString() != QStringLiteral("raw")
        || (!connector.isEmpty() && screen != data.cend() && (screen->typeId() != QMetaType::QString || screen->toString() != connector))
        || widthValue.typeId() != QMetaType::UInt || heightValue.typeId() != QMetaType::UInt
        || strideValue.typeId() != QMetaType::UInt || formatValue.typeId() != QMetaType::UInt
        || scaleValue.typeId() != QMetaType::Double) {
        return std::nullopt;
    }

    const uint width = widthValue.toUInt();
    const uint height = heightValue.toUInt();
    const uint stride = strideValue.toUInt();
    const uint formatNumber = formatValue.toUInt();
    const qreal scale = scaleValue.toDouble();
    if (width == 0 || width > std::numeric_limits<int>::max()
        || height == 0 || height > std::numeric_limits<int>::max()
        || formatNumber <= QImage::Format_Invalid || formatNumber >= QImage::NImageFormats
        || !std::isfinite(scale) || scale <= 0) {
        return std::nullopt;
    }

    const auto format = static_cast<QImage::Format>(formatNumber);
    // Indexed formats need a palette, which the screenshot protocol does not transmit.
    if (format == QImage::Format_Mono || format == QImage::Format_MonoLSB || format == QImage::Format_Indexed8) {
        return std::nullopt;
    }

    const int depth = QImage(1, 1, format).depth();
    const quint64 rowBytes = (quint64(width) * depth + 7) / 8;
    const quint64 byteCount = quint64(stride) * height;
    const auto alignment = depth >= 64 ? alignof(quint64) : alignof(quint32);
    if (depth == 0 || stride < rowBytes || stride % alignment != 0 || byteCount > maxFrameBytes) {
        return std::nullopt;
    }

    return RawFrame{static_cast<int>(width), static_cast<int>(height), static_cast<qsizetype>(stride),
                    static_cast<qsizetype>(byteCount), format, scale};
}

CaptureResult receiveFrame(QDBusPendingReply<QVariantMap> &reply, int readFd, const QDeadlineTimer &deadline,
                           const QString &connector, const QString &subject)
{
    QByteArray pixels;
    std::optional<RawFrame> frame;
    bool atEnd = false;

    // Drain the pipe while the D-Bus reply is pending. A frame can exceed the pipe capacity.
    while (!deadline.hasExpired()) {
        if (!frame && reply.isFinished()) {
            if (reply.isError()) {
                return failure(describeError(reply.error(), subject));
            }
            frame = parseMetadata(reply.value(), connector);
            // Qt keeps the outgoing D-Bus message (and its write descriptor) in the pending call.
            // Release it before waiting for pipe EOF.
            reply = QDBusPendingReply<QVariantMap>();
            if (!frame || pixels.size() > frame->bytes) {
                return failure(CaptureText::tr("KWin returned invalid image data for %1.").arg(subject));
            }
            pixels.reserve(frame->bytes);
        }
        if (frame && atEnd) {
            break;
        }

        const int remainingMs = static_cast<int>(deadline.remainingTime());
        const int pollTimeout = frame ? remainingMs : std::min(remainingMs, replyCheckIntervalMs);
        pollfd readEvent{atEnd ? -1 : readFd, POLLIN, 0};
        const int ready = poll(&readEvent, 1, pollTimeout);
        if (ready == 0 || (ready < 0 && errno == EINTR)) {
            continue;
        }
        if (ready < 0 || (readEvent.revents & (POLLERR | POLLNVAL))) {
            return failure(CaptureText::tr("Could not read the screenshot from KWin."));
        }

        const qsizetype capacity = (frame ? frame->bytes : maxFrameBytes) - pixels.size();
        char buffer[64 * 1024];
        const ssize_t count = read(readFd, buffer, std::min<qsizetype>(sizeof(buffer), capacity + 1));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return failure(CaptureText::tr("Could not read the screenshot from KWin."));
        }
        if (count > capacity) {
            return failure(CaptureText::tr("KWin returned invalid image data for %1.").arg(subject));
        }
        atEnd = count == 0;
        pixels.append(buffer, count);
    }

    if (!frame || !atEnd) {
        if (!frame && reply.isFinished() && reply.isError()) {
            return failure(describeError(reply.error(), subject));
        }
        return failure(CaptureText::tr("KWin timed out while capturing %1.").arg(subject));
    }
    if (pixels.size() != frame->bytes) {
        return failure(CaptureText::tr("KWin returned incomplete image data for %1.").arg(subject));
    }

    auto storage = std::make_unique<QByteArray>(std::move(pixels));
    QImage image(reinterpret_cast<uchar *>(storage->data()), frame->width, frame->height, frame->stride,
                 frame->format, [](void *buffer) { delete static_cast<QByteArray *>(buffer); }, storage.get());
    if (image.isNull()) {
        return failure(CaptureText::tr("Could not create the screenshot image for %1.").arg(subject));
    }
    (void)storage.release(); // The image owns the bytes until the last shared copy is destroyed.
    return {std::move(image), frame->scale, {}};
}

} // namespace

namespace
{

// Runs one ScreenShot2 call. method takes the leading arguments, then options and the pipe.
CaptureResult capture(const QString &method, const QVariantList &leading, bool includeCursor, const QString &connector,
                      const QString &subject)
{
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.connectionCapabilities().testFlag(QDBusConnection::UnixFileDescriptorPassing)) {
        return failure(CaptureText::tr("The KDE screenshot service is unavailable on the session bus."));
    }

    int pipeFds[2];
    if (pipe2(pipeFds, O_CLOEXEC) != 0) {
        return failure(CaptureText::tr("Could not create the connection for screenshot data."));
    }
    QDBusUnixFileDescriptor reader;
    reader.giveFileDescriptor(pipeFds[0]);
    QDBusUnixFileDescriptor writer;
    writer.giveFileDescriptor(pipeFds[1]);
    // Only our read end is nonblocking. KWin controls the behavior of its write end.
    if (fcntl(reader.fileDescriptor(), F_SETFL, O_NONBLOCK) == -1) {
        return failure(CaptureText::tr("Could not prepare the connection for screenshot data."));
    }

    QVariantMap options{
        {QStringLiteral("native-resolution"), true},
        {QStringLiteral("include-cursor"), includeCursor},
    };
    if (connector.isEmpty()) {
        // Window captures: only the window content, like the window looks to its user.
        options.insert(QStringLiteral("include-decoration"), false);
        options.insert(QStringLiteral("include-shadow"), false);
    }
    QDeadlineTimer deadline(captureTimeoutMs, Qt::PreciseTimer);
    QDBusPendingReply<QVariantMap> reply = [&] {
        QDBusMessage request = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
                                                              QStringLiteral("/org/kde/KWin/ScreenShot2"),
                                                              QStringLiteral("org.kde.KWin.ScreenShot2"), method);
        request.setInteractiveAuthorizationAllowed(false);
        QVariantList arguments = leading;
        arguments << options << QVariant::fromValue(writer);
        request.setArguments(arguments);
        LatencyTrace::mark(LatencyTrace::Stage::Request);
        return bus.asyncCall(request, captureTimeoutMs);
    }();
    writer = QDBusUnixFileDescriptor();

    try {
        return receiveFrame(reply, reader.fileDescriptor(), deadline, connector, subject);
    } catch (const std::bad_alloc &) {
        return failure(CaptureText::tr("There is not enough memory to capture %1.").arg(subject));
    }
}

} // namespace

CaptureResult CaptureService::captureScreen(const QString &connector, bool includeCursor)
{
    if (connector.isEmpty()) {
        return failure(CaptureText::tr("Choose a monitor to capture."));
    }
    return capture(QStringLiteral("CaptureScreen"), {connector}, includeCursor, connector,
                   CaptureText::tr("monitor %1").arg(connector));
}

CaptureResult CaptureService::captureWindow(const QString &handle, bool includeCursor)
{
    if (handle.isEmpty()) {
        return failure(CaptureText::tr("Choose a window to capture."));
    }
    return capture(QStringLiteral("CaptureWindow"), {handle}, includeCursor, QString(), CaptureText::tr("the window"));
}

} // namespace glimply
