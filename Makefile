# Written by [@saphhic](https://github.com/saphhic)
# Date: October 30th 2026
# Description: Coordinates builds for the kernel, bootloader, user programs, and ISO image.

SHELL := /bin/sh

BUILD_DIR := build
ISO_ROOT := $(BUILD_DIR)/iso-root
ISO_FILE := $(BUILD_DIR)/WeLoveC-UEFI.iso
ESP_IMAGE := $(BUILD_DIR)/efiboot.img
ESP_SIZE ?= 96M
USERSPACE_SIZE ?= 64M
USER_BUILD_DIR := $(BUILD_DIR)/userland
USER_INIT_ELF := $(USER_BUILD_DIR)/init.elf
USER_FBSTDIO_OBJ := $(USER_BUILD_DIR)/fbstdio.o
USER_EXECCONF_OBJ := $(USER_BUILD_DIR)/exec_conf.o
USER_TLS_BUILD_DIR := $(USER_BUILD_DIR)/tls
USER_TLS_PORT_OBJ := $(USER_TLS_BUILD_DIR)/tls_port.o
USER_TLS_CLIENT_OBJ := $(USER_TLS_BUILD_DIR)/tls_client.o
MBEDTLS_VERSION := 4.1.1
MBEDTLS_ARCHIVE := third_party/mbedtls/mbedtls-$(MBEDTLS_VERSION).tar.bz2
MBEDTLS_SHA256 := 3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c
MBEDTLS_SOURCE_DIR := $(BUILD_DIR)/third_party/mbedtls-$(MBEDTLS_VERSION)
MBEDTLS_EXTRACT_STAMP := $(BUILD_DIR)/third_party/.mbedtls-$(MBEDTLS_VERSION)-extracted
MBEDTLS_BUILD_DIR := $(USER_BUILD_DIR)/mbedtls
MBEDTLS_TLS_LIB := $(MBEDTLS_BUILD_DIR)/library/libmbedtls.a
MBEDTLS_X509_LIB := $(MBEDTLS_BUILD_DIR)/library/libmbedx509.a
MBEDTLS_PSA_LIB := $(MBEDTLS_BUILD_DIR)/tf-psa-crypto/core/libtfpsacrypto.a
MBEDTLS_LIBS := $(MBEDTLS_TLS_LIB) $(MBEDTLS_X509_LIB) $(MBEDTLS_PSA_LIB)
USERSPACE_IMAGE := $(BUILD_DIR)/userspace.img
USERLAND_DEPFILES := $(shell find $(USER_BUILD_DIR) -type f -name '*.d' -print 2>/dev/null)
USERLAND_BIN_FILES := $(shell find userland/bin -type f -print | sort)
USERLAND_BIN_DIRS := $(shell find userland/bin -type d -print | sort)
PROJECT_SOURCE_FILES := $(shell find bootloader kernel userland -type d \( -name build -o -name .git \) -prune -o -type f -print | sort)
PROJECT_SOURCE_DIRS := $(shell find bootloader kernel userland -type d \( -name build -o -name .git \) -prune -o -type d -print | sort)
# Keep the versioned sources while publishing the executable at /bin/shell.
USERLAND_SHELL_SOURCE_DIR := userland/bin/shell_experimental
USERLAND_SHELL_SOURCE_NAME := $(notdir $(USERLAND_SHELL_SOURCE_DIR))
USERLAND_PROGRAM_DIRS := $(filter-out $(USERLAND_SHELL_SOURCE_DIR),$(shell find userland/bin -mindepth 1 -maxdepth 1 -type d -print | sort))
USERLAND_PROGRAM_NAMES := $(notdir $(patsubst %/,%,$(USERLAND_PROGRAM_DIRS)))
USER_CC ?= clang
USER_CXX ?= clang++
USER_LD ?= ld.lld

userland_program_objects = \
	$(patsubst userland/bin/%.c,$(USER_BUILD_DIR)/bin/%.o,$(if $(filter shell,$(1)),$(filter-out $(USERLAND_SHELL_SOURCE_DIR)/external/%.c,$(filter $(USERLAND_SHELL_SOURCE_DIR)/%.c,$(USERLAND_BIN_FILES))),$(filter userland/bin/$(1)/%.c,$(USERLAND_BIN_FILES)))) \
	$(patsubst userland/bin/%.cpp,$(USER_BUILD_DIR)/bin/%.o,$(if $(filter shell,$(1)),$(filter $(USERLAND_SHELL_SOURCE_DIR)/%.cpp,$(USERLAND_BIN_FILES)),$(filter userland/bin/$(1)/%.cpp,$(USERLAND_BIN_FILES)))) \
	$(patsubst userland/bin/%.S,$(USER_BUILD_DIR)/bin/%.o,$(if $(filter shell,$(1)),$(filter $(USERLAND_SHELL_SOURCE_DIR)/%.S,$(USERLAND_BIN_FILES)),$(filter userland/bin/$(1)/%.S,$(USERLAND_BIN_FILES))))

define USERLAND_PROGRAM_RULES
USERLAND_PROGRAM_OBJECTS_$(1) := $(call userland_program_objects,$(1))
USERLAND_PROGRAM_ELF_$(1) := $(USER_BUILD_DIR)/bin/$(1)/$(1).elf

$$(USERLAND_PROGRAM_ELF_$(1)): $$(USER_BUILD_DIR)/start.o \
	$$(USERLAND_PROGRAM_OBJECTS_$(1)) $$(USER_FBSTDIO_OBJ) \
	$$(USER_EXECCONF_OBJ) userland/user.ld
	@mkdir -p $$(dir $$@)
	$$(USER_LD) -nostdlib -static -z max-page-size=0x1000 \
		-T userland/user.ld -o $$@ $$(USER_BUILD_DIR)/start.o \
		$$(USERLAND_PROGRAM_OBJECTS_$(1)) $$(USER_FBSTDIO_OBJ) \
		$$(USER_EXECCONF_OBJ)
endef

$(foreach program,$(USERLAND_PROGRAM_NAMES),$(eval $(call USERLAND_PROGRAM_RULES,$(program))))
USERLAND_PROGRAM_ELFS := $(foreach program,$(USERLAND_PROGRAM_NAMES),$(USERLAND_PROGRAM_ELF_$(program)))
USERLAND_SHELL_COMMAND_SOURCES := $(sort $(wildcard $(USERLAND_SHELL_SOURCE_DIR)/external/*.c))
USERLAND_SHELL_COMMAND_NAMES := $(basename $(notdir $(USERLAND_SHELL_COMMAND_SOURCES)))

define USERLAND_SHELL_COMMAND_RULES
USERLAND_SHELL_COMMAND_ELF_$(1) := $(USER_BUILD_DIR)/bin/shell/external/$(1).elf
USERLAND_SHELL_COMMAND_EXTRA_$(1) := $(if $(filter install,$(1)),$(USER_TLS_CLIENT_OBJ) $(USER_TLS_PORT_OBJ) $(MBEDTLS_LIBS))

$$(USERLAND_SHELL_COMMAND_ELF_$(1)): $$(USER_BUILD_DIR)/start.o \
	$$(USER_BUILD_DIR)/bin/$$(USERLAND_SHELL_SOURCE_NAME)/external/$(1).o $$(USER_FBSTDIO_OBJ) \
	$$(USER_EXECCONF_OBJ) $$(USERLAND_SHELL_COMMAND_EXTRA_$(1)) userland/user.ld
	@mkdir -p $$(dir $$@)
	$$(USER_LD) -nostdlib -static -z max-page-size=0x1000 \
		-T userland/user.ld -o $$@ $$(USER_BUILD_DIR)/start.o \
		$$(USER_BUILD_DIR)/bin/$$(USERLAND_SHELL_SOURCE_NAME)/external/$(1).o $$(USER_FBSTDIO_OBJ) \
		$$(USER_EXECCONF_OBJ) $$(USERLAND_SHELL_COMMAND_EXTRA_$(1))
endef

$(foreach command,$(USERLAND_SHELL_COMMAND_NAMES),$(eval $(call USERLAND_SHELL_COMMAND_RULES,$(command))))
USERLAND_SHELL_COMMAND_ELFS := $(foreach command,$(USERLAND_SHELL_COMMAND_NAMES),$(USERLAND_SHELL_COMMAND_ELF_$(command)))

XORRISO ?= xorriso
MKFS_FAT ?= mkfs.fat
MCOPY ?= mcopy
MMD ?= mmd

ESP_FILES := bootloader/build/ESP/EFI/BOOT/BOOTX64.EFI \
             bootloader/build/ESP/EFI/WELOVEC/kernel.elf \
             bootloader/build/ESP/EFI/WELOVEC/boot.cfg \
             $(USERSPACE_IMAGE)

.PHONY: all kernel bootloader demo userland iso check-iso-tools clean help
all: iso

kernel:
	$(MAKE) -C kernel all

bootloader:
	$(MAKE) -C bootloader all

demo: kernel
	$(MAKE) -C bootloader demo

userland: $(USER_INIT_ELF) $(USERSPACE_IMAGE)

check-iso-tools:
	@missing=0; \
	for tool in "$(firstword $(XORRISO))" "$(firstword $(MKFS_FAT))" \
	            "$(firstword $(MCOPY))" "$(firstword $(MMD))"; do \
		if ! command -v "$$tool" >/dev/null 2>&1; then \
			printf 'Missing ISO build tool: %s\n' "$$tool" >&2; \
			missing=1; \
		fi; \
	done; \
	if [ "$$missing" -ne 0 ]; then exit 1; fi

$(filter-out $(USERSPACE_IMAGE),$(ESP_FILES)): | demo

$(ESP_IMAGE): $(ESP_FILES) | check-iso-tools demo
	@mkdir -p $(dir $@)
	rm -f $@
	truncate -s $(ESP_SIZE) $@
	$(MKFS_FAT) -F 16 -n WELOVEC $@
	$(MMD) -i $@ ::/EFI
	$(MMD) -i $@ ::/EFI/BOOT
	$(MMD) -i $@ ::/EFI/WELOVEC
	$(MCOPY) -i $@ bootloader/build/ESP/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	$(MCOPY) -i $@ bootloader/build/ESP/EFI/WELOVEC/kernel.elf ::/EFI/WELOVEC/kernel.elf
	$(MCOPY) -i $@ bootloader/build/ESP/EFI/WELOVEC/boot.cfg ::/EFI/WELOVEC/boot.cfg
	$(MCOPY) -i $@ $(USERSPACE_IMAGE) ::/EFI/WELOVEC/userspace.img

$(USER_BUILD_DIR)/start.o: userland/start.S
	@mkdir -p $(dir $@)
	$(USER_CC) -target x86_64-unknown-elf -m64 -ffreestanding -fno-pic -fno-pie \
		-mno-red-zone -c $< -o $@

$(USER_BUILD_DIR)/bin/%.o: userland/bin/%.c
	@mkdir -p $(dir $@)
	$(USER_CC) -target x86_64-unknown-elf -m64 -mcmodel=small -mno-red-zone \
		-ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra \
		-Ikernel/include -Iuserland -Iuserland/bin -std=c11 -MMD -MP -c $< -o $@

$(MBEDTLS_EXTRACT_STAMP): $(MBEDTLS_ARCHIVE)
	@if command -v sha256sum >/dev/null 2>&1; then \
		printf '%s  %s\n' '$(MBEDTLS_SHA256)' '$<' | sha256sum -c -; \
	elif command -v shasum >/dev/null 2>&1; then \
		printf '%s  %s\n' '$(MBEDTLS_SHA256)' '$<' | shasum -a 256 -c -; \
	else \
		printf 'Missing SHA-256 utility (sha256sum or shasum).\n' >&2; exit 1; \
	fi
	@mkdir -p $(dir $(MBEDTLS_SOURCE_DIR))
	tar -xf $< -C $(dir $(MBEDTLS_SOURCE_DIR))
	@touch $@

$(MBEDTLS_TLS_LIB): $(MBEDTLS_EXTRACT_STAMP) \
		userland/lib/tls/mbedtls_user_config.h \
		userland/lib/tls/tf_psa_user_config.h \
		userland/lib/freestanding/include/stdlib.h \
		userland/lib/freestanding/include/string.h
	@cmake -S "$(MBEDTLS_SOURCE_DIR)" -B "$(MBEDTLS_BUILD_DIR)" \
		-DCMAKE_SYSTEM_NAME=Generic \
		-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
		-DCMAKE_C_COMPILER="$(USER_CC)" \
		-DCMAKE_C_COMPILER_TARGET=x86_64-unknown-elf \
		-DCMAKE_C_STANDARD_INCLUDE_DIRECTORIES="$(abspath userland/lib/freestanding/include)" \
		-DCMAKE_C_FLAGS="-m64 -mno-red-zone -mcmodel=small -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables" \
		-DCMAKE_BUILD_TYPE=Release \
		-DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DGEN_FILES=OFF \
		-DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
		"-DMBEDTLS_USER_CONFIG_FILE=$(abspath userland/lib/tls/mbedtls_user_config.h)" \
		"-DTF_PSA_CRYPTO_USER_CONFIG_FILE=$(abspath userland/lib/tls/tf_psa_user_config.h)"
	@cmake --build "$(MBEDTLS_BUILD_DIR)" --target mbedtls --parallel 4
	@touch $(MBEDTLS_LIBS)

$(USER_TLS_PORT_OBJ): userland/lib/tls/tls_port.c \
		userland/lib/tls/mbedtls_user_config.h \
		userland/lib/tls/tf_psa_user_config.h $(MBEDTLS_EXTRACT_STAMP)
	@mkdir -p $(dir $@)
	$(USER_CC) -target x86_64-unknown-elf -m64 -mcmodel=small -mno-red-zone \
		-ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra \
		-Ikernel/include -Iuserland/lib/freestanding/include -Iuserland/lib/tls \
		-I"$(MBEDTLS_SOURCE_DIR)/include" \
		-I"$(MBEDTLS_SOURCE_DIR)/tf-psa-crypto/include" \
		-I"$(MBEDTLS_SOURCE_DIR)/tf-psa-crypto/drivers/builtin/include" \
		-DMBEDTLS_USER_CONFIG_FILE=\"mbedtls_user_config.h\" \
		-DTF_PSA_CRYPTO_USER_CONFIG_FILE=\"tf_psa_user_config.h\" \
		-std=c11 -MMD -MP -c $< -o $@

$(USER_TLS_CLIENT_OBJ): userland/lib/tls/tls_client.c \
		userland/lib/tls/tls_client.h \
		userland/lib/tls/mbedtls_user_config.h \
		userland/lib/tls/tf_psa_user_config.h $(MBEDTLS_EXTRACT_STAMP)
	@mkdir -p $(dir $@)
	$(USER_CC) -target x86_64-unknown-elf -m64 -mcmodel=small -mno-red-zone \
		-ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra \
		-Ikernel/include -Iuserland/lib/freestanding/include -Iuserland/lib/tls \
		-I"$(MBEDTLS_SOURCE_DIR)/include" \
		-I"$(MBEDTLS_SOURCE_DIR)/tf-psa-crypto/include" \
		-I"$(MBEDTLS_SOURCE_DIR)/tf-psa-crypto/drivers/builtin/include" \
		-DMBEDTLS_USER_CONFIG_FILE=\"mbedtls_user_config.h\" \
		-DTF_PSA_CRYPTO_USER_CONFIG_FILE=\"tf_psa_user_config.h\" \
		-std=c11 -MMD -MP -c $< -o $@

$(MBEDTLS_X509_LIB) $(MBEDTLS_PSA_LIB): $(MBEDTLS_TLS_LIB)
	@test -f $@

$(USER_BUILD_DIR)/bin/%.o: userland/bin/%.cpp
	@mkdir -p $(dir $@)
	$(USER_CXX) -target x86_64-unknown-elf -m64 -mcmodel=small -mno-red-zone \
		-ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-exceptions \
		-fno-rtti -Wall -Wextra -Ikernel/include -Iuserland -Iuserland/bin \
		-std=c++20 -MMD -MP -c $< -o $@

$(USER_BUILD_DIR)/bin/%.o: userland/bin/%.S
	@mkdir -p $(dir $@)
	$(USER_CC) -target x86_64-unknown-elf -m64 -ffreestanding -fno-pic -fno-pie \
		-mno-red-zone -c $< -o $@

$(USER_BUILD_DIR)/init.o: userland/init.c kernel/include/user/syscall.h \
		kernel/include/user/security.h kernel/include/user/filesystem.h \
		kernel/include/user/exec_conf.h
	@mkdir -p $(dir $@)
	$(USER_CC) -target x86_64-unknown-elf -m64 -mcmodel=small -mno-red-zone \
		-ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra \
		-Ikernel/include -std=c11 -MMD -MP -c $< -o $@

$(USER_FBSTDIO_OBJ): userland/lib/fbstdio.c kernel/include/user/stdio.h \
		kernel/include/user/framebuffer.h kernel/include/user/syscall.h
	@mkdir -p $(dir $@)
	$(USER_CC) -target x86_64-unknown-elf -m64 -mcmodel=small -mno-red-zone \
		-ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra \
		-Ikernel/include -std=c11 -MMD -MP -c $< -o $@

$(USER_EXECCONF_OBJ): userland/lib/exec_conf.c kernel/include/user/exec_conf.h \
		kernel/include/user/filesystem.h kernel/include/user/security.h \
		kernel/include/user/syscall.h
	@mkdir -p $(dir $@)
	$(USER_CC) -target x86_64-unknown-elf -m64 -mcmodel=small -mno-red-zone \
		-ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra \
		-Ikernel/include -std=c11 -MMD -MP -c $< -o $@

$(USER_INIT_ELF): $(USER_BUILD_DIR)/start.o $(USER_BUILD_DIR)/init.o \
		$(USER_FBSTDIO_OBJ) $(USER_EXECCONF_OBJ) userland/user.ld
	@mkdir -p $(dir $@)
	$(USER_LD) -nostdlib -static -z max-page-size=0x1000 \
		-T userland/user.ld -o $@ $(USER_BUILD_DIR)/start.o \
		$(USER_BUILD_DIR)/init.o $(USER_FBSTDIO_OBJ) $(USER_EXECCONF_OBJ)

$(USERSPACE_IMAGE): Makefile README.md $(PROJECT_SOURCE_FILES) \
		$(USER_INIT_ELF) userland/startup.conf \
		$(USERLAND_BIN_FILES) $(USERLAND_BIN_DIRS) $(USERLAND_PROGRAM_ELFS) \
		$(USERLAND_SHELL_COMMAND_ELFS) | check-iso-tools
	@mkdir -p $(dir $@)
	rm -f $@
	truncate -s $(USERSPACE_SIZE) $@
	$(MKFS_FAT) -F 32 -n WLCUSER $@
	$(MMD) -i $@ ::/userspace
	$(MMD) -i $@ ::/userspace/bin
	@set -e; for source_dir in $(PROJECT_SOURCE_DIRS); do \
		$(MMD) -i $@ "::/$$source_dir"; \
	done
	@set -e; for source_file in $(PROJECT_SOURCE_FILES); do \
		$(MCOPY) -i $@ "$$source_file" "::/$$source_file"; \
	done
	$(MCOPY) -i $@ $(USER_INIT_ELF) ::/userspace/bin/init.elf
	$(MCOPY) -i $@ userland/startup.conf ::/userspace/bin/startup.conf
	@set -e; find userland/bin -mindepth 1 -type d -print | sort | \
	while IFS= read -r source_dir; do \
		relative=$${source_dir#userland/bin/}; \
		$(MMD) -i $@ "::/userspace/bin/$$relative"; \
	done
	$(MMD) -i $@ ::/userspace/bin/shell/bin
	@set -e; for source_file in $(USERLAND_SHELL_SOURCE_DIR)/shell.conf $(USERLAND_SHELL_SOURCE_DIR)/commands/*.conf; do \
		relative=$${source_file#$(USERLAND_SHELL_SOURCE_DIR)/}; \
		$(MCOPY) -i $@ "$$source_file" "::/userspace/bin/shell/$$relative"; \
	done
	@set -e; find userland/bin -type f -print | sort | \
	while IFS= read -r source_file; do \
		relative=$${source_file#userland/bin/}; \
		$(MCOPY) -i $@ "$$source_file" "::/userspace/bin/$$relative"; \
	done
	@set -e; for program_elf in $(USERLAND_PROGRAM_ELFS); do \
		relative=$${program_elf#$(USER_BUILD_DIR)/bin/}; \
		$(MCOPY) -i $@ "$$program_elf" "::/userspace/bin/$$relative"; \
	done
	@set -e; for command_elf in $(USERLAND_SHELL_COMMAND_ELFS); do \
		name=$${command_elf##*/}; \
		$(MCOPY) -i $@ "$$command_elf" "::/userspace/bin/shell/bin/$$name"; \
	done

$(ISO_ROOT)/EFI/BOOT/BOOTX64.EFI: bootloader/build/ESP/EFI/BOOT/BOOTX64.EFI | demo
	@mkdir -p $(dir $@)
	cp $< $@

$(ISO_ROOT)/EFI/WELOVEC/kernel.elf: bootloader/build/ESP/EFI/WELOVEC/kernel.elf | demo
	@mkdir -p $(dir $@)
	cp $< $@

$(ISO_ROOT)/EFI/WELOVEC/boot.cfg: bootloader/build/ESP/EFI/WELOVEC/boot.cfg | demo
	@mkdir -p $(dir $@)
	cp $< $@

$(ISO_ROOT)/images/efiboot.img: $(ESP_IMAGE)
	@mkdir -p $(dir $@)
	cp $< $@

$(ISO_FILE): $(ISO_ROOT)/EFI/BOOT/BOOTX64.EFI \
             $(ISO_ROOT)/EFI/WELOVEC/kernel.elf \
             $(ISO_ROOT)/EFI/WELOVEC/boot.cfg \
             $(ISO_ROOT)/images/efiboot.img | check-iso-tools
	$(XORRISO) -as mkisofs -r -J -V WELOVEC_UEFI -o $@ \
		-e images/efiboot.img -no-emul-boot \
		-efi-boot-part --efi-boot-image $(ISO_ROOT)

iso: $(ISO_FILE)
	@printf 'Created %s\n' '$(ISO_FILE)'

help:
	@printf 'make kernel      Compile per-directory kernel archives and link kernel.elf.\n'
	@printf 'make demo        Build the UEFI loader and ESP directory.\n'
	@printf 'make userland    Build each userland/bin app, Mbed TLS, and the FAT32 userspace image.\n'
	@printf 'make iso         Create a UEFI-bootable ISO with xorriso.\n'
	@printf 'make clean       Remove all generated build files.\n'
	@printf 'Build tools: clang, CMake, tar, xorriso, mkfs.fat, mtools (mcopy and mmd).\n'

clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C bootloader clean
	rm -rf $(BUILD_DIR)

-include $(USERLAND_DEPFILES)
