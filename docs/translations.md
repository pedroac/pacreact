# Translations

pachud follows `LANG`/`LC_MESSAGES` (and `LANGUAGE`, which also selects the font and text direction). Only what it shows is translated: the HUD messages (battery, memory, network, volume, brightness, timers and alarms). Command-line help and error replies stay in English.

Included: Arabic, Chinese (Simplified), Dutch, French, German, Italian, Japanese, Korean, Polish, Portuguese (`pt_PT`, `pt_BR`), Russian, Spanish and Turkish. These were machine-written and should be reviewed by native speakers.

Catalogs are looked up through the system locale, so the locale itself must exist (`locale -a`), or `LANGUAGE` must be set together with an installed locale.

## Fonts and scripts

Fonts are picked per string, from the characters it contains, not only from the UI language:

| Text | Font | Notes |
|---|---|---|
| Latin, Cyrillic, Greek | Noto Sans, else DejaVu Sans, else Liberation Sans | regular and bold |
| Arabic, Hebrew | DejaVu Sans | shaped and bidi-reordered with HarfBuzz and FriBidi; DejaVu also covers Latin, so `wlan0` inside Arabic text renders |
| Chinese, Japanese, Korean | Noto Sans CJK (install `noto-fonts-cjk`) | the Han variant follows the UI language (ja, ko, zh_CN, zh_TW, zh_HK), else the script of the text |

This also applies to notifications: a Japanese notification in an English UI is drawn with the CJK font. Fonts load the first time a script is needed, so Latin-only use never touches the others. If a font file is missing, pachud falls back to Cairo's default sans-serif, which may show empty boxes for that script.

## Right-to-left

For Arabic, Persian, Hebrew and Urdu UI languages the layout is mirrored: the accent bar moves to the right, sliders fill from the right, and text is right-aligned.

## Adding a language

```bash
make pot                                    # refresh po/pachud.pot
msginit -i po/pachud.pot -o po/de.po -l de_DE.UTF-8
# translate po/de.po, then:
make && sudo make install
```

`make update-po` merges new strings into existing catalogs. Keep the `printf` directives (`%d`, `%ld`, `%s`, `%%`) as in the original: `msgfmt --check` rejects mismatches. For a new CJK or right-to-left language, add it to the `scripts` table in `src/text.c` so the right font and direction are used.

## Without translations

`make I18N=0` builds an English-only binary without gettext catalogs, FreeType, HarfBuzz or FriBidi.
