#include "annotationdocument.h"

#include "annotationcommands.h"

#include <algorithm>

namespace glimply
{

AnnotationDocument::AnnotationDocument(QObject *parent)
    : QObject(parent)
    , undoStack_(new QUndoStack(this))
{
}

int AnnotationDocument::addItem(AnnotationItem item)
{
    if (item.id <= 0) {
        item.id = nextId_++;
    } else {
        nextId_ = std::max(nextId_, item.id + 1);
    }
    if (item.kind == ItemKind::Step) {
        item.stepNumber = nextStepNumber();
    }
    const int assignedId = item.id;
    undoStack_->push(new AddItemCommand(this, std::move(item)));
    return assignedId;
}

void AnnotationDocument::removeItem(int id)
{
    if (indexOfId(id) < 0) {
        return;
    }
    undoStack_->push(new RemoveItemCommand(this, id));
}

void AnnotationDocument::commitModification(const AnnotationItem &before, const AnnotationItem &after, int mergeKey)
{
    if (before.id <= 0 || indexOfId(before.id) < 0) {
        return;
    }
    undoStack_->push(new ModifyItemCommand(this, before, after, mergeKey));
}

int AnnotationDocument::duplicateItem(int id, const QPointF &offset)
{
    const AnnotationItem *existing = findItem(id);
    if (!existing) {
        return 0;
    }
    AnnotationItem copy = *existing;
    copy.id = 0;
    copy.translate(offset);
    return addItem(std::move(copy));
}

void AnnotationDocument::bringToFront(int id)
{
    const int idx = indexOfId(id);
    const int top = int(items_.size()) - 1;
    if (idx < 0 || idx >= top) {
        return;
    }
    undoStack_->push(new ReorderItemCommand(this, id, idx, top));
}

void AnnotationDocument::sendToBack(int id)
{
    const int idx = indexOfId(id);
    if (idx <= 0) {
        return;
    }
    undoStack_->push(new ReorderItemCommand(this, id, idx, 0));
}

void AnnotationDocument::clear()
{
    if (items_.isEmpty() && undoStack_->count() == 0 && selectedId_ == 0) {
        return;
    }
    items_.clear();
    undoStack_->clear();
    setSelectedIdInternal(0);
    Q_EMIT changed();
}

void AnnotationDocument::setItemPreview(const AnnotationItem &item)
{
    const int idx = indexOfId(item.id);
    if (idx < 0) {
        return;
    }
    items_[idx] = item;
    Q_EMIT changed();
}

void AnnotationDocument::select(int id)
{
    const int target = indexOfId(id) >= 0 ? id : 0;
    setSelectedIdInternal(target);
}

void AnnotationDocument::clearSelection()
{
    setSelectedIdInternal(0);
}

int AnnotationDocument::selectedId() const
{
    return selectedId_;
}

const AnnotationItem *AnnotationDocument::selectedItem() const
{
    return findItem(selectedId_);
}

const AnnotationItem *AnnotationDocument::findItem(int id) const
{
    const int idx = indexOfId(id);
    return idx >= 0 ? &items_.at(idx) : nullptr;
}

const QList<AnnotationItem> &AnnotationDocument::items() const
{
    return items_;
}

int AnnotationDocument::count() const
{
    return int(items_.size());
}

bool AnnotationDocument::isEmpty() const
{
    return items_.isEmpty();
}

bool AnnotationDocument::hasRedactions() const
{
    return std::any_of(items_.cbegin(), items_.cend(),
                       [](const AnnotationItem &item) { return item.kind == ItemKind::Redact; });
}

int AnnotationDocument::nextStepNumber() const
{
    return 1 + int(std::count_if(items_.cbegin(), items_.cend(),
                                 [](const AnnotationItem &item) { return item.kind == ItemKind::Step; }));
}

int AnnotationDocument::topmostHitAt(const QPointF &logicalPos, qreal tolerance) const
{
    for (auto it = items_.crbegin(); it != items_.crend(); ++it) {
        if (it->kind != ItemKind::Spotlight && it->hitTest(logicalPos, tolerance)) {
            return it->id;
        }
    }
    for (auto it = items_.crbegin(); it != items_.crend(); ++it) {
        if (it->kind == ItemKind::Spotlight && it->hitTest(logicalPos, tolerance)) {
            return it->id;
        }
    }
    return 0;
}

QUndoStack &AnnotationDocument::undoStack()
{
    return *undoStack_;
}

const QUndoStack &AnnotationDocument::undoStack() const
{
    return *undoStack_;
}

void AnnotationDocument::translateAll(const QPointF &delta)
{
    for (AnnotationItem &item : items_) {
        item.translate(delta);
    }
    Q_EMIT changed();
}

QList<AnnotationItem> AnnotationDocument::translatedItems(const QPointF &delta, const QRectF &clipBounds) const
{
    QList<AnnotationItem> out;
    for (AnnotationItem item : items_) {
        if (clipBounds.isValid() && !item.intersectsLogicalRect(clipBounds)) {
            continue;
        }
        item.translate(delta);
        out.append(std::move(item));
    }
    return out;
}

std::unique_ptr<AnnotationDocument> AnnotationDocument::cloneTranslated(const QPointF &delta,
                                                                        const QRectF &clipBounds,
                                                                        QObject *parent) const
{
    auto copy = std::make_unique<AnnotationDocument>(parent);
    copy->replaceItemsRaw(translatedItems(delta, clipBounds), 0);
    return copy;
}

void AnnotationDocument::replaceItemsRaw(QList<AnnotationItem> items, int selectedId)
{
    items_ = std::move(items);
    undoStack_->clear();
    int maxId = 0;
    for (const AnnotationItem &item : std::as_const(items_)) {
        maxId = std::max(maxId, item.id);
    }
    nextId_ = maxId + 1;
    setSelectedIdInternal(indexOfId(selectedId) >= 0 ? selectedId : 0);
    Q_EMIT changed();
}

int AnnotationDocument::indexOfId(int id) const
{
    if (id <= 0) {
        return -1;
    }
    for (qsizetype i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id) {
            return int(i);
        }
    }
    return -1;
}

void AnnotationDocument::setSelectedIdInternal(int id)
{
    if (selectedId_ != id) {
        selectedId_ = id;
        Q_EMIT selectionChanged(selectedId_);
        Q_EMIT changed();
    }
}

} // namespace glimply
