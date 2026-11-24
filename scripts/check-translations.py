#!/usr/bin/env python3
"""Checks glimply's translation catalogs (#51).

Application catalogs: i18n/glimply_<lang>.ts (Qt). Plasma widget catalogs:
plasmoid/po/<lang>/<domain>.po (gettext). English is the source language.

Errors (exit code 1):
  - a catalog is missing, or carries other messages than the rest (refresh with
    scripts/update-translations.sh)
  - an entry is untranslated (unless --allow-unfinished)
  - placeholders (%1, %n, %L1) differ from the source
  - the & accelerator count differs from the source, or accelerators that are unique in the
    source collide in the translation
  - a plural form is missing or empty
  - obsolete entries are left in a catalog
Warnings (reported, no failure):
  - translation longer than 150 % of the source
  - translation identical to the source, unless allowed in i18n/glossary.md

Usage: scripts/check-translations.py [--allow-unfinished] [--verbose] [--lang LANG ...]
Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LANGUAGES = ["ar", "de", "es", "fr", "hi", "id", "it", "ja", "ko", "nl", "pl", "pt", "ru", "tr",
             "uk", "vi", "zh_CN", "zh_TW"]
# Plural forms per language as Qt counts them (lupdate creates this many numerusform entries).
QT_PLURAL_FORMS = {"ar": 6, "de": 2, "es": 2, "fr": 2, "hi": 2, "id": 1, "it": 2, "ja": 1, "ko": 1,
                   "nl": 2, "pl": 3, "pt": 2, "ru": 3, "tr": 1, "uk": 3, "vi": 1, "zh_CN": 1,
                   "zh_TW": 1}
TS_DIR = ROOT / "i18n"
TS_BASE = "glimply"
PO_DIR = ROOT / "plasmoid" / "po"
PO_DOMAIN = "plasma_applet_io.github.m00sfett.glimply.widget"
GLOSSARY = TS_DIR / "glossary.md"

QT_PLACEHOLDER = re.compile(r"%L?\d+|%n")
PO_PLACEHOLDER = re.compile(r"%\d+")
LENGTH_LIMIT = 1.5
LENGTH_MIN_SOURCE = 4  # very short sources ("OK", "%1") vary too much to judge


@dataclass
class Message:
    context: str
    source: str
    comment: str = ""
    plural_source: str = ""  # gettext msgid_plural
    numerus: bool = False
    forms: list[str] = field(default_factory=list)
    unfinished: bool = False
    obsolete: bool = False

    @property
    def key(self) -> tuple[str, str, str]:
        return (self.context, self.source, self.comment)


@dataclass
class Report:
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    def error(self, where: str, text: str) -> None:
        self.errors.append(f"{where}: {text}")

    def warn(self, where: str, text: str) -> None:
        self.warnings.append(f"{where}: {text}")


def short(text: str, limit: int = 60) -> str:
    flat = text.replace("\n", "\\n")
    return flat if len(flat) <= limit else flat[: limit - 1] + "…"


def allowed_identical() -> set[str]:
    if not GLOSSARY.exists():
        return set()
    match = re.search(r"```allow-identical\n(.*?)```", GLOSSARY.read_text(encoding="utf-8"), re.S)
    if not match:
        return set()
    return {line.strip() for line in match.group(1).splitlines() if line.strip()}


# --- Qt .ts -------------------------------------------------------------------------------------

def read_ts(path: Path) -> list[Message]:
    root = ET.parse(path).getroot()
    messages: list[Message] = []
    for context in root.iter("context"):
        name = context.findtext("name") or ""
        for node in context.iter("message"):
            source = node.findtext("source") or ""
            comment = node.findtext("comment") or ""
            translation = node.find("translation")
            msg = Message(context=name, source=source, comment=comment,
                          numerus=node.get("numerus") == "yes")
            if translation is not None:
                kind = translation.get("type", "")
                msg.unfinished = kind == "unfinished"
                msg.obsolete = kind in ("obsolete", "vanished")
                if msg.numerus:
                    msg.forms = [form.text or "" for form in translation.iter("numerusform")]
                else:
                    msg.forms = [translation.text or ""]
            else:
                msg.unfinished = True
            messages.append(msg)
    return messages


# --- gettext .po --------------------------------------------------------------------------------

def unquote(text: str) -> str:
    text = text.strip()
    if text.startswith('"') and text.endswith('"'):
        text = text[1:-1]
    return (text.replace('\\"', '"').replace("\\n", "\n").replace("\\t", "\t")
            .replace("\\\\", "\\"))


def read_po(path: Path) -> tuple[list[Message], int]:
    """Returns the messages (header excluded) and nplurals from the header."""
    entries: list[dict[str, object]] = []
    current: dict[str, object] = {}
    last_key: str | None = None

    def flush() -> None:
        nonlocal current, last_key
        if "msgid" in current:
            entries.append(current)
        current = {}
        last_key = None

    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line:
            flush()
            continue
        if line.startswith("#~"):
            current["obsolete"] = True
            continue
        if line.startswith("#,"):
            current["flags"] = line[2:].strip()
            continue
        if line.startswith("#"):
            continue
        match = re.match(r'(msgctxt|msgid_plural|msgid|msgstr(?:\[\d+\])?)\s+(".*")$', line)
        if match:
            if match.group(1) in ("msgctxt", "msgid") and "msgid" in current and \
                    any(k.startswith("msgstr") for k in current):
                flush()
            last_key = match.group(1)
            current[last_key] = unquote(match.group(2))
            continue
        if line.startswith('"') and last_key:
            current[last_key] = str(current[last_key]) + unquote(line)
    flush()

    nplurals = 1
    messages: list[Message] = []
    for entry in entries:
        msgid = str(entry["msgid"])
        if msgid == "" and "msgctxt" not in entry:
            header = str(entry.get("msgstr", ""))
            found = re.search(r"nplurals\s*=\s*(\d+)", header)
            if found:
                nplurals = int(found.group(1))
            continue
        msg = Message(context=str(entry.get("msgctxt", "")), source=msgid,
                      plural_source=str(entry.get("msgid_plural", "")),
                      obsolete=bool(entry.get("obsolete")))
        msg.numerus = bool(msg.plural_source)
        if msg.numerus:
            indices = sorted(int(k[7:-1]) for k in entry if k.startswith("msgstr["))
            msg.forms = [str(entry[f"msgstr[{i}]"]) for i in indices]
        else:
            msg.forms = [str(entry.get("msgstr", ""))]
        msg.unfinished = "fuzzy" in str(entry.get("flags", "")) or not any(msg.forms) or \
            any(form == "" for form in msg.forms)
        messages.append(msg)
    return messages, nplurals


# --- checks -------------------------------------------------------------------------------------

def accelerators(text: str) -> list[str]:
    """Accelerator letters: a single & followed by a letter or digit (&& is a literal &)."""
    return [m.group(1).lower() for m in re.finditer(r"(?<!&)&([^\s&])", text.replace("&&", ""))]


def check_message(where: str, msg: Message, pattern: re.Pattern[str], nplurals: int | None,
                  allow_unfinished: bool, allowed: set[str], report: Report,
                  is_source_language: bool) -> None:
    label = f"{where} [{msg.context}] \"{short(msg.source)}\""
    if msg.obsolete:
        report.error(label, "obsolete entry left in the catalog")
        return
    if msg.unfinished or not msg.forms or any(not form.strip() for form in msg.forms):
        if not allow_unfinished:
            report.error(label, "untranslated")
        return
    if msg.numerus and nplurals is not None and len(msg.forms) != nplurals:
        report.error(label, f"{len(msg.forms)} plural forms, the language has {nplurals}")

    sources = [msg.source] + ([msg.plural_source] if msg.plural_source else [])
    # The count may be left out of a plural form ("1 file"): %n in Qt, %1 in gettext (KI18n).
    count = ({"%1"} if msg.plural_source else {"%n"}) if msg.numerus else set()
    expected = sorted(set(pattern.findall(" ".join(sources))) - count)
    for index, form in enumerate(msg.forms):
        found = sorted(set(pattern.findall(form)) - count)
        if found != expected:
            report.error(label, f"placeholders {found} in form {index}, source has {expected}")
        if msg.numerus and "%n" in msg.source and "%n" not in form and len(msg.forms) > 1:
            report.warn(label, f"plural form {index} does not show %n")
        if len(accelerators(form)) != len(accelerators(msg.source)):
            report.error(label, f"accelerator count differs in form {index}: \"{short(form)}\"")

    if is_source_language:
        return
    first = msg.forms[0]
    if len(msg.source) >= LENGTH_MIN_SOURCE and len(first) > LENGTH_LIMIT * len(msg.source):
        report.warn(label, f"{len(first) * 100 // len(msg.source)} % of the source length")
    if first == msg.source and msg.source not in allowed and re.search(r"[A-Za-z]{2}", msg.source):
        report.warn(label, "identical to the English source")


def check_accelerator_collisions(where: str, messages: list[Message], report: Report) -> None:
    by_context: dict[str, list[Message]] = defaultdict(list)
    for msg in messages:
        if accelerators(msg.source) and msg.forms and not msg.unfinished:
            by_context[msg.context].append(msg)
    for context, group in by_context.items():
        source_keys = [accelerators(m.source)[0] for m in group]
        if len(set(source_keys)) != len(source_keys):
            continue  # the source already shares letters here; nothing to compare against
        seen: dict[str, str] = {}
        for msg in group:
            letters = accelerators(msg.forms[0])
            if not letters:
                continue
            if letters[0] in seen:
                report.error(f"{where} [{context}]",
                             f"accelerator '{letters[0]}' used by \"{short(seen[letters[0]])}\" and "
                             f"\"{short(msg.forms[0])}\"")
            seen[letters[0]] = msg.forms[0]


def check_set(kind: str, catalogs: dict[str, list[Message]], report: Report) -> None:
    """Every catalog must carry the same messages."""
    union: set[tuple[str, str, str]] = set()
    for messages in catalogs.values():
        union |= {m.key for m in messages if not m.obsolete}
    for lang, messages in catalogs.items():
        missing = union - {m.key for m in messages}
        for context, source, _ in sorted(missing)[:10]:
            report.error(f"{kind} {lang}", f"missing [{context}] \"{short(source)}\"")
        if len(missing) > 10:
            report.error(f"{kind} {lang}", f"... and {len(missing) - 10} more missing messages")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--allow-unfinished", action="store_true",
                        help="do not report untranslated entries")
    parser.add_argument("--verbose", action="store_true", help="list every warning")
    parser.add_argument("--lang", action="append", choices=LANGUAGES,
                        help="check only these languages (repeatable)")
    args = parser.parse_args()
    languages = args.lang or LANGUAGES
    allowed = allowed_identical()
    report = Report()

    # Application catalogs.
    ts_catalogs: dict[str, list[Message]] = {}
    for lang in languages:
        path = TS_DIR / f"{TS_BASE}_{lang}.ts"
        if not path.exists():
            report.error(str(path.relative_to(ROOT)), "missing catalog")
            continue
        try:
            ts_catalogs[lang] = read_ts(path)
        except ET.ParseError as error:
            report.error(str(path.relative_to(ROOT)), f"not valid XML: {error}")
    if len(ts_catalogs) > 1:
        check_set("ts", ts_catalogs, report)
    for lang, messages in ts_catalogs.items():
        where = f"{TS_BASE}_{lang}.ts"
        for msg in messages:
            check_message(where, msg, QT_PLACEHOLDER, QT_PLURAL_FORMS[lang] if msg.numerus else None,
                          args.allow_unfinished, allowed, report,
                          is_source_language=False)
        check_accelerator_collisions(where, messages, report)
    # English plural forms.
    plurals = TS_DIR / f"{TS_BASE}_en.ts"
    if plurals.exists():
        for msg in read_ts(plurals):
            check_message(plurals.name, msg, QT_PLACEHOLDER, 2 if msg.numerus else None,
                          args.allow_unfinished, allowed, report, is_source_language=True)
    else:
        report.error(str(plurals.relative_to(ROOT)), "missing catalog (English plural forms)")

    # Plasma widget catalogs.
    template = PO_DIR / f"{PO_DOMAIN}.pot"
    po_catalogs: dict[str, list[Message]] = {}
    if not template.exists():
        report.error(str(template.relative_to(ROOT)), "missing template")
    else:
        po_catalogs["template"] = read_po(template)[0]
    for lang in languages:
        path = PO_DIR / lang / f"{PO_DOMAIN}.po"
        if not path.exists():
            report.error(str(path.relative_to(ROOT)), "missing catalog")
            continue
        messages, nplurals = read_po(path)
        po_catalogs[lang] = messages
        where = f"po/{lang}"
        for msg in messages:
            check_message(where, msg, PO_PLACEHOLDER, nplurals, args.allow_unfinished, allowed, report,
                          is_source_language=False)
        check_accelerator_collisions(where, messages, report)
    if len(po_catalogs) > 1:
        check_set("po", po_catalogs, report)

    for line in report.errors:
        print(f"error: {line}")
    if args.verbose:
        for line in report.warnings:
            print(f"warning: {line}")
    elif report.warnings:
        counts: dict[str, int] = defaultdict(int)
        for line in report.warnings:
            counts[line.split(" ", 1)[0]] += 1
        summary = ", ".join(f"{name} {count}" for name, count in sorted(counts.items()))
        print(f"{len(report.warnings)} warnings ({summary}); --verbose lists them")
    print(f"{len(report.errors)} errors in {len(ts_catalogs)} application and "
          f"{len([k for k in po_catalogs if k != 'template'])} widget catalogs")
    return 1 if report.errors else 0


if __name__ == "__main__":
    sys.exit(main())
