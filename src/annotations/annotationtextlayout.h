#pragma once

#include "annotationitem.h"

#include <QFont>
#include <QPainter>
#include <QSizeF>

namespace itsova
{

// Resolves item.font against the application default font while keeping bold/italic/pointSize.
[[nodiscard]] QFont resolvedAnnotationFont(const AnnotationItem &item);

// Measures the logical bounding size of item.text for Point Text (unbounded line width) or
// Paragraph Text (wrapped to std::max(60.0, item.rect.width())).
[[nodiscard]] QSizeF measureAnnotationText(const AnnotationItem &item);

// Draws item.text inside logicalRect using QTextLayout (HarfBuzz-safe for RTL, ZWNJ, CJK, and
// emoji) and applies item.legibility (None, Shadow, Outline, or Pill).
void drawAnnotationText(QPainter &painter, const AnnotationItem &item);

} // namespace itsova
