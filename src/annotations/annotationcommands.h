#pragma once

#include "annotationitem.h"

#include <QList>
#include <QPair>
#include <QUndoCommand>

namespace glimply
{

class AnnotationDocument;

class AddItemCommand final : public QUndoCommand
{
public:
    AddItemCommand(AnnotationDocument *doc, AnnotationItem item);

    void undo() override;
    void redo() override;

private:
    AnnotationDocument *doc_;
    AnnotationItem item_;
    int index_ = -1;
    int previousSelectedId_ = 0;
};

class RemoveItemCommand final : public QUndoCommand
{
public:
    RemoveItemCommand(AnnotationDocument *doc, int itemId);

    void undo() override;
    void redo() override;

private:
    AnnotationDocument *doc_;
    AnnotationItem removedItem_;
    int index_ = -1;
    int previousSelectedId_ = 0;
    QList<QPair<int, int>> previousStepNumbers_;
};

class ModifyItemCommand final : public QUndoCommand
{
public:
    ModifyItemCommand(AnnotationDocument *doc, AnnotationItem before, AnnotationItem after, int mergeKey = -1);

    int id() const override;
    bool mergeWith(const QUndoCommand *other) override;
    void undo() override;
    void redo() override;

private:
    AnnotationDocument *doc_;
    AnnotationItem before_;
    AnnotationItem after_;
    int mergeKey_ = -1;
};

class ReorderItemCommand final : public QUndoCommand
{
public:
    ReorderItemCommand(AnnotationDocument *doc, int itemId, int fromIndex, int toIndex);

    void undo() override;
    void redo() override;

private:
    AnnotationDocument *doc_;
    int itemId_ = 0;
    int fromIndex_ = -1;
    int toIndex_ = -1;
};

} // namespace glimply
