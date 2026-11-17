# Twin Turbo for Game Boy Advance
# Needs any arm-none-eabi GCC (devkitARM works, so does Ubuntu's gcc-arm-none-eabi
# + libnewlib-arm-none-eabi). Generated assets are committed in src/gen, so
# Python is only needed if you change the art or music (make assets).

PREFIX  ?= arm-none-eabi-
CC      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
TARGET  := twinturbo
BUILD   := build/obj

SRCS    := $(wildcard src/*.c) $(wildcard src/gen/*.c)
OBJS    := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS)) $(BUILD)/crt0.o

ARCH    := -mcpu=arm7tdmi -mtune=arm7tdmi -mthumb-interwork
CFLAGS  := $(ARCH) -mthumb -O2 -Wall -Wno-misleading-indentation -ffunction-sections -fdata-sections -fno-strict-aliasing -std=gnu11
LDFLAGS := $(ARCH) -mthumb -nostartfiles -T src/gba.ld -Wl,--gc-sections -specs=nano.specs -specs=nosys.specs -Wl,-Map,$(BUILD)/$(TARGET).map

.PHONY: all clean assets test

all: $(TARGET).gba

$(TARGET).gba: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@if command -v gbafix >/dev/null 2>&1; then gbafix $@ -tTWINTURBO -cATTE -mGG; else python3 tools/gbafix.py $@; fi
	@ls -l $@

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc

$(BUILD)/%.iwram.o: src/%.iwram.c
	@mkdir -p $(dir $@)
	$(CC) $(ARCH) -marm -O2 -Wall -Wno-misleading-indentation -fno-strict-aliasing -std=gnu11 -MMD -MP -c $< -o $@

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/crt0.o: src/crt0.s
	@mkdir -p $(dir $@)
	$(CC) $(ARCH) -x assembler-with-cpp -c $< -o $@

assets:
	python3 tools/gen_tables.py
	python3 tools/gen_samples.py
	python3 tools/gen_level.py
	python3 tools/gen_run.py
	python3 tools/gen_title.py
	python3 tools/gen_drive.py
	python3 tools/compose.py

-include $(OBJS:.o=.d)

clean:
	rm -rf build $(TARGET).gba

# Headless smoke test with libmgba (Ubuntu: apt install libmgba-dev)
test: $(TARGET).gba
	@mkdir -p build/shots
	$(MAKE) -C tools/harness
	tools/harness/harness $(TARGET).gba tests/smoke.txt
	tools/harness/harness $(TARGET).gba tests/title.txt
	tools/harness/harness $(TARGET).gba tests/drive.txt
