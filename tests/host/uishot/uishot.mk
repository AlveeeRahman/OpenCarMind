# ESP-Mosaico screens on the Mac (included by tests/host/Makefile): LVGL 9.5 from managed_components (an S31 idf.py build
# fetches it) with the S31 build's LVGL settings (uishot/lv_host_kconfig.h), mosaico_display.c and mosaico_apps.c with
# stubs for the hardware and the firmware data (uishot/stubs, uishot/fw_stubs.c). Two targets:
#   make textfit   builds every screen in both themes with worst-case texts and fails on any text that is cut, clipped,
#                  overlapping, outside the rounded screen corners or set below 16 px
#   make uishot    the same, and writes one PNG per screen and theme to UISHOT_OUT (default uishot/out)
# The demo case (<screen>_demo_<theme>.png) uses realistic example values for the README and the manual. The other
# screens show the Mac's clock, and the status bar width follows it: UISHOT_CLOCK=HH:MM fixes it (UISHOT_CLOCK=20:08 make textfit).
LVGL_DIR  ?= ../../managed_components/lvgl__lvgl
UISHOT_OUT ?= uishot/out
UI_B       = uishot/build
UI_CFLAGS  = -std=gnu11 -O1 -g -DLV_CONF_KCONFIG_EXTERNAL_INCLUDE='"lv_host_kconfig.h"' -DLV_CONF_SKIP -DUISHOT_HOST=1 -DCARMIND_VERSION='"mossREV 2.0.0"' \
             -Iuishot -Iuishot/stubs -I$(LVGL_DIR) -I$(LVGL_DIR)/src
LVGL_SRCS  = $(shell find $(LVGL_DIR)/src -name '*.c' 2>/dev/null)
LVGL_OBJS  = $(patsubst $(LVGL_DIR)/src/%.c,$(UI_B)/lvgl/%.o,$(LVGL_SRCS))
UI_FW      = ../../main/mosaico_logos.c ../../main/mosaico_icons.c ../../main/econ.c \
             ../../main/ui_settings_logic.c ../../main/ui_events_logic.c ../../main/ui_rows_logic.c ../../main/wifi_ui_logic.c \
             ../../main/clock_set.c ../../main/power_state.c ../../main/fault_engine.c ../../main/harden.c ../../main/os_core.c ../../main/reset_class.c
UI_FW_OBJS = $(patsubst ../../main/%.c,$(UI_B)/fw/%.o,$(UI_FW))

$(UI_B)/lvgl/%.o: $(LVGL_DIR)/src/%.c uishot/lv_host_kconfig.h
	@mkdir -p $(dir $@)
	@$(CC) $(UI_CFLAGS) -w -c $< -o $@

$(UI_B)/fw/%.o: ../../main/%.c uishot/lv_host_kconfig.h $(wildcard uishot/stubs/*.h uishot/stubs/*/*.h) ../../main/mosaico_ui.h
	@mkdir -p $(dir $@)
	$(CC) $(UI_CFLAGS) -I../../main -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -c $< -o $@

uishot_bin: uishot/uishot.c uishot/uishot_apps.c uishot/fw_stubs.c ../../main/mosaico_display.c ../../main/mosaico_apps.c ../../main/mosaico_ui.h $(LVGL_OBJS) $(UI_FW_OBJS)
	@test -d $(LVGL_DIR)/src || { echo "LVGL not found at $(LVGL_DIR): run an S31 idf.py build once (it fetches managed_components)"; exit 1; }
	@$(CC) $(UI_CFLAGS) -I../../main -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
	    -o $@ uishot/uishot.c uishot/uishot_apps.c uishot/fw_stubs.c $(UI_FW_OBJS) $(LVGL_OBJS) -lz -lm

textfit: uishot_bin
	./uishot_bin --check

uishot: uishot_bin
	mkdir -p $(UISHOT_OUT)
	./uishot_bin --check --png $(UISHOT_OUT)

uishot_lvgl: $(LVGL_OBJS)
