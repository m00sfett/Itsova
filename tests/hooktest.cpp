#include "hookrunner.h"
#include "targets.h"

#include <KConfig>

#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace glimply;

class HookTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void buildHookCommandSafety();
    void overridePrecedenceAndPersistence();
    void realExecutionEnvVarsAndMetacharacters();
    void failureAndTimeoutDoNotBlockSubsequent();
    void runTestCreatesAndCleansTempPng();
};

void HookTest::buildHookCommandSafety()
{
    const QString tricky1 = QStringLiteral("/tmp/shot $(touch /tmp/pwned) 'a;b' | cat.png");
    const QString tricky2 = QStringLiteral("/tmp/another file \"quoted\".png");

    const ParsedHookCommand cmd =
        buildHookCommand(QStringLiteral("/usr/bin/python3 \"my script.py\" --flag %f --label=\"x y\""),
                         {tricky1, tricky2});

    QCOMPARE(cmd.error, QString());
    QCOMPARE(cmd.program, QStringLiteral("/usr/bin/python3"));
    QCOMPARE(cmd.arguments,
             (QStringList{QStringLiteral("my script.py"), QStringLiteral("--flag"), tricky1, tricky2,
                          QStringLiteral("--label=x y")}));

    const ParsedHookCommand empty = buildHookCommand(QStringLiteral("   "), {tricky1});
    QVERIFY(!empty.error.isEmpty());
}

void HookTest::overridePrecedenceAndPersistence()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const ScreenInfo screen{QStringLiteral("DP-1"), QRect(0, 0, 1920, 1080), {}, {}, {}};
    TargetStore store;
    store.hook = {true, QStringLiteral("/bin/global %f"), 45, false};

    const QString aDefaultId = store.addArea(screen, QRect(0, 0, 100, 100)).id;
    const QString aCustomId = store.addArea(screen, QRect(10, 10, 100, 100)).id;
    store.area(aCustomId)->hookOverride = {HookOverrideMode::Custom, QStringLiteral("/bin/area %f"), 15};
    const QString aDisabledId = store.addArea(screen, QRect(20, 20, 100, 100)).id;
    store.area(aDisabledId)->hookOverride = {HookOverrideMode::Disabled, {}, 30};

    const QString grpCustomId = store.addGroup(QStringLiteral("GCustom")).id;
    {
        Group *grpCustom = store.group(grpCustomId);
        grpCustom->targetIds = {aDefaultId, aCustomId, aDisabledId};
        grpCustom->hookOverride = {HookOverrideMode::Custom, QStringLiteral("/bin/group %f"), 25};
    }

    const QString grpDisabledId = store.addGroup(QStringLiteral("GDisabled")).id;
    {
        Group *grpDisabled = store.group(grpDisabledId);
        grpDisabled->targetIds = {aDefaultId};
        grpDisabled->hookOverride = {HookOverrideMode::Disabled, {}, 30};
    }

    // Global fallback.
    QCOMPARE(store.effectiveHookConfig(aDefaultId).command, QStringLiteral("/bin/global %f"));
    QVERIFY(store.effectiveHookConfig(aDefaultId).enabled);

    // Group overrides global when target uses global default.
    QCOMPARE(store.effectiveHookConfig(aDefaultId, grpCustomId).command, QStringLiteral("/bin/group %f"));
    QCOMPARE(store.effectiveHookConfig(aDefaultId, grpCustomId).timeoutSeconds, 25);

    // Target overrides group.
    QCOMPARE(store.effectiveHookConfig(aCustomId, grpCustomId).command, QStringLiteral("/bin/area %f"));
    QVERIFY(!store.effectiveHookConfig(aDisabledId, grpCustomId).enabled);

    // Disabled group blocks default target.
    QVERIFY(!store.effectiveHookConfig(aDefaultId, grpDisabledId).enabled);
    // Disabled interval kill-switch.
    QVERIFY(!store.effectiveHookConfig(aCustomId, grpCustomId, true).enabled);
    store.hook.runOnInterval = true;
    QVERIFY(store.effectiveHookConfig(aCustomId, grpCustomId, true).enabled);

    // Custom target works even if global hook is disabled.
    store.hook.enabled = false;
    QVERIFY(!store.effectiveHookConfig(aDefaultId).enabled);
    QVERIFY(store.effectiveHookConfig(aCustomId).enabled);

    store.setTargetHookOverride(screenTargetId(QStringLiteral("DP-1")),
                                {HookOverrideMode::Custom, QStringLiteral("/bin/mon %f"), 12});

    const QString ini = tempDir.filePath(QStringLiteral("glimplyrc"));
    {
        KConfig cfg(ini, KConfig::SimpleConfig);
        store.save(cfg);
    }
    TargetStore loaded;
    {
        KConfig cfg(ini, KConfig::SimpleConfig);
        loaded.load(cfg);
    }
    QCOMPARE(loaded.hook, store.hook);
    QCOMPARE(loaded.targetHookOverride(aCustomId), store.area(aCustomId)->hookOverride);
    QCOMPARE(loaded.targetHookOverride(aDisabledId), store.area(aDisabledId)->hookOverride);
    QCOMPARE(loaded.targetHookOverride(screenTargetId(QStringLiteral("DP-1"))),
             (HookOverride{HookOverrideMode::Custom, QStringLiteral("/bin/mon %f"), 12}));
    QCOMPARE(loaded.group(grpCustomId)->hookOverride, store.group(grpCustomId)->hookOverride);
}

void HookTest::realExecutionEnvVarsAndMetacharacters()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString scriptPath = tempDir.filePath(QStringLiteral("inspect.py"));
    const QString reportPath = tempDir.filePath(QStringLiteral("report.txt"));
    {
        QFile f(scriptPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(
            "import os, sys\n"
            "out = os.environ.get('REPORT_OUT')\n"
            "with open(out, 'w') as fp:\n"
            "    fp.write('ARGV=' + '|'.join(sys.argv[1:]) + '\\n')\n"
            "    for k in ['GLIMPLY_FILES','GLIMPLY_FILE','GLIMPLY_FILE_COUNT','GLIMPLY_TARGET_ID','GLIMPLY_TARGET_NAME','GLIMPLY_TRIGGER_KIND','GLIMPLY_TIMESTAMP','GLIMPLY_FORMAT']:\n"
            "        fp.write(f'{k}={os.environ.get(k, \"\")}\\n')\n");
        f.close();
    }

    qputenv("REPORT_OUT", reportPath.toUtf8());
    const QString dangerousName = tempDir.filePath(QStringLiteral("shot $(echo NO_SHELL) 'a;b'.png"));

    HookRunner runner;
    QSignalSpy failSpy(&runner, &HookRunner::hookFailed);
    bool doneCalled = false;
    HookResult result;

    HookInvocation inv;
    inv.command = QStringLiteral("/usr/bin/python3 \"%1\" %f").arg(scriptPath);
    inv.timeoutSeconds = 10;
    inv.files = {dangerousName};
    inv.targetId = QStringLiteral("area:abc");
    inv.targetName = QStringLiteral("Work Area");
    inv.triggerKind = QStringLiteral("trigger");
    inv.timestamp = QDateTime(QDate(2026, 11, 24), QTime(15, 45, 0));
    inv.format = QStringLiteral("webp");

    runner.run(inv, [&](const HookResult &r) {
        result = r;
        doneCalled = true;
    });

    QTRY_VERIFY_WITH_TIMEOUT(doneCalled, 5000);
    qunsetenv("REPORT_OUT");
    QCOMPARE(failSpy.count(), 0);
    QVERIFY(result.started);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(!result.timedOut);

    QFile rep(reportPath);
    QVERIFY(rep.open(QIODevice::ReadOnly));
    const QString report = QString::fromUtf8(rep.readAll());
    QVERIFY(report.contains(QStringLiteral("ARGV=%1").arg(dangerousName)));
    QVERIFY(report.contains(QStringLiteral("GLIMPLY_FILE=%1\n").arg(dangerousName)));
    QVERIFY(report.contains(QStringLiteral("GLIMPLY_FILE_COUNT=1\n")));
    QVERIFY(report.contains(QStringLiteral("GLIMPLY_TARGET_ID=area:abc\n")));
    QVERIFY(report.contains(QStringLiteral("GLIMPLY_TARGET_NAME=Work Area\n")));
    QVERIFY(report.contains(QStringLiteral("GLIMPLY_TRIGGER_KIND=trigger\n")));
    QVERIFY(report.contains(QStringLiteral("GLIMPLY_FORMAT=webp\n")));
}

void HookTest::failureAndTimeoutDoNotBlockSubsequent()
{
    HookRunner runner;
    QSignalSpy failSpy(&runner, &HookRunner::hookFailed);

    HookResult errRes;
    bool errDone = false;
    HookInvocation badInv;
    badInv.command = QStringLiteral("/usr/bin/python3 -c \"import sys; sys.stderr.write('boom-err\\n'); sys.exit(7)\"");
    badInv.files = {QStringLiteral("/tmp/a.png")};
    badInv.targetName = QStringLiteral("BrokenTarget");
    runner.run(badInv, [&](const HookResult &r) {
        errRes = r;
        errDone = true;
    });

    HookResult timeRes;
    bool timeDone = false;
    HookInvocation slowInv;
    slowInv.command = QStringLiteral("/usr/bin/python3 -c \"import time; time.sleep(10)\"");
    slowInv.timeoutMsOverride = 180;
    slowInv.files = {QStringLiteral("/tmp/b.png")};
    slowInv.targetName = QStringLiteral("SlowTarget");
    runner.run(slowInv, [&](const HookResult &r) {
        timeRes = r;
        timeDone = true;
    });

    HookResult okRes;
    bool okDone = false;
    HookInvocation okInv;
    okInv.command = QStringLiteral("/usr/bin/python3 -c \"print('ok-after-fail')\"");
    okInv.files = {QStringLiteral("/tmp/c.png")};
    runner.run(okInv, [&](const HookResult &r) {
        okRes = r;
        okDone = true;
    });

    QTRY_VERIFY_WITH_TIMEOUT(errDone && timeDone && okDone, 5000);
    QCOMPARE(errRes.exitCode, 7);
    QVERIFY(errRes.stdErr.contains(QStringLiteral("boom-err")));
    QVERIFY(timeRes.timedOut);
    QCOMPARE(okRes.exitCode, 0);
    QVERIFY(okRes.stdOut.contains(QStringLiteral("ok-after-fail")));
    QCOMPARE(failSpy.count(), 2);
}

void HookTest::runTestCreatesAndCleansTempPng()
{
    HookRunner runner;
    QSignalSpy failSpy(&runner, &HookRunner::hookFailed);

    QString capturedFilePath;
    HookResult testRes;
    bool done = false;

    const QString cmd = QStringLiteral(
        "/usr/bin/python3 -c \"import os, sys; "
        "assert os.environ['GLIMPLY_TRIGGER_KIND'] == 'test'; "
        "data = open(sys.argv[1], 'rb').read(8); "
        "assert data == bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]); "
        "print(sys.argv[1])\" %f");

    runner.runTest(cmd, 10, [&](const HookResult &r) {
        testRes = r;
        capturedFilePath = r.stdOut.trimmed();
        done = true;
    });

    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(failSpy.count(), 0); // Test failures/results never emit capture hookFailed notification.
    QCOMPARE(testRes.exitCode, 0);
    QVERIFY(testRes.isTest);
    QVERIFY(!capturedFilePath.isEmpty());
    // Wait until runTest's shared QTemporaryDir goes out of scope after callback finishes.
    QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(capturedFilePath), 1000);
}

QTEST_MAIN(HookTest)
#include "hooktest.moc"
