#pragma once

#include <QDateTime>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;

namespace itsova
{

struct ParsedHookCommand
{
    QString program;
    QStringList arguments;
    QString error;
};

struct HookInvocation
{
    QString command;
    int timeoutSeconds = 30;
    int timeoutMsOverride = 0; // if > 0, overrides timeoutSeconds (used by tests)
    QStringList files;
    QString targetId;
    QString targetName;
    QString triggerKind; // trigger, overlay, print, cli, interval, editor, mcp, test
    QDateTime timestamp;
    QString format;
    bool isTest = false;
};

struct HookResult
{
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;
    QString errorString;
    QString stdOut;
    QString stdErr;
    qint64 elapsedMs = 0;
    QString targetName;
    bool isTest = false;
};

// Splits rawCommand with QProcess::splitCommand and expands %f directly into argv entries.
// Never invokes a shell.
[[nodiscard]] ParsedHookCommand buildHookCommand(const QString &rawCommand, const QStringList &files);

// Executes post-capture commands asynchronously via QProcess without shell interpolation.
class HookRunner : public QObject
{
    Q_OBJECT

public:
    static constexpr int maxConcurrentHooks = 4;
    static constexpr qsizetype maxCapturedOutputBytes = 16384;

    explicit HookRunner(QObject *parent = nullptr);
    ~HookRunner() override;

    void run(const HookInvocation &invocation, std::function<void(const HookResult &)> onDone = {});
    void runTest(const QString &command, int timeoutSeconds, std::function<void(const HookResult &)> onDone);

    [[nodiscard]] int activeCount() const;
    [[nodiscard]] int queuedCount() const;

Q_SIGNALS:
    void hookFailed(const itsova::HookResult &result);

private:
    struct PendingJob
    {
        HookInvocation invocation;
        std::function<void(const HookResult &)> onDone;
    };

    void startJob(PendingJob job);
    void drainQueue();

    QQueue<PendingJob> queue_;
    QSet<QProcess *> active_;
};

} // namespace itsova
