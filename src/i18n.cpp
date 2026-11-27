#include "i18n.h"

#include <KConfig>
#include <KConfigGroup>

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

#include <algorithm>

namespace itsova
{

const QStringList &translatedUiLanguages()
{
    static const QStringList languages{
        QStringLiteral("ar"), QStringLiteral("de"), QStringLiteral("es"), QStringLiteral("fr"),
        QStringLiteral("hi"), QStringLiteral("id"), QStringLiteral("it"), QStringLiteral("ja"),
        QStringLiteral("ko"), QStringLiteral("nl"), QStringLiteral("pl"), QStringLiteral("pt"),
        QStringLiteral("ru"), QStringLiteral("tr"), QStringLiteral("uk"), QStringLiteral("vi"),
        QStringLiteral("zh_CN"), QStringLiteral("zh_TW"),
    };
    return languages;
}

QString matchUiLanguage(const QString &code)
{
    QString normalized = code.trimmed();
    // POSIX locales may carry an encoding or modifier: "de_DE.UTF-8", "sr_RS@latin".
    normalized = normalized.section(QLatin1Char('.'), 0, 0).section(QLatin1Char('@'), 0, 0);
    normalized.replace(QLatin1Char('-'), QLatin1Char('_'));
    const QStringList parts = normalized.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return {};
    }
    const QString language = parts.first().toLower();
    if (language == QLatin1String(sourceUiLanguage)) {
        return QString::fromLatin1(sourceUiLanguage);
    }
    if (language == QLatin1String("zh")) {
        // Script first ("zh_Hant_HK"), then region; Simplified is the default.
        for (const QString &part : parts.mid(1)) {
            const QString lower = part.toLower();
            if (lower == QLatin1String("hant") || lower == QLatin1String("tw") || lower == QLatin1String("hk")
                || lower == QLatin1String("mo")) {
                return QStringLiteral("zh_TW");
            }
            if (lower == QLatin1String("hans") || lower == QLatin1String("cn") || lower == QLatin1String("sg")) {
                return QStringLiteral("zh_CN");
            }
        }
        return QStringLiteral("zh_CN");
    }
    if (language == QLatin1String("in")) {
        return QStringLiteral("id"); // legacy code for Indonesian
    }
    if (translatedUiLanguages().contains(language)) {
        return language;
    }
    // A stored value may already be a supported code with a region, e.g. "zh_CN".
    if (translatedUiLanguages().contains(normalized)) {
        return normalized;
    }
    return {};
}

QString resolveUiLanguage(const QString &configured, const QStringList &systemUiLanguages)
{
    if (!configured.trimmed().isEmpty()) {
        const QString match = matchUiLanguage(configured);
        if (!match.isEmpty()) {
            return match;
        }
    }
    for (const QString &code : systemUiLanguages) {
        const QString match = matchUiLanguage(code);
        if (!match.isEmpty()) {
            return match;
        }
    }
    return QString::fromLatin1(sourceUiLanguage);
}

QStringList uiLanguagesFromEnvironment(const QString &language, const QString &lcAll, const QString &lcMessages,
                                       const QString &lang)
{
    QStringList result;
    for (const QString &entry : language.split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
        result << entry.trimmed();
    }
    if (!result.isEmpty()) {
        return result;
    }
    for (const QString &value : {lcAll, lcMessages, lang}) {
        if (!value.trimmed().isEmpty()) {
            return {value.trimmed()};
        }
    }
    return {};
}

QStringList systemUiLanguages()
{
    // Read the environment directly: QLocale::system() caches its data on first use, and the
    // language must be decided before the application object exists.
    return uiLanguagesFromEnvironment(qEnvironmentVariable("LANGUAGE"), qEnvironmentVariable("LC_ALL"),
                                      qEnvironmentVariable("LC_MESSAGES"), qEnvironmentVariable("LANG"));
}

QString configuredUiLanguage(const KConfig &config)
{
    return config.group(QStringLiteral("General")).readEntry("Language", QString()).trimmed();
}

QString configuredUiLanguage()
{
    const KConfig config(QStringLiteral("itsovarc"), KConfig::SimpleConfig);
    return configuredUiLanguage(config);
}

const QList<UiLanguageChoice> &uiLanguageChoices()
{
    static const QList<UiLanguageChoice> choices = [] {
        QList<UiLanguageChoice> list{
            {QStringLiteral("ar"), QStringLiteral("العربية"), QStringLiteral("Arabic")},
            {QStringLiteral("zh_CN"), QStringLiteral("中文（简体）"), QStringLiteral("Chinese (Simplified)")},
            {QStringLiteral("zh_TW"), QStringLiteral("中文（繁體）"), QStringLiteral("Chinese (Traditional)")},
            {QStringLiteral("nl"), QStringLiteral("Nederlands"), QStringLiteral("Dutch")},
            {QStringLiteral("en"), QStringLiteral("English"), QStringLiteral("English")},
            {QStringLiteral("fr"), QStringLiteral("Français"), QStringLiteral("French")},
            {QStringLiteral("de"), QStringLiteral("Deutsch"), QStringLiteral("German")},
            {QStringLiteral("hi"), QStringLiteral("हिन्दी"), QStringLiteral("Hindi")},
            {QStringLiteral("id"), QStringLiteral("Bahasa Indonesia"), QStringLiteral("Indonesian")},
            {QStringLiteral("it"), QStringLiteral("Italiano"), QStringLiteral("Italian")},
            {QStringLiteral("ja"), QStringLiteral("日本語"), QStringLiteral("Japanese")},
            {QStringLiteral("ko"), QStringLiteral("한국어"), QStringLiteral("Korean")},
            {QStringLiteral("pl"), QStringLiteral("Polski"), QStringLiteral("Polish")},
            {QStringLiteral("pt"), QStringLiteral("Português (Brasil)"), QStringLiteral("Portuguese")},
            {QStringLiteral("ru"), QStringLiteral("Русский"), QStringLiteral("Russian")},
            {QStringLiteral("es"), QStringLiteral("Español"), QStringLiteral("Spanish")},
            {QStringLiteral("tr"), QStringLiteral("Türkçe"), QStringLiteral("Turkish")},
            {QStringLiteral("uk"), QStringLiteral("Українська"), QStringLiteral("Ukrainian")},
            {QStringLiteral("vi"), QStringLiteral("Tiếng Việt"), QStringLiteral("Vietnamese")},
        };
        std::sort(list.begin(), list.end(), [](const UiLanguageChoice &a, const UiLanguageChoice &b) {
            return a.englishName < b.englishName;
        });
        return list;
    }();
    return choices;
}

QString uiLanguageNativeName(const QString &code)
{
    for (const UiLanguageChoice &choice : uiLanguageChoices()) {
        if (choice.code == code) {
            return choice.nativeName;
        }
    }
    return code;
}

namespace
{
QString &activeLanguageStorage()
{
    static QString language = QString::fromLatin1(sourceUiLanguage);
    return language;
}
} // namespace

void setActiveUiLanguage(const QString &code)
{
    activeLanguageStorage() = code;
}

QString activeUiLanguage()
{
    return activeLanguageStorage();
}

QStringList translationDirectories()
{
    QStringList dirs;
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        dirs << appDir << QDir::cleanPath(appDir + QStringLiteral("/../share/itsova/translations"));
    }
    for (const QString &dataDir : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        const QString dir = dataDir + QStringLiteral("/itsova/translations");
        if (!dirs.contains(dir)) {
            dirs << dir;
        }
    }
    return dirs;
}

} // namespace itsova
