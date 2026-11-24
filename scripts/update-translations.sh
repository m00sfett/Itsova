#!/usr/bin/env bash
# Refreshes the translation sources from the code (#51): the application catalogs
# i18n/glimply_<lang>.ts through lupdate, and the Plasma widget catalogs plasmoid/po through
# xgettext and msgmerge. Existing translations are kept; new strings arrive untranslated.
# Usage: scripts/update-translations.sh [build-dir]   (default: build)
set -euo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${source_dir}/build}"
languages=(ar de es fr hi id it ja ko nl pl pt ru tr uk vi zh_CN zh_TW)
domain="plasma_applet_io.github.m00sfett.glimply.widget"
po_dir="${source_dir}/plasmoid/po"
template="${po_dir}/${domain}.pot"

# Application: CMake knows the sources of every target, lupdate reads them.
if [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
    cmake -S "${source_dir}" -B "${build_dir}"
fi
cmake --build "${build_dir}" --target update_translations

# Plasma widget: KI18n keywords as used by KDE's own extraction scripts.
version="$(sed -n 's/^project(glimply VERSION \([0-9.]*\).*/\1/p' "${source_dir}/CMakeLists.txt")"
mkdir -p "${po_dir}"
mapfile -t qml_files < <(cd "${source_dir}" && find plasmoid/contents -name '*.qml' -o -name '*.js' | sort)
(cd "${source_dir}" && xgettext --from-code=UTF-8 -C --kde \
    -ci18n -ki18n:1 -ki18nc:1c,2 -ki18np:1,2 -ki18ncp:1c,2,3 \
    -kki18n:1 -kki18nc:1c,2 -kki18np:1,2 -kki18ncp:1c,2,3 \
    -kxi18n:1 -kxi18nc:1c,2 -kxi18np:1,2 -kxi18ncp:1c,2,3 \
    --package-name=glimply --package-version="${version}" \
    --sort-by-file --no-wrap -o "${template}" "${qml_files[@]}")
# The creation date changes on every run and carries no information.
sed -i '/^"POT-Creation-Date:/d' "${template}"

# gettext plural rules per language (msginit does not know all of them).
plural_forms() {
    case "$1" in
        ar) echo 'nplurals=6; plural=(n==0 ? 0 : n==1 ? 1 : n==2 ? 2 : n%100>=3 && n%100<=10 ? 3 : n%100>=11 ? 4 : 5);' ;;
        de|es|hi|it|nl|tr) echo 'nplurals=2; plural=(n != 1);' ;;
        fr|pt) echo 'nplurals=2; plural=(n > 1);' ;;
        id|ja|ko|vi|zh_CN|zh_TW) echo 'nplurals=1; plural=0;' ;;
        pl) echo 'nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);' ;;
        ru|uk) echo 'nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);' ;;
        *) echo "Unknown language $1" >&2; return 1 ;;
    esac
}

for lang in "${languages[@]}"; do
    po="${po_dir}/${lang}/${domain}.po"
    mkdir -p "$(dirname "${po}")"
    if [[ -f "${po}" ]]; then
        msgmerge --quiet --update --backup=none --no-fuzzy-matching --no-wrap "${po}" "${template}"
        msgattrib --no-obsolete --no-wrap -o "${po}" "${po}"
    else
        msginit --no-translator --no-wrap --locale="${lang}.UTF-8" -i "${template}" -o "${po}"
        # msginit fills Language with the full locale; KDE expects the catalog name.
        sed -i "s/^\"Language: .*\\\\n\"$/\"Language: ${lang}\\\\n\"/" "${po}"
    fi
    sed -i "s/^\"Project-Id-Version: .*\\\\n\"$/\"Project-Id-Version: glimply ${version}\\\\n\"/" "${po}"
    forms="$(plural_forms "${lang}")"
    forms="${forms//&/\\&}" # & means "the match" in a sed replacement
    sed -i "s/^\"Plural-Forms: .*\\\\n\"$/\"Plural-Forms: ${forms}\\\\n\"/" "${po}"
done
echo "Translation sources updated. Check them with scripts/check-translations.py."
