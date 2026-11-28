#include "recordingfinalizer.h"
#include "recordingplan.h"
#include "recordingmetadata.h"
#include "targets.h"

#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <cmath>
#include <numbers>

using namespace itsova;

class RecordingTest : public QObject
{
    Q_OBJECT

private:
    static QByteArray convertMedia(const QString &tool, const QStringList &args)
    {
        QProcess process;
        process.start(tool, QStringList{QStringLiteral("-v"), QStringLiteral("error")} + args);
        if (!process.waitForFinished(30000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            qWarning().noquote() << process.readAllStandardError().trimmed();
            return {};
        }
        return process.readAllStandardOutput();
    }

    static QByteArray decodedAudio(const QString &path, int index)
    {
        return convertMedia(QStringLiteral("ffmpeg"),
                            {QStringLiteral("-i"), path, QStringLiteral("-map"),
                             QStringLiteral("0:a:%1").arg(index), QStringLiteral("-ac"), QStringLiteral("1"),
                             QStringLiteral("-ar"), QString::number(recordingAudioRate), QStringLiteral("-f"),
                             QStringLiteral("s16le"), QStringLiteral("-")});
    }

    static double toneStrength(const QByteArray &pcm, double start, int hz)
    {
        const int first = qRound(start * recordingAudioRate);
        const int count = recordingAudioRate / 20;
        if ((first + count) * 2 > pcm.size()) {
            return 0;
        }
        double sine = 0;
        double cosine = 0;
        for (int i = 0; i < count; ++i) {
            const double phase = i * 2 * std::numbers::pi * hz / recordingAudioRate;
            const qint16 sample = qFromLittleEndian<qint16>(pcm.constData() + (first + i) * 2);
            sine += sample * std::sin(phase);
            cosine += sample * std::cos(phase);
        }
        return 2 * std::hypot(sine, cosine) / count;
    }

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
    void recordingAudioSelection()
    {
        using Source = RecordingAudioSource;
        RecordingAudioSelection audio;
        QVERIFY(audio.includes(Source::None));
        QVERIFY(audio.sources().isEmpty());

        audio.toggle(Source::Microphone);
        audio.toggle(Source::System);
        QCOMPARE(audio.sources(), (QList{Source::System, Source::Microphone}));
        QVERIFY(!audio.includes(Source::None));
        QVERIFY(audio.includes(Source::System));
        QVERIFY(audio.includes(Source::Microphone));

        audio.toggle(Source::Application);
        QCOMPARE(audio.sources(), (QList{Source::Application, Source::Microphone}));
        QVERIFY(!audio.includes(Source::System));
        RecordingTarget target;
        QCOMPARE(target.availableAudio(audio).sources(), QList{Source::Microphone});
        target.kind = RecordingTarget::Kind::Window;
        QCOMPARE(target.availableAudio(audio).sources(), QList{Source::Microphone});
        target.windowPid = 1024;
        QCOMPARE(target.availableAudio(audio).sources(), audio.sources());

        audio.toggle(Source::Application);
        QCOMPARE(audio.sources(), QList{Source::Microphone});
        audio.toggle(Source::System);
        audio.toggle(Source::Microphone);
        QCOMPARE(audio.sources(), QList{Source::System});
        audio.toggle(Source::Microphone);
        audio.toggle(Source::None);
        QVERIFY(audio.includes(Source::None));
        QVERIFY(audio.sources().isEmpty());
    }

    // #98: the tooltip always says how to stop.
    void stopHints()
    {
        QCOMPARE(recordingStopHint(QString()), QStringLiteral("Click to stop"));
        QCOMPARE(recordingStopHint(QStringLiteral("Meta+R")), QStringLiteral("Click or press Meta+R to stop"));
    }

    // #125: the hint never lands in the recorded picture; only a recording that covers everything
    // waits for it.
    void hintPlacement()
    {
        const QSize hint(360, 56);
        const QRect left(0, 0, 1920, 1080);
        const QRect right(1920, 0, 2560, 1440);
        const QList<QRect> two{left, right};

        // A monitor that is not recorded takes it.
        RecordingHintPlacement p = planRecordingHint(false, right, two, hint);
        QCOMPARE(p.screen, 0);
        QVERIFY(left.contains(p.rect));
        QVERIFY(!p.rect.intersects(right));
        QVERIFY(!p.waitBeforeRecording);

        // All monitors: nowhere free, so it waits in the middle of the largest covered monitor.
        p = planRecordingHint(false, left.united(right), two, hint);
        QCOMPARE(p.screen, 1);
        QVERIFY(p.waitBeforeRecording);
        QVERIFY((p.rect.center() - right.center()).manhattanLength() <= 2);

        // The only monitor, recorded whole: waits.
        p = planRecordingHint(false, left, {left}, hint);
        QVERIFY(p.waitBeforeRecording);

        // A region on the only monitor: beside it, in the largest free strip.
        const QRect region(100, 100, 800, 600);
        p = planRecordingHint(false, region, {left}, hint);
        QCOMPARE(p.screen, 0);
        QVERIFY(!p.waitBeforeRecording);
        QVERIFY(left.contains(p.rect));
        QVERIFY(!p.rect.intersects(region));
        QVERIFY(p.rect.left() > region.right()); // the strip right of the region is the largest

        // Too little room beside the region: waits.
        p = planRecordingHint(false, left.adjusted(20, 20, -20, -20), {left}, hint);
        QVERIFY(p.waitBeforeRecording);

        // A window: on its monitor, never waits.
        p = planRecordingHint(true, QRect(2000, 100, 800, 600), two, hint);
        QCOMPARE(p.screen, 1);
        QVERIFY(right.contains(p.rect));
        QVERIFY(!p.waitBeforeRecording);

        QCOMPARE(planRecordingHint(false, region, {}, hint).screen, -1);
    }

    // #103: monitor, position and scale travel with the recording.
    void metadataRoundTrip()
    {
        RecordingMetadata meta;
        meta.kind = QStringLiteral("area");
        meta.targetId = QStringLiteral("selection");
        meta.streamScale = 1.25;
        meta.region = QRect(1800, 0, 400, 300);
        RecordingMonitorInfo monitor;
        monitor.connector = QStringLiteral("DP-1");
        monitor.manufacturer = QStringLiteral("Dell");
        monitor.model = QStringLiteral("U2720Q");
        monitor.serial = QStringLiteral("ABC123");
        monitor.logical = QRect(0, 0, 1920, 1080);
        monitor.video = QRect(0, 0, 150, 375);
        monitor.scale = 1.25;
        monitor.resolution = QSize(2400, 1350);
        meta.monitors = {monitor};
        const std::optional<RecordingMetadata> back = parseRecordingMetadata(recordingMetadataJson(meta));
        QVERIFY(back);
        QCOMPARE(back->kind, meta.kind);
        QCOMPARE(back->targetId, meta.targetId);
        QCOMPARE(back->streamScale, 1.25);
        QCOMPARE(back->region, meta.region);
        QCOMPARE(back->monitors.size(), 1);
        QCOMPARE(back->monitors.first().serial, QStringLiteral("ABC123"));
        QCOMPARE(back->monitors.first().video, QRect(0, 0, 150, 375));
        QCOMPARE(back->monitors.first().resolution, QSize(2400, 1350));
        QVERIFY(!parseRecordingMetadata(QStringLiteral("{\"version\":99}")));
        QVERIFY(!parseRecordingMetadata(QStringLiteral("not json")));
    }

    void readsLegacyRecordingTag()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()
            || QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty()) {
            QSKIP("ffmpeg/ffprobe not installed");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("legacy.mp4"));
        RecordingMetadata meta;
        meta.kind = QStringLiteral("monitor");
        meta.targetId = QStringLiteral("screen:DP-1");
        QProcess ffmpeg;
        ffmpeg.start(QStringLiteral("ffmpeg"),
                     {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"),
                      QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("color=size=16x16:d=0.1"),
                      QStringLiteral("-c:v"), QStringLiteral("mpeg4"), QStringLiteral("-movflags"),
                      QStringLiteral("+use_metadata_tags"), QStringLiteral("-metadata"),
                      QStringLiteral("glimply=") + recordingMetadataJson(meta), path});
        QVERIFY(ffmpeg.waitForFinished(10000));
        QCOMPARE(ffmpeg.exitCode(), 0);
        const auto restored = readRecordingMetadata(path);
        QVERIFY(restored);
        QCOMPARE(restored->targetId, meta.targetId);
    }

    void metadataPlacesEveryMonitorInTheVideo()
    {
        ScreenInfo left;
        left.connector = QStringLiteral("DP-1");
        left.geometry = QRect(0, 0, 1920, 1080);
        ScreenInfo right;
        right.connector = QStringLiteral("DP-2");
        right.geometry = QRect(1920, 0, 2048, 1152);
        right.serialNumber = QStringLiteral("S2");
        RecordingTarget target;
        target.targetId = QStringLiteral("selection");
        target.region = QRect(1800, 0, 400, 300);
        target.scale = 1.25;
        const RecordingMetadata meta = buildRecordingMetadata(target, {left, right}, {1.0, 1.25});
        QCOMPARE(meta.kind, QStringLiteral("area"));
        QCOMPARE(meta.monitors.size(), 2);
        QCOMPARE(meta.monitors.at(0).video, QRect(0, 0, 150, 375));
        QCOMPARE(meta.monitors.at(1).video, QRect(150, 0, 350, 375));
        QCOMPARE(meta.monitors.at(1).resolution, QSize(2560, 1440));
        QCOMPARE(meta.monitors.at(1).serial, QStringLiteral("S2"));

        RecordingTarget window;
        window.kind = RecordingTarget::Kind::Window;
        window.targetId = QStringLiteral("window:{x}");
        window.region = QRect(2000, 100, 800, 600);
        const RecordingMetadata windowMeta = buildRecordingMetadata(window, {left, right}, {1.0, 1.25});
        QCOMPARE(windowMeta.kind, QStringLiteral("window"));
        QCOMPARE(windowMeta.monitors.size(), 1);
        QCOMPARE(windowMeta.monitors.first().connector, QStringLiteral("DP-2"));
        QCOMPARE(windowMeta.windowRect, QRect(80, 100, 800, 600)); // on its monitor
        QCOMPARE(windowMeta.streamScale, 1.25);
    }

    void finalizerKeepsMetadata_data()
    {
        QTest::addColumn<QString>("extension");
        QTest::addColumn<QStringList>("codec");
        QTest::addColumn<qint64>("targetMs");
        QTest::newRow("mp4 extended") << QStringLiteral("mp4") << QStringList{QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-bf"), QStringLiteral("0"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p")} << qint64(3000);
        QTest::newRow("mp4 nothing to extend") << QStringLiteral("mp4") << QStringList{QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-bf"), QStringLiteral("0"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p")} << qint64(1000);
        QTest::newRow("webm") << QStringLiteral("webm") << QStringList{QStringLiteral("-c:v"), QStringLiteral("libvpx-vp9"), QStringLiteral("-deadline"), QStringLiteral("realtime")} << qint64(3000);
    }

    void finalizerKeepsMetadata()
    {
        QFETCH(QString, extension);
        QFETCH(QStringList, codec);
        QFETCH(qint64, targetMs);
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()
            || QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty()) {
            QSKIP("ffmpeg/ffprobe not installed");
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("meta.") + extension);
        QProcess make;
        make.start(QStringLiteral("ffmpeg"), QStringList{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"),
                                                         QStringLiteral("lavfi"), QStringLiteral("-i"),
                                                         QStringLiteral("testsrc=size=64x48:rate=10:duration=1")}
                                                 + codec + QStringList{path});
        QVERIFY(make.waitForFinished(60000));
        if (make.exitCode() != 0) {
            QSKIP("this ffmpeg cannot write the test clip");
        }
        RecordingMetadata meta;
        meta.kind = QStringLiteral("monitor");
        meta.targetId = QStringLiteral("screen:DP-1");
        meta.streamScale = 1.25;
        meta.region = QRect(0, 0, 1920, 1080);
        RecordingFinalizer finalizer;
        QSignalSpy done(&finalizer, &RecordingFinalizer::done);
        finalizer.finalize(path, targetMs, {}, recordingMetadataJson(meta));
        QVERIFY(done.wait(60000));
        const std::optional<RecordingMetadata> read = readRecordingMetadata(path);
        QVERIFY2(read, qPrintable(done.first().at(1).toString()));
        QCOMPARE(read->targetId, meta.targetId);
        QCOMPARE(read->streamScale, 1.25);
        QCOMPARE(read->region, meta.region);
    }

    void windowResizeLetterboxGraph()
    {
        // Constant or matching-aspect window size requires no letterboxing pass.
        QVERIFY(letterboxFilterGraph(QSize(640, 508), {{0.0, QSize(640, 508)}}).isEmpty());
        QVERIFY(letterboxFilterGraph(QSize(640, 480), {{0.0, QSize(640, 480)}, {1.0, QSize(1280, 960)}}).isEmpty());

        // 640x508 -> maximized 1920x1080 -> restored 640x508 produces 640x360 letterboxed with 74px bars.
        const QString graph = letterboxFilterGraph(
            QSize(640, 508), {{0.0, QSize(640, 508)}, {2.0, QSize(1920, 1080)}, {4.5, QSize(640, 508)}});
        QVERIFY(!graph.isEmpty());
        QVERIFY2(graph.contains(QStringLiteral("scale=w='640':h='if(lt(t,2.000),508,if(lt(t,4.500),360,508))'")),
                 qPrintable(graph));
        QVERIFY2(graph.contains(QStringLiteral("pad=w=640:h=508:x='(ow-iw)/2':y='(oh-ih)/2':color=black:eval=frame")),
                 qPrintable(graph));
        QVERIFY(!graph.contains(QStringLiteral("setpts")));

        // Wide canvas -> narrow window produces pillarboxed output.
        const QString pillar = letterboxFilterGraph(QSize(1280, 720), {{0.0, QSize(1280, 720)}, {1.0, QSize(400, 800)}});
        QVERIFY2(pillar.contains(QStringLiteral("scale=w='if(lt(t,1.000),1280,360)':h='720'")), qPrintable(pillar));
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

    void finalizerKeepsAudioInSync_data()
    {
        QTest::addColumn<QString>("extension");
        QTest::addColumn<bool>("resize");
        QTest::newRow("mp4") << QStringLiteral("mp4") << false;
        QTest::newRow("letterboxed mp4") << QStringLiteral("mp4") << true;
        QTest::newRow("letterboxed webm") << QStringLiteral("webm") << true;
    }

    void finalizerKeepsAudioInSync()
    {
        QFETCH(QString, extension);
        QFETCH(bool, resize);
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()
            || QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty()) {
            QSKIP("ffmpeg/ffprobe not installed");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("sparse.") + extension);
        const QString desktop = dir.filePath(QStringLiteral("desktop.pcm"));
        const QString mic = dir.filePath(QStringLiteral("mic.pcm"));
        const bool mp4 = extension == QLatin1String("mp4");
        const QStringList videoCodec = mp4
            ? QStringList{QStringLiteral("libx264"), QStringLiteral("-bf"), QStringLiteral("0"),
                          QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p")}
            : QStringList{QStringLiteral("libvpx-vp9")};
        QProcess make;
        make.start(QStringLiteral("ffmpeg"),
                   QStringList{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"),
                               QStringLiteral("lavfi"), QStringLiteral("-i"),
                               QStringLiteral("color=red:size=64x48:rate=10:duration=5"),
                               QStringLiteral("-vf"), QStringLiteral("select='eq(n,0)+eq(n,4)+eq(n,21)+eq(n,29)+eq(n,41)+eq(n,43)'"),
                               QStringLiteral("-fps_mode"), QStringLiteral("passthrough"), QStringLiteral("-c:v")}
                       + videoCodec + QStringList{path});
        QVERIFY(make.waitForFinished(30000));
        if (make.exitCode() != 0) {
            QSKIP("this ffmpeg cannot write the test clip");
        }
        // The 1 kHz bursts are outside pauses. The 2 kHz bursts occur only while paused.
        const QString wave = QStringLiteral(
            "(between(t,0.2,0.4)+between(t,2.2,2.4)+between(t,4.2,4.4))*0.3*sin(2*PI*1000*t)"
            "+(between(t,1,2)+between(t,3,4)+between(t,5,6.2))*0.3*sin(2*PI*2000*t)");
        const auto makePcm = [](const QString &source, const QString &destination) {
            QProcess audio;
            audio.start(QStringLiteral("ffmpeg"),
                        {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                         QStringLiteral("-i"), source, QStringLiteral("-ac"), QStringLiteral("2"),
                         QStringLiteral("-f"), QStringLiteral("s16le"), destination});
            return audio.waitForFinished(30000) && audio.exitCode() == 0;
        };
        QVERIFY(makePcm(QStringLiteral("aevalsrc='%1':s=48000:d=6.2").arg(wave), desktop));
        QVERIFY(makePcm(QStringLiteral("sine=f=400:r=48000:d=4.7"), mic));

        RecordingFinalizer finalizer;
        QSignalSpy done(&finalizer, &RecordingFinalizer::done);
        const QList<WindowResizeSample> sizes = resize
            ? QList<WindowResizeSample>{{0, QSize(64, 48)}, {0.9, QSize(64, 32)}, {2.5, QSize(64, 48)}}
            : QList<WindowResizeSample>();
        RecordingMetadata meta;
        meta.targetId = QStringLiteral("window:fixture");
        finalizer.finalize(path, 3000, {{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.2}}, recordingMetadataJson(meta), sizes,
                           {{desktop, QStringLiteral("System audio"), 0},
                            {dir.filePath(QStringLiteral("missing.pcm")), QStringLiteral("Unavailable"), 0},
                            {mic, QStringLiteral("Microphone"), 1500000}});
        QVERIFY(done.wait(30000));
        QCOMPARE(done.size(), 1);
        QVERIFY2(done.first().at(0).toBool(), qPrintable(done.first().at(1).toString()));

        const QString tag = mp4 ? QStringLiteral("handler_name") : QStringLiteral("title");
        const QByteArray tracks = convertMedia(
            QStringLiteral("ffprobe"), {QStringLiteral("-select_streams"), QStringLiteral("a"),
                                        QStringLiteral("-show_entries"), QStringLiteral("stream_tags=") + tag,
                                        QStringLiteral("-of"), QStringLiteral("csv=p=0"), path});
        QCOMPARE(tracks.trimmed(), QByteArray("System audio\nMicrophone"));
        const auto metadata = readRecordingMetadata(path);
        QVERIFY(metadata);
        QCOMPARE(metadata->targetId, meta.targetId);

        const QStringList packets = QString::fromUtf8(
                                        convertMedia(QStringLiteral("ffprobe"), ffprobePacketArguments(path).mid(2)))
                                        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QList<double> editedPts{0.0, 0.4, 1.1, 1.9, 2.1, 2.3};
        QCOMPARE(packets.size(), editedPts.size());
        for (qsizetype i = 0; i < packets.size(); ++i) {
            const double pts = packets.at(i).section(QLatin1Char(','), 0, 0).toDouble();
            QVERIFY2(qAbs(pts - editedPts.at(i)) < 0.015, qPrintable(packets.at(i)));
        }
        QVERIFY2(qAbs(packets.last().section(QLatin1Char(','), 1, 1).toDouble() - 0.7) < 0.025,
                 qPrintable(packets.last()));
        if (resize) {
            const QByteArray pixels = convertMedia(
                QStringLiteral("ffmpeg"), {QStringLiteral("-i"), path, QStringLiteral("-map"), QStringLiteral("0:v"),
                                            QStringLiteral("-fps_mode"), QStringLiteral("passthrough"), QStringLiteral("-f"),
                                            QStringLiteral("rawvideo"), QStringLiteral("-pix_fmt"),
                                            QStringLiteral("rgb24"), QStringLiteral("-")});
            constexpr int frameBytes = 64 * 48 * 3;
            constexpr int center = (24 * 64 + 32) * 3;
            QCOMPARE(pixels.size(), editedPts.size() * frameBytes);
            QVERIFY(quint8(pixels.at(0)) > 200); // first frame fills the canvas
            QVERIFY(quint8(pixels.at(2 * frameBytes)) < 20); // first frame after resize has a bar
            QVERIFY(quint8(pixels.at(2 * frameBytes + center)) > 200);
        }

        const QByteArray systemAudio = decodedAudio(path, 0);
        const QByteArray micAudio = decodedAudio(path, 1);
        for (const QByteArray &pcm : {systemAudio, micAudio}) {
            QVERIFY(qAbs(double(pcm.size()) / (2 * recordingAudioRate) - 3.0) < 0.025);
        }
        for (int segment = 0; segment < 3; ++segment) {
            QVERIFY(toneStrength(systemAudio, segment + 0.25, 1000) > 2000);
            QVERIFY(toneStrength(systemAudio, segment + 0.05, 2000) < 150);
            QVERIFY(toneStrength(systemAudio, segment + 0.75, 2000) < 150);
        }
        QVERIFY(toneStrength(micAudio, 0.75, 400) < 150);
        QVERIFY(toneStrength(micAudio, 1.05, 400) > 1000);
        QVERIFY(toneStrength(micAudio, 2.9, 400) > 1000);
    }

    void finalizerKeepsTheVideoWithoutUsableAudio_data()
    {
        QTest::addColumn<QString>("extension");
        QTest::addColumn<QStringList>("codec");
        QTest::newRow("missing mp4 sidecar")
            << QStringLiteral("mp4")
            << QStringList{QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-bf"), QStringLiteral("0")};
        QTest::newRow("gif with audio") << QStringLiteral("gif") << QStringList{};
    }

    void finalizerKeepsTheVideoWithoutUsableAudio()
    {
        QFETCH(QString, extension);
        QFETCH(QStringList, codec);
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()
            || QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty()) {
            QSKIP("ffmpeg/ffprobe not installed");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("video.") + extension);
        QProcess make;
        make.start(QStringLiteral("ffmpeg"),
                   QStringList{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"),
                               QStringLiteral("lavfi"), QStringLiteral("-i"),
                               QStringLiteral("testsrc=size=64x48:rate=10:duration=1")}
                       + codec + QStringList{path});
        QVERIFY(make.waitForFinished(30000));
        if (make.exitCode() != 0) {
            QSKIP("this ffmpeg cannot write the test clip");
        }
        QFile pcm(dir.filePath(QStringLiteral("audio.pcm")));
        if (extension == QLatin1String("gif")) {
            QVERIFY(pcm.open(QIODevice::WriteOnly));
            const QByteArray samples(4800 * recordingAudioFrameBytes, 'a');
            QCOMPARE(pcm.write(samples), samples.size());
            pcm.close();
        }
        QFile video(path);
        QVERIFY(video.open(QIODevice::ReadOnly));
        const QByteArray original = video.readAll();
        video.close();
        QVERIFY(!original.isEmpty());

        RecordingFinalizer finalizer;
        QSignalSpy done(&finalizer, &RecordingFinalizer::done);
        finalizer.finalize(path, 1000, {}, {}, {}, {{pcm.fileName(), QStringLiteral("Microphone"), 0}});
        QVERIFY(done.wait(30000));
        QVERIFY(video.open(QIODevice::ReadOnly));
        QCOMPARE(video.readAll(), original);
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
