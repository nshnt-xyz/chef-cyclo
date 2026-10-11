# chefui: the UI platform (LVGL v9.6 on the MDSS fbdev) -- library,
# demo application and host tests. Specification:
# docs/archive/ui-platform.md; LVGL is the third_party/lvgl submodule,
# configured by lv_conf.h here. Everything is built under build/.
#
#   make device   static aarch64 musl build for the phone:
#                 build/device/libchefui.a + build/device/chefui-demo
#                 (stripped). scripts/mkinitramfs.sh runs this and installs
#                 the demo as /usr/bin/chefui-demo.
#   make host     the same library and demo against LVGL's SDL2 driver for
#                 the PC (needs libsdl2-dev): build/host/chefui-demo
#   make test     host unit tests (no device, no SDL, no real fb): the
#                 pure modules under ASan/UBSan, and the fbdev backend +
#                 main loop end to end against temporary files with ioctl
#                 wrapped (tests/test_chefui.c); the system UI kit, state
#                 sources, settings file and app (tests/test_sys*.c,
#                 tests/test_app.c); D-Bus sources against a fake
#                 NetworkManager/BlueZ on a private bus when libsystemd
#                 and dbus-daemon are installed (skipped otherwise)
#   make font-check  regenerate kit/fonts into a temporary copy and compare
#                 (needs python3 fontTools and npx)
#
# The system UI app chef-settings (apps/settings/) links libchefkit (kit/
# widgets and fonts, sys/ state sources and settings) and, on the phone,
# static basu for D-Bus (scripts/setup-toolchain.sh fetches it to
# toolchain/basu). The host build uses libsystemd's sd-bus when pkg-config
# finds it, else builds without Wi-Fi/Bluetooth state (--fake-state).
#
# Only LVGL's own sources are compiled with -w. DEVICE_CC/HOST_CC and
# LVGL can be overridden on the command line.

REPO_ROOT := ../..
LVGL ?= $(REPO_ROOT)/third_party/lvgl
DEVICE_CC ?= $(REPO_ROOT)/toolchain/aarch64-musl/bin/aarch64-buildroot-linux-musl-gcc
DEVICE_AR ?= $(patsubst %-gcc,%-ar,$(DEVICE_CC))
DEVICE_STRIP ?= $(patsubst %-gcc,%-strip,$(DEVICE_CC))
HOST_CC ?= cc
HOST_AR ?= ar
SDL_CFLAGS ?= $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS ?= $(shell pkg-config --libs sdl2 2>/dev/null)

CHEF_WARN := -std=gnu11 -Wall -Wextra
LV_DEFS := -DLV_CONF_INCLUDE_SIMPLE
INC := -I. -I.. -I$(LVGL) -Ikit -Isys -Iapps/settings
BASU ?= $(REPO_ROOT)/toolchain/basu
SDBUS_LIBS ?= $(shell pkg-config --libs libsystemd 2>/dev/null)
OPT := -O2 -g -ffunction-sections -fdata-sections
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

LV_SRCS := $(shell find $(LVGL)/src -name '*.c' 2>/dev/null)
PURE := copy.c plan.c touch.c buttons.c fbscreen.c
HDRS := chefui.h chefui_internal.h copy.h plan.h touch.h buttons.h fbscreen.h lv_conf.h ../fbdev.h ../evdev.h

DEV_LIB_SRCS := chefui.c backend_fb.c backends_device.c $(PURE)
HOST_LIB_SRCS := chefui.c backend_sdl.c backends_host.c $(PURE)

# System UI kit + state sources (libchefkit) and the chef-settings app.
FONT_SRCS := $(sort $(wildcard kit/fonts/chefkit_*.c))
KIT_SRCS := kit/theme.c kit/widgets.c $(FONT_SRCS)
SYS_SRCS := sys/sysstate.c sys/settings.c sys/sha256.c sys/battery.c sys/clock.c sys/gps.c \
	sys/storage.c sys/fake.c
APP_SRCS := apps/settings/app.c apps/settings/policy.c apps/settings/text.c
KIT_HDRS := kit/theme.h kit/widgets.h kit/fonts.h kit/icons.h sys/sysstate.h sys/sys.h \
	sys/settings.h sys/sha256.h sys/linebuf.h apps/settings/app.h apps/settings/policy.h \
	apps/settings/text.h
HDRS += $(KIT_HDRS)
ifneq ($(SDBUS_LIBS),)
HOST_NETBUS := sys/netbus.c
else
HOST_NETBUS := sys/netbus_none.c
endif

B := build

.PHONY: all device host test clean check-lvgl

all: device

check-lvgl:
	@test -f $(LVGL)/lvgl.h || { echo "LVGL not found at $(LVGL): git submodule update --init third_party/lvgl" >&2; exit 1; }

# ---------------------------------------------------------------- LVGL
# One object tree per configuration: device (aarch64, NEON), host (SDL)
# and test (x86, no SDL).

$(B)/device/lvgl/%.o: $(LVGL)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(DEVICE_CC) $(OPT) $(LV_DEFS) $(INC) -w -c -o $@ $<

$(B)/host/lvgl/%.o: $(LVGL)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(HOST_CC) $(OPT) $(LV_DEFS) -DCHEFUI_SDL=1 $(SDL_CFLAGS) $(INC) -w -c -o $@ $<

$(B)/test/lvgl/%.o: $(LVGL)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(HOST_CC) -O1 -g $(LV_DEFS) $(INC) -w -c -o $@ $<

$(B)/device/liblvgl.a: $(patsubst $(LVGL)/%.c,$(B)/device/lvgl/%.o,$(LV_SRCS))
	rm -f $@; $(DEVICE_AR) rcs $@ $^

$(B)/host/liblvgl.a: $(patsubst $(LVGL)/%.c,$(B)/host/lvgl/%.o,$(LV_SRCS))
	rm -f $@; $(HOST_AR) rcs $@ $^

$(B)/test/liblvgl.a: $(patsubst $(LVGL)/%.c,$(B)/test/lvgl/%.o,$(LV_SRCS))
	rm -f $@; $(HOST_AR) rcs $@ $^

# -------------------------------------------------------------- device

$(B)/device/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(DEVICE_CC) $(CHEF_WARN) $(OPT) $(LV_DEFS) $(INC) -c -o $@ $<

$(B)/device/libchefui.a: $(patsubst %.c,$(B)/device/%.o,$(DEV_LIB_SRCS))
	rm -f $@; $(DEVICE_AR) rcs $@ $^

$(B)/device/chefui-demo: $(B)/device/demo.o $(B)/device/libchefui.a $(B)/device/liblvgl.a
	$(DEVICE_CC) -static -Wl,--gc-sections -o $@.unstripped $^ -lm
	$(DEVICE_STRIP) -o $@ $@.unstripped

# basu headers only for netbus.c (CHEF_BASU selects <basu/sd-bus.h>)
$(B)/device/sys/netbus.o: sys/netbus.c $(HDRS)
	@mkdir -p $(dir $@)
	@test -f $(BASU)/lib/libbasu.a || { echo "basu not found at $(BASU): run scripts/setup-toolchain.sh" >&2; exit 1; }
	$(DEVICE_CC) $(CHEF_WARN) $(OPT) $(LV_DEFS) $(INC) -DCHEF_BASU -I$(BASU)/include -c -o $@ $<

$(B)/device/libchefkit.a: $(patsubst %.c,$(B)/device/%.o,$(KIT_SRCS) $(SYS_SRCS) sys/netbus.c)
	rm -f $@; $(DEVICE_AR) rcs $@ $^

$(B)/device/chef-settings: $(patsubst %.c,$(B)/device/%.o,$(APP_SRCS) apps/settings/main.c) $(B)/device/libchefkit.a $(B)/device/libchefui.a $(B)/device/liblvgl.a
	$(DEVICE_CC) -static -Wl,--gc-sections -o $@.unstripped $^ $(BASU)/lib/libbasu.a -pthread -lrt -lm
	$(DEVICE_STRIP) -o $@ $@.unstripped

device: check-lvgl $(B)/device/chefui-demo $(B)/device/chef-settings
	@ls -l $(B)/device/chefui-demo $(B)/device/chef-settings

# ---------------------------------------------------------------- host

$(B)/host/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CHEF_WARN) $(OPT) $(LV_DEFS) -DCHEFUI_SDL=1 $(SDL_CFLAGS) $(INC) -c -o $@ $<

$(B)/host/libchefui-host.a: $(patsubst %.c,$(B)/host/%.o,$(HOST_LIB_SRCS))
	rm -f $@; $(HOST_AR) rcs $@ $^

$(B)/host/chefui-demo: $(B)/host/demo.o $(B)/host/libchefui-host.a $(B)/host/liblvgl.a
	$(HOST_CC) -Wl,--gc-sections -o $@ $^ $(SDL_LIBS) -lm

$(B)/host/libchefkit.a: $(patsubst %.c,$(B)/host/%.o,$(KIT_SRCS) $(SYS_SRCS) $(HOST_NETBUS))
	rm -f $@; $(HOST_AR) rcs $@ $^

$(B)/host/chef-settings: $(patsubst %.c,$(B)/host/%.o,$(APP_SRCS) apps/settings/main.c) $(B)/host/libchefkit.a $(B)/host/libchefui-host.a $(B)/host/liblvgl.a
	$(HOST_CC) -Wl,--gc-sections -o $@ $^ $(SDL_LIBS) $(SDBUS_LIBS) -lm -pthread

host: check-lvgl
	@pkg-config --exists sdl2 || { echo "SDL2 development files missing: install libsdl2-dev" >&2; exit 1; }
	$(MAKE) $(B)/host/chefui-demo $(B)/host/chef-settings
	@echo "run: $(B)/host/chefui-demo [-a] [-r 90] ...  (P = power, Up/Down = volume)"

# ---------------------------------------------------------------- test

T := $(B)/test
TEST_CFLAGS := $(CHEF_WARN) -g -O1 $(SAN) $(INC)

$(T)/test-copy: tests/test_copy.c copy.c copy.h tests/check.h
	@mkdir -p $(T)
	$(HOST_CC) $(TEST_CFLAGS) -o $@ tests/test_copy.c copy.c

$(T)/test-plan: tests/test_plan.c plan.c plan.h copy.h tests/check.h
	@mkdir -p $(T)
	$(HOST_CC) $(TEST_CFLAGS) -o $@ tests/test_plan.c plan.c

$(T)/test-touch: tests/test_touch.c touch.c copy.c touch.h copy.h ../evdev.h tests/check.h
	@mkdir -p $(T)
	$(HOST_CC) $(TEST_CFLAGS) -o $@ tests/test_touch.c touch.c copy.c -Wl,--wrap=ioctl

$(T)/test-buttons: tests/test_buttons.c buttons.c buttons.h tests/check.h
	@mkdir -p $(T)
	$(HOST_CC) $(TEST_CFLAGS) -o $@ tests/test_buttons.c buttons.c

$(T)/test-fbscreen: tests/test_fbscreen.c fbscreen.c fbscreen.h ../fbdev.h tests/check.h
	@mkdir -p $(T)
	$(HOST_CC) $(TEST_CFLAGS) -o $@ tests/test_fbscreen.c fbscreen.c -Wl,--wrap=ioctl

# The fbdev library + LVGL (no SDL), sanitized chefui sources, wrapped ioctl.
$(T)/chefui/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(TEST_CFLAGS) $(LV_DEFS) -c -o $@ $<

$(T)/test-chefui: tests/test_chefui.c tests/check.h $(patsubst %.c,$(T)/chefui/%.o,$(DEV_LIB_SRCS)) $(T)/liblvgl.a
	$(HOST_CC) $(TEST_CFLAGS) $(LV_DEFS) -o $@ tests/test_chefui.c \
		$(patsubst %.c,$(T)/chefui/%.o,$(DEV_LIB_SRCS)) $(T)/liblvgl.a -lm -Wl,--wrap=ioctl

# System UI: kit, sources, settings and app on fakeloop (no chefui loop,
# no display hardware). Sanitized like the rest.
SYSUI_OBJS := $(patsubst %.c,$(T)/chefui/%.o,$(KIT_SRCS) $(SYS_SRCS) $(APP_SRCS))
FAKELOOP := tests/fakeloop.c tests/fakeloop.h tests/check.h

$(T)/test-settings: tests/test_settings.c $(FAKELOOP) $(SYSUI_OBJS) $(T)/liblvgl.a
	$(HOST_CC) $(TEST_CFLAGS) $(LV_DEFS) -Itests -o $@ tests/test_settings.c tests/fakeloop.c \
		$(SYSUI_OBJS) $(T)/liblvgl.a -lm -pthread -Wl,--wrap=mkdir,--wrap=fsync,--wrap=rename

$(T)/test-sys: tests/test_sys.c $(FAKELOOP) $(SYSUI_OBJS) $(T)/liblvgl.a
	$(HOST_CC) $(TEST_CFLAGS) $(LV_DEFS) -Itests -o $@ tests/test_sys.c tests/fakeloop.c \
		$(SYSUI_OBJS) $(T)/liblvgl.a -lm -pthread

$(T)/test-app: tests/test_app.c $(FAKELOOP) $(SYSUI_OBJS) $(T)/liblvgl.a
	$(HOST_CC) $(TEST_CFLAGS) $(LV_DEFS) -Itests -o $@ tests/test_app.c tests/fakeloop.c \
		$(SYSUI_OBJS) $(T)/liblvgl.a -lm -pthread

# D-Bus sources against a fake NetworkManager/BlueZ on a private
# dbus-daemon (tests/test_netbus.c); needs libsystemd and dbus-daemon.
$(T)/chefui/sys/netbus.o: sys/netbus.c $(HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(TEST_CFLAGS) $(LV_DEFS) -c -o $@ $<

$(T)/test-netbus: tests/test_netbus.c $(FAKELOOP) $(SYSUI_OBJS) $(T)/chefui/sys/netbus.o $(T)/liblvgl.a
	$(HOST_CC) $(TEST_CFLAGS) $(LV_DEFS) -Itests -o $@ tests/test_netbus.c tests/fakeloop.c \
		$(SYSUI_OBJS) $(T)/chefui/sys/netbus.o $(T)/liblvgl.a -lm -pthread $(SDBUS_LIBS)

TESTS := $(T)/test-copy $(T)/test-plan $(T)/test-touch $(T)/test-buttons $(T)/test-fbscreen $(T)/test-chefui \
	$(T)/test-settings $(T)/test-sys $(T)/test-app
ifneq ($(SDBUS_LIBS),)
TESTS += $(T)/test-netbus
endif

test: check-lvgl $(TESTS)
	$(T)/test-copy
	$(T)/test-plan
	$(T)/test-touch
	$(T)/test-buttons
	$(T)/test-fbscreen
	$(T)/test-chefui
	$(T)/test-settings
	$(T)/test-sys
	$(T)/test-app
	@if [ -x $(T)/test-netbus ] && command -v dbus-daemon >/dev/null; then $(T)/test-netbus; \
	else echo "test-netbus: skipped (needs libsystemd and dbus-daemon)"; fi

# Fonts are reproducible from kit/fonts/src with the pinned tools.
.PHONY: font-check
font-check:
	@rm -rf $(B)/font-check && mkdir -p $(B)/font-check && cp -r kit/fonts $(B)/font-check/
	@$(B)/font-check/fonts/gen-fonts.sh >/dev/null
	@for f in kit/fonts/chefkit_*.c; do cmp -s $$f $(B)/font-check/fonts/$$(basename $$f) || { echo "font-check: $$f differs from a fresh generation" >&2; exit 1; }; done
	@test "$$(ls $(B)/font-check/fonts/chefkit_*.c | wc -l)" = "$$(ls kit/fonts/chefkit_*.c | wc -l)" || { echo "font-check: file set differs" >&2; exit 1; }
	@echo "font-check: $$(ls kit/fonts/chefkit_*.c | wc -l) fonts reproduce byte for byte"

clean:
	rm -rf $(B)

# Optional SDL layout/calibration suite (offscreen, no device paths).
$(B)/host/test-demo: tests/test_demo_host.c tests/check.h demo.c $(B)/host/libchefui-host.a $(B)/host/liblvgl.a
	$(HOST_CC) $(CHEF_WARN) $(OPT) $(LV_DEFS) -DCHEFUI_SDL=1 $(SDL_CFLAGS) $(INC) -o $@ $< $(B)/host/libchefui-host.a $(B)/host/liblvgl.a $(SDL_LIBS) -lm

.PHONY: host-test
host-test: $(B)/host/test-demo $(B)/host/chef-settings
	@for r in 0 90 180 270; do SDL_VIDEODRIVER=offscreen $(B)/host/test-demo $$r || exit; SDL_VIDEODRIVER=offscreen $(B)/host/test-demo $$r off-before-frame || exit; done
	@# chef-settings renders P01/P02 in both themes from the design state
	@# (build/host/captures/*.ppm, inputs for compare-ref.py)
	@mkdir -p $(B)/host/captures $(B)/host/captures/run
	@for t in dark light; do for p in hub display; do \
		SDL_VIDEODRIVER=offscreen $(B)/host/chef-settings --fake-state apps/settings/design.state \
			--theme $$t --brightness 70 --page $$p --snapshot $(B)/host/captures/$$p-$$t.ppm \
			--data $(B)/host/captures/nodata --run $(B)/host/captures/run 2>/dev/null || \
			{ echo "chef-settings capture $$p $$t failed" >&2; exit 1; }; \
	done; done
	@echo "chef-settings: 4 captures in $(B)/host/captures"
	@tests/test_settings_proc.sh $(B)/host/chef-settings apps/settings/design.state
