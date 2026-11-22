PYTHON ?= python3
OPT ?= -Os
LTOFLAGS ?= -flto
HOST_CC ?= cc
NDS32_PREFIX ?= $(CURDIR)/toolchain/nds32be-elf
NDS32_CC := $(NDS32_PREFIX)/bin/nds32be-elf-gcc
NDS32_OBJDUMP := $(NDS32_PREFIX)/bin/nds32be-elf-objdump
BUILD_DIR ?= build
OUTPUT_DIR ?= target_firmware

# N9 instruction encoding with little-endian runtime data (PSW.BE clear).
FIRMWARE_FLAGS := -mcpu=n9 -march=v2 -mno-16-bit -mreduced-regs \
	-mlittle-endian -ffreestanding -nostdlib $(OPT) -Wall -Wextra \
	-fno-delete-null-pointer-checks -fno-jump-tables -fstack-usage $(LTOFLAGS) -Iinclude
SOURCES := $(sort $(wildcard src/*.c)) src/entry.S src/independent_vectors.S
TOOLS := $(wildcard tools/*.py)

.DELETE_ON_ERROR:
.PHONY: all verbose check clean toolchain
all: $(OUTPUT_DIR)/mt7601u.bin
verbose: $(OUTPUT_DIR)/mt7601u-verbose.bin

define image
$(BUILD_DIR)/$(1)/%.o: src/%.c $(wildcard include/*.h) Makefile
	@mkdir -p $$(@D)
	$(NDS32_CC) $(FIRMWARE_FLAGS) -DINDEPENDENT_LOGGING=$(2) -c -o $$@ $$<

$(BUILD_DIR)/$(1)/%.o: src/%.S $(wildcard include/*.h) Makefile
	@mkdir -p $$(@D)
	$(NDS32_CC) $(FIRMWARE_FLAGS) -DINDEPENDENT_LOGGING=$(2) -c -o $$@ $$<

$(OUTPUT_DIR)/$(1).elf: $(addprefix $(BUILD_DIR)/$(1)/,$(addsuffix .o,$(basename $(notdir $(SOURCES))))) station.ld Makefile
	@mkdir -p $$(@D)
	$(NDS32_CC) $(FIRMWARE_FLAGS) -dumpdir $(BUILD_DIR)/$(1)/ -T station.ld -Wl,--nmagic,-Map,$$(basename $$@).map \
		-o $$@ $$(filter %.o,$$^)

$(OUTPUT_DIR)/$(1).bin: $(OUTPUT_DIR)/$(1).elf $(TOOLS)
	$(PYTHON) tools/pack_mt76_elf.py --elf $$< -o $$@
	$(PYTHON) tools/check_efuse_layout.py --elf $$< --image $$@ \
		--objdump $(NDS32_OBJDUMP) > $(BUILD_DIR)/$(1)/layout.json
	$(PYTHON) tools/check_mt7601u_stack.py --elf $$< --directory $(BUILD_DIR)/$(1) \
		--objdump $(NDS32_OBJDUMP) > $(BUILD_DIR)/$(1)/stack.json
endef

$(eval $(call image,mt7601u,0))
$(eval $(call image,mt7601u-verbose,1))

check: all verbose
	$(PYTHON) tools/check_production.py --output $(OUTPUT_DIR) --cc "$(HOST_CC)"

toolchain:
	sh toolchain/build-nds32be-elf.sh

clean:
	rm -rf -- $(BUILD_DIR) $(OUTPUT_DIR)
