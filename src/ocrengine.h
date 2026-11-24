#pragma once

#include <QImage>
#include <QList>
#include <QMutex>
#include <QRect>
#include <QString>
#include <QStringList>

#include <optional>

namespace glimply
{

struct OcrResult
{
    bool ok = false;
    QString text;
    QString error;
};

// Optional local text recognition (#27). glimply builds and runs without any OCR backend; the
// engine only looks for one at runtime. An integrated engine can later implement this interface.
class OcrEngine
{
public:
    virtual ~OcrEngine() = default;

    [[nodiscard]] virtual bool available() const = 0;
    // Human-readable backend description, e.g. "tesseract 5.5.1"; empty when unavailable.
    [[nodiscard]] virtual QString description() const = 0;
    // Installed recognition languages (backend codes such as "deu", "eng").
    [[nodiscard]] virtual QStringList languages() const = 0;
    // Thread-safe; blocks until the text is recognized or the timeout hits.
    [[nodiscard]] virtual OcrResult recognize(const QImage &image, const QStringList &languages) const = 0;
};

// Runs the tesseract command line tool: the image goes in on stdin as PNG, the text comes back on
// stdout. No temporary files and no shell.
class TesseractOcrEngine final : public OcrEngine
{
public:
    static constexpr int TimeoutMs = 30'000;

    // An empty executable means "tesseract" from PATH.
    explicit TesseractOcrEngine(QString executable = QString());

    [[nodiscard]] bool available() const override;
    [[nodiscard]] QString description() const override;
    [[nodiscard]] QStringList languages() const override;
    [[nodiscard]] OcrResult recognize(const QImage &image, const QStringList &languages) const override;
    // Forgets cached detection results (call when the settings open, after installing packages).
    void refresh();

private:
    [[nodiscard]] QString executable() const;

    QString configuredExecutable_;
    mutable QMutex mutex_;
    mutable std::optional<QString> resolved_;
    mutable std::optional<QStringList> languages_;
    mutable std::optional<QString> description_;
};

// Parses `tesseract --list-langs` output; "osd" (orientation detection) is kept here.
[[nodiscard]] QStringList parseTesseractLanguages(const QString &output);
// The configured languages that are installed, or all installed except "osd" when none are.
[[nodiscard]] QStringList effectiveOcrLanguages(const QStringList &configured, const QStringList &installed);
[[nodiscard]] QStringList tesseractArguments(const QStringList &languages);
// Indices of rects in reading order: top to bottom, then left to right on the same line.
[[nodiscard]] QList<int> ocrReadingOrder(const QList<QRect> &rects);
// Joins non-empty texts with a blank line.
[[nodiscard]] QString joinOcrTexts(const QStringList &texts);
// One-line preview of at most maxChars characters for notifications.
[[nodiscard]] QString ocrPreview(const QString &text, int maxChars = 80);

} // namespace glimply
