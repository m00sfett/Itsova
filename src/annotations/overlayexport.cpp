#include "overlayexport.h"

#include "annotationrenderer.h"
#include "triggerplan.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QRectF>

#include <algorithm>

namespace itsova
{

QList<QString> annotatedConnectors(const QList<ScreenInfo> &screens, const AnnotationDocument &doc)
{
    QList<QString> hit;
    if (doc.isEmpty()) {
        return hit;
    }
    for (const ScreenInfo &screen : screens) {
        const QRectF screenRect(screen.geometry);
        for (const AnnotationItem &item : doc.items()) {
            if (item.intersectsLogicalRect(screenRect)) {
                hit.append(screen.connector);
                break;
            }
        }
    }
    return hit;
}

QImage renderAnnotatedMonitor(const ScreenInfo &screen, const CaptureResult &frame, const AnnotationDocument &doc)
{
    if (frame.image.isNull() || doc.isEmpty()) {
        return frame.image;
    }
    const qreal scale = frame.scale > 0.0 ? frame.scale : 1.0;
    return renderAnnotatedCrop(frame.image, QRectF(screen.geometry), scale, doc);
}

QImage renderAnnotatedAllMonitors(const QList<ScreenInfo> &screens, const QHash<QString, CaptureResult> &frames,
                                  const AnnotationDocument &doc, QString *error)
{
    QImage composed = composeAllMonitors(screens, frames, error);
    if (composed.isNull() || doc.isEmpty()) {
        return composed;
    }
    QRect bounds;
    qreal maxScale = 0.0;
    for (const ScreenInfo &screen : screens) {
        bounds = bounds.united(screen.geometry);
        maxScale = std::max(maxScale, frames.value(screen.connector).scale);
    }
    if (bounds.isEmpty() || maxScale <= 0.0) {
        return composed;
    }
    return renderAnnotatedCrop(composed, QRectF(bounds), maxScale, doc);
}

bool shouldSavePrintImmediately(bool printOpensOverlay, bool forceShowOverlay, bool saveImmediatelyOnPrint)
{
    if (!printOpensOverlay && !forceShowOverlay) {
        return true;
    }
    return saveImmediatelyOnPrint;
}

bool shouldPurgeDeferredPrintOnEditorSave(const QString &deferredPrintPath, const QString &savedPath)
{
    return !deferredPrintPath.isEmpty() && savedPath != deferredPrintPath;
}

bool shouldBypassRawPrintFastPath(bool frozen, const QString &printPath, const AnnotationDocument &doc)
{
    return frozen && !printPath.isEmpty() && doc.isEmpty();
}

bool shouldDeletePrintFileOnCopy(const AnnotationDocument &doc, const QString &printPath)
{
    return !printPath.isEmpty() && doc.hasRedactions();
}

bool purgeUnredactedPrintFile(const QString &printPath, bool clearClipboard)
{
    if (clearClipboard && QGuiApplication::clipboard()) {
        QGuiApplication::clipboard()->clear();
    }
    if (printPath.isEmpty()) {
        return true;
    }
    if (QFile::exists(printPath)) {
        QFile::remove(printPath);
    }
    return !QFile::exists(printPath);
}

} // namespace itsova
