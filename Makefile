# The code shared by the pac tools (config_file, fontfind, ...)
PACOMMON ?= pacommon
ifeq ($(wildcard $(PACOMMON)/pac.mk),)
$(error pacommon is not at $(PACOMMON): run: git submodule update --init, or set PACOMMON=path/to/pacommon)
endif
include $(PACOMMON)/pac.mk

CC ?= gcc

# NOTIFY=0 builds without the notification daemon: no libsystemd (sd-bus).
NOTIFY ?= 1
# SPEECH=none builds without speech: no libspeechd (speech-dispatcher). The default, speechd, speaks
# what the HUD shows (see docs/usage.md); `speech = no` in the settings turns it off at run time.
# I18N=0 builds an English-only binary: no gettext catalogs, FreeType, HarfBuzz or FriBidi,
# and no per-language fonts.
I18N ?= 1

PKGS = wayland-client cairo $(PAC_SPEECH_PKGS)
ifeq ($(NOTIFY),1)
PKGS += libsystemd
endif
ifeq ($(I18N),1)
PKGS += freetype2 harfbuzz fribidi
endif
CFLAGS ?= -Os -fno-plt -Wall -Wextra -Wshadow -Wvla -Wformat-security
LDFLAGS ?= -s
# Hardening: fortified libc calls, stack protector, PIE, full RELRO and immediate binding.
# Control-flow protection only exists on x86 compilers, so ask the compiler.
CET_CFLAGS := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fcf-protection=full \
                -o /dev/null 2>/dev/null && echo -fcf-protection=full)
ALL_CFLAGS := $(CFLAGS) -DSYSCONFDIR='"$(SYSCONFDIR)"' -ffunction-sections -fdata-sections -std=gnu11 -D_GNU_SOURCE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong \
              -fstack-clash-protection $(CET_CFLAGS) -fPIE $(PAC_SPEECH_CFLAGS) -MMD -MP -Ibuild -I$(PACOMMON)/src \
              $(shell pkg-config --cflags $(PKGS))
ALL_LDFLAGS := $(LDFLAGS) -Wl,--as-needed -Wl,--gc-sections -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
ALL_LIBS := $(LDLIBS) $(shell pkg-config --libs $(PKGS))

WL_PROTOCOLS = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
PROTOCOLS = protocol/wlr-layer-shell-unstable-v1.xml \
            $(WL_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml \
            $(WL_PROTOCOLS)/staging/alpha-modifier/alpha-modifier-v1.xml \
            $(WL_PROTOCOLS)/staging/fractional-scale/fractional-scale-v1.xml \
            $(WL_PROTOCOLS)/stable/viewporter/viewporter.xml \
            $(WL_PROTOCOLS)/staging/xdg-activation/xdg-activation-v1.xml \
            $(WL_PROTOCOLS)/staging/ext-idle-notify/ext-idle-notify-v1.xml \
            $(WL_PROTOCOLS)/unstable/xdg-output/xdg-output-unstable-v1.xml

PROTO_NAMES = $(basename $(notdir $(PROTOCOLS)))
PROTO_HEADERS = $(PROTO_NAMES:%=build/%-client-protocol.h)
PROTO_OBJS = $(PROTO_NAMES:%=build/%-client-protocol.o)

SRCS = $(wildcard src/*.c)
ifneq ($(NOTIFY),1)
SRCS := $(filter-out src/notify.c src/history.c,$(SRCS))
ALL_CFLAGS += -DNO_NOTIFY
endif
ifneq ($(I18N),1)
SRCS := $(filter-out src/shape.c,$(SRCS))
endif
COMMON_SRCS = $(PACOMMON)/src/config_file.c $(PACOMMON)/src/daemon_value.c $(PACOMMON)/src/fontfind.c $(PACOMMON)/src/language.c $(PACOMMON)/src/length.c $(PACOMMON)/src/setting.c $(PACOMMON)/src/speech.c $(PACOMMON)/src/utf8.c
COMMON_OBJS = $(COMMON_SRCS:$(PACOMMON)/src/%.c=build/pacommon_%.o)
OBJS = $(SRCS:src/%.c=build/%.o) $(PROTO_OBJS) $(COMMON_OBJS)
TARGET = pacreact

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LOCALEDIR ?= $(PREFIX)/share/locale
export LOCALEDIR

ifeq ($(I18N),1)
ifneq ($(shell command -v msgfmt 2>/dev/null),)
LINGUAS = $(basename $(notdir $(wildcard po/*.po)))
else
$(info msgfmt not found: building without translation catalogs (install gettext, or use I18N=0))
endif
endif
MOS = $(LINGUAS:%=build/locale/%/LC_MESSAGES/$(TARGET).mo)

all: $(TARGET) $(MOS)

$(TARGET): $(OBJS)
	$(CC) $(ALL_LDFLAGS) $^ -o $@ $(ALL_LIBS)

build/pacommon_%.o: $(PACOMMON)/src/%.c | $(PROTO_HEADERS) build/config.h
	$(CC) $(ALL_CFLAGS) -Isrc -DPACOMMON_I18N='"i18n.h"' -c $< -o $@

build/%.o: src/%.c | $(PROTO_HEADERS) build/config.h
	$(CC) $(ALL_CFLAGS) -c $< -o $@

build/%.o: build/%.c | $(PROTO_HEADERS)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

# Rewritten only when its content changes, so a new LOCALEDIR or I18N rebuilds what uses it.
build/config.h: FORCE | build
	@case "$$LOCALEDIR" in *[[:cntrl:]]*) echo "LOCALEDIR must not contain control characters" >&2; exit 1;; esac; \
	{ printf '#define LOCALEDIR "%s"\n' "$$(printf '%s' "$$LOCALEDIR" | sed 's/[\\"]/\\&/g')"; \
	   [ '$(I18N)' = 1 ] || echo '#define NO_I18N 1'; } > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

build/locale/%/LC_MESSAGES/$(TARGET).mo: po/%.po
	@mkdir -p $(dir $@)
	msgfmt --check -o $@ $<

pot:
	xgettext --from-code=UTF-8 --keyword=_ --keyword=N_ --add-comments=TRANSLATORS: \
		--add-location=file --package-name=$(TARGET) -o po/$(TARGET).pot $(sort $(SRCS)) $(PACOMMON)/src/config_file.c $(PACOMMON)/src/speech.c

update-po: pot
	for po in po/*.po; do msgmerge --update --backup=none --add-location=file $$po po/$(TARGET).pot; done

vpath %.xml $(sort $(dir $(PROTOCOLS)))

build/%-client-protocol.h: %.xml | build
	wayland-scanner client-header $< $@

build/%-client-protocol.c: %.xml | build
	wayland-scanner private-code $< $@

build:
	mkdir -p build

clean:
	rm -rf build $(TARGET)

UNITDIR ?= $(PREFIX)/lib/systemd/user
MANDIR ?= $(PREFIX)/share/man
DBUSDIR ?= $(PREFIX)/share/dbus-1/services

install: all
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm644 docs/pacreact.1 $(DESTDIR)$(MANDIR)/man1/pacreact.1
	install -Dm644 pacreact.conf $(DESTDIR)$(PREFIX)/share/doc/pacreact/pacreact.conf
	$(call pac_install_keep,pacreact.conf,$(DESTDIR)$(SYSCONFDIR)/pac/pacreact.conf,644)
	$(pac_install_shared_conf)
	for lang in $(LINGUAS); do \
		install -Dm644 build/locale/$$lang/LC_MESSAGES/$(TARGET).mo \
			$(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/$(TARGET).mo; \
	done
	sed 's|@BINDIR@|$(BINDIR)|' contrib/pacreact.service.in > build/pacreact.service
	$(call pac_install_keep,build/pacreact.service,$(DESTDIR)$(UNITDIR)/pacreact.service,644)
ifeq ($(NOTIFY),1)
	sed 's|@BINDIR@|$(BINDIR)|' contrib/org.pacreact.Notifications.service.in \
		> build/org.pacreact.Notifications.service
	$(call pac_install_keep,build/org.pacreact.Notifications.service,$(DESTDIR)$(DBUSDIR)/org.pacreact.Notifications.service,644)
endif

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET) $(DESTDIR)$(MANDIR)/man1/pacreact.1 $(DESTDIR)$(PREFIX)/share/doc/pacreact/pacreact.conf
	$(call pac_uninstall_keep,pacreact.conf,$(DESTDIR)$(SYSCONFDIR)/pac/pacreact.conf)
	$(call pac_uninstall_keep,build/pacreact.service,$(DESTDIR)$(UNITDIR)/pacreact.service)
	$(call pac_uninstall_keep,build/org.pacreact.Notifications.service,$(DESTDIR)$(DBUSDIR)/org.pacreact.Notifications.service)
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/$(TARGET).mo; \
	done

-include $(wildcard build/*.d)


# The tests again under AddressSanitizer and UBSan.
sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS="-g -fsanitize=address,undefined $(CFLAGS)" LDFLAGS="-fsanitize=address,undefined $(LDFLAGS)"

.PHONY: sanitize all clean install uninstall pot update-po test FORCE

# The tests build the logic on its own, with stubs for the display, the HUD and the loop. Some
# #include the source under test so that its static internals are reachable.
TEST_CC = $(CC) $(CFLAGS) -D_GNU_SOURCE -Wall -Wextra -Ibuild -Isrc -I$(PACOMMON)/src
TESTS = clock timer activity hud ipc sound dnd image icon sicon scale display alerts daemon util config watch
ifeq ($(I18N),1)
TESTS += text
endif
ifeq ($(NOTIFY),1)
TESTS += notify history
endif

ifeq ($(I18N),1)
FT_TEST = $(shell pkg-config --cflags --libs freetype2)
endif
TEST_SRC_clock = src/util.c
TEST_SRC_sound = src/sound.c src/util.c
TEST_SRC_timer = src/state.c
TEST_SRC_activity = src/util.c
TEST_SRC_dnd = src/timer.c src/state.c
TEST_SRC_image = src/image.c
TEST_SRC_icon = src/icon.c src/util.c
TEST_SRC_util = src/util.c
TEST_SRC_config = src/settings_file.c $(PACOMMON)/src/config_file.c
TEST_SRC_watch = src/watch.c
TEST_SRC_daemon = src/watch.c $(PACOMMON)/src/daemon_value.c
TEST_SRC_alerts = src/state.c src/util.c
TEST_SRC_text = $(PACOMMON)/src/fontfind.c src/shape.c $(PACOMMON)/src/utf8.c $(PACOMMON)/src/language.c
TEST_SRC_display = $(PROTO_OBJS) src/util.c src/watch.c
TEST_SRC_notify = src/notify.c src/image.c src/icon.c src/util.c
TEST_SRC_hud = $(PACOMMON)/src/palette.c $(PACOMMON)/src/fontfind.c src/pngload.c src/sicon.c src/icon.c src/util.c
TEST_SRC_sicon = $(PACOMMON)/src/fontfind.c src/sicon.c src/icon.c src/pngload.c src/util.c
TEST_LIBS_hud = $(shell pkg-config --libs cairo) $(FT_TEST) -lm
TEST_LIBS_sicon = $(shell pkg-config --libs cairo) $(FT_TEST) -lm
TEST_LIBS_text = $(shell pkg-config --cflags --libs cairo freetype2 harfbuzz fribidi) -lm
TEST_LIBS_display = $(shell pkg-config --libs wayland-client cairo) -lm
TEST_LIBS_notify = $(shell pkg-config --libs libsystemd 2>/dev/null)

test: $(TESTS:%=build/test_%)
	@set -e; for t in $^; do $$t; done

build/test_%: tests/test_%.c $(wildcard src/*.c src/*.h) | build build/config.h $(PROTO_HEADERS) $(PROTO_OBJS)
	$(TEST_CC) tests/test_$*.c $(TEST_SRC_$*) $(PACOMMON)/src/setting.c $(PACOMMON)/src/length.c -o $@ $(TEST_LIBS_$*) -lm
