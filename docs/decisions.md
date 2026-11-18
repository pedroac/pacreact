# Decisions of pacreact

Implementation decisions that are not obvious from the code (principle 52): what was decided, why, what else was considered, and when. Newest last.

The design of pacreact, with its measurements, is in [design.md](design.md). Decisions made after it are here.

## Limits and defaults in one `tunables.h`

- **Decided:** Every numeric limit, timing and default that was a `#define` in a source file moved to `src/tunables.h`, each under `#ifndef` so that `-DNAME=value` overrides it, with a `_Static_assert` that it is positive (principle 12). A name used in more than one file got its file's name as a prefix.
- **Considered:** Leave them where they were (rejected: principle 12 asks for one place); a generated `config.h` (rejected: more machinery for the same effect).
- **When:** 2026-11-15
- **Note:** Many comments in `tunables.h` only say what the value is and its unit; **why** each default is what it is has not been written down yet, and each such entry says so. Whoever knows the reason should write it there.

## Hardened build flags

- **Decided:** PIE, full RELRO, `-z now`, no executable stack, `-fstack-protector-strong`, `-fstack-clash-protection`, `_FORTIFY_SOURCE=3` and `-fcf-protection=full` where the compiler accepts it, added in the Makefile as `ALL_CFLAGS` / `ALL_LDFLAGS` so that overriding `CFLAGS` or `LDFLAGS` cannot drop them (principle 39).
- **Considered:** Putting the flags in `pac.mk` (not done: each tool's Makefile already carries them, and the tools differ in what they link).
- **When:** written down 2026-11-15; the flags were already in the Makefile

## `I18N=0` builds an English-only binary

- **Decided:** The Makefile has `I18N ?= 1`. `I18N=0` defines `NO_I18N`: `_()` becomes the text itself, `i18n.c` compiles to nothing, and no catalogs are built or installed (principle 10). Checked by building with `I18N=0` and finding no gettext symbols in `nm -D`.
- **Considered:** Keeping gettext always on (rejected: libintl is in glibc but not in every libc, and an optional feature must be removable). The unit tests were not run with `I18N=0` (they check translations).
- **When:** 2026-11-15

## Private state files

- **Decided:** the `timers` and `alerts` files in the state directory are created with `open(O_CREAT|O_EXCL|O_NOFOLLOW, 0600)` (`state_create()` in `state.c`) and renamed into place, instead of `fopen(..., "w")`, which left the mode to the umask (principle 42).
- **Considered:** `umask(077)` for the whole process (rejected: it would change the mode of files other code creates).
- **When:** 2026-11-15
- **Note:** there is no test yet that creates the file under a permissive umask and finds 0600 (principle 42's check); it should be added.

## Two things that talk to a network

- **Decided:** the `probe_host` ping (an opt-in ICMP echo to an address the person names) and the questions to the CUPS server (`CUPS_SERVER`, normally the local one) about print jobs are kept, because they are what the network-slowdown and printing alerts are. The probe is off unless `probe_host` is set, and nothing is sent anywhere else. This is the reading of principle 44 ("sends nothing anywhere"): nothing leaves the machine unless the person asked for it.
- **Considered:** removing the probe (rejected: it is the only way to notice a slow link without a daemon on the other end).
- **When:** 2026-11-15
- **Note:** the author should confirm this reading, because `strace -f -e trace=network` on an idle tool with `probe_host` set will show network calls.

## Allocations

- **Decided:** the display, the glyph array and the SVG output buffer are allocated, because their
  size is known only at run time and they outlive the call. The history and the settings are fixed
  blocks; the SVG buffer is `SVG_OUT_MAX` and is freed after the icon is decoded.
- **When:** 2026-11-15

## `utf8` and the choice of language come from pacommon

- **Decided:** `utf8_decode` and `pac_set_language` are `pacommon`'s (`utf8.c`, `language.c`). pacreact's own `utf8.c/h` and `i18n.c` are gone; `i18n.h` keeps only the gettext macros. The `i18n.c` of the entry above no longer exists: with `I18N=0` nothing of gettext is linked by it, and `utf8.c` is now always built (it is 20 lines, and `-Wl,--gc-sections` drops it if unused).
- **Why:** the same code was in pacpeek and pacgreet; one tested copy is easier to keep right (`pacommon/docs/decisions.md`, 2026-11-18).
- **When:** 2026-11-18
