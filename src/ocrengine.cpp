#include "ocrengine.h"

#include <QBuffer>
#include <QDeadlineTimer>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <numeric>

namespace itsova
{
namespace
{

constexpr int probeTimeoutMs = 5'000;
constexpr qint64 maxOutputBytes = 4 * 1024 * 1024;

QString runProbe(const QString &program, const QStringList &args)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, args);
    if (!process.waitForFinished(probeTimeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    return QString::fromUtf8(process.readAll());
}

} // namespace

TesseractOcrEngine::TesseractOcrEngine(QString executable)
    : configuredExecutable_(std::move(executable))
{
}

QString TesseractOcrEngine::executable() const
{
    QMutexLocker lock(&mutex_);
    if (!resolved_) {
        resolved_ = configuredExecutable_.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("tesseract"))
                                                    : configuredExecutable_;
    }
    return *resolved_;
}

bool TesseractOcrEngine::available() const
{
    return !executable().isEmpty();
}

QString TesseractOcrEngine::description() const
{
    const QString program = executable();
    if (program.isEmpty()) {
        return {};
    }
    {
        QMutexLocker lock(&mutex_);
        if (description_) {
            return *description_;
        }
    }
    const QString output = runProbe(program, {QStringLiteral("--version")});
    const QString first = output.section(QLatin1Char('\n'), 0, 0).trimmed();
    QMutexLocker lock(&mutex_);
    description_ = first.isEmpty() ? QStringLiteral("tesseract") : first;
    return *description_;
}

QStringList TesseractOcrEngine::languages() const
{
    const QString program = executable();
    if (program.isEmpty()) {
        return {};
    }
    {
        QMutexLocker lock(&mutex_);
        if (languages_) {
            return *languages_;
        }
    }
    const QStringList parsed = parseTesseractLanguages(runProbe(program, {QStringLiteral("--list-langs")}));
    QMutexLocker lock(&mutex_);
    languages_ = parsed;
    return parsed;
}

void TesseractOcrEngine::refresh()
{
    QMutexLocker lock(&mutex_);
    resolved_.reset();
    languages_.reset();
    description_.reset();
}

OcrResult TesseractOcrEngine::recognize(const QImage &image, const QStringList &langs) const
{
    OcrResult result;
    const QString program = executable();
    if (program.isEmpty()) {
        result.error = QStringLiteral("tesseract is not installed");
        return result;
    }
    if (image.isNull()) {
        result.error = QStringLiteral("empty image");
        return result;
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
        result.error = QStringLiteral("could not encode the image");
        return result;
    }

    QProcess process;
    process.start(program, tesseractArguments(langs));
    if (!process.waitForStarted(probeTimeoutMs)) {
        result.error = process.errorString();
        return result;
    }
    process.write(png);
    process.closeWriteChannel();
    QByteArray out;
    QByteArray err;
    QDeadlineTimer deadline(TimeoutMs);
    while (process.state() != QProcess::NotRunning) {
        if (deadline.hasExpired()) {
            process.kill();
            process.waitForFinished(1000);
            result.error = QStringLiteral("text recognition timed out after %1 s").arg(TimeoutMs / 1000);
            return result;
        }
        process.waitForReadyRead(100);
        out += process.readAllStandardOutput();
        err += process.readAllStandardError();
        if (out.size() > maxOutputBytes) {
            process.kill();
            process.waitForFinished(1000);
            result.error = QStringLiteral("text recognition produced too much output");
            return result;
        }
    }
    out += process.readAllStandardOutput();
    err += process.readAllStandardError();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString message = QString::fromUtf8(err).trimmed().section(QLatin1Char('\n'), 0, 0);
        result.error = message.isEmpty() ? QStringLiteral("tesseract exited with code %1").arg(process.exitCode())
                                         : message;
        return result;
    }
    QString text = QString::fromUtf8(out);
    text.remove(QLatin1Char('\f'));
    // Trailing whitespace per line and around the block is noise from the layout analysis.
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString &line : lines) {
        while (!line.isEmpty() && line.back().isSpace()) {
            line.chop(1);
        }
    }
    result.text = lines.join(QLatin1Char('\n')).trimmed();
    result.ok = true;
    return result;
}

QStringList parseTesseractLanguages(const QString &output)
{
    QStringList result;
    const QStringList lines = output.split(QLatin1Char('\n'));
    static const QRegularExpression code(QStringLiteral("^[A-Za-z0-9_+\\-]+$"));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        // The first line is a header such as `List of available languages in "/usr/share/tessdata/" (2):`.
        if (line.isEmpty() || line.contains(QLatin1Char(' ')) || !code.match(line).hasMatch()) {
            continue;
        }
        if (!result.contains(line)) {
            result.append(line);
        }
    }
    return result;
}

QStringList effectiveOcrLanguages(const QStringList &configured, const QStringList &installed)
{
    QStringList result;
    for (const QString &lang : configured) {
        if (installed.contains(lang) && !result.contains(lang)) {
            result.append(lang);
        }
    }
    if (!result.isEmpty()) {
        return result;
    }
    for (const QString &lang : installed) {
        if (lang != QLatin1String("osd") && lang != QLatin1String("equ")) {
            result.append(lang);
        }
    }
    return result;
}

QStringList tesseractArguments(const QStringList &languages)
{
    QStringList args{QStringLiteral("stdin"), QStringLiteral("stdout")};
    if (!languages.isEmpty()) {
        args << QStringLiteral("-l") << languages.join(QLatin1Char('+'));
    }
    args << QStringLiteral("--psm") << QStringLiteral("3");
    return args;
}

QList<int> ocrReadingOrder(const QList<QRect> &rects)
{
    QList<int> order(rects.size());
    std::iota(order.begin(), order.end(), 0);
    // Strict top-to-bottom, then left-to-right (a strict weak ordering, unlike fuzzy line grouping).
    std::stable_sort(order.begin(), order.end(), [&rects](int a, int b) {
        const QRect &ra = rects.at(a);
        const QRect &rb = rects.at(b);
        return ra.top() != rb.top() ? ra.top() < rb.top() : ra.left() < rb.left();
    });
    return order;
}

QString joinOcrTexts(const QStringList &texts)
{
    QStringList parts;
    for (const QString &text : texts) {
        const QString trimmed = text.trimmed();
        if (!trimmed.isEmpty()) {
            parts.append(trimmed);
        }
    }
    return parts.join(QStringLiteral("\n\n"));
}

QString ocrPreview(const QString &text, int maxChars)
{
    QString flat = text.simplified();
    if (flat.size() > maxChars) {
        flat = flat.left(std::max(1, maxChars - 1)).trimmed() + QChar(0x2026);
    }
    return flat;
}

} // namespace itsova
