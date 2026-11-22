#pragma once

#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>

namespace glimply
{

// One rectangle drawn in the capture overlay, in logical coordinates local to its monitor.
struct Selection {
    int id = 0;
    QString connector;
    QRect rect;
};

// The rectangles of one overlay session. Rectangles may overlap; the most recently touched one
// is on top and wins clicks. At most one rectangle is active (it has handles and the action bar).
class SelectionModel
{
public:
    // Adds a rectangle on top and makes it active. Returns its id.
    int add(const QString &connector, const QRect &rect);
    void setRect(int id, const QRect &rect);
    void remove(int id);
    void clear();

    // Makes id active and moves it on top.
    void activate(int id);
    void deactivate();

    [[nodiscard]] int activeId() const;
    [[nodiscard]] const Selection *active() const;
    [[nodiscard]] const Selection *find(int id) const;
    [[nodiscard]] int count() const;
    [[nodiscard]] bool isEmpty() const;

    // Rectangles from bottom to top (paint order).
    [[nodiscard]] const QList<Selection> &stack() const;
    // Rectangles in the order they were drawn; numbers and file names follow this order.
    [[nodiscard]] QList<Selection> inDrawingOrder() const;
    // 1-based position of id in drawing order, 0 if unknown.
    [[nodiscard]] int number(int id) const;

    // Topmost rectangle on connector that contains pos, or 0.
    [[nodiscard]] int topmostAt(const QString &connector, const QPoint &pos) const;

private:
    QList<Selection> stack_;
    int activeId_ = 0;
    int nextId_ = 1;
};

} // namespace glimply
