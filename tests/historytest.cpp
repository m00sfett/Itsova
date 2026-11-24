#include "historystore.h"
#include "thumbnailcache.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

using namespace glimply;

namespace
{

HistoryEntry makeEntry(const QStringList &files, const QString &kind = QStringLiteral("manual"))
{
    HistoryEntry entry;
    entry.targetId = QStringLiteral("area:1");
    entry.targetName = QStringLiteral("Work Area");
    entry.triggerKind = kind;
    entry.files = files;
    return entry;
}

QString writeImage(const QString &path, const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(Qt::red);
    image.save(path, "PNG");
    return path;
}

} // namespace

class HistoryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void recordsNewestFirst();
    void ignoresIntervalAndEmptyEntries();
    void trimsToLimit();
    void limitIsClamped();
    void rerecordedFileMovesToTop();
    void pruneDropsMissingFilesAndEmptyEntries();
    void removeFileAndEntry();
    void persistsRoundTripWithOwnerOnlyMode();
    void corruptFileIsReportedAndIgnored();
    void jsonRoundTripForDBus();
    void thumbnailIsGeneratedScaledAndSwept();
};

void HistoryTest::recordsNewestFirst()
{
    HistoryStore store(QString(), 20);
    QVERIFY(store.record(makeEntry({QStringLiteral("/a.png")})));
    QVERIFY(store.record(makeEntry({QStringLiteral("/b.png"), QStringLiteral("/c.png")})));
    QCOMPARE(store.entries().size(), 2);
    QCOMPARE(store.entries().first().files, QStringList({QStringLiteral("/b.png"), QStringLiteral("/c.png")}));
    QVERIFY(!store.entries().first().id.isEmpty());
    QVERIFY(store.entries().first().timestamp.isValid());
    QVERIFY(store.containsFile(QStringLiteral("/a.png")));
    QVERIFY(!store.containsFile(QStringLiteral("/d.png")));
}

void HistoryTest::ignoresIntervalAndEmptyEntries()
{
    HistoryStore store(QString(), 20);
    QVERIFY(!store.record(makeEntry({QStringLiteral("/a.png")}, QStringLiteral("interval"))));
    QVERIFY(!store.record(makeEntry({})));
    QVERIFY(!store.record(makeEntry({QString()})));
    QVERIFY(store.entries().isEmpty());
}

void HistoryTest::trimsToLimit()
{
    HistoryStore store(QString(), 5);
    for (int i = 0; i < 8; ++i) {
        store.record(makeEntry({QStringLiteral("/%1.png").arg(i)}));
    }
    QCOMPARE(store.entries().size(), 5);
    QCOMPARE(store.entries().first().files.first(), QStringLiteral("/7.png"));
    QCOMPARE(store.entries().last().files.first(), QStringLiteral("/3.png"));
}

void HistoryTest::limitIsClamped()
{
    HistoryStore store(QString(), 1);
    QCOMPARE(store.limit(), HistoryStore::MinLimit);
    QVERIFY(store.setLimit(1000));
    QCOMPARE(store.limit(), HistoryStore::MaxLimit);
    for (int i = 0; i < 10; ++i) {
        store.record(makeEntry({QStringLiteral("/%1.png").arg(i)}));
    }
    QVERIFY(store.setLimit(6));
    QCOMPARE(store.entries().size(), 6);
    QVERIFY(!store.setLimit(6));
}

void HistoryTest::rerecordedFileMovesToTop()
{
    HistoryStore store(QString(), 20);
    store.record(makeEntry({QStringLiteral("/a.png")}));
    store.record(makeEntry({QStringLiteral("/b.png")}));
    store.record(makeEntry({QStringLiteral("/a.png")}, QStringLiteral("editor")));
    QCOMPARE(store.entries().size(), 2);
    QCOMPARE(store.entries().first().files.first(), QStringLiteral("/a.png"));
    QCOMPARE(store.entries().first().triggerKind, QStringLiteral("editor"));
}

void HistoryTest::pruneDropsMissingFilesAndEmptyEntries()
{
    HistoryStore store(QString(), 20);
    store.record(makeEntry({QStringLiteral("/gone.png")}));
    store.record(makeEntry({QStringLiteral("/keep.png"), QStringLiteral("/gone2.png")}));
    const QSet<QString> existing{QStringLiteral("/keep.png")};
    QVERIFY(store.prune([&existing](const QString &path) { return existing.contains(path); }));
    QCOMPARE(store.entries().size(), 1);
    QCOMPARE(store.entries().first().files, QStringList({QStringLiteral("/keep.png")}));
    QVERIFY(!store.prune([&existing](const QString &path) { return existing.contains(path); }));
}

void HistoryTest::removeFileAndEntry()
{
    HistoryStore store(QString(), 20);
    store.record(makeEntry({QStringLiteral("/a.png"), QStringLiteral("/b.png")}));
    const QString id = store.entries().first().id;
    QVERIFY(store.removeFile(QStringLiteral("/a.png")));
    QCOMPARE(store.entries().first().files, QStringList({QStringLiteral("/b.png")}));
    QVERIFY(!store.removeFile(QStringLiteral("/a.png")));
    QVERIFY(store.entry(id).has_value());
    QVERIFY(store.removeEntry(id));
    QVERIFY(store.entries().isEmpty());
    QVERIFY(!store.removeEntry(id));
}

void HistoryTest::persistsRoundTripWithOwnerOnlyMode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("state/glimply/history.json"));
    HistoryStore store(path, 20);
    store.record(makeEntry({QStringLiteral("/a.png")}));
    store.record(makeEntry({QStringLiteral("/b.png"), QStringLiteral("/c.png")}, QStringLiteral("cli")));
    QString error;
    QVERIFY2(store.save(&error), qPrintable(error));
    QCOMPARE(QFileInfo(path).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
             QFileDevice::Permissions());

    HistoryStore loaded(path, 20);
    QVERIFY(loaded.load(&error));
    QCOMPARE(loaded.entries().size(), 2);
    QCOMPARE(loaded.entries().first().triggerKind, QStringLiteral("cli"));
    QCOMPARE(loaded.entries().first().files.size(), 2);
    QCOMPARE(loaded.entries().first().id, store.entries().first().id);
    QCOMPARE(loaded.entries().first().timestamp, store.entries().first().timestamp);
    QCOMPARE(loaded.entries().first().targetName, QStringLiteral("Work Area"));

    HistoryStore small(path, 5);
    QVERIFY(small.load());
    QCOMPARE(small.entries().size(), 2);

    HistoryStore missing(dir.filePath(QStringLiteral("nothing.json")), 20);
    QVERIFY(missing.load());
    QVERIFY(missing.entries().isEmpty());
}

void HistoryTest::corruptFileIsReportedAndIgnored()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("history.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{ not json");
    file.close();
    HistoryStore store(path, 20);
    QString error;
    QVERIFY(!store.load(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(store.entries().isEmpty());
}

void HistoryTest::jsonRoundTripForDBus()
{
    HistoryStore store(QString(), 20);
    store.record(makeEntry({QStringLiteral("/x/one.png"), QStringLiteral("/x/two.png")}));
    const QJsonArray json = store.toJson([](const QString &path) { return path + QStringLiteral(".thumb"); });
    QCOMPARE(json.size(), 1);
    const QJsonObject first = json.first().toObject();
    QCOMPARE(first.value(QStringLiteral("targetName")).toString(), QStringLiteral("Work Area"));
    const QJsonObject file = first.value(QStringLiteral("files")).toArray().first().toObject();
    QCOMPARE(file.value(QStringLiteral("name")).toString(), QStringLiteral("one.png"));
    QCOMPARE(file.value(QStringLiteral("thumbnail")).toString(), QStringLiteral("/x/one.png.thumb"));

    const QList<HistoryEntry> parsed = HistoryStore::fromJson(json);
    QCOMPARE(parsed.size(), 1);
    QCOMPARE(parsed.first().files, store.entries().first().files);
    QCOMPARE(parsed.first().timestamp, store.entries().first().timestamp);
}

void HistoryTest::thumbnailIsGeneratedScaledAndSwept()
{
    QTemporaryDir dir;
    const QString big = writeImage(dir.filePath(QStringLiteral("big.png")), QSize(1920, 1080));
    const QString small = writeImage(dir.filePath(QStringLiteral("small.png")), QSize(40, 30));
    ThumbnailCache cache(dir.filePath(QStringLiteral("thumbs")));

    QVERIFY(cache.existingThumbnail(big).isEmpty());
    const QString thumb = cache.generate(big);
    QVERIFY(!thumb.isEmpty());
    QCOMPARE(cache.existingThumbnail(big), thumb);
    const QImage scaled(thumb);
    QCOMPARE(scaled.width(), ThumbnailCache::MaxEdge);
    QCOMPARE(scaled.height(), 144);
    QCOMPARE(QImage(cache.generate(small)).size(), QSize(40, 30));
    QVERIFY(cache.generate(dir.filePath(QStringLiteral("missing.png"))).isEmpty());

    cache.sweep({small});
    QVERIFY(!QFileInfo::exists(thumb));
    QVERIFY(!cache.existingThumbnail(small).isEmpty());
}

QTEST_GUILESS_MAIN(HistoryTest)
#include "historytest.moc"
