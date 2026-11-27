#pragma once

#include "annotationdocument.h"
#include "captureservice.h"
#include "targets.h"

#include <QHash>
#include <QImage>
#include <QList>
#include <QString>

namespace itsova
{

// Returns connected monitor connectors whose logical geometry intersects at least one item in doc.
[[nodiscard]] QList<QString> annotatedConnectors(const QList<ScreenInfo> &screens, const AnnotationDocument &doc);

// Renders annotations overlapping screen onto a native monitor frame (returns frame.image unchanged when doc is empty).
[[nodiscard]] QImage renderAnnotatedMonitor(const ScreenInfo &screen,
                                            const CaptureResult &frame,
                                            const AnnotationDocument &doc);

// Composes all connected screens at the highest monitor scale and burns intersecting annotations across the virtual desktop.
[[nodiscard]] QImage renderAnnotatedAllMonitors(const QList<ScreenInfo> &screens,
                                                const QHash<QString, CaptureResult> &frames,
                                                const AnnotationDocument &doc,
                                                QString *error = nullptr);

// Pure security policy guards preventing raw Print snapshots from leaking unredacted pixels.
[[nodiscard]] bool shouldBypassRawPrintFastPath(bool frozen, const QString &printPath, const AnnotationDocument &doc);
[[nodiscard]] bool shouldDeletePrintFileOnCopy(const AnnotationDocument &doc, const QString &printPath);
bool purgeUnredactedPrintFile(const QString &printPath, bool clearClipboard);

} // namespace itsova
