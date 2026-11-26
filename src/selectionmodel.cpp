#include "selectionmodel.h"

#include <algorithm>

namespace glimply
{

int SelectionModel::add(const QString &connector, const QRect &rect)
{
    const int id = nextId_++;
    stack_.append({id, connector, rect});
    activeId_ = id;
    return id;
}

int SelectionModel::add(const QRect &globalRect)
{
    return add(QString(), globalRect);
}

void SelectionModel::setRect(int id, const QRect &rect)
{
    for (Selection &selection : stack_) {
        if (selection.id == id) {
            selection.rect = rect;
            return;
        }
    }
}

void SelectionModel::setNativeNudge(int id, const QMargins &nudge)
{
    for (Selection &selection : stack_) {
        if (selection.id == id) {
            selection.nativeNudge = nudge;
            return;
        }
    }
}

void SelectionModel::remove(int id)
{
    stack_.removeIf([id](const Selection &selection) { return selection.id == id; });
    if (activeId_ == id) {
        activeId_ = 0;
    }
}

void SelectionModel::clear()
{
    stack_.clear();
    activeId_ = 0;
}

void SelectionModel::activate(int id)
{
    const auto it = std::find_if(stack_.begin(), stack_.end(), [id](const Selection &s) { return s.id == id; });
    if (it == stack_.end()) {
        return;
    }
    const Selection selection = *it;
    stack_.erase(it);
    stack_.append(selection);
    activeId_ = id;
}

void SelectionModel::deactivate()
{
    activeId_ = 0;
}

int SelectionModel::activeId() const
{
    return activeId_;
}

const Selection *SelectionModel::active() const
{
    return find(activeId_);
}

const Selection *SelectionModel::find(int id) const
{
    const auto it = std::find_if(stack_.cbegin(), stack_.cend(), [id](const Selection &s) { return s.id == id; });
    return it == stack_.cend() ? nullptr : &*it;
}

int SelectionModel::count() const
{
    return int(stack_.size());
}

bool SelectionModel::isEmpty() const
{
    return stack_.isEmpty();
}

const QList<Selection> &SelectionModel::stack() const
{
    return stack_;
}

QList<Selection> SelectionModel::inDrawingOrder() const
{
    QList<Selection> ordered = stack_;
    std::sort(ordered.begin(), ordered.end(), [](const Selection &a, const Selection &b) { return a.id < b.id; });
    return ordered;
}

int SelectionModel::number(int id) const
{
    if (!find(id)) {
        return 0;
    }
    return 1 + int(std::count_if(stack_.cbegin(), stack_.cend(), [id](const Selection &s) { return s.id < id; }));
}

int SelectionModel::topmostAt(const QString &connector, const QPoint &pos) const
{
    for (auto it = stack_.crbegin(); it != stack_.crend(); ++it) {
        if (it->connector == connector && it->rect.contains(pos)) {
            return it->id;
        }
    }
    return 0;
}

int SelectionModel::topmostAt(const QPoint &globalPos) const
{
    return topmostAt(QString(), globalPos);
}

} // namespace glimply
