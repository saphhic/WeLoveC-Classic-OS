# GPU driver foundation

## Current scope

DisplayService now has a driver-independent GPU discovery core. It scans PCI,
binds devices present in the registry, and publishes status, capabilities, PCI
address, and BAR information without depending on the desktop. Its first
registered device is AMD Rembrandt `1002:1681`.

Rembrandt probing is deliberately passive. It reads the PCI identity, revision,
and configured BARs; it does not change registers, enable bus mastering, reset
the GPU, or map MMIO. UEFI GOP therefore retains ownership of the display, and
the current framebuffer continues to serve the console and desktop. Discovery
does not advertise native capabilities that are not implemented yet.

Framebuffer drawing transactions check for an initialized driver that provides
`PRESENT` and supports the active GOP mode. If one is available, it presents
the complete frame and keeps the GOP copy synchronized. Rasterization still
runs on the CPU against the working buffer. If there is no compatible driver,
or presentation fails, the CPU/GOP path is used. A presentation error disables
further attempts for the current boot. Rembrandt does not provide `PRESENT`
yet: its PSP may become operational, but GC/SDMA/DCN and the presentation path
are still pending, so the desktop continues to use GOP and the CPU.

Boot protocol version 2 also preserves, when UEFI supplies them, the BDF of the
device that provides GOP and the address ranges and sizes of its memory BARs.
The kernel accepts these values only when the BDF matches the detected PCI
device and the BAR bases match the current configuration. This avoids probing
BAR sizes by writing all ones during discovery.

## Layers

- `gpu.hpp` defines the service contract: scan status, device list,
  capabilities, GOP status, and generic mode and presentation operations.
- `gpu.cpp` performs a single PCI scan, binds a driver by ID, manages its
  lifecycle, and validates arguments to common calls.
- `Drivers/gpu_driver.hpp` separates PCI device lifecycle from GPU-specific
  operations.
- `Drivers/registry.cpp` is the static registry of available drivers.
- `Drivers/amd/rembrandt.cpp` binds the requested ID and reports resources.
  Its preflight scans the eleven required blobs after the VFS is mounted,
  checks each manifest SHA-256, validates firmware headers and bounds, and
  validates the PSP TOC, TA, and ASD tables. It reads IP Discovery and the tOS
  heartbeat. It does not write to the PSP: the allocator supplies CPU physical
  addresses, and there is no GPU/GART translation yet to guarantee a valid
  address for GPCOM. It does not load SoS or configure TMR.
- `pci::read_bars` interprets endpoint, bridge, and CardBus BARs without
  writing to PCI configuration space. It reports addresses and types. Sizes
  are considered valid only when UEFI provides them for the same device.
- `gpu_resource.cpp` maps validated BAR subranges as uncached MMIO, checks
  bounds, and avoids overlapping the GOP framebuffer. The VMM rejects physical
  aliases with different cache policies and allows multiple mappings of shared
  pages when they use the same policy. It also allocates and frees contiguous
  DMA buffers below 4 GiB.
- `gpu_firmware.cpp` loads VFS files up to 64 MiB and validates SHA-256 when the
  driver provides a manifest entry. It also validates the common AMD header and
  can locate PSP v2.0/v2.1 package components with size and bounds checks.
  Rembrandt also validates PSP v1 TOC/ASD and TA v1/v2 packages before touching
  the GPU.
- `third_party/firmware/amdgpu` contains Yellow Carp blobs from the
  `20260410` linux-firmware tag. `third_party/firmware/LICENSE.amdgpu` keeps
  the AMD license, and `README.md` records the source. The Makefile packages
  the blobs under `/userspace/firmware/amdgpu` and copies the license into the
  VFS.
- `tools/gpu/generate_rembrandt_firmware_manifest.py` calculates blob sizes
  and SHA-256 hashes and generates the compiled manifest. Preflight reads this
  manifest each time the service starts; transferring firmware to the GPU is
  still pending.

## Reference hardware profile

Linux identifies Rembrandt/Yellow Carp with GC 10.3.3, DCN 3.1.2, VCN 3.1.1,
SDMA 5.2.3, and MP0 13.0.3. These IP block versions guide development; the
kernel has not read them from the GPU yet. AMDGPU uses firmware per block,
including PSP packages and Yellow Carp firmware for GC, SDMA, VCN, and DMCUB.
The first bring-up should initialize PSP and validate each block's firmware
before attempting to take control of the display.

Technical references for formats and the hardware profile: [Linux APU
table](https://docs.kernel.org/6.6/gpu/amdgpu/driver-misc.html#accelerated-processing-units-apu-info),
[AMDGPU headers](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h),
and [PSP v13 selection](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/psp_v13_0.c).

## Rules for the next stage

1. Define device ownership and handoff from GOP before touching the display
   engine. Keep the GOP path available if native startup fails.
2. Implement address limits and IOMMU protection. The current allocator only
   guarantees contiguous memory below 4 GiB; it does not provide a GPU
   isolation policy.
3. Define valid GPU addresses and DMA/IOMMU isolation. The allocator's
   physical address alone is not enough to program a GPCOM ring.
4. Complete PSP 13.0.3 startup: reserve and configure TMR, prepare and
   transfer signed firmware, create GPCOM, and complete its commands with a
   timeout. The kernel currently only detects whether tOS/GPCOM is ready.
5. Implement GPU memory initialization, GC/SDMA firmware, queues, and fences in
   stages; then add modes, connectors, and native presentation.
6. Advertise a capability only after its operation works and has an error path
   that preserves the previous display.

## Extending the service

A new driver provides a `Driver` with PCI IDs, a `probe` function, and, when
ready, callbacks for initialization, shutdown, mode enumeration/configuration,
and presentation. Add it to the static array in `Drivers/registry.cpp`. Keep
`probe` read-only; hardware initialization runs separately, and the core marks
the device initialized only when it returns `WELOVEC_GPU_OK`. Startup calls the
service after mounting the VFS so future callbacks can load firmware. On
shutdown or driver failure, the service releases its registered MMIO mappings.

Mode and presentation APIs currently return `WELOVEC_GPU_UNSUPPORTED` for
Rembrandt. Device status reports whether every required blob passed hash and
structural validation. PSP flags report only passively read states; they do not
mean that the driver created a queue. System status reports GOP and the current
resolution. The device list marks which device provided GOP when UEFI exposes
that relationship. The code does not take control of the display or graphics
engine yet.

## Rembrandt firmware manifest

The generator requires eleven Yellow Carp files for PSP, graphics, SDMA, and
display: `yellow_carp_toc.bin`, `yellow_carp_ta.bin`, `yellow_carp_asd.bin`,
`yellow_carp_pfp.bin`, `yellow_carp_me.bin`, `yellow_carp_ce.bin`,
`yellow_carp_mec.bin`, `yellow_carp_mec2.bin`, `yellow_carp_rlc.bin`,
`yellow_carp_sdma.bin`, and `yellow_carp_dmcub.bin`. `yellow_carp_vcn.bin` is
added at the end as optional video firmware when available. The generated
header separates required entries with `kRembrandtRequiredFirmwareCount`.
Generate it with:

```sh
python3 tools/gpu/generate_rembrandt_firmware_manifest.py \
    third_party/firmware/amdgpu \
    --vfs-prefix /userspace/firmware \
    --output kernel/IDM/DisplayService/Drivers/amd/rembrandt_firmware_manifest.hpp
```

Blob paths in the VFS use
`/userspace/firmware/amdgpu/<filename>`. A normal build generates the manifest
and adds the blobs to the user image; the license is stored at
`/userspace/firmware/LICENSE.amdgpu`. When DisplayService starts, preflight
loads all eleven required files, verifies size, SHA-256, and the AMD header,
then frees each temporary copy. If a file is missing or does not match, the
system reports the failure and keeps GOP active. It then reads IP Discovery and
C2PMSG_81 to detect the tOS heartbeat. Valid GPU addresses, IOMMU isolation,
host-side SoS/TMR startup, queue creation, GPU memory and GC/SDMA engine
initialization, DMCUB/DCN loading, native presentation, and real hardware
testing are still missing. The local SHA-256 pins the file accepted by the
kernel but does not replace PSP hardware signature verification.
