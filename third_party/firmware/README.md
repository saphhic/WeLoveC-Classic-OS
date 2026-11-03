# AMD Yellow Carp firmware

The files in `amdgpu/` are unmodified binary firmware from the
`20260410` release tag of the upstream
[linux-firmware repository](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/refs/tags/20260410/amdgpu/).
They are provided for the AMD Rembrandt / Yellow Carp device (`1002:1681`).

`LICENSE.amdgpu` is the license distributed with those firmware files. Keep it
with any copy of the blobs. The userspace FAT32 image includes the license in
its VFS firmware directory. The driver must use these files only as firmware;
the license prohibits reverse engineering, decompilation, and disassembly.

The generated Rembrandt manifest records each file's size and SHA-256. During
DisplayService startup, the Rembrandt driver checks the eleven required blobs
and their AMD common headers before any hardware initialization. It validates
the PSP TOC, TA, and ASD package tables, then reads IP Discovery and the PSP
secure-OS sign of life without writing to hardware.
It does not submit firmware or start the PSP/GPU; GPCOM, TMR, and native
initialization are still pending. These hashes verify the local copy; they do
not replace the hardware's firmware signature checks.
