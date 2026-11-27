#include "captureservice.h"
#include "imageoutput.h"

#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    application.setDesktopFileName(QStringLiteral("org.itsova.CaptureSmoke"));
    QTextStream report(stdout);
    if (qEnvironmentVariable("ITSOVA_CAPTURE_SMOKE") != QStringLiteral("1")) {
        report << "Set ITSOVA_CAPTURE_SMOKE=1 to run this live desktop check.\n";
        return 2;
    }

    QTemporaryDir output(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)
                         + QStringLiteral("/itsova-smoke-XXXXXX"));
    if (!output.isValid())
        return 1;

    QSettings config(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("itsova"), QStringLiteral("capture-smoke"));
    const QString selected = config.value(QStringLiteral("screen")).toString();
    const QRect area = config.value(QStringLiteral("area"), QRect(8, 64, 400, 300)).toRect();
    const auto timestamp = QDateTime::currentDateTime();
    const itsova::OutputWriter writer(output.path());
    int tested = 0;
    for (QScreen *screen : application.screens()) {
        if (!selected.isEmpty() && screen->name() != selected)
            continue;
        if (!QRect(QPoint(), screen->size()).contains(area) || !area.isValid()) {
            report << "The configured area is outside " << screen->name() << '\n';
            return 1;
        }
        QByteArray first;
        for (int i = 0; i < 2; ++i) {
            const auto frame = itsova::CaptureService::captureScreen(screen->name());
            if (frame.image.isNull()) {
                report << frame.error << '\n';
                return 1;
            }
            QString error;
            const auto crop = itsova::cropLogicalArea(frame.image, area, frame.scale, &error);
            // On Wayland QScreen::devicePixelRatio() may be the integer buffer scale.
            // Check the crop against the native frame and logical monitor dimensions.
            const qreal sx = qreal(frame.image.width()) / screen->size().width();
            const qreal sy = qreal(frame.image.height()) / screen->size().height();
            const QSize expected(qRound((area.x() + area.width()) * sx) - qRound(area.x() * sx),
                                 qRound((area.y() + area.height()) * sy) - qRound(area.y() * sy));
            if (crop.isNull() || crop.size() != expected) {
                report << screen->name() << ": got " << crop.width() << 'x' << crop.height()
                       << ", expected " << expected.width() << 'x' << expected.height() << ". " << error << '\n';
                return 1;
            }
            QString path;
            if (!writer.writeImage(crop, screen->name(), timestamp, &path, &error)) {
                report << error << '\n';
                return 1;
            }
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return 1;
            const auto encoded = file.readAll();
            if (encoded.isEmpty() || (i == 1 && first != encoded)) {
                report << screen->name() << ": the two captures differ. Check that the selected area is still.\n";
                return 1;
            }
            first = encoded;
            if (i == 1)
                report << screen->name() << ": " << crop.width() << 'x' << crop.height() << ", identical PNGs\n";
        }
        ++tested;
    }
    if (!tested)
        report << "No matching monitor.\n";
    return tested ? 0 : 1;
}
