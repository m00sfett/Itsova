#include "recordingfinalizer.h"
#include "recordingplan.h"
#include "targets.h"

#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace glimply;

class RecordingTest : public QObject
{
    Q_OBJECT

private:
    static double probeDuration(const QString &path)
    {
        QProcess probe;
        probe.start(QStringLiteral("ffprobe"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                                                QStringLiteral("format=duration"), QStringLiteral("-of"),
                                                QStringLiteral("csv=p=0"), path});
        probe.waitForFinished(30000);
        return QString::fromUtf8(probe.readAllStandardOutput()).trimmed().toDouble();
    }

private Q_SLOTS:
    // #98: the first three recordings say how to stop; the tooltip always does.
    void stopHints()
    {
        QVERIFY(shouldShowRecordingStopHint(0));
        QVERIFY(shouldShowRecordingStopHint(2));
        QVERIFY(!shouldShowRecordingStopHint(3));
        QVERIFY(!shouldShowRecordingStopHint(10));
        QCOMPARE(recordingStopHint(QString()), QStringLiteral("Click to stop"));
        QCOMPARE(recordingStopHint(QStringLiteral("Meta+R")), QStringLiteral("Click or press Meta+R to stop"));
    }

    void formatsRoundTrip()
    {
        for (const RecordingFormat format : allRecordingFormats()) {
            QCOMPARE(recordingFormatFromKey(recordingFormatKey(format)), format);
        }
        QCOMPARE(recordingFormatFromKey(QStringLiteral("GIF")), RecordingFormat::Gif);
        QCOMPARE(recordingFormatFromKey(QStringLiteral("avi")), RecordingFormat::Mp4);
        QCOMPARE(recordingExtension(RecordingFormat::WebM), QStringLiteral("webm"));
    }

    void gifKeepsItsFrameRateLow()
    {
        QCOMPARE(effectiveFrameRate(RecordingFormat::Mp4, 60), 60);
        QCOMPARE(effectiveFrameRate(RecordingFormat::Gif, 60), maxGifFrameRate);
        QCOMPARE(effectiveFrameRate(RecordingFormat::WebM, 1000), 60);
        QCOMPARE(effectiveFrameRate(RecordingFormat::Mp4, 0), 15);
    }

    void regionsUseTheHighestScaleAndStayOnScreen()
    {
        const QList<QRect> screens{QRect(0, 0, 3072, 1728), QRect(3072, 0, 1920, 1080)};
        const QList<qreal> scales{1.25, 1.0};
        QCOMPARE(regionScale(QRect(3100, 10, 100, 100), screens, scales), 1.0);
        QCOMPARE(regionScale(QRect(3000, 10, 200, 100), screens, scales), 1.25); // spans both
        QCOMPARE(regionScale(QRect(9000, 0, 10, 10), screens, scales), 1.0);
        QCOMPARE(clipRegionToScreens(QRect(-50, -50, 100, 100), screens), QRect(0, 0, 50, 50));
        QVERIFY(clipRegionToScreens(QRect(9000, 0, 10, 10), screens).isEmpty());
    }

    void encodedSizesAreEven()
    {
        // 101 × 1.0 is odd: one logical pixel less. 3 × 1.5 = 4.5 rounds to 5: odd as well.
        QCOMPARE(evenSizedRegion(QRect(0, 0, 101, 100), 1.0), QRect(0, 0, 100, 100));
        QCOMPARE(evenSizedRegion(QRect(10, 10, 100, 100), 1.25), QRect(10, 10, 99, 99)); // 125 → 124
        QCOMPARE(evenSizedRegion(QRect(0, 0, 200, 100), 1.0), QRect(0, 0, 200, 100));
    }

    void elapsedTimeReadsLikeAClock()
    {
        QCOMPARE(formatElapsed(0), QStringLiteral("0:00"));
        QCOMPARE(formatElapsed(7999), QStringLiteral("0:07"));
        QCOMPARE(formatElapsed(754000), QStringLiteral("12:34"));
        QCOMPARE(formatElapsed(3723000), QStringLiteral("1:02:03"));
        QCOMPARE(formatElapsed(-5), QStringLiteral("0:00"));
    }

    void clockLeavesOutPauses()
    {
        RecordingClock clock;
        QCOMPARE(clock.elapsed(500), 0);
        clock.start(1000);
        QCOMPARE(clock.elapsed(3000), 2000);
        clock.pause(3000);
        QCOMPARE(clock.elapsed(9000), 2000);
        clock.resume(9000);
        QCOMPARE(clock.elapsed(10000), 3000);
        QVERIFY(!clock.paused());
        QCOMPARE(clock.pauses().size(), 1);
        QCOMPARE(clock.pauses().first(), (QPair<qint64, qint64>(2000, 8000)));
    }

    void fileNamesFollowTheTemplate()
    {
        const QDateTime ts(QDate(2026, 11, 25), QTime(14, 5, 6));
        QCOMPARE(recordingFileName(QStringLiteral("{date}_{time}_{area}.png"), QStringLiteral("DP-1"), ts,
                                   RecordingFormat::Mp4),
                 QStringLiteral("2026-11-25_14-05-06_DP-1.mp4"));
        QCOMPARE(recordingFileName(QStringLiteral("{area}.{ext}"), QStringLiteral("window-konsole"), ts,
                                   RecordingFormat::Gif),
                 QStringLiteral("window-konsole.gif"));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QCOMPARE(uniqueRecordingPath(dir.path(), QStringLiteral("a.mp4")), dir.filePath(QStringLiteral("a.mp4")));
        QFile taken(dir.filePath(QStringLiteral("a.mp4")));
        QVERIFY(taken.open(QIODevice::WriteOnly));
        taken.close();
        QCOMPARE(uniqueRecordingPath(dir.path(), QStringLiteral("a.mp4")), dir.filePath(QStringLiteral("a-2.mp4")));
        QVERIFY(isRecordingFile(QStringLiteral("/x/y.MP4")));
        QVERIFY(isRecordingFile(QStringLiteral("y.gif")));
        QVERIFY(!isRecordingFile(QStringLiteral("y.png")));
    }

    void recordingTargetIdsRoundTrip()
    {
        const QRect rect(-10, 20, 300, 400);
        QCOMPARE(regionFromRecordingTargetId(regionRecordingTargetId(rect)), std::optional<QRect>(rect));
        QVERIFY(!regionFromRecordingTargetId(QStringLiteral("region:1,2,0,4")));
        QVERIFY(!regionFromRecordingTargetId(QStringLiteral("region:a,b,c,d")));
        QVERIFY(!regionFromRecordingTargetId(QStringLiteral("screen:DP-1")));
        const QString window = windowRecordingTargetId(QStringLiteral("{1234}"));
        QVERIFY(isWindowRecordingTargetId(window));
        QCOMPARE(windowHandleFromRecordingTargetId(window), QStringLiteral("{1234}"));
        QVERIFY(!isWindowRecordingTargetId(QStringLiteral("window:")));
    }

    void tailFixHoldsTheLastFrame()
    {
        // Frames until 6.9 s, the recording ran 10 s: the last frame shows for 3.1 s.
        const QString csv = QStringLiteral("0.000000,0.033333\n3.000000,0.100000\n6.900000,0.033333\n");
        const std::optional<TailFix> fix = planTailFix(csv, 10.0);
        QVERIFY(fix);
        QCOMPARE(fix->lastPacket, 2);
        QVERIFY(qAbs(fix->extraSeconds - 3.1) < 1e-9);
        // Ends with the recording already, or nothing readable: leave the file alone.
        QVERIFY(!planTailFix(csv, 6.95));
        QVERIFY(!planTailFix(QString(), 10.0));
        QVERIFY(!planTailFix(QStringLiteral("N/A,N/A\n"), 10.0));
        QVERIFY(!planTailFix(QStringLiteral("0.0,0.1\n0.4,0.1\n0.2,0.1\n"), 10.0)); // B-frames
        const QStringList args = tailFixArguments(QStringLiteral("in.gif"), QStringLiteral("out.gif"), *fix);
        QVERIFY(args.contains(QStringLiteral("copy")));
        QVERIFY(args.join(QLatin1Char(' ')).contains(QStringLiteral("eq(N\\,2)\\,3.100000/TB_OUT")));
    }

    void pausesAreCutOut()
    {
        // 0–3 s recorded, paused 3–7 s (no frames), recorded until 9 s: 5 s of recording.
        const QString csv = QStringLiteral("0.0,0.1\n2.9,0.1\n7.1,0.1\n8.9,0.1\n");
        const std::optional<TailFix> fix = planTailFix(csv, 5.0, {{3.0, 7.0}});
        QVERIFY(fix);
        QCOMPARE(fix->pauses.size(), 1);
        QCOMPARE(fix->extraSeconds, 0.0); // 8.9 − 4 = 4.9, the last frame reaches 5.0
        const QString filter = tailFixArguments(QStringLiteral("a.mp4"), QStringLiteral("b.mp4"), *fix).at(10);
        QVERIFY2(filter.contains(QStringLiteral("pts=PTS-(gte(PTS*TB_OUT\\,5.000000)*4.000000)/TB_OUT")),
                 qPrintable(filter));
        QVERIFY(filter.contains(QStringLiteral(":dts=DTS-(")));
        QVERIFY(!filter.contains(QStringLiteral("duration=")));
        // A pause at the very end (stopped while paused) has no frames after it.
        QVERIFY(!planTailFix(QStringLiteral("0.0,0.1\n2.9,0.1\n"), 3.0, {{3.0, 6.0}}));
    }

    void finalizerCutsARealPause()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()
            || QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty()) {
            QSKIP("ffmpeg/ffprobe not installed");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // 4 s of frames with nothing between 1 s and 3 s, like a paused recording.
        const QString path = dir.filePath(QStringLiteral("paused.mp4"));
        QProcess make;
        make.start(QStringLiteral("ffmpeg"),
                   {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                    QStringLiteral("-i"), QStringLiteral("testsrc=size=64x48:rate=10:duration=4"), QStringLiteral("-vf"),
                    QStringLiteral("select='not(between(t\\,1.05\\,2.95))'"), QStringLiteral("-fps_mode"),
                    QStringLiteral("passthrough"), QStringLiteral("-c:v"), QStringLiteral("libx264"),
                    QStringLiteral("-bf"), QStringLiteral("0"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), path});
        QVERIFY(make.waitForFinished(60000));
        if (make.exitCode() != 0) {
            QSKIP("this ffmpeg cannot write the test clip");
        }
        QVERIFY(probeDuration(path) > 3.5);
        RecordingFinalizer finalizer;
        QSignalSpy done(&finalizer, &RecordingFinalizer::done);
        finalizer.finalize(path, 2000, {{1.0, 3.0}});
        QVERIFY(done.wait(60000));
        QVERIFY2(done.first().at(0).toBool(), qPrintable(done.first().at(1).toString()));
        QVERIFY2(qAbs(probeDuration(path) - 2.0) < 0.15, qPrintable(QString::number(probeDuration(path))));
    }

    void finalizerExtendsARealFile_data()
    {
        QTest::addColumn<QString>("extension");
        QTest::addColumn<QStringList>("codec");
        QTest::newRow("mp4") << QStringLiteral("mp4") << QStringList{QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-bf"), QStringLiteral("0"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p")};
        QTest::newRow("webm") << QStringLiteral("webm") << QStringList{QStringLiteral("-c:v"), QStringLiteral("libvpx-vp9"), QStringLiteral("-deadline"), QStringLiteral("realtime")};
        QTest::newRow("gif") << QStringLiteral("gif") << QStringList{};
    }

    void finalizerExtendsARealFile()
    {
        QFETCH(QString, extension);
        QFETCH(QStringList, codec);
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()
            || QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty()) {
            QSKIP("ffmpeg/ffprobe not installed");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("clip.") + extension);
        QProcess make;
        make.start(QStringLiteral("ffmpeg"), QStringList{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"),
                                                         QStringLiteral("lavfi"), QStringLiteral("-i"),
                                                         QStringLiteral("testsrc=size=64x48:rate=10:duration=1")}
                                                 + codec + QStringList{path});
        QVERIFY(make.waitForFinished(60000));
        if (make.exitCode() != 0) {
            QSKIP("this ffmpeg cannot write the test clip");
        }
        QVERIFY(probeDuration(path) < 1.5);

        RecordingFinalizer finalizer;
        QSignalSpy done(&finalizer, &RecordingFinalizer::done);
        finalizer.finalize(path, 3000);
        QVERIFY(done.wait(60000));
        QVERIFY2(done.first().at(0).toBool(), qPrintable(done.first().at(1).toString()));
        QVERIFY2(qAbs(probeDuration(path) - 3.0) < 0.15, qPrintable(QString::number(probeDuration(path))));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral(".clip.part.") + extension)));

        // A second pass has nothing left to do.
        QSignalSpy again(&finalizer, &RecordingFinalizer::done);
        finalizer.finalize(path, 3000);
        QVERIFY(again.wait(60000));
        QCOMPARE(again.first().at(0).toBool(), false);
    }
};

QTEST_GUILESS_MAIN(RecordingTest)
#include "recordingtest.moc"
