#pragma once

#include <QList>
#include <QString>
#include <QStringList>

class KConfig;

namespace glimply
{

// English is the source language and needs no translation file.
inline constexpr char sourceUiLanguage[] = "en";

// The UI languages glimply ships besides English (#51), as translation file suffixes.
[[nodiscard]] const QStringList &translatedUiLanguages();

// Maps a locale code such as "de-DE", "pt_BR", "zh-Hant-HK" or "en_GB.UTF-8" to "en" or one of
// translatedUiLanguages(); empty when glimply has no matching translation.
[[nodiscard]] QString matchUiLanguage(const QString &code);

// The language the UI uses: the configured one when supported, else the first supported entry of
// the system's UI languages (QLocale::uiLanguages() order), else English.
[[nodiscard]] QString resolveUiLanguage(const QString &configured, const QStringList &systemUiLanguages);

// The system's UI languages as gettext and Qt read them: LANGUAGE (colon-separated) when set,
// else the first of LC_ALL, LC_MESSAGES and LANG.
[[nodiscard]] QStringList uiLanguagesFromEnvironment(const QString &language, const QString &lcAll,
                                                     const QString &lcMessages, const QString &lang);
[[nodiscard]] QStringList systemUiLanguages();

// [General] Language from glimplyrc; empty means "follow the system".
[[nodiscard]] QString configuredUiLanguage(const KConfig &config);
[[nodiscard]] QString configuredUiLanguage();

// A UI language as offered in the settings: its code, its own name in its own script, and its
// English name (the sort key, so the order does not change with the UI language).
struct UiLanguageChoice
{
    QString code;
    QString nativeName;
    QString englishName;
};
// English and every translated language, sorted by English name.
[[nodiscard]] const QList<UiLanguageChoice> &uiLanguageChoices();
// The native name for a code ("de" -> "Deutsch"); the code itself when unknown.
[[nodiscard]] QString uiLanguageNativeName(const QString &code);

// The language this process runs in, decided once at startup.
void setActiveUiLanguage(const QString &code);
[[nodiscard]] QString activeUiLanguage();

// Directories searched for glimply_<lang>.qm, most specific first: next to the executable (build
// tree), the install prefix relative to the executable, then the XDG data directories.
[[nodiscard]] QStringList translationDirectories();

} // namespace glimply
