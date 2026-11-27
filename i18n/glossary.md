# itsova translation glossary

Binding rules for every translation of itsova (application catalogs `i18n/itsova_<lang>.ts`
and Plasma widget catalogs `plasmoid/po/<lang>/*.po`). English is the source language.
`scripts/check-translations.py` enforces the mechanical rules; the rest is up to the translator.

## General rules

- Translate meaning, not words. Use the tone of the desktop environment in that language: short,
  neutral, no exclamation marks, no marketing.
- Use the KDE Plasma terminology of the language where it exists (Settings, Cancel, Save As…,
  Copy to Clipboard, Notifications, System Tray).
- Address the user the way KDE does in that language. Commands and menu entries use the usual
  command form (German: infinitive, "Datei speichern"); questions use the usual polite form
  (German: "Möchten Sie …?").
- Keep placeholders exactly: `%1`, `%2`, … and `%n` must all appear in the translation, each as
  often as in the source. Their order may change.
- Keep `&` accelerators when the source has one (exactly one `&` before a letter). In Chinese,
  Japanese and Korean put the accelerator in brackets after the text: `Datei(&F)`.
- Keep the typographic characters of the source where the language uses them: the ellipsis `…`
  after a command that asks for more input, `×` in sizes (`1920 × 1080`), `·` as separator.
  Use the quotation marks of the language instead of `“ ”`.
- Keep line breaks (`\n`) where the source has them.
- Plural forms: fill every form the language has. `%n` must appear in each form, except where the
  language never shows the number in that form.
- Stay below about 150 % of the source length for buttons, menu entries and short labels.

## Never translate

These stay exactly as written (they are names, keys, commands or file names):

- `itsova` (always lower case, also at the start of a sentence)
- `KDE`, `Plasma`, `KWin`, `Wayland`, `D-Bus`, `MCP`, `JSON`, `CLI`, `OCR`, `PNG`, `JPEG`, `WebP`,
  `AVIF`, `tesseract`, `Spectacle`
- key names: `Print`, `Esc`, `Enter`, `Space`, `Tab`, `Shift`, `Ctrl`, `Alt`, `Meta`, `F1`
- command lines, option names and file names: `itsova capture`, `--json`, `itsovarc`,
  `~/.config/…`, `%HOME%`-style variables and anything in backticks
- format tokens in file name patterns: `{date}`, `{time}`, `{target}`, `{n}` and similar

## Fixed terminology

Translate these consistently; once a term is chosen for a language, use it everywhere.

| English | Meaning in itsova | Note |
|---|---|---|
| Area | A saved rectangle on one or more monitors that can be captured again | Not "region" in the sense of a country |
| Group | Several areas captured together | |
| Monitor | A physical screen | Use the usual desktop word for screen/display |
| All Monitors | Every connected screen at once | |
| Window | An application window | |
| Capture (noun) | One screenshot | |
| Capture (verb) | Take a screenshot | |
| Overlay | The full-screen selection layer for interactive captures | May stay "Overlay" where common |
| Frozen / Live | Overlay modes: frozen still image vs. the live desktop | |
| Quick Editor | The annotation window opened after a capture | A product feature name: translate, keep both words |
| Pin | A screenshot kept on top of all windows (also the verb "pin to screen") | |
| Interval | Repeated captures every N seconds | |
| Target | Anything that can be captured: area, group, monitor, window | |
| Trigger | Starting a capture (shortcut, menu, widget, CLI) | |
| Hook | A user command run after a capture | May stay "Hook" where common |
| Redact / Redaction | Making parts of an image unreadable (pixelate, blur, black box) | |
| Annotation | Arrows, text, shapes drawn on a capture | |
| History | The list of recent captures | |
| Text recognition | OCR, reading text from an image | |
| Allowlist | MCP access mode that only allows listed targets | |

## Language notes

- `ar`: right-to-left. Do not add direction marks; Qt handles the layout. Keep Latin names
  (`itsova`, `KWin`) as they are.
- `de`: KDE style, infinitives for actions ("Bereich speichern"), "Bildschirmfoto" for screenshot,
  "Bildschirm" for monitor.
- `ja`, `ko`, `zh_CN`, `zh_TW`: no spaces between words; accelerators in brackets `(&S)`.
- `pt`: Brazilian Portuguese, as KDE `pt_BR`.
- `zh_CN`: Simplified Chinese (mainland); `zh_TW`: Traditional Chinese (Taiwan) vocabulary, not a
  character conversion of `zh_CN`.

## Allowed identical translations

Entries whose translation may be identical to the English source without a warning, one source
text per line. The check script reads this block.

```allow-identical
itsova
itsova %1
KWin
MCP
OCR
PNG
JPEG
WebP
AVIF
JSON
CLI
D-Bus
%1
%1 (%2)
%1 · %2
%1 · %2 (%3)
%1 × %2
%1 · %2 × %3
%1 %
%1%
OK
Hook
Overlay
Pin
Pins (%1)
Plasma
Wayland
Spectacle
tesseract
```
