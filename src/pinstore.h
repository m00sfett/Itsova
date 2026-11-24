#pragma once

#include <QDateTime>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>

#include <optional>

namespace glimply
{

// One pinned reference screenshot (#28). The image lives next to pins.json as <id>.png.
struct PinRecord
{
    QString id;
    // Output the pin was last placed on, and its top-left corner relative to that output
    // (logical coordinates), so a pin follows its monitor when the layout changes.
    QString connector;
    QPoint offset;
    double zoom = 1.0;
    double opacity = 1.0;
    QDateTime created;
};

// A connected output as needed for placing pins.
struct PinOutput
{
    QString connector;
    QRect geometry; // logical, global
};

// Persistent list of pins in ~/.local/share/glimply/pins/. Pins stay until the user removes them.
class PinStore
{
public:
    explicit PinStore(QString directory = defaultDirectory());

    [[nodiscard]] static QString defaultDirectory();
    [[nodiscard]] QString directory() const;
    [[nodiscard]] QString imagePath(const QString &id) const;

    bool load(QString *error = nullptr);
    bool save(QString *error = nullptr) const;

    // Writes the image (owner-only PNG) and appends a record. Returns the record, or nullopt when
    // the image could not be written.
    std::optional<PinRecord> add(const QImage &image, const QString &connector, const QPoint &offset,
                                 QString *error = nullptr);
    // Updates an existing record (matched by id). Returns false for unknown ids.
    bool update(const PinRecord &record);
    // Removes the record and deletes its image. Returns false for unknown ids.
    bool remove(const QString &id);
    [[nodiscard]] std::optional<PinRecord> pin(const QString &id) const;
    [[nodiscard]] const QList<PinRecord> &pins() const;

    // Whether the user hid all pins (tray toggle); persisted with the list.
    [[nodiscard]] bool hidden() const;
    void setHidden(bool hidden);

private:
    QString directory_;
    QList<PinRecord> pins_;
    bool hidden_ = false;
};

// Fixed zoom steps from 25 % to 400 %.
[[nodiscard]] const QList<double> &pinZoomSteps();
// Next step up (direction > 0) or down (direction < 0) from the current zoom.
[[nodiscard]] double nextPinZoom(double current, int direction);
// Opacity in 10 % steps between 20 % and 100 %.
[[nodiscard]] double clampPinOpacity(double opacity);
[[nodiscard]] double nextPinOpacity(double current, int direction);

// Global logical top-left for a pin: on its own output when connected, otherwise on the primary
// output (the first entry) with the offset clamped so the pin stays reachable. size is the pin's
// logical size, used for clamping.
[[nodiscard]] QPoint resolvePinPosition(const PinRecord &record, const QList<PinOutput> &outputs,
                                        const QSize &size);
// Output that contains a global point (or the nearest one), with the point relative to it.
[[nodiscard]] std::optional<std::pair<QString, QPoint>> pinOutputAt(const QPoint &globalTopLeft,
                                                                     const QList<PinOutput> &outputs);

} // namespace glimply
