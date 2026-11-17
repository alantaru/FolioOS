# Translating FolioOS

FolioOS has **no language built into the code**. Interface texts, keyboard layouts,
smart quotes, date formats, plural rules and the fonts a script needs all live in plain
text files that anyone can edit — on any computer, without compiling anything.

| What | Where (in the repository) | Where (on the USB drive) | Format |
|---|---|---|---|
| Interface translations | `assets/i18n/<code>.po` | `FOLIO/LANGUAGES/<code>.po` | GNU gettext PO |
| Translation template | `assets/i18n/folio.pot` | — | GNU gettext POT |
| Keyboard layouts | `assets/keyboards/<code>.txt` | `FOLIO/KEYBOARDS/<code>.txt` | plain text rules |
| Fonts for a script | `tools/fetch-cjk-fonts.ps1` | `FOLIO/FONTS/_LANGUAGE/` | TrueType (`.ttf`) |

The source language is **English**. Every other language — including Portuguese, the
language FolioOS was born in — is a translation like any other.

---

## 1. Improve an existing language

1. Open `assets/i18n/<code>.po` in [Poedit](https://poedit.net), any PO editor
   (Lokalize, Virtaal, Weblate, Crowdin…) or a plain text editor.
2. Fix or complete the `msgstr` lines. Leave `msgid` untouched — it is the key.
3. Check your work (see *Validating*), then open a pull request.

To test on a real machine, copy the `.po` file to `FOLIO/LANGUAGES/` on a FolioOS drive
and restart. No build needed.

## 2. Add a new language

1. Copy `assets/i18n/folio.pot` to `assets/i18n/<code>.po`, using a
   [BCP 47](https://www.w3.org/International/articles/language-tags/) code:
   `fr`, `pt-BR`, `zh-CN`, `sr-Latn`…
2. Fill in the header (the first entry, with the empty `msgid ""`):

   ```po
   "Language: hi\n"
   "Plural-Forms: nplurals=2; plural=(n != 1);\n"
   "X-Language-Name: हिन्दी (Hindi)\n"
   "X-Keyboard: us\n"
   "X-Quotes: “”‘’\n"
   "X-Fonts: \n"
   "X-Date-Format: {d} {month} {yyyy}\n"
   "X-Decimal: .\n"
   ```

   | Header | Meaning |
   |---|---|
   | `Plural-Forms` | Standard gettext plural rule. Copy it from the [Unicode CLDR / gettext tables](https://docs.translatehouse.org/projects/localization-guide/en/latest/l10n/pluralforms.html). FolioOS evaluates the C expression at runtime. |
   | `X-Language-Name` | The name shown in *Settings › Language*, written in the language itself. If the script needs extra fonts, add the English name in parentheses so it stays readable when the fonts are missing. |
   | `X-Keyboard` | Default keyboard layout for fresh installs (a file in `FOLIO/KEYBOARDS`). |
   | `X-Quotes` | Smart quotes, 4 characters: double open, double close, single open, single close. Examples: `“”‘’`, `«»“”`, `„“‚‘`, `「」『』`. |
   | `X-Fonts` | Fonts needed by the script, relative to `FOLIO/FONTS`, separated by `;`. Loaded only while the language is active. If they are missing, FolioOS refuses to switch and tells the user which pack to install — the screen never becomes unreadable. |
   | `X-Date-Format` | Used by *Insert today's date*. Tokens: `{d}`, `{dd}`, `{month}` (translated month name), `{mm}`, `{yyyy}`. |
   | `X-Decimal` | Decimal separator for numbers such as `1.62 lines` (default `.`). |

3. Translate every `msgstr`. Then validate and open a pull request.

FolioOS discovers languages by listing `FOLIO/LANGUAGES` — there is no list to update
anywhere in the code.

## 3. Rules for translators

**Placeholders.** Keep every `%s`, `%d`, `%02d`, `%.1f` of the original, with the same
types. If your grammar needs a different order, use positional placeholders:

```po
msgid "+%d in %dh%02d"
msgstr "%2$dsa%3$02d içinde +%1$d"
```

A translation whose placeholders do not match is **rejected when loading** (the English
text is shown instead), so a mistake can never crash the system — but please validate.

**Plurals.** Entries with `msgid_plural` need one `msgstr[N]` per plural form of your
language (`nplurals`). Languages without plural forms (Chinese, Japanese, Korean…) use
`nplurals=1; plural=0;` and a single `msgstr[0]`.

**Keyboard shortcuts.** Translate key names the way they are printed on keyboards in your
country (German: `Strg+S`, `Umschalt`, `Bild↑`). The shortcut itself does not change.

**Fuzzy entries.** Entries marked `#, fuzzy` are ignored until the mark is removed.

**Length.** Bars and buttons have limited room. Prefer short, natural wording; long texts are
cut with an ellipsis rather than overflowing.

**Tone.** FolioOS speaks to writers: calm, friendly, direct. Avoid technical jargon.

## 4. Validating

From the repository root (Windows, with MinGW-w64 `gcc`):

```powershell
gcc -O2 -o build\i18n.exe tools\i18n.c
build\i18n.exe check assets\i18n\folio.pot assets\i18n\<code>.po            # formats + coverage
build\i18n.exe check assets\i18n\folio.pot --strict assets\i18n\<code>.po   # also fails if anything is missing
```

`.\build.ps1` runs the same check for every language, plus a negative control that makes
sure the checker itself rejects broken placeholders. On Linux/macOS the tool builds with any
C compiler (`cc -O2 -o i18n tools/i18n.c`).

## 5. When developers change the interface

Interface texts are marked in the C code:

```c
ui_toast(L("Saved"));                                   /* translated at runtime  */
static const char *names[] = { N_("Narrow"), N_("Wide") };   /* marked; L(names[i]) when drawn */
fmt(buf, sizeof buf, LN("%d word", "%d words", n), n);  /* plural                 */
```

`build.ps1` regenerates `assets/i18n/folio.pot` automatically. To bring every `.po` up to
date (new texts added empty, old ones kept, removed ones marked obsolete):

```powershell
build\i18n.exe update assets\i18n\folio.pot (Get-ChildItem assets\i18n\*.po)
```

## 6. Keyboard layouts

The UEFI firmware reports every key as if the keyboard were American. A layout file tells
FolioOS what each US key must type instead. Copy an existing file from
`assets/keyboards/` and edit it:

```text
# comment
name    Deutsch (QWERTZ)        name shown in Settings (in the layout's own language)
include _compose.txt            shared dead-key compositions (accent + letter)
map     y z                     US key → character (shifted keys: use the shifted US character)
map     Y Z
altgr   q @                     the same, with AltGr held
dead    ´ ` ^                   outputs that are dead keys
compose ´ e é                   dead key + letter → result
```

A character can be written literally, as `U+00E9`, or as `space`. Files whose names start
with `_` are not listed (they are included by other layouts).

## 7. Scripts and fonts

FolioOS draws text with its own TrueType rasterizer, glyph by glyph, left to right. That
covers Latin, Greek and Cyrillic, and CJK interfaces with the fonts from
`FOLIO-FONTS-CJK.zip`. Scripts that need contextual shaping or right-to-left layout
(Arabic, Hebrew, Devanagari, Thai…) need a text-shaping engine first — see the roadmap in
the README. Translations for them are welcome anyway: they will light up as soon as the
engine lands.

## 8. The README in your language

The short READMEs (`README.<code>.md`) are generated from one template so they stay identical
in structure. Edit or add your language's table in `tools/readmes.pl`, then run
`perl tools/readmes.pl`: it rewrites every `README.<code>.md` and the language bar at the top
of `README.md`. Do not edit the generated files by hand.
