#include "annotationcommands.h"

#include "annotationdocument.h"

#include <QCoreApplication>

#include <algorithm>

namespace glimply
{

AddItemCommand::AddItemCommand(AnnotationDocument *doc, AnnotationItem item)
    : QUndoCommand(QCoreApplication::translate("AnnotationCommands", "Add Annotation"))
    , doc_(doc)
    , item_(std::move(item))
    , previousSelectedId_(doc ? doc->selectedId() : 0)
{
}

void AddItemCommand::redo()
{
    if (!doc_) {
        return;
    }
    if (index_ < 0 || index_ > doc_->items_.size()) {
        index_ = int(doc_->items_.size());
    }
    doc_->items_.insert(index_, item_);
    doc_->setSelectedIdInternal(item_.id);
    Q_EMIT doc_->changed();
}

void AddItemCommand::undo()
{
    if (!doc_) {
        return;
    }
    const int idx = doc_->indexOfId(item_.id);
    if (idx >= 0) {
        index_ = idx;
        doc_->items_.removeAt(idx);
    }
    const int restoreSel = doc_->indexOfId(previousSelectedId_) >= 0 ? previousSelectedId_ : 0;
    doc_->setSelectedIdInternal(restoreSel);
    Q_EMIT doc_->changed();
}

RemoveItemCommand::RemoveItemCommand(AnnotationDocument *doc, int itemId)
    : QUndoCommand(QCoreApplication::translate("AnnotationCommands", "Delete Annotation"))
    , doc_(doc)
    , previousSelectedId_(doc ? doc->selectedId() : 0)
{
    if (!doc_) {
        return;
    }
    index_ = doc_->indexOfId(itemId);
    if (index_ >= 0) {
        removedItem_ = doc_->items_.at(index_);
        if (removedItem_.kind == ItemKind::Step) {
            for (const AnnotationItem &it : std::as_const(doc_->items_)) {
                if (it.id != itemId && it.kind == ItemKind::Step) {
                    previousStepNumbers_.append({it.id, it.stepNumber});
                }
            }
        }
    }
}

void RemoveItemCommand::redo()
{
    if (!doc_ || removedItem_.id == 0) {
        return;
    }
    const int idx = doc_->indexOfId(removedItem_.id);
    if (idx >= 0) {
        index_ = idx;
        doc_->items_.removeAt(idx);
    }
    if (removedItem_.kind == ItemKind::Step) {
        int nextNum = 1;
        for (AnnotationItem &it : doc_->items_) {
            if (it.kind == ItemKind::Step) {
                it.stepNumber = nextNum++;
            }
        }
    }
    if (doc_->selectedId() == removedItem_.id) {
        doc_->setSelectedIdInternal(0);
    }
    Q_EMIT doc_->changed();
}

void RemoveItemCommand::undo()
{
    if (!doc_ || removedItem_.id == 0) {
        return;
    }
    const int insertIdx = std::clamp(index_, 0, int(doc_->items_.size()));
    doc_->items_.insert(insertIdx, removedItem_);
    if (removedItem_.kind == ItemKind::Step) {
        for (const auto &[id, num] : std::as_const(previousStepNumbers_)) {
            const int idx = doc_->indexOfId(id);
            if (idx >= 0) {
                doc_->items_[idx].stepNumber = num;
            }
        }
    }
    const int restoreSel = doc_->indexOfId(previousSelectedId_) >= 0 ? previousSelectedId_ : removedItem_.id;
    doc_->setSelectedIdInternal(restoreSel);
    Q_EMIT doc_->changed();
}

ModifyItemCommand::ModifyItemCommand(AnnotationDocument *doc, AnnotationItem before, AnnotationItem after,
                                     int mergeKey)
    : QUndoCommand(QCoreApplication::translate("AnnotationCommands", "Modify Annotation"))
    , doc_(doc)
    , before_(std::move(before))
    , after_(std::move(after))
    , mergeKey_(mergeKey)
{
}

int ModifyItemCommand::id() const
{
    return mergeKey_ > 0 ? 10000 + mergeKey_ : -1;
}

bool ModifyItemCommand::mergeWith(const QUndoCommand *other)
{
    if (mergeKey_ <= 0 || other->id() != id()) {
        return false;
    }
    const auto *next = static_cast<const ModifyItemCommand *>(other);
    if (next->doc_ != doc_ || next->after_.id != after_.id) {
        return false;
    }
    after_ = next->after_;
    return true;
}

void ModifyItemCommand::redo()
{
    if (!doc_) {
        return;
    }
    const int idx = doc_->indexOfId(after_.id);
    if (idx >= 0) {
        doc_->items_[idx] = after_;
        doc_->setSelectedIdInternal(after_.id);
        Q_EMIT doc_->changed();
    }
}

void ModifyItemCommand::undo()
{
    if (!doc_) {
        return;
    }
    const int idx = doc_->indexOfId(before_.id);
    if (idx >= 0) {
        doc_->items_[idx] = before_;
        doc_->setSelectedIdInternal(before_.id);
        Q_EMIT doc_->changed();
    }
}

ReorderItemCommand::ReorderItemCommand(AnnotationDocument *doc, int itemId, int fromIndex, int toIndex)
    : QUndoCommand(QCoreApplication::translate("AnnotationCommands", "Reorder Annotation"))
    , doc_(doc)
    , itemId_(itemId)
    , fromIndex_(fromIndex)
    , toIndex_(toIndex)
{
}

void ReorderItemCommand::redo()
{
    if (!doc_) {
        return;
    }
    const int currentIdx = doc_->indexOfId(itemId_);
    if (currentIdx < 0 || toIndex_ < 0 || toIndex_ >= doc_->items_.size()) {
        return;
    }
    doc_->items_.move(currentIdx, toIndex_);
    doc_->setSelectedIdInternal(itemId_);
    Q_EMIT doc_->changed();
}

void ReorderItemCommand::undo()
{
    if (!doc_) {
        return;
    }
    const int currentIdx = doc_->indexOfId(itemId_);
    if (currentIdx < 0 || fromIndex_ < 0 || fromIndex_ >= doc_->items_.size()) {
        return;
    }
    doc_->items_.move(currentIdx, fromIndex_);
    doc_->setSelectedIdInternal(itemId_);
    Q_EMIT doc_->changed();
}

} // namespace glimply
