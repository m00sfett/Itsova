#include "hookrunner.h"

#include "imageoutput.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>

namespace glimply
{
namespace
{

class HookText
{
    Q_DECLARE_TR_FUNCTIONS(HookRunner)
};

void appendBounded(QByteArray &target, const QByteArray &chunk, qsizetype limit)
{
    if (chunk.isEmpty() || target.size() >= limit) {
        return;
    }
    const qsizetype remaining = limit - target.size();
    target.append(chunk.constData(), static_cast<int>(std::min(chunk.size(), remaining)));
}

} // namespace

ParsedHookCommand buildHookCommand(const QString &rawCommand, const QStringList &files)
{
    ParsedHookCommand parsed;
    const QString trimmed = rawCommand.trimmed();
    if (trimmed.isEmpty()) {
        parsed.error = HookText::tr("Hook command is empty.");
        return parsed;
    }
    const QStringList tokens = QProcess::splitCommand(trimmed);
    if (tokens.isEmpty() || tokens.first().isEmpty()) {
        parsed.error = HookText::tr("Could not parse hook command.");
        return parsed;
    }
    parsed.program = tokens.first();
    for (int i = 1; i < tokens.size(); ++i) {
        const QString &token = tokens.at(i);
        if (token == QLatin1String("%f")) {
            for (const QString &file : files) {
                parsed.arguments.append(file);
            }
        } else if (token.contains(QLatin1String("%f"))) {
            if (!files.isEmpty()) {
                QString copy = token;
                copy.replace(QLatin1String("%f"), files.first());
                parsed.arguments.append(copy);
            }
        } else {
            parsed.arguments.append(token);
        }
    }
    return parsed;
}

HookRunner::HookRunner(QObject *parent)
    : QObject(parent)
{
}

HookRunner::~HookRunner()
{
    queue_.clear();
    const QList<QProcess *> procs = active_.values();
    for (QProcess *proc : procs) {
        proc->disconnect();
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
            proc->waitForFinished(500);
        }
        proc->deleteLater();
    }
    active_.clear();
}

int HookRunner::activeCount() const
{
    return active_.size();
}

int HookRunner::queuedCount() const
{
    return queue_.size();
}

void HookRunner::run(const HookInvocation &invocation, std::function<void(const HookResult &)> onDone)
{
    PendingJob job{invocation, std::move(onDone)};
    if (active_.size() >= maxConcurrentHooks) {
        queue_.enqueue(std::move(job));
        return;
    }
    startJob(std::move(job));
}

void HookRunner::runTest(const QString &command, int timeoutSeconds, std::function<void(const HookResult &)> onDone)
{
    auto tempDir = std::make_shared<QTemporaryDir>();
    if (!tempDir->isValid()) {
        HookResult fail;
        fail.isTest = true;
        fail.errorString = tr("Could not create a temporary directory for testing.");
        if (onDone) {
            onDone(fail);
        }
        return;
    }

    const QString testFile = QDir(tempDir->path()).filePath(QStringLiteral("glimply-hook-test.png"));
    QImage sample(1, 1, QImage::Format_RGBA8888);
    sample.fill(Qt::black);
    QString writeErr;
    if (!writeImageToFile(testFile, sample, ExportOptions{}, &writeErr)) {
        HookResult fail;
        fail.isTest = true;
        fail.errorString = writeErr;
        if (onDone) {
            onDone(fail);
        }
        return;
    }

    HookInvocation inv;
    inv.command = command;
    inv.timeoutSeconds = std::clamp(timeoutSeconds, 5, 300);
    inv.files = {testFile};
    inv.targetId = QStringLiteral("test");
    inv.targetName = QStringLiteral("Test Capture");
    inv.triggerKind = QStringLiteral("test");
    inv.timestamp = QDateTime::currentDateTime();
    inv.format = QStringLiteral("png");
    inv.isTest = true;

    run(inv, [tempDir, callback = std::move(onDone)](const HookResult &result) {
        Q_UNUSED(tempDir);
        if (callback) {
            callback(result);
        }
    });
}

void HookRunner::startJob(PendingJob job)
{
    const HookInvocation inv = job.invocation;
    const ParsedHookCommand parsed = buildHookCommand(inv.command, inv.files);
    if (!parsed.error.isEmpty()) {
        HookResult res;
        res.errorString = parsed.error;
        res.targetName = inv.targetName;
        res.isTest = inv.isTest;
        if (!inv.isTest) {
            Q_EMIT hookFailed(res);
        }
        if (job.onDone) {
            job.onDone(res);
        }
        return;
    }

    auto *process = new QProcess(this);
    active_.insert(process);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GLIMPLY_FILES"), inv.files.join(QLatin1Char('\n')));
    env.insert(QStringLiteral("GLIMPLY_FILE"), inv.files.isEmpty() ? QString() : inv.files.first());
    env.insert(QStringLiteral("GLIMPLY_FILE_COUNT"), QString::number(inv.files.size()));
    env.insert(QStringLiteral("GLIMPLY_TARGET_ID"), inv.targetId);
    env.insert(QStringLiteral("GLIMPLY_TARGET_NAME"), inv.targetName);
    env.insert(QStringLiteral("GLIMPLY_TRIGGER_KIND"), inv.triggerKind);
    env.insert(QStringLiteral("GLIMPLY_TIMESTAMP"),
               (inv.timestamp.isValid() ? inv.timestamp : QDateTime::currentDateTime()).toString(Qt::ISODate));
    env.insert(QStringLiteral("GLIMPLY_FORMAT"), inv.format);
    process->setProcessEnvironment(env);

    struct SharedState
    {
        QElapsedTimer elapsed;
        QByteArray outBuf;
        QByteArray errBuf;
        bool timedOut = false;
        bool started = false;
        bool completed = false;
    };
    auto state = std::make_shared<SharedState>();
    state->elapsed.start();

    auto *timeoutTimer = new QTimer(process);
    timeoutTimer->setSingleShot(true);
    const int timeoutMs = inv.timeoutMsOverride > 0 ? inv.timeoutMsOverride
                                                    : std::clamp(inv.timeoutSeconds, 5, 300) * 1000;
    timeoutTimer->setInterval(timeoutMs);

    connect(timeoutTimer, &QTimer::timeout, process, [process, state] {
        if (state->completed) {
            return;
        }
        state->timedOut = true;
        process->terminate();
        QTimer::singleShot(2000, process, [process] {
            if (process->state() != QProcess::NotRunning) {
                process->kill();
            }
        });
    });

    connect(process, &QProcess::started, process, [state] { state->started = true; });
    connect(process, &QProcess::readyReadStandardOutput, process, [process, state] {
        appendBounded(state->outBuf, process->readAllStandardOutput(), maxCapturedOutputBytes);
    });
    connect(process, &QProcess::readyReadStandardError, process, [process, state] {
        appendBounded(state->errBuf, process->readAllStandardError(), maxCapturedOutputBytes);
    });

    const auto finishJob = [this, process, timeoutTimer, state, inv, callback = std::move(job.onDone)](
                               int exitCode, const QString &errorString) {
        if (state->completed) {
            return;
        }
        state->completed = true;
        timeoutTimer->stop();
        appendBounded(state->outBuf, process->readAllStandardOutput(), maxCapturedOutputBytes);
        appendBounded(state->errBuf, process->readAllStandardError(), maxCapturedOutputBytes);

        active_.remove(process);
        process->deleteLater();

        HookResult res;
        res.started = state->started;
        res.timedOut = state->timedOut;
        res.exitCode = exitCode;
        res.errorString = errorString;
        res.stdOut = QString::fromUtf8(state->outBuf);
        res.stdErr = QString::fromUtf8(state->errBuf);
        res.elapsedMs = state->elapsed.elapsed();
        res.targetName = inv.targetName;
        res.isTest = inv.isTest;

        if (!inv.isTest && (!res.started || res.timedOut || res.exitCode != 0)) {
            Q_EMIT hookFailed(res);
        }
        if (callback) {
            callback(res);
        }
        drainQueue();
    };

    connect(process, &QProcess::errorOccurred, process,
            [process, state, finishJob](QProcess::ProcessError err) {
                if (err == QProcess::FailedToStart && !state->completed) {
                    finishJob(-1, process->errorString());
                }
            });

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), process,
            [process, state, finishJob](int exitCode, QProcess::ExitStatus exitStatus) {
                QString errStr;
                if (state->timedOut) {
                    errStr = HookText::tr("Command timed out.");
                } else if (exitStatus == QProcess::CrashExit) {
                    errStr = process->errorString();
                } else if (exitCode != 0) {
                    errStr = HookText::tr("Exited with status %1.").arg(exitCode);
                }
                finishJob(exitCode, errStr);
            });

    timeoutTimer->start();
    process->start(parsed.program, parsed.arguments);
}

void HookRunner::drainQueue()
{
    while (active_.size() < maxConcurrentHooks && !queue_.isEmpty()) {
        startJob(queue_.dequeue());
    }
}

} // namespace glimply
