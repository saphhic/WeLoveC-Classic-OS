# WeLoveC Classic — unified documentation

Written by [@saphhic](https://github.com/saphhic)  \
Date: October 31st 2026

WeLoveC is a x86_64 UEFI operating system made by one person (me) it is still in it early stages
but its now usable, the version is on 1.7 because this OS is older than this repo.

Description: a single document containing the README guides for the WeLoveC Classic operating system.

## Contents

- [WeLoveC Classic build](#welovec-classic-build)
- [WeLoveC UEFI boot manager](#welovec-uefi-boot-manager)
- [startupd](#startupd)
- [Kernel memory management](#kernel-memory-management)
- [Processes and user threads](#processes-and-user-threads)
- [x86_64 Architecture Support](#x86_64-architecture-support)
- [ELF64 execution core](#elf64-execution-core)
- [Keyboard and pointing-device input](#keyboard-and-pointing-device-input)
- [x86_64 scheduler](#x86_64-scheduler)
- [Filesystem layer](#filesystem-layer)
- [Users, passwords, and privileges](#users-passwords-and-privileges)
- [Userland programs](#userland-programs)
- [Web browser](#web-browser)
- [WeLoveC lightweight desktop](#welovec-lightweight-desktop)

## Files kept without headers

Source code, headers, configuration files, and build scripts contain the author, date, and a short description at the top. Comments were not inserted into resources whose formats treat them as data: binary `*.dat` tables, the certificate bundle `userland/certs/ca-bundle.pem`, the font license `kernel/IDM/DisplayService/IntegratedFontService/ubuntu-font-licence-1.0.txt`, or the packaged Mbed TLS source archive in `third_party/`. The `build/` directories contain generated outputs.

- The `*.dat` files are binary snapshots of ACPI/UEFI tables: `apic.dat` describes interrupt controllers; `batb.dat`, boot options; `bgrt.dat`, boot graphics data; `cdit.dat`, component integrity resources; `crat.dat`, processor resource affinity; `dsdt.dat` and `ssdt1.dat`–`ssdt24.dat`, system description tables; `ecdt.dat`, embedded controller resources; `facp.dat` and `facs.dat`, ACPI configuration and state; `fpdt.dat`, boot performance metrics; `hpet.dat`, the high precision timer; `ivrs.dat`, I/O virtualization mappings; `mcfg.dat`, PCIe configuration; `msdm.dat`, Windows activation data; `sbst.dat`, smart battery data; `tpm2.dat`, TPM 2.0 configuration; `uefi.dat`, UEFI firmware data; `vfct.dat`, video configuration; and `wsmt.dat`, SMM mitigations.
- `userland/certs/ca-bundle.pem` contains root certificates used to verify TLS servers.
- `ubuntu-font-licence-1.0.txt` preserves the terms for the bundled Ubuntu font.
- `third_party/mbedtls/` contains the upstream Mbed TLS source archive used by the build.
- `third_party/netsurf/netsurf-all-3.11.tar.gz` contains NetSurf 3.11 and its component license notices. The browser build extracts Hubbub and ParserUtils from it.
- `build/` and `*/build/` contain generated executables and object files; the Makefiles recreate them.

## WeLoveC Classic build

*Source: `README.md`.*

Run these commands from the repository root:

```sh
make kernel   # compile and link kernel/build/kernel.elf
make demo     # build the UEFI app and populate bootloader/build/ESP/
make userland # build a ring-3 init ELF and its FAT32 userspace image
make iso      # create build/WeLoveC-UEFI.iso with xorriso
make clean
```

Building userland requires `clang`, `cmake`, `tar`, `perl`, and `sha256sum` (or
`shasum`). The pinned Mbed TLS source archive is hash-checked, built locally
for the freestanding x86-64 target, and linked into the `install` command. The
browser build extracts NetSurf's Hubbub HTML parser and ParserUtils library,
then compiles them as static freestanding libraries without Iconv.

The kernel Makefile discovers each directory containing `.c`, `.cpp`, or `.S`
sources, compiles those sources, archives each directory separately, then links
the archives into `kernel.elf`. Object paths mirror the source tree. The GOP
framebuffer display service and current Ethernet Service sources are included
in the kernel build.

The ISO target creates a FAT EFI boot image, puts the loader, kernel, boot
configuration, and FAT32 userland image in it, and asks `xorriso` to write the
UEFI-bootable ISO. Startup loads the sample ring-3 `init.elf` from that image.
User programs can use the syscall, filesystem, process, and security APIs in
`kernel/include/user/`; the process section in this document describes implemented calls and current limits. Keyboard input is initialized by startup: PS/2 is probed
directly, with USB HID and virtio-input protocol adapters under
`kernel/arch/x86_64/input/`.

The ISO build needs
`xorriso`, `mkfs.fat` (from dosfstools), and `mcopy` plus `mmd` (from mtools).
Override tool names with `XORRISO`, `MKFS_FAT`, `MCOPY`, and `MMD` if needed.

## WeLoveC UEFI boot manager

*Source: `bootloader/README.md`.*

This directory contains an x86_64 UEFI application. From this directory,
`make` writes the removable-media fallback application to
`build/EFI/BOOT/BOOTX64.EFI`. To build the complete kernel and boot image, use
the repository-root Makefile: `make kernel`, `make demo`, or `make iso`.
`make demo` assembles the loader, kernel ELF, and `boot.cfg` under `build/ESP/`.
`make iso` creates the UEFI ISO with xorriso; see the root `README.md` for the
host tools it requires.

The boot manager provides a GRUB-style text menu, automatic discovery of
configured ELF kernels, a command shell, memory-map inspection, block-device
inventory, automatic timeout boot, and reboot/poweroff commands. It discovers
filesystems through UEFI Simple File System and disks through UEFI Block I/O.
It does not parse partition tables or filesystem formats itself; those remain
UEFI firmware services during the menu and file-loading stages.

### Kernel image and entry ABI

The loader accepts little-endian ELF64 `ET_EXEC`, machine `EM_X86_64`, with
`PT_LOAD` segments. It loads each segment at `p_paddr`, clears its BSS tail,
maps it at `p_vaddr`, and jumps to `e_entry`. Physical segment ranges must be
below 512 GiB, page ranges must not overlap, and `p_vaddr` and `p_paddr` must
have matching page offsets. Virtual addresses must be canonical. Keep load
segments on separate page boundaries so their permissions can remain distinct.
The entry point must be inside an executable load segment. Dynamic ELF,
relocations, compressed kernels, and kernel signature verification are not
supported. Secure Boot can validate the EFI application only if it is signed
and trusted by firmware; this loader does not authenticate the ELF kernel.

On x86_64, the loader builds page tables matching the active firmware mode
(four or five levels), identity maps UEFI-described RAM below 512 GiB and
aliases that RAM at `0xffff800000000000`, maps ELF virtual segments with their
ELF permissions, enables NX when the CPU supports it, enables CR0.WP, calls
`ExitBootServices`, then jumps to the kernel. MMIO ranges are left unmapped so
the kernel can choose cache attributes when it maps devices. The GOP framebuffer
is an exception: its pages are identity/direct mapped with write-combining via
PAT when the CPU supports it, with an uncached fallback for early kernel output.
RAM mappings are read/write and non-executable when NX is
available, except pages shared with ELF segments or the transition stub, which
follow those pages' permissions in both the identity and kernel virtual mappings.
The kernel can discard or replace these mappings after it establishes its own
address space. `WelovecBootInfo.framebuffer.base` remains a physical address;
the early console uses the direct-map alias supplied by this loader.

The transition stub installs a flat GDT and an IDT whose entries halt on
exceptions, clears FS/GS base state, and enters the kernel with interrupts
disabled. The kernel should install its own GDT/IDT before enabling interrupts.

The handoff is `WelovecBootInfo` from `include/boot_protocol.h`. Its pointer is
passed in `RCX` using the UEFI x64 calling convention, matching the current
`kernel/arch/x86_64/boot/kernel_entry.S` entry stub. Handoff pointers are
physical addresses. RAM-backed handoff data and the GOP framebuffer are
identity mapped; other MMIO stays unmapped. Version 1 carries:

- flags indicating UEFI handoff, paging mode, NX support, installed GDT/IDT,
  and that interrupts are disabled;

- UEFI memory-map address, byte size, descriptor size, descriptor version, and
  descriptor count;
- framebuffer base, size, resolution, pixel format, scanline stride, and pixel
  masks for bit-mask GOP modes;
- ACPI RSDP and SMBIOS entry-point physical addresses when firmware publishes
  them;
- loaded kernel segment ranges and ELF permissions;
- optional initrd module, command line, detected block-device metadata, and 32
  bytes from the UEFI RNG or the CPU RDSEED/RDRAND fallback when available;
- page-table root and level count, plus kernel physical bounds.

The kernel owns the memory map after `ExitBootServices`; UEFI boot services
must not be called. Firmware runtime services are not mapped or exposed in the
handoff. The boot protocol is versioned and sized so future fields can be
appended without changing version 1 layouts.

### Menu and shell

Use Up/Down and Enter to select a kernel. `S` opens the shell, `M` prints the
memory map, `D` lists block devices, and `Q` leaves the application. Shell
commands are `help`, `kernels`, `disks`, `mem`, `map`, `boot N`, `reboot`,
`poweroff`, `clear`, and `exit`.

The shell's memory commands report UEFI descriptors, loader allocations, and
boot-services memory before handoff. The kernel should reserve loader pages
that contain the handoff, page tables, kernel segments, command line, or initrd;
it can reclaim boot-services memory after `ExitBootServices`. The shell does
not manage memory on behalf of the kernel.
Storage detection reports UEFI Block I/O media and filesystem-backed volumes;
it does not mount or modify disks.

### Current repository status

The kernel is compiled by `kernel/Makefile` into per-directory archives and
linked with `minimal-kernel.ld`. The root Makefile connects that ELF to this
loader and the ISO image. `kernel/IDM/EthernetService` is excluded by default
until its missing headers and source dependencies are resolved. Larger kernels
can use their own linker setup as long as the resulting ELF follows the ABI above.

## startupd

*Source: `kernel/StartupL/README.md`.*

Before startupd runs, `startup.c` initializes security and checks whether the
account database contains an administrator. On first boot it opens the kernel
account setup screen, which accepts a username and masked password. User or
password validation errors appear in red below the password field. Once the
administrator is stored successfully, startup continues; if no keyboard is
available or account creation fails, startup stops before launching programs.

After security, the filesystem, and optional networking initialize, `startupd`
looks for a configuration in `/bin` and `/userspace/bin`. It checks
`startup.conf` first; otherwise it selects a `.conf` from `/bin`, then from
`/userspace/bin`.

A configuration starts a static ELF64 x86-64 program with guest credentials:

```ini
# /userspace/bin/startup.conf (or /bin/startup.conf)
exec=/userspace/bin/shell.elf
```

`binary=/absolute/path/to/program.elf` is also accepted. Blank lines, comments
starting with `#`, and unrelated `key=value` settings are ignored. If the
configuration is absent, invalid, or its program cannot be loaded, startupd
tries `/bin/init.elf` and `/userspace/bin/init.elf` in that order. If neither
can be started, the kernel remains idle; an ELF program must be installed at
one of those paths to enter user space.

The bundled `init.elf` starts as `guest`, checks VFS access through shared
`/tmp`, and prompts for an account login. It starts the configured shell only
after successful authentication, so the shell inherits that user's identity.
The shell starts in `/userland/tmp`; `/kernel`, `/bootloader`, `/userland`, and
`/userspace` are sibling paths. If the shell exits, init restarts it in the
same authenticated session.

The FAT32 volume is mounted at `/userspace`; `/bin` belongs to the temporary
RAM filesystem for the current boot.

## Kernel memory management

*Source: `kernel/arch/common/MM/README.md`.*

`memory_manager_initialize()` consumes the versioned UEFI handoff before the
kernel enables interrupts. It initializes the active x86_64 page-table view,
the physical frame allocator, kernel virtual-memory arenas, and kernel heap.
The current minimal kernel then halts; there is not yet a scheduler.

### PMM

`PMM` tracks 4 KiB frames with free and allocation bitmaps placed in a
conventional-memory run selected from the UEFI map. It can allocate one frame
or aligned contiguous runs and rejects freeing reserved or already-free
frames. It makes UEFI Conventional Memory and Boot Services memory available
after `ExitBootServices`, reserves the first MiB, and leaves EFI Loader memory
reserved so the handoff, kernel image, initrd, and active page tables remain
valid. It records the free frame count after those reservations; user `mmap`
uses that boot-time capacity as its per-call ceiling while still allocating
from the current free list. Memory above the loader's 512 GiB direct-map window
is not managed yet.

The allocator metadata size is proportional to the highest managed physical
address. It uses two bits per frame across separate bitmaps so reserved frames
cannot be accidentally freed through the public API.

### Paging and VMM

The x86_64 paging layer validates the active four- or five-level root from
`WelovecBootInfo`, translates addresses, maps/unmaps/protects 4 KiB pages, and
splits inherited 1 GiB or 2 MiB mappings when a 4 KiB mapping is needed. It
reclaims page-table frames it created once their tables become empty. ELF
permissions are honored, NX is used when enabled by the bootloader, and the
direct map stays supervisor-only.

The kernel VMM owns two ranges within one shared higher-half page-table slot:
one for PMM-backed kernel pages, and one for physical mappings. `map_physical()`
defaults to uncached mappings for MMIO; `unmap_physical()` requires the exact
returned pointer and requested size. `AddressSpace` can create isolated
user-half roots while sharing the kernel mappings, allocate owned anonymous
pages, switch CR3, and destroy its user mappings. No process or scheduler uses
it yet.

### Kernel heap

`Heap/heap.hpp` provides `kmalloc`, `kzalloc`, `kcalloc`, `krealloc`, and
`kfree`, plus C++ namespace APIs and usage statistics. Requests up to 2048
bytes use size-class slab pages; larger requests use page-backed VMM mappings.
Allocations are 16-byte aligned, zero-size allocations return null, and
`krealloc` preserves the original requested byte count. Empty slab pages are
recycled while each cache keeps one available page. The heap initializes after
VMM and serializes operations with an IRQ-saving lock.

This supplies the core kernel allocation API, not Linux allocator parity: it
does not implement GFP/reclaim policies, per-CPU caches, NUMA, guard pages, or
memory-pressure reclaim. The global lock and bounded VMM mapping metadata are
intended for this kernel's bring-up stage and will need to grow with workload
and SMP support.

Physical addresses use `PhysicalAddress`; pointers into RAM use the bootloader's
`0xffff800000000000` direct map. MMIO must go through `map_physical()` rather
than `direct_map()`.

## Processes and user threads

*Source: `kernel/arch/common/Process/README.md`.*

The process manager owns a user page-table root and tracks process and thread
lifetime. It is initialized after memory and the scheduler, before
`scheduler_start()`. Kernel threads use process ID zero. User threads in one
process share an address space and each receive a private 64 KiB stack with an
unmapped guard page. The page at `0x10000` contains the thread-return stub.

Kernel code can create processes, map zeroed user pages, copy program images,
apply page permissions, and start threads. ELF entry stacks use the SysV
`argc`/`argv` layout. The C manager API is in `manager.h`.

### User ABI

User programs enter through `SYSCALL`. The ABI and inline wrappers live in
`include/user/syscall.h`; filesystem and process structures are in
`include/user/filesystem.h` and `include/user/process.h`. Implemented calls
cover process/thread exit and creation, yield/sleep, identity and capabilities,
ELF exec, account queries, ticks, console output, file descriptors, anonymous
memory, child wait/query, and process termination.

Descriptors 0, 1, and 2 are keyboard input, stdout, and stderr. Open file
descriptors are process-local, limited to 29 files per process and 128 across
the kernel. `read`, `write`, and `seek` operate on these descriptors. Writable
paths are restricted to `/home/user` and `/userspace`; writing elsewhere
requires the system-configuration capability. VFS file I/O is limited to
16 MiB per operation.

`welovec_mmap()` creates zeroed read/write, non-executable memory, up to the
free-page capacity reported from the boot-time UEFI memory map. If that capacity
is unavailable, the per-call ceiling falls back to 500 MiB. Allocation still
uses current free physical pages and can fail after startup allocations consume
memory. `welovec_munmap()` requires the exact address and length of a mapping.
`welovec_waitpid()` waits for a child or returns immediately with
`WELOVEC_WAIT_NOHANG`. Process query is limited to the caller, its children, or
callers with process-management capability. `welovec_kill()` supports signal 0
and signals interrupt (2), kill (9), and terminate (15). User handlers for
interrupt and terminate can be installed with `welovec_sigaction()`; the
kernel enters them at a syscall return boundary and restores the saved
registers when the handler returns. Signal masks and `siginfo` are not
implemented. A signal queued for another process is observed when that process
next enters the kernel; sleeping or blocked calls are not interrupted yet.

The ELF loader is in `arch/x86_64/exec/elf_loader.h`; it loads static x86-64
ELF `ET_EXEC` images from the VFS. User-facing exec, account, and identity
functions are in `include/user/security.h`; privileged operations are checked
against process capabilities. PIE and dynamic linking are not implemented.

## x86_64 Architecture Support

*Source: `kernel/arch/x86_64/README.md`.*

Architecture support for the **x86_64** (64-bit) platform.

Written by [@saphhic](https://github.com/saphhic)  
Date: 29 July 2026

### Overview

This directory contains the x86_64 port of the kernel.  
It is designed for **UEFI** systems and includes support for the **Limine** bootloader as well as Multiboot.

### Features

----- Added July 26th ------
- I/O ports
- CPU Instructions
- Control Registers
- Model Specific Registers
- CPU Discovery
- Memory Ordering
- RFLAGS
- Interrupt Control
- Syscall handler
- Syscall Functions
----------------------------

### License

This Directory is under GNU General Public License v3.0 Released on 2007.
The license is as follows:

-----------------------------------------------------------------------
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, version 3 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
-----------------------------------------------------------------------

To see the full license check the [GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.html) file.

### Note [IMPORTANT]

In the boot folder when bootloader is added a boot.asm file is needed, and in the mm folder goes functions such as paging or vmm, keep this in mind. when adding something to x86_64.

The x86 is written in C++, theres no specific reason as to why, i (saphhic) am just used to write files in C++ so all of the C++ files CAN be changed to C files whitout breaking anything, this is because none of these scripts contains C++ memory maneging functions.

The Assembler code is writen in GAS sintax, in the future if you reading this, plan to make a linker script or a Makefile for the whole kernel or OS keep that in mind.

### Contact

If you encounter any issues whit my code feel free to change it, fix it or directly contact me,
i would aprecieate a lot the feedback!

--------------------------------------------------------
- Email: <thbichosecundario@gmail.com>
- Github: [@saphhic](https://github.com/saphhic)
- Project Repository: (https://github.com/Dynt-Project)
--------------------------------------------------------

## ELF64 execution core

*Source: `kernel/arch/x86_64/exec/README.md`.*

The kernel exposes `welovec_exec_launch_elf64()` in `elf_loader.h`. It reads an
executable through the VFS, validates its ELF headers, creates a process/address
space, loads its segments, applies page permissions, prepares the initial user
stack, and queues the entry thread with the scheduler. It does not run the
program synchronously; the scheduler starts it when scheduling is enabled.

The first loader supports static, little-endian x86-64 ELF `ET_EXEC` images.
It rejects PIE (`ET_DYN`), interpreters and dynamic sections, TLS, executable
stacks, writable-and-executable segments, and `PT_LOAD` segments that overlap
the same virtual page. The loader accepts files up to 64 MiB and images up to
128 MiB of mapped pages. Load pages are writable and non-executable only while
the kernel copies them; final permissions are applied before the thread is
queued. The thread enters at `e_entry` with a SysV-style initial stack containing
`argc`, `argv`, an empty environment, and auxiliary vector entries.

The C syscall wrappers for user binaries are in `include/user/syscall.h` and
`include/user/security.h`. Programs can launch an ELF with inherited
credentials, launch an admin process when they already hold the admin
capability, or start a process as a registered user when they hold the user
management capability.

Kernel code can launch an ELF after filesystem, paging, scheduler, process
manager, and security initialization:

```c
#include "arch/x86_64/exec/elf_loader.h"

const char *args[] = { "/userspace/demo.elf", "hello" };
struct welovec_exec_options options = { 2, args, 1 };
struct welovec_exec_result result;
enum welovec_exec_error error = welovec_exec_launch_elf64(
    args[0], &options, &result);
```

Trusted kernel startup code can use `welovec_exec_launch_elf64_as_admin()` to
provision a root shell. User processes cannot invoke that kernel function
directly; their admin launch requests are checked by the syscall dispatcher.

If `options` is `NULL`, or `argument_count` is zero, `argv[0]` defaults to the
executable path. Use the process manager API to query, terminate, and destroy a
process after it exits.

## Keyboard and pointing-device input

*Source: `kernel/arch/x86_64/input/README.md`.*

Keyboard transports normalize keys to USB HID usages and feed the shared
character queue. Pointing-device transports normalize motion and button state
to `welovec_pointer_event`; applications poll those records with
`welovec_pointer_poll()` from `user/input.h`. Relative events carry movement,
wheel and button state. Absolute devices carry coordinates normalized to
0..65535. Events identify the transport that generated them.

### Driver layers

- `ps2_keyboard.cpp` configures the keyboard on i8042 port 1; the shared
  controller interface in `ps2_controller.hpp` serializes commands and routes
  IRQ/polled bytes. `ps2_mouse.cpp` probes AUX/port 2, identifies Synaptics
  touchpads using the standard PS/2 handshake, requests their relative mode,
  and decodes standard packets plus common four-byte wheel/five-button IDs.
  Native absolute Synaptics packets and PS/2 pass-through ports are not yet
  implemented.
- `xhci.cpp` discovers xHCI controllers through PCI and enumerates directly
  connected USB HID boot keyboards/mice, generic relative HID pointers and
  standard HID multitouch touchpads. It configures multiple supported HID
  interfaces on one composite USB device, such as a mouse that also exposes a
  keyboard interface. The touchpad report-descriptor parser
  reads contact IDs, tip state, X/Y and buttons, then provides one-finger
  pointer motion, two-finger scrolling, and one/two-finger tap clicks. USB
  EP0 enumeration performs one xHCI soft retry on a USB transaction error.
  USB hubs, hot-plugging and vendor-specific report layouts are not implemented.
- `virtio_input.cpp` discovers modern-capability virtio-input PCI devices,
  checks the event capabilities before binding, and handles keyboard events,
  relative mouse axes/buttons and absolute tablet/touchpad axes. Absolute axis
  ranges come from the device configuration.
- `mouse.cpp` is the common bounded event queue and source registry. The
  `SYS_POINTER_POLL` syscall copies events safely into a user process, and
  `SYS_POINTER_SOURCES` reports transports detected during startup.
- `pci/` serializes legacy PCI configuration mechanism 1 access. It does not
  yet read PCIe ECAM through ACPI MCFG. `mm/dma/` allocates contiguous DMA
  regions below 4 GiB for device rings and buffers.

`keyboard::initialize()` probes PS/2, virtio-input, xHCI and ACPI-described
I²C-HID devices during startup. Supported devices are registered automatically;
no mouse vendor selection is required. Startup prints the detected pointer
transports and separate USB/ACPI probe results, including the ACPI table stage
that failed when firmware data cannot be read.

### Current scope

This covers standard PS/2 relative devices and a Synaptics relative-mode
fallback, USB HID boot mice, generic relative USB HID pointers, USB HID
multitouch touchpads, virtio-input pointing devices and standard HID-over-I²C
touchpads described by ACPI on AMD AMDI0010/AMD0010 and Intel Serial IO
DesignWare controllers (INT33C2/INT33C3/INT3432/INT3433 and compatible INTC IDs).
The I²C matcher accepts generic PNP0C50 IDs and common ELAN, Synaptics,
Goodix and ALPS IDs, then verifies the HID descriptor before binding. The ACPI
reader accepts string IDs and compressed EISA integer IDs in `_HID`/`_CID`,
including static properties added through an unambiguous AML `Scope` and
literal I²C resource buffers returned by a scoped `_CRS` method. It
validates table checksums and resource bounds, and every controller wait is
finite. It reads static integer returns from `_DSM` when present and verifies
candidate descriptor registers against the HID-over-I²C descriptor before
use; it does not evaluate arbitrary AML. If firmware leaves the address blank
in a static I²C resource template, the driver probes that controller's legal
7-bit or 10-bit addresses and binds only after validating the HID descriptor
and touchpad report. If `_CRS` cannot be resolved statically, it also probes
7-bit addresses on recognized DesignWare controllers; it binds only after
both descriptors validate, without executing the firmware's AML method. AMD
and Intel DesignWare controller
matching accepts newer compatible ACPI IDs, then checks the controller's
component signature before binding. Seven-bit and ten-bit I²C resource
addresses are supported.
Firmware timing values are reused on both AMD and Intel DesignWare
controllers; AMD timing is derived from a 150 MHz clock only when firmware
left those registers uninitialized. Both are configured for I²C master mode,
and the HID reset-complete input packet is polled for up to five seconds
before reading the report descriptor. The reset acknowledgement is read as
the protocol's two-byte zero-length field.
I²C reports are polled because GPIO interrupt routing is not implemented yet;
polling backs off on repeated bus errors but continues retrying. Dynamic AML
methods, other I²C-controller families, vendor-specific report formats, PS/2
Synaptics absolute/pass-through mode, ALPS/Elantech extensions, USB hubs and
hot-plugging remain unsupported. Bluetooth input is intentionally excluded.

The integrated AMD I²C path builds successfully but has not been exercised on
the ThinkPad L14 Gen 5 hardware. Firmware differences in its ACPI namespace,
power resources or HID report descriptor may still need adjustment. Startup
prints the I²C probe result to distinguish ACPI discovery, controller startup
and HID descriptor/report failures.

Keyboard text conversion remains the fixed US QWERTY layout.

## x86_64 scheduler

*Source: `kernel/arch/x86_64/scheduler/README.md`.*

The scheduler provides preemptive kernel and user threads with four MLFQ priority
levels, FIFO queues per level, time-slice demotion, periodic priority boosts,
tick-based sleep, explicit block/wake, yield, and deferred reclamation of
finished task stacks. Thread stacks are 16 KiB and allocated from the kernel
heap. The timer saves and restores x87/SSE state with FXSAVE/FXRSTOR.

Call `scheduler_initialize()` after the memory manager is ready. Create
threads with `scheduler_create_kernel_thread()` before or after
`scheduler_start()`. Returning from a thread entry exits that thread. The
boot thread becomes the idle context and the PIT supplies 100 scheduler ticks
per second. `scheduler_sleep(n)` sleeps for `n` ticks.

The current implementation is single-CPU and uses the legacy PIC and PIT. User
tasks use the process manager in `arch/common/Process`, switch CR3 on context
switch, and use per-thread kernel stacks for interrupts and syscalls. It does
not migrate tasks across CPUs, save AVX/XSAVE state, or implement Linux's full
wait queues and resource policies.

## Filesystem layer

*Source: `kernel/filesystem/README.md`.*

The kernel initializes the block-device registry, probes for a FAT32 volume,
and then initializes the VFS from `kernel_main`. The in-memory VFS stays at
`/`; a detected FAT32 volume is mounted at `/userspace`.

`/tmp` is a shared, boot-local directory. Guest and authenticated user
processes can read, create, replace, and remove files there. It has no per-file
ownership, so do not store private data in `/tmp`. Account homes remain private
to their authenticated user, and `/userspace/system` remains administrator-only.

### Devices and mounting

`arch/common/Block/block_device.hpp` defines the kernel block-device interface.
The bootloader can pass an optional FAT32 image as a module with
`fat32=\EFI\WELOVEC\userspace.img` in `boot.cfg`. That image is backed by the
loaded module's RAM, so writes last for the current boot only. The kernel also
probes legacy ATA/IDE disks using the x86-64 PIO driver. FAT32 auto-detection
checks raw volumes, MBR primary/logical partitions, and GPT partitions.

The volume parser checks the FAT32 cluster-count range, BPB geometry, FAT
capacity, active FAT selection, mirroring flags, and FSInfo signatures before
mounting. FAT entries are mirrored when enabled, and FAT32's reserved high
bits are preserved when an entry changes.

### FAT32 operations

The implementation supports chained file reads and writes, cluster allocation
and freeing, directory growth, empty-directory removal, file removal and file
moves, FAT mirroring, FSInfo updates, 8.3 aliases, and VFAT long names encoded
as UTF-16 (paths use UTF-8). GPT headers and partition tables are CRC checked.
FAT32 has no journal, so an interrupted write can still leave metadata needing
repair.

Use the existing VFS calls for text paths. For binary reads, use
`vfs_read_file()` and release its result with `vfs_free_file()`. The legacy
`read_file()` returns a borrowed pointer for RAM files and an allocated buffer
for FAT files; new code should use the ownership-explicit API.

### Current driver and API limits

The block interface is ready for additional drivers, but the current physical
disk driver handles legacy IDE-compatible ATA PIO only. NVMe, AHCI, virtio-blk,
USB mass storage, and kernel access through UEFI Block I/O are not implemented.
The VFS does not yet expose file descriptors, permissions, symlinks, or async
I/O. Unicode LFN data is stored correctly; non-ASCII name comparisons are
byte-exact while ASCII comparisons ignore case.

## Users, passwords, and privileges

*Source: `kernel/security/README.md`.*

The kernel keeps a process identity (UID, GID, capabilities) and an
authentication bit for its login session. New processes begin as `guest`
(UID/GID 1000, no capabilities). A successful login changes the current
process identity; logout restores guest credentials. Child processes inherit
their parent's session state.

### Accounts and first boot

The kernel always reserves `root` (UID 0) and `guest` (UID 1000) internally;
neither accepts a password login. Custom accounts have an automatically
assigned UID, a private home path `/home/<name>`, and are stored in
`/userspace/system/security0.db` and `security1.db`. The two versioned slots
carry sequence numbers and CRCs so a torn update can fall back to the last
valid copy. A corrupt database stops security initialization instead of
silently starting account setup.

Passwords must contain 7–128 bytes. The kernel stores PBKDF2-HMAC-SHA-256
hashes (600,000 iterations) with a 16-byte per-account salt, never plaintext.
When available, salt derivation is keyed by 32 bytes from the UEFI RNG or the
CPU RDSEED/RDRAND fallback in the bootloader or kernel. On machines without
either source, the kernel derives a domain-separated salt from the account UID,
database sequence, per-boot account counter, username, and CPU timestamp. That
fallback permits setup in virtual machines without a hardware RNG and keeps
salts distinct between accounts in the database. It does not provide
cryptographic unpredictability or guarantee different salts across separate
installations; a working hardware RNG is preferred. A salt is public and
exists to prevent shared precomputed password tables, not to act as a secret.
Login errors do
not reveal whether a username exists, and repeated failures trigger a
temporary per-account login delay. Unknown usernames use a separate rate-limit
bucket so they cannot lock out a real account.

Startup calls `welovec_security_initialize(boot_info)` after mounting the VFS
and checks `welovec_security_setup_required()` on every boot. If it returns 1,
the kernel initializes keyboard input and runs `welovec_create_user_setup()`
before starting user space. The setup screen asks for a username and a masked
password, validates the account requirements, and displays validation or
storage errors in red below the password field. Startup stops if setup cannot
complete. After the initial administrator is saved, startup continues
normally. User space then asks for the account name and password before
starting the shell; the shell inherits that authenticated session. The
kernel-only `welovec_security_create_initial_admin()` function
only succeeds while the database is empty and gives the first account
administrator capabilities. Later accounts must be created through the user
syscall by an authenticated account with `MANAGE_USERS`; granting administrator
capabilities also requires `EXEC_AS_ADMIN`.

```c
if (welovec_security_setup_required()) {
    /* Startup calls this screen before launching any user programs. */
    if (!welovec_create_user_setup()) return;
}
```

The account database follows the mounted FAT volume. The ISO boot image is
loaded into writable memory, so writes to that image alone do not survive a
reboot; durable accounts require mounting a persistent writable disk volume.

### User-space API

Include `user/security.h`. `welovec_login(username, password)` returns zero on
success; `welovec_check_logged_in()` checks the session bit;
`welovec_get_user_status(&status)` returns UID, GID, name, authentication state,
and the current capability mask. `welovec_user_privilege_status()` returns
that mask or zero while logged out. `welovec_logout()` drops the session.

`welovec_create_user(name, password, flags)` returns the new UID. Use flags 0
for an ordinary account or `WELOVEC_USER_CREATE_ADMIN` to create an
administrator. The kernel checks authentication and capabilities regardless
of what the caller puts in `flags`. `welovec_list_users(records, capacity)`
returns the number of accounts; pass `(NULL, 0)` to query the required count.
Records include names, UIDs/GIDs, and capabilities so `arun` can identify an
administrator before asking for the password; password hashes and salts are
never exposed.

The old passwordless `USER_ADD` and unrestricted `USER_QUERY` syscall IDs 13
and 14 are reserved and no longer handled. The account database files are
kernel-only, even for user-space administrators. Other files under
`/userspace/system` require an authenticated system administrator. Authenticated
users can access their own `/home/<name>` directory; system administrators can
access all home directories and system paths. All sessions may use `/tmp`, a
shared temporary directory without per-file ownership; its contents are not
private and do not survive reboot when backed by the RAM VFS.

`welovec_exec()` inherits the caller's credentials and session.
`welovec_exec_admin()` requires an authenticated account with
`EXEC_AS_ADMIN`. `welovec_exec_user()` requires `MANAGE_USERS`; the caller's
own identity does not change.

This implementation provides kernel-enforced process sessions and account
permissions, but it does not encrypt the account database or protect it from
someone who can modify the disk offline. Filesystem ownership metadata and
password changes are not implemented yet. PBKDF2 is CPU-hard but not
memory-hard; the work factor should be benchmarked on target hardware and the
database format can be extended when a memory-hard KDF is available.

## Userland programs

*Source: `userland/README.md`.*

The root `make iso` target builds the freestanding x86-64 user programs and puts
them in a FAT32 image mounted by the kernel at `/userspace`. `startupd` launches
`init.elf` as `guest`; init checks the VFS, prompts for the saved account's
credentials, then starts the shell configured by `userland/bin/shell/shell.conf`.
The image exposes `bootloader`, `kernel`, and `userland` as sibling paths to
`/userspace`; generated build directories are excluded. The shell starts in
`/userland/tmp` and inherits the authenticated identity. Init keeps that session
when it restarts the shell after `exit`.

Programs include headers from `kernel/include/user/`. Syscalls return a result
or a negative errno. The framebuffer, keyboard, IPv4/TCP, filesystem, process,
and account APIs are described in [SYSCALLS.md](userland/SYSCALLS.md).
The user ELF startup passes `argc` and `argv` to `userland_main`, runs
preinitializers and C/C++ constructors before it, and runs finalizers if the
program returns. The linker keeps exception tables in the executable. These
ABI pieces prepare for C++ programs; the userland C++ standard library and
exception runtime are not provided yet. See [CLANG_COMPILER.md](CLANG_COMPILER.md)
for the compiler port status.

### Shell layout

`userland/bin/shell1.2/maincmd.c` runs the terminal. Parsing, path handling, and
prompt formatting live under `shell1.2/src/` and `shell1.2/include/`. The
Makefile packages this source tree as `/userspace/bin/shell/shell.elf`, copies
its `shell.conf` and command descriptions into `/userspace/bin/shell/`, and
compiles each source in `shell1.2/external/` as its own ELF under
`/userspace/bin/shell/bin/`. `help` reads the command descriptions at runtime
and shows each `description=`.

The built-in `help`, `apps`, `reboot`, and `shutdown` are handled by the shell.
`apps` recursively lists ELF files throughout the VFS, paging directories with
`FS_LIST_RANGE`; it prints each file's absolute path. Shell command ELFs live under
`/userspace/bin/shell/bin/`, and the shell starts in the separate, initially
empty `/userland/tmp` directory. Other
commands are config entries: filesystem tools (`ls`, `cat`, `mkdir`, `rm`,
`rmdir`, `touch`, `write`, `cp`, `mv`, and `stat`), network tools (`ethernet`, `netinfo`,
`ping`, and `dns`), `install`, `time`, `exec`, and `arun`. `cd`, `pwd`, and
`exit` are config-declared shell actions. The prompt uses `user@path:>`.

`exec <path.elf> [args...]` starts a program with the current user's identity.
`arun <command> [args...]` asks for the administrator password and starts a
configured command with administrator credentials; `arun exec <path.elf>
[args...]` does the same for an ELF path. Filesystem syscalls still enforce
home-directory and system-path permissions. `arun` uses the shell's current
directory for relative paths and expands `~` to `/userland/tmp`; change
directories with the shell's built-in `cd` before invoking it.

`install https://host/program.elf [destination]` downloads an ELF over TLS 1.2
or newer. It checks the server name and certificate chain against
`/userland/certs/ca-bundle.pem`, using the kernel UTC clock and
cryptographic random syscall. The HTTPS URL must point directly to an ELF
response with status 200; redirects and chunked transfer encoding are not
supported yet. Hostnames use A records first and fall back to AAAA; bracketed
IPv6 literals such as `https://[2001:db8::1]/program.elf` are supported. The
HTTP form remains available, with a warning that its server and bytes are
unauthenticated. The installer enforces a 500,000,000-byte-per-second ceiling;
it reserves up to 500 MiB for the complete HTTP response and retries with a
250 MiB buffer if that allocation fails. The kernel derives each process's
`mmap` ceiling from free pages recorded at boot and allocates from the current
free list, so later memory use can still make a reservation fail.
The executable written to disk is
still limited to 16 MiB by the current filesystem write path. Without a
destination the installer writes to `/userspace/bin/shell/bin/`; installing
there also creates a `.conf` entry for `help`.

`install -all https://host/file.img [destination]` saves the response without
checking for ELF format or registering it as a shell command. Large files are
written in 16 MiB blocks. With no destination, authenticated users save into
their `/home/<user>/` directory and guests save into `/tmp/`. The download is
still bounded by the response buffer (500 MiB, or 250 MiB if the larger buffer
cannot be allocated). This saves an image as a file; it does not flash a disk.

The kernel configures IPv6 link-local addresses and SLAAC global addresses from
router advertisements, resolves neighbors with NDP, and carries TCP over
IPv6. DNS AAAA queries currently go through the configured IPv4 DNS server;
DHCPv6, RDNSS, scoped link-local URL zones, IPv6 extension headers, and
duplicate-address detection for the link-local address are not implemented.

TLS is implemented as a userland Mbed TLS adapter in `lib/tls/`. It connects
Mbed TLS BIO callbacks to the process-owned TCP syscalls, reads trust anchors
from the filesystem, and requires certificate verification and hostname
verification. `third_party/mbedtls/` pins the upstream 4.1.1 source archive;
the root Makefile builds its TLS, X.509, and PSA crypto libraries for the
freestanding target and links them only into `install.elf`. PSA randomness is
provided by `welovec_secure_random()`, and certificate dates use the UTC RTC
syscall.

The root Makefile discovers each immediate subdirectory of `userland/bin` as a
program. The shell's `external/` sources are explicitly excluded from the shell
ELF and linked one by one, then copied to its `bin/` directory. Configuration
files are copied while preserving their source paths. Each standalone program
defines `userland_main`; C++ programs should declare it with `extern "C"`.

## Web browser

`userland/bin/browser/browser.elf` is a windowed, text-only browser prototype.
The desktop discovers it with the other programs under `/userspace/bin`; open
it from the Start menu's Programs list. Enter a URL, use `u` to edit the
address, `r` to reload, arrows or `j`/`k` to scroll, Space/`b` to move a page,
`[`/`]` to select a link, Enter to open it, `h`/`f` to move through the
16-entry history, and `q` to end the browser process. The desktop keeps an
exited program's window visible until it is closed with the window's close
button. An optional URL argument can be passed when launching the ELF.

The browser uses the shared `userland/lib/web/` code. `url.c` parses bounded
HTTP(S) URLs and resolves relative redirects. `http.c` performs GET requests
over the process-owned TCP interface; it handles `Content-Length` and chunked
responses, limits response headers to 16 KiB, follows at most five redirects,
and refuses HTTPS-to-HTTP downgrades. The browser caps each response body at
256 KiB. `tls_client.c` provides TLS 1.2 or newer and validates the server name
and certificate chain against `/userland/certs/ca-bundle.pem`. Certificate
errors stay visible and HTTPS is never retried over HTTP. No credentials or
cookies are sent. Plain HTTP responses are marked as unencrypted in the status
line. Network failures distinguish DNS resolution, TCP connection, request
send, and response receive errors. TCP connection failures also distinguish a
missing route, missing secure kernel entropy, a rejected port, a failed SYN
send, and a handshake timeout. A timeout only means that no TCP handshake reply
arrived; it cannot tell by itself whether the server, port, or a firewall
silenced the request. HTTPS opens TCP port 443 before starting TLS. A page that
contains only HTML can still use either HTTP or HTTPS; if a server only offers
plain HTTP, its URL must begin with `http://`. TCP requires secure entropy for
sequence numbers and local ports. The kernel uses the UEFI RNG, CPU RDSEED/RDRAND,
or its VirtIO RNG driver. For QEMU, add `-device virtio-rng-pci` to the existing
VM command line and reboot; predictable random values are not used because the
same generator also protects TLS.

`html_text.c` uses NetSurf's Hubbub parser in token-handler mode with character
decoding from ParserUtils. It omits `head`, `script`, and `style`, applies simple
block and `pre` text formatting, and displays image `alt` text. It collects up
to 64 HTTP(S) links, resolves relative paths against the first valid
`<base href>` or the final page URL, and marks them with numbers in the
document; the selected link opens with Enter. Hubbub's tree builder and DOM are
not enabled, so this does not repair malformed document structure. There is no
CSS layout, JavaScript, image loading, audio/video, form submission, tabs,
bookmarks, downloads, cookies, or per-site permissions. Plain-text responses
are also displayed. The URL parser lacks IDNA and complete percent
canonicalization. The program is not sandboxed, so it has the normal user
process's filesystem and network access.

The TCP stack remains experimental: it uses stop-and-wait transmission, has no
congestion control or out-of-order buffering, and does not yet have complete
FIN handling. Runtime interoperability and packet-loss checks are still needed.
See [BROWSER_READINESS.md](BROWSER_READINESS.md) for the implementation status
and the remaining work before this prototype can be treated as a general-purpose
browser.

## WeLoveC lightweight desktop

*Source: `userland/bin/desktop/README.md`; an identical copy also existed in `userland/bin/desktop_addon/userland/bin/desktop/README.md`.*

A single desktop ELF (`/userspace/bin/desktop/desktop.elf`) shares the same
format as `userland/bin/shell`; the root Makefile discovers it automatically.
Built-in tools are separate ELFs under `/userspace/bin/<name>/<name>.elf`:
Calculator, Code, Editor, Files, Geometry, System Info, and Task Manager each
run as a user process. Code is a compact source editor with a file explorer,
line numbers, basic C/C++ syntax colors, and open/save controls. The desktop
owns the window chrome and composites each process-owned drawing surface.

### Usage
- From the shell: `desktop` (command `shell/commands/desktop.conf`)
- Boot directly: in `userland/init.c` change `/userspace/bin/shell/shell.conf`
  to `/userspace/bin/desktop/desktop.conf` (the menu Terminal still launches the shell).

### Features
Moveable and resizable windows from the bottom-right corner, minimize,
maximize, close, focus, and Tab to cycle. The shell and other ELFs run inside
windows; keyboard and mouse input follow the focused window. The Start menu
displays horizontal icons. Files uses icons by file type (`.elf`, `.conf`, and
others), and Programs lists up to six applications at
`/userspace/bin/<x>/<x>.elf`.
The taskbar includes a UTC clock. The menu launches Terminal, Files, Editor,
Code, Task Manager, System Info, Calculator, Geometry, Run..., restart, and
shutdown. Code opens as `/userspace/bin/code/code.elf`; see
[`CODE_EDITOR.md`](CODE_EDITOR.md) for its guide.

### Memory
No back buffer for the desktop: preserves state and redraws only damaged regions.
Program surfaces are allocated when opening ELFs and share an 8 MiB limit
across all windows; they are freed upon closing. Each built-in tool is its own
ELF and runs in one of these surfaces. Large editor buffers use `mmap` and are
freed when the window closes.

### Current Kernel Limitations
- `FS_LIST` returns the first 32 entries. `FS_LIST_RANGE` adds offset paging;
  the Files window still displays its first 32 entries per directory.
- No Ctrl+letter, Home/End, or Delete key handling in the keyboard layer: the editor saves using on-screen buttons.
- Fixed 16x16 text (FB_TEXT); no framebuffer reading capabilities.
