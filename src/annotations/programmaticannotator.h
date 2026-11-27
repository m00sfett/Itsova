#pragma once

#include "imageoutput.h"

#include <QJsonArray>
#include <QString>

namespace itsova
{

struct AnnotateRequest
{
    QString inputPath;
    QJsonArray operations;
    QString outputPath;
    QString allowedOutputDir;
    bool overwrite = false;
    ExportOptions exportOptions;
    bool formatExplicit = false;
};

struct AnnotateOutputTarget
{
    bool ok = false;
    QString resolvedPath;
    bool willOverwrite = false;
    QString error;
};

// Validates and resolves the target output path for annotate_image, enforcing overwrite protection,
// symlink rejection, and optional capture-directory confinement.
[[nodiscard]] AnnotateOutputTarget resolveAnnotateOutputTarget(const QString &inputPath,
                                                               const QString &requestedOutputPath,
                                                               const ExportOptions &exportOptions,
                                                               bool formatExplicit,
                                                               const QString &allowedOutputDir,
                                                               bool overwrite);

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

// Applies programmatic vector annotations and pixel redactions (solid opaque fill, pixelate, or blur) onto an existing
// image file using AnnotationDocument + renderAnnotatedCrop.
[[nodiscard]] AnnotateResult applyProgrammaticAnnotations(const AnnotateRequest &request);

} // namespace itsova
