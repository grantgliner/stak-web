# STAK! for Nintendo 64 (libdragon). See README.md.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk

GBA_SRC = ../gba/src

# The browser emulator this ROM targets (EmulatorJS mupen64plus_next) can't boot
# a compressed ELF; the patched IPL3 (libdragon/) fixes its memory detection.
N64_ROM_ELFCOMPRESS = 0
N64_ROM_HEADER ?= $(CURDIR)/libdragon/ipl3_dev.z64
N64_CFLAGS += -O2 -std=gnu11 -DN64_PORT -DHOST -include stdio.h -I$(CURDIR)/src -I$(GBA_SRC) -Wno-error \
              -Wno-misleading-indentation -Wno-unused-function -Wno-unused-variable -ftrivial-auto-var-init=uninitialized $(EXTRA)

OBJS = $(BUILD_DIR)/main.o $(BUILD_DIR)/game.o $(BUILD_DIR)/render.o $(BUILD_DIR)/audio.o $(BUILD_DIR)/gen/assets_gen.o \
       $(BUILD_DIR)/physics.o $(BUILD_DIR)/pieces_gen.o $(BUILD_DIR)/tables_gen.o

all: stak64.z64

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD_DIR)/physics.o: $(GBA_SRC)/physics.iwram.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(CC) -c $(CFLAGS) -o $@ $<
$(BUILD_DIR)/%_gen.o: $(GBA_SRC)/gen/%_gen.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD_DIR)/stak64.elf: $(OBJS)
stak64.z64: N64_ROM_TITLE = "STAK!"
stak64.z64: N64_ROM_SAVETYPE = eeprom4k

assets:
	python3 tools/gen_assets.py

clean:
	rm -rf $(BUILD_DIR) *.z64

.PHONY: all clean assets
