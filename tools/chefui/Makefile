# chefui: the UI platform (LVGL v9.6 on the MDSS fbdev) -- library,
# demo application and host tests. Specification:
# docs/next-steps/ui-platform.md; LVGL is the third_party/lvgl submodule,
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
#                 wrapped (tests/test_chefui.c)
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
INC := -I. -I.. -I$(LVGL)
OPT := -O2 -g -ffunction-sections -fdata-sections
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

LV_SRCS := $(shell find $(LVGL)/src -name '*.c' 2>/dev/null)
PURE := copy.c plan.c touch.c buttons.c fbscreen.c
HDRS := chefui.h chefui_internal.h copy.h plan.h touch.h buttons.h fbscreen.h lv_conf.h ../fbdev.h

DEV_LIB_SRCS := chefui.c backend_fb.c backends_device.c $(PURE)
HOST_LIB_SRCS := chefui.c backend_sdl.c backends_host.c $(PURE)

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

device: check-lvgl $(B)/device/chefui-demo
	@ls -l $(B)/device/chefui-demo

# ---------------------------------------------------------------- host

$(B)/host/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CHEF_WARN) $(OPT) $(LV_DEFS) -DCHEFUI_SDL=1 $(SDL_CFLAGS) $(INC) -c -o $@ $<

$(B)/host/libchefui-host.a: $(patsubst %.c,$(B)/host/%.o,$(HOST_LIB_SRCS))
	rm -f $@; $(HOST_AR) rcs $@ $^

$(B)/host/chefui-demo: $(B)/host/demo.o $(B)/host/libchefui-host.a $(B)/host/liblvgl.a
	$(HOST_CC) -Wl,--gc-sections -o $@ $^ $(SDL_LIBS) -lm

host: check-lvgl
	@pkg-config --exists sdl2 || { echo "SDL2 development files missing: install libsdl2-dev" >&2; exit 1; }
	$(MAKE) $(B)/host/chefui-demo
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

$(T)/test-touch: tests/test_touch.c touch.c copy.c touch.h copy.h tests/check.h
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

TESTS := $(T)/test-copy $(T)/test-plan $(T)/test-touch $(T)/test-buttons $(T)/test-fbscreen $(T)/test-chefui

test: check-lvgl $(TESTS)
	$(T)/test-copy
	$(T)/test-plan
	$(T)/test-touch
	$(T)/test-buttons
	$(T)/test-fbscreen
	$(T)/test-chefui

clean:
	rm -rf $(B)

# Optional SDL layout/calibration suite (offscreen, no device paths).
$(B)/host/test-demo: tests/test_demo_host.c tests/check.h demo.c $(B)/host/libchefui-host.a $(B)/host/liblvgl.a
	$(HOST_CC) $(CHEF_WARN) $(OPT) $(LV_DEFS) -DCHEFUI_SDL=1 $(SDL_CFLAGS) $(INC) -o $@ $< $(B)/host/libchefui-host.a $(B)/host/liblvgl.a $(SDL_LIBS) -lm

.PHONY: host-test
host-test: $(B)/host/test-demo
	@for r in 0 90 180 270; do SDL_VIDEODRIVER=offscreen $(B)/host/test-demo $$r || exit; SDL_VIDEODRIVER=offscreen $(B)/host/test-demo $$r off-before-frame || exit; done
