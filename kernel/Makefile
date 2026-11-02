# Written by [@saphhic](https://github.com/saphhic)
# Date: October 30th 2026
# Description: Builds the kernel and links its object files into the kernel executable.

# Build every source-bearing directory as its own archive, then link the archives.
# Object paths mirror the source tree so moved files cannot collide by basename.

CLANG ?= clang
CLANGXX ?= clang++
KERNEL_LD ?= ld.lld
AR ?= llvm-ar
TARGET ?= x86_64-unknown-elf
BUILD_DIR ?= build
OBJ_DIR := $(BUILD_DIR)/obj
LIB_DIR := $(BUILD_DIR)/lib
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
LINKER_SCRIPT := ../bootloader/minimal-kernel.ld

KERNEL_EXCLUDE_DIRS ?=

COMMON_FLAGS := -target $(TARGET) -m64 -mcmodel=kernel -mno-red-zone \
                -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
                -fno-asynchronous-unwind-tables -fno-unwind-tables \
                -ffunction-sections -fdata-sections -Wall -Wextra \
                -I. -Iinclude
CFLAGS := $(COMMON_FLAGS) -std=c11 -fno-builtin
CXXFLAGS := $(COMMON_FLAGS) -std=c++20 -fno-exceptions -fno-rtti \
            -fno-threadsafe-statics -fno-use-cxa-atexit
ASFLAGS := -target $(TARGET) -m64 -ffreestanding -fno-pic -fno-pie -mno-red-zone

FIND_PRUNES := $(foreach dir,$(KERNEL_EXCLUDE_DIRS),-path './$(dir)' -prune -o)
KERNEL_SOURCES := $(shell find . $(FIND_PRUNES) -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.S' \) -print | sort)
KERNEL_ENTRY_SOURCE := ./arch/x86_64/boot/kernel_entry.S
KERNEL_ENTRY_OBJECT := $(patsubst ./%,$(OBJ_DIR)/%.o,$(KERNEL_ENTRY_SOURCE))
KERNEL_SOURCE_DIRS := $(sort $(patsubst %/,%,$(dir $(KERNEL_SOURCES))))

module_key = $(if $(filter .,$(1)),root,$(subst /,_,$(patsubst ./%,%,$(1))))
module_sources = $(filter-out $(KERNEL_ENTRY_SOURCE),$(foreach source,$(KERNEL_SOURCES),$(if $(filter $(1)/%,$(source)),$(if $(findstring /,$(patsubst $(1)/%,%,$(source))),,$(source)))))
module_objects = $(patsubst ./%,$(OBJ_DIR)/%.o,$(call module_sources,$(1)))
KERNEL_LIBRARIES := $(foreach dir,$(KERNEL_SOURCE_DIRS),$(LIB_DIR)/libkernel-$(call module_key,$(dir)).a)

.PHONY: all kernel clean list
all: $(KERNEL_ELF)
kernel: $(KERNEL_ELF)

$(OBJ_DIR)/%.c.o: ./%.c
	@mkdir -p $(dir $@)
	$(CLANG) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.cpp.o: ./%.cpp
	@mkdir -p $(dir $@)
	$(CLANGXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Framebuffer drawing is the CPU-only compositor's hottest path; optimize this
# module while leaving the rest of the kernel easy to debug.
$(OBJ_DIR)/IDM/DisplayService/framebufferc.cpp.o: ./IDM/DisplayService/framebufferc.cpp Makefile
	@mkdir -p $(dir $@)
	$(CLANGXX) $(CXXFLAGS) -O2 -MMD -MP -c $< -o $@

# Password KDF work is intentionally expensive; optimize this crypto code even
# in debug kernel builds so normal logins do not stall the whole machine.
$(OBJ_DIR)/security/password_hash.cpp.o: ./security/password_hash.cpp Makefile
	@mkdir -p $(dir $@)
	$(CLANGXX) $(CXXFLAGS) -O2 -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.S.o: ./%.S
	@mkdir -p $(dir $@)
	$(CLANG) $(ASFLAGS) -c $< -o $@

define module_archive_rule
$(LIB_DIR)/libkernel-$(call module_key,$(1)).a: $(call module_objects,$(1))
	@mkdir -p $$(dir $$@)
	rm -f $$@
	$$(AR) rcs $$@ $$^
endef
$(foreach dir,$(KERNEL_SOURCE_DIRS),$(eval $(call module_archive_rule,$(dir))))

$(KERNEL_ELF): $(KERNEL_ENTRY_OBJECT) $(KERNEL_LIBRARIES) $(LINKER_SCRIPT)
	$(KERNEL_LD) -nostdlib -static -z max-page-size=0x1000 --build-id=none \
		--gc-sections -T $(LINKER_SCRIPT) -o $@ $(KERNEL_ENTRY_OBJECT) \
		--start-group $(KERNEL_LIBRARIES) --end-group

$(OBJ_DIR)/arch/x86_64/boot/kernel_entry.S.o: ./arch/x86_64/boot/kernel_entry.S
	@mkdir -p $(dir $@)
	$(CLANG) $(ASFLAGS) -c $< -o $@

list:
	@printf 'Source directories built as separate archives:\n'
	@printf '  %s\n' $(KERNEL_SOURCE_DIRS)
	@printf 'Excluded source directories: %s\n' '$(KERNEL_EXCLUDE_DIRS)'
	@printf 'Libraries linked into %s:\n' '$(KERNEL_ELF)'
	@printf '  %s\n' $(KERNEL_LIBRARIES)

clean:
	rm -rf $(BUILD_DIR)

DEPFILES := $(shell find $(OBJ_DIR) -type f -name '*.d' -print 2>/dev/null)
-include $(DEPFILES)
