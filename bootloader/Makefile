# Written by [@saphhic](https://github.com/saphhic)
# Date: October 30th 2026
# Description: Builds the UEFI application and prepares the bootloader image.

CLANG ?= clang
LLD_LINK ?= lld-link
BUILD := build
EFI_OUT := $(BUILD)/EFI/BOOT/BOOTX64.EFI
TARGET := x86_64-pc-win32-coff
CFLAGS := -target $(TARGET) -std=c11 -O2 -ffreestanding -fno-builtin \
          -fno-stack-protector -fno-exceptions -fno-unwind-tables \
          -fno-asynchronous-unwind-tables -fshort-wchar -mno-red-zone \
          -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Iinclude
KERNEL_ELF ?= ../kernel/build/kernel.elf

.PHONY: all demo clean
all: $(EFI_OUT)

demo: $(BUILD)/ESP/EFI/BOOT/BOOTX64.EFI \
      $(BUILD)/ESP/EFI/WELOVEC/kernel.elf \
      $(BUILD)/ESP/EFI/WELOVEC/boot.cfg

$(BUILD)/loader.obj: loader.c include/uefi.h include/boot_protocol.h ../kernel/include/boot/boot_protocol.h
	@mkdir -p $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/enter_kernel.obj: enter_kernel.S
	@mkdir -p $(BUILD)
	$(CLANG) -target $(TARGET) -c $< -o $@

$(EFI_OUT): $(BUILD)/loader.obj $(BUILD)/enter_kernel.obj
	@mkdir -p $(dir $@)
	$(LLD_LINK) /subsystem:efi_application /entry:efi_main /machine:x64 \
		/nodefaultlib /filealign:512 /out:$@ $(BUILD)/loader.obj $(BUILD)/enter_kernel.obj

$(KERNEL_ELF):
	$(MAKE) -C ../kernel

$(BUILD)/ESP/EFI/BOOT/BOOTX64.EFI: $(EFI_OUT)
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/ESP/EFI/WELOVEC/kernel.elf: $(KERNEL_ELF)
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/ESP/EFI/WELOVEC/boot.cfg: boot.cfg.example
	@mkdir -p $(dir $@)
	cp $< $@

clean:
	rm -rf $(BUILD)
