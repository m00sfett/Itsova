#pragma once

#include "annotationitem.h"

#include <QList>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QUndoStack>

#include <memory>

namespace glimply
{

class AddItemCommand;
class ModifyItemCommand;
class RemoveItemCommand;
class ReorderItemCommand;

// Holds the non-destructive vector annotation items and undo history for a single canvas
// (either global logical desktop coordinates in CaptureSession or image-logical coordinates in
// QuickEditorWindow).
class AnnotationDocument : public QObject
{
    Q_OBJECT

public:
    explicit AnnotationDocument(QObject *parent = nullptr);

    // Undoable mutations.
    int addItem(AnnotationItem item);
    void removeItem(int id);
    void commitModification(const AnnotationItem &before, const AnnotationItem &after, int mergeKey = -1);
    int duplicateItem(int id, const QPointF &offset = QPointF(12.0, 12.0));
    void bringToFront(int id);
    void sendToBack(int id);
    void clear();

    // Live in-progress preview update during a drag (does not push onto undoStack).
    void setItemPreview(const AnnotationItem &item);

    // Selection management.
    void select(int id);
    void clearSelection();
    [[nodiscard]] int selectedId() const;
    [[nodiscard]] const AnnotationItem *selectedItem() const;
    [[nodiscard]] const AnnotationItem *findItem(int id) const;

    // Paint order (index 0 bottom, last() top).
    [[nodiscard]] const QList<AnnotationItem> &items() const;
    [[nodiscard]] int count() const;
    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] bool hasRedactions() const;
    [[nodiscard]] int nextStepNumber() const;

    // Checks non-Spotlight items top-to-bottom first, then Spotlight items.
    [[nodiscard]] int topmostHitAt(const QPointF &logicalPos, qreal tolerance = 6.0) const;

    [[nodiscard]] QUndoStack &undoStack();
    [[nodiscard]] const QUndoStack &undoStack() const;

    // Coordinate translation helpers for overlay-to-editor handoff and undoable crops.
    void translateAll(const QPointF &delta);
    [[nodiscard]] QList<AnnotationItem> translatedItems(const QPointF &delta,
                                                        const QRectF &clipBounds = {}) const;
    [[nodiscard]] std::unique_ptr<AnnotationDocument> cloneTranslated(const QPointF &delta,
                                                                      const QRectF &clipBounds = {},
                                                                      QObject *parent = nullptr) const;
    void replaceItemsRaw(QList<AnnotationItem> items, int selectedId = 0);

Q_SIGNALS:
    void changed();
    void selectionChanged(int selectedId);

private:
    friend class AddItemCommand;
    friend class RemoveItemCommand;
    friend class ModifyItemCommand;
    friend class ReorderItemCommand;

    [[nodiscard]] int indexOfId(int id) const;
    void setSelectedIdInternal(int id);

    QList<AnnotationItem> items_;
    int selectedId_ = 0;
    int nextId_ = 1;
    QUndoStack *undoStack_ = nullptr;
};

} // namespace glimply
