#include "i18n.h"

#include <KConfig>
#include <KConfigGroup>

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace glimply
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
    const KConfig config(QStringLiteral("glimplyrc"), KConfig::SimpleConfig);
    return configuredUiLanguage(config);
}

QStringList translationDirectories()
{
    QStringList dirs;
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        dirs << appDir << QDir::cleanPath(appDir + QStringLiteral("/../share/glimply/translations"));
    }
    for (const QString &dataDir : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        const QString dir = dataDir + QStringLiteral("/glimply/translations");
        if (!dirs.contains(dir)) {
            dirs << dir;
        }
    }
    return dirs;
}

} // namespace glimply
