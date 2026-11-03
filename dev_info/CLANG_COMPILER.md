# Native Clang compiler

## Goal

Provide a shell command implemented as a WeLoveC userland ELF. The command
should compile C and C++ source files into statically linked WeLoveC ELF
programs, search a documented sysroot for headers and libraries, and preserve
Clang's source-location diagnostics. The initial language modes should follow
the project build: C11 and C++20.

## Selected implementation

Use Clang and LLVM rather than implementing a C/C++ frontend from scratch. The
upstream source release selected for the port audit is LLVM 23.1.4. Clang
provides the C-family frontend and diagnostics; LLVM supplies x86-64 code
generation; LLD can link the final ELF in process. A small WeLoveC driver can
translate command-line options into the target's freestanding compile and link
settings.

The output target is initially `x86_64-unknown-elf` with the userland ABI:
`userland/start.S`, `userland/user.ld`, and the syscall headers under
`kernel/include/user/`. Programs must be static ELF64 executables with an
`_start` entry point; they cannot depend on a Linux dynamic loader.

## Porting work

Clang and LLVM need a host support layer for WeLoveC. Upstream Unix support
uses process creation, pipes, directory iteration, canonical path resolution,
memory mapping, and file metadata APIs. The current kernel exposes file,
memory, process-exec, and wait syscalls, but does not expose `fork`, pipes, a
POSIX `opendir`/`realpath` layer, or a hosted C library. The port must either
adapt LLVM's filesystem and process support to those syscalls or use an
in-process compiler pipeline with a WeLoveC virtual filesystem.

The target sysroot also needs the Clang builtin headers, C library headers and
runtime, C++ standard headers and library, C++ ABI support, and compiler
runtime routines. Current `userland/lib/freestanding/include/` contains only
a small C header set; it does not include libc++ or a complete C runtime.
Exception support additionally needs unwinding code and retained unwind data.

The user ELF startup now preserves `.preinit_array`, `.init_array`, and
`.fini_array`, calls those functions in the required order, and retains
`.eh_frame` and `.gcc_except_table` in the read-only load segment. This is
foundational ABI support only; no Clang compiler ELF or C++ standard library
has been built for WeLoveC yet.

## Command integration

The shell compiles external commands as ELF files and loads their
`commands/*.conf` descriptions from the filesystem. Add the compiler command
configuration only after the compiler ELF, sysroot layout, and linker settings
are usable together. A placeholder command would otherwise report success
without compiling source files.
