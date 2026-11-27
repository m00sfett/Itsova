#include "i18n.h"

#include <KConfig>
#include <KConfigGroup>

#include <QLocale>
#include <QTemporaryDir>
#include <QTest>

using namespace itsova;

class I18nTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void supportedLanguages()
    {
        const QStringList &languages = translatedUiLanguages();
        QCOMPARE(languages.size(), 18);
        QVERIFY(!languages.contains(QStringLiteral("en")));
        QVERIFY(languages.contains(QStringLiteral("zh_CN")));
        QVERIFY(languages.contains(QStringLiteral("zh_TW")));
    }

    void matchLanguage_data()
    {
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("expected");
        QTest::newRow("plain") << QStringLiteral("de") << QStringLiteral("de");
        QTest::newRow("bcp47 region") << QStringLiteral("de-AT") << QStringLiteral("de");
        QTest::newRow("posix encoding") << QStringLiteral("fr_FR.UTF-8") << QStringLiteral("fr");
        QTest::newRow("modifier") << QStringLiteral("nl_BE@euro") << QStringLiteral("nl");
        QTest::newRow("brazil") << QStringLiteral("pt_BR") << QStringLiteral("pt");
        QTest::newRow("english variant") << QStringLiteral("en_GB") << QStringLiteral("en");
        QTest::newRow("upper case") << QStringLiteral("JA_jp") << QStringLiteral("ja");
        QTest::newRow("zh default") << QStringLiteral("zh") << QStringLiteral("zh_CN");
        QTest::newRow("zh cn") << QStringLiteral("zh_CN") << QStringLiteral("zh_CN");
        QTest::newRow("zh tw") << QStringLiteral("zh-TW") << QStringLiteral("zh_TW");
        QTest::newRow("zh hk") << QStringLiteral("zh_HK.UTF-8") << QStringLiteral("zh_TW");
        QTest::newRow("zh hant") << QStringLiteral("zh-Hant-SG") << QStringLiteral("zh_TW");
        QTest::newRow("zh hans") << QStringLiteral("zh-Hans-HK") << QStringLiteral("zh_CN");
        QTest::newRow("legacy indonesian") << QStringLiteral("in_ID") << QStringLiteral("id");
        QTest::newRow("unsupported") << QStringLiteral("sv_SE") << QString();
        QTest::newRow("posix C") << QStringLiteral("C") << QString();
        QTest::newRow("empty") << QString() << QString();
    }

    void matchLanguage()
    {
        QFETCH(QString, code);
        QFETCH(QString, expected);
        QCOMPARE(matchUiLanguage(code), expected);
    }

    void resolveLanguage()
    {
        // The setting wins when it is supported.
        QCOMPARE(resolveUiLanguage(QStringLiteral("ja"), {QStringLiteral("de_DE")}), QStringLiteral("ja"));
        // An unknown setting falls back to the system.
        QCOMPARE(resolveUiLanguage(QStringLiteral("xx"), {QStringLiteral("de_DE")}), QStringLiteral("de"));
        // First supported system language in order.
        QCOMPARE(resolveUiLanguage(QString(), {QStringLiteral("sv"), QStringLiteral("pl"), QStringLiteral("de")}),
                 QStringLiteral("pl"));
        // English in the list is a match, not a miss.
        QCOMPARE(resolveUiLanguage(QString(), {QStringLiteral("en_US"), QStringLiteral("de")}), QStringLiteral("en"));
        // Nothing supported: English.
        QCOMPARE(resolveUiLanguage(QString(), {QStringLiteral("sv_SE")}), QStringLiteral("en"));
        QCOMPARE(resolveUiLanguage(QString(), {}), QStringLiteral("en"));
    }

    void environment()
    {
        QCOMPARE(uiLanguagesFromEnvironment(QStringLiteral("de:en"), QStringLiteral("fr_FR.UTF-8"), {}, {}),
                 (QStringList{QStringLiteral("de"), QStringLiteral("en")}));
        QCOMPARE(uiLanguagesFromEnvironment({}, QStringLiteral("fr_FR.UTF-8"), QStringLiteral("it_IT"), {}),
                 QStringList{QStringLiteral("fr_FR.UTF-8")});
        QCOMPARE(uiLanguagesFromEnvironment({}, {}, QStringLiteral("it_IT"), QStringLiteral("es_ES")),
                 QStringList{QStringLiteral("it_IT")});
        QCOMPARE(uiLanguagesFromEnvironment(QStringLiteral(":"), {}, {}, QStringLiteral("es_ES")),
                 QStringList{QStringLiteral("es_ES")});
        QVERIFY(uiLanguagesFromEnvironment({}, {}, {}, {}).isEmpty());
    }

    void choices()
    {
        const QList<UiLanguageChoice> &list = uiLanguageChoices();
        QCOMPARE(list.size(), translatedUiLanguages().size() + 1);
        for (int i = 1; i < list.size(); ++i) {
            QVERIFY2(list.at(i - 1).englishName < list.at(i).englishName, qPrintable(list.at(i).englishName));
        }
        for (const QString &code : translatedUiLanguages()) {
            QVERIFY2(uiLanguageNativeName(code) != code, qPrintable(code));
        }
        QCOMPARE(uiLanguageNativeName(QStringLiteral("en")), QStringLiteral("English"));
        QCOMPARE(uiLanguageNativeName(QStringLiteral("xx")), QStringLiteral("xx"));
        QCOMPARE(activeUiLanguage(), QStringLiteral("en"));
        setActiveUiLanguage(QStringLiteral("de"));
        QCOMPARE(activeUiLanguage(), QStringLiteral("de"));
    }

    void configured()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        KConfig config(dir.filePath(QStringLiteral("itsovarc")), KConfig::SimpleConfig);
        QCOMPARE(configuredUiLanguage(config), QString());
        config.group(QStringLiteral("General")).writeEntry("Language", QStringLiteral(" zh_TW "));
        QCOMPARE(configuredUiLanguage(config), QStringLiteral("zh_TW"));
    }

    void rtlDirection()
    {
        QCOMPARE(QLocale(QStringLiteral("ar")).textDirection(), Qt::RightToLeft);
        QCOMPARE(QLocale(QStringLiteral("en")).textDirection(), Qt::LeftToRight);
        for (const QString &code : translatedUiLanguages()) {
            const Qt::LayoutDirection expected =
                code == QLatin1String("ar") ? Qt::RightToLeft : Qt::LeftToRight;
            QCOMPARE(QLocale(code).textDirection(), expected);
        }
    }
};

QTEST_GUILESS_MAIN(I18nTest)
#include "i18ntest.moc"
