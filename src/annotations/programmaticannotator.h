#pragma once

#include "imageoutput.h"

#include <QJsonArray>
#include <QString>

namespace glimply
{

struct AnnotateRequest
{
    QString inputPath;
    QJsonArray operations;
    QString outputPath;
    bool overwrite = false;
    ExportOptions exportOptions;
    bool formatExplicit = false;
};

struct AnnotateResult
{
    bool ok = false;
    QString outputPath;
    int width = 0;
    int height = 0;
    QString format;
    qint64 bytes = 0;
    int appliedCount = 0;
    QString error;
};

// Applies programmatic vector annotations and irreversible pixel redactions onto an existing
// image file using AnnotationDocument + renderAnnotatedCrop.
[[nodiscard]] AnnotateResult applyProgrammaticAnnotations(const AnnotateRequest &request);

} // namespace glimply
