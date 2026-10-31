// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Loads the kernel through UEFI, prepares the x86-64 environment, and transfers control to the kernel.

#include "include/uefi.h"
#include "include/boot_protocol.h"

_Static_assert(sizeof(EFI_MEMORY_DESCRIPTOR) == sizeof(WelovecMemoryDescriptor),
               "UEFI memory-map prefix must match the shared kernel ABI");

#define PAGE_SIZE 4096ULL
#define PAGE_MASK (~(PAGE_SIZE - 1ULL))
#define PT_WINDOW_SIZE (512ULL * 1024ULL * 1024ULL * 1024ULL)
#define PT_WINDOW_END (PT_WINDOW_SIZE - 1ULL)
#define DIRECT_MAP_BASE UINT64_C(0xffff800000000000)
#define MAX_FILESYSTEMS 64
#define MAX_ENTRIES 64
#define MAX_PATH 256
#define MAX_TITLE 80
#define MAX_CMDLINE 256
#define MAX_ELF_FILE (128ULL * 1024ULL * 1024ULL)
#define MAX_INITRD_FILE (512ULL * 1024ULL * 1024ULL)
#define MAX_FILESYSTEM_IMAGE (512ULL * 1024ULL * 1024ULL)
#define MAX_CONFIG_FILE (1024ULL * 1024ULL)
#define MAX_ELF_PHDRS 128
#define MAX_MEMORY_MAP_BYTES (8ULL * 1024ULL * 1024ULL)
#define MAX_PAGE_TABLE_ALLOCS 2048

#define PTE_PRESENT UINT64_C(0x001)
#define PTE_WRITE   UINT64_C(0x002)
#define PTE_WRITE_THROUGH UINT64_C(0x008)
#define PTE_CACHE_DISABLE UINT64_C(0x010)
#define PTE_LARGE   UINT64_C(0x080)
#define PTE_NX      UINT64_C(0x8000000000000000)
#define PTE_ADDR    UINT64_C(0x000ffffffffff000)
#define PDE_2M_ADDR UINT64_C(0x000fffffffe00000)

#define SCAN_UP 1
#define SCAN_DOWN 2
#define SCAN_ESC 23

EFI_GUID gEfiLoadedImageProtocolGuid = {0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
EFI_GUID gEfiSimpleFileSystemProtocolGuid = {0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
EFI_GUID gEfiBlockIoProtocolGuid = {0x964e5b21,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
EFI_GUID gEfiGraphicsOutputProtocolGuid = {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
EFI_GUID gEfiFileInfoGuid = {0x09576e92,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
EFI_GUID gEfiRngProtocolGuid = {0x3152bca5,0xeade,0x433d,{0x86,0x2e,0xc0,0x1c,0xdc,0x29,0x1f,0x44}};

typedef struct {
    EFI_HANDLE handle;
    EFI_FILE_PROTOCOL *root;
} Filesystem;

typedef struct {
    uint32_t filesystem;
    char title[MAX_TITLE];
    char kernel[MAX_PATH];
    char initrd[MAX_PATH];
    char fat32_image[MAX_PATH];
    char command_line[MAX_CMDLINE];
} BootEntry;

typedef struct {
    uint64_t address;
    uint64_t pages;
} PageAllocation;

typedef struct {
    uint64_t paddr;
    uint64_t vaddr;
    uint64_t offset;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t flags;
    uint64_t alloc_base;
    uint64_t alloc_pages;
} ElfSegment;

static EFI_SYSTEM_TABLE *gST;
static EFI_BOOT_SERVICES *gBS;
static EFI_HANDLE gImage;
static Filesystem gFilesystems[MAX_FILESYSTEMS];
static uint32_t gFilesystemCount;
static BootEntry gEntries[MAX_ENTRIES];
static uint32_t gEntryCount;
static WelovecStorageDevice gStorage[WELOVEC_BOOTINFO_MAX_STORAGE];
static uint32_t gStorageCount;
static uint32_t gDefaultEntry;
static uint32_t gTimeout = 5;
static uint8_t gNxSupported;
static uint8_t gLa57;
static uint8_t gPatSupported;
static PageAllocation gPageTables[MAX_PAGE_TABLE_ALLOCS];
static uint32_t gPageTableAllocCount;

static int memcmp_local(const void *a, const void *b, UINTN n);
static void boot_entry(uint32_t index);

static void *memset_local(void *dst, int c, UINTN n) {
    uint8_t *p = (uint8_t *)dst;
    for (UINTN i = 0; i < n; ++i) p[i] = (uint8_t)c;
    return dst;
}

static void *memcpy_local(void *dst, const void *src, UINTN n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (UINTN i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

/* The compiler may lower aggregate copies to the freestanding C ABI symbol. */
void *memcpy(void *dst, const void *src, size_t n) { return memcpy_local(dst, src, (UINTN)n); }
void *memset(void *dst, int c, size_t n) { return memset_local(dst, c, (UINTN)n); }

static int str_len(const char *s) { int n = 0; while (s[n]) ++n; return n; }
static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static int str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = (char)(y + ('a' - 'A'));
        if (x != y) return 0;
    }
    return *a == *b;
}
static int starts_with(const char *s, const char *prefix) {
    while (*prefix) if (*s++ != *prefix++) return 0;
    return 1;
}
static void str_copy(char *dst, UINTN cap, const char *src) {
    if (!cap) return;
    UINTN i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}
static int ascii_from_u16(char *dst, UINTN cap, const CHAR16 *src) {
    UINTN i = 0;
    if (!cap) return 0;
    while (src[i] && i + 1 < cap) {
        if (src[i] > 0x7f) return 0;
        dst[i] = (char)src[i];
        ++i;
    }
    dst[i] = 0;
    return src[i] == 0;
}
static void u16_from_ascii(CHAR16 *dst, UINTN cap, const char *src) {
    UINTN i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = (CHAR16)(uint8_t)src[i]; ++i; }
    dst[i] = 0;
}

static void out_u16(const CHAR16 *s) { gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)s); }
static void out(const char *s) {
    CHAR16 chunk[160];
    UINTN i = 0;
    while (*s) {
        while (*s && i < 158) chunk[i++] = (uint8_t)*s++;
        chunk[i] = 0;
        gST->ConOut->OutputString(gST->ConOut, chunk);
        i = 0;
    }
}
static void out_char(char c) { CHAR16 s[2] = {(CHAR16)(uint8_t)c, 0}; gST->ConOut->OutputString(gST->ConOut, s); }
static void out_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; ++i) buf[2 + i] = digits[(value >> ((15 - i) * 4)) & 0xf];
    buf[18] = 0; out(buf);
}
static void out_dec(uint64_t value) {
    char buf[24];
    UINTN i = 0;
    do { buf[i++] = (char)('0' + (value % 10)); value /= 10; } while (value && i < sizeof(buf) - 1);
    while (i) out_char(buf[--i]);
}
static void newline(void) { out("\r\n"); }
static void clear_screen(void) { gST->ConOut->ClearScreen(gST->ConOut); }
static void set_color(UINTN attr) { gST->ConOut->SetAttribute(gST->ConOut, attr); }

static EFI_STATUS alloc_pages_below(UINTN type, uint64_t pages, uint64_t *address) {
    *address = PT_WINDOW_END;
    return gBS->AllocatePages(type, EfiLoaderData, pages, address);
}
static EFI_STATUS alloc_any_pages(uint64_t pages, PageAllocation *a) {
    a->address = PT_WINDOW_END;
    EFI_STATUS s = alloc_pages_below(EfiAllocateMaxAddress, pages, &a->address);
    if (!EFI_ERROR(s)) a->pages = pages;
    return s;
}
static void free_pages(PageAllocation *a) {
    if (a->address && a->pages) gBS->FreePages(a->address, a->pages);
    a->address = 0; a->pages = 0;
}
static EFI_STATUS alloc_pool(UINTN size, void **p) { return gBS->AllocatePool(EfiLoaderData, size, p); }

static EFI_STATUS open_path(uint32_t fs_index, const char *path, EFI_FILE_PROTOCOL **file) {
    if (fs_index >= gFilesystemCount || !gFilesystems[fs_index].root) return EFI_NOT_FOUND;
    CHAR16 wide[MAX_PATH];
    u16_from_ascii(wide, MAX_PATH, path);
    return gFilesystems[fs_index].root->Open(gFilesystems[fs_index].root, file, wide, EFI_FILE_MODE_READ, 0);
}

static EFI_STATUS file_info(EFI_FILE_PROTOCOL *file, EFI_FILE_INFO **info_out) {
    UINTN size = 0;
    EFI_STATUS s = file->GetInfo(file, &gEfiFileInfoGuid, &size, 0);
    if (s != EFI_BUFFER_TOO_SMALL || size < offsetof(EFI_FILE_INFO, FileName) + sizeof(CHAR16)) return EFI_DEVICE_ERROR;
    EFI_FILE_INFO *info = 0;
    s = alloc_pool(size, (void **)&info);
    if (EFI_ERROR(s)) return s;
    s = file->GetInfo(file, &gEfiFileInfoGuid, &size, info);
    if (EFI_ERROR(s)) { gBS->FreePool(info); return s; }
    *info_out = info;
    return EFI_SUCCESS;
}

static EFI_STATUS read_whole_file(uint32_t fs, const char *path, uint64_t maximum, PageAllocation *allocation, uint64_t *file_size) {
    allocation->address = 0; allocation->pages = 0;
    EFI_FILE_PROTOCOL *file = 0;
    EFI_STATUS s = open_path(fs, path, &file);
    if (EFI_ERROR(s)) return s;
    EFI_FILE_INFO *info = 0;
    s = file_info(file, &info);
    if (EFI_ERROR(s)) { file->Close(file); return s; }
    uint64_t bytes = info->FileSize;
    gBS->FreePool(info);
    if (bytes > maximum) { file->Close(file); return EFI_BAD_BUFFER_SIZE; }
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!pages) pages = 1;
    s = alloc_any_pages(pages, allocation);
    if (EFI_ERROR(s)) { file->Close(file); return s; }
    uint8_t *dest = (uint8_t *)(uintptr_t)allocation->address;
    uint64_t done = 0;
    while (done < bytes) {
        UINTN chunk = (UINTN)((bytes - done) > (1024 * 1024) ? (1024 * 1024) : (bytes - done));
        UINTN got = chunk;
        s = file->Read(file, &got, dest + done);
        if (EFI_ERROR(s) || !got) {
            free_pages(allocation); file->Close(file);
            return EFI_ERROR(s) ? s : EFI_DEVICE_ERROR;
        }
        done += got;
    }
    file->Close(file);
    *file_size = bytes;
    return EFI_SUCCESS;
}

static int parse_u32(const char *s, uint32_t *value) {
    uint64_t n = 0;
    if (!*s) return 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return 0;
        n = n * 10 + (uint32_t)(*s - '0');
        if (n > UINT32_MAX) return 0;
    }
    *value = (uint32_t)n;
    return 1;
}

static void commit_entry(BootEntry *e) {
    if (e->kernel[0] && gEntryCount < MAX_ENTRIES) {
        if (!e->title[0]) str_copy(e->title, sizeof(e->title), e->kernel);
        gEntries[gEntryCount++] = *e;
    }
    memset_local(e, 0, sizeof(*e));
}

static void parse_config(uint32_t fs) {
    PageAllocation data = {0}; uint64_t size = 0;
    if (EFI_ERROR(read_whole_file(fs, "\\EFI\\WELOVEC\\boot.cfg", MAX_CONFIG_FILE, &data, &size))) return;
    char *text = (char *)(uintptr_t)data.address;
    BootEntry current;
    memset_local(&current, 0, sizeof(current));
    current.filesystem = fs;
    UINTN pos = 0;
    int saw_config = 0;
    while (pos < size) {
        char line[512]; UINTN n = 0;
        while (pos < size && text[pos] != '\n' && n + 1 < sizeof(line)) {
            char c = text[pos++];
            if (c != '\r') line[n++] = c;
        }
        if (pos < size && text[pos] == '\n') ++pos;
        line[n] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '#') continue;
        char *eq = p;
        while (*eq && *eq != '=') ++eq;
        if (!*eq) continue;
        *eq++ = 0;
        while (*eq == ' ' || *eq == '\t') ++eq;
        UINTN end = str_len(eq);
        while (end && (eq[end - 1] == ' ' || eq[end - 1] == '\t')) eq[--end] = 0;
        if (str_ieq(p, "timeout")) {
            uint32_t t; if (parse_u32(eq, &t) && t <= 60) gTimeout = t;
        } else if (str_ieq(p, "default")) {
            uint32_t d; if (parse_u32(eq, &d)) gDefaultEntry = d;
        } else if (str_ieq(p, "title")) {
            if (current.kernel[0]) commit_entry(&current);
            current.filesystem = fs;
            str_copy(current.title, sizeof(current.title), eq); saw_config = 1;
        } else if (str_ieq(p, "kernel")) {
            str_copy(current.kernel, sizeof(current.kernel), eq); saw_config = 1;
        } else if (str_ieq(p, "initrd")) {
            str_copy(current.initrd, sizeof(current.initrd), eq);
        } else if (str_ieq(p, "fat32")) {
            str_copy(current.fat32_image, sizeof(current.fat32_image), eq);
        } else if (str_ieq(p, "cmdline")) {
            str_copy(current.command_line, sizeof(current.command_line), eq);
        }
    }
    commit_entry(&current);
    free_pages(&data);
    (void)saw_config;
}

static int ends_elf(const char *s) {
    int n = str_len(s);
    return n >= 4 && s[n-4] == '.' && (s[n-3] == 'e' || s[n-3] == 'E') &&
           (s[n-2] == 'l' || s[n-2] == 'L') && (s[n-1] == 'f' || s[n-1] == 'F');
}

static void scan_directory(uint32_t fs, const char *dir_path) {
    if (gEntryCount >= MAX_ENTRIES) return;
    EFI_FILE_PROTOCOL *dir = 0;
    if (EFI_ERROR(open_path(fs, dir_path, &dir))) return;
    uint8_t info_buf[2048];
    for (;;) {
        UINTN size = sizeof(info_buf);
        EFI_STATUS s = dir->Read(dir, &size, info_buf);
        if (EFI_ERROR(s) || !size) break;
        EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
        if (info->Attribute & 0x10) continue;
        char name[128];
        if (!ascii_from_u16(name, sizeof(name), info->FileName) || !ends_elf(name)) continue;
        BootEntry e; memset_local(&e, 0, sizeof(e)); e.filesystem = fs;
        int plen = str_len(dir_path), nlen = str_len(name);
        if ((UINTN)(plen + nlen + 2) >= sizeof(e.kernel)) continue;
        str_copy(e.kernel, sizeof(e.kernel), dir_path);
        if (e.kernel[plen - 1] != '\\') e.kernel[plen++] = '\\', e.kernel[plen] = 0;
        str_copy(e.kernel + plen, sizeof(e.kernel) - plen, name);
        str_copy(e.title, sizeof(e.title), name);
        if (gEntryCount < MAX_ENTRIES) gEntries[gEntryCount++] = e;
    }
    dir->Close(dir);
}

static void discover_storage(void) {
    EFI_HANDLE *handles = 0; UINTN count = 0;
    EFI_STATUS s = gBS->LocateHandleBuffer(ByProtocol, &gEfiBlockIoProtocolGuid, 0, &count, &handles);
    if (EFI_ERROR(s)) return;
    for (UINTN i = 0; i < count && gStorageCount < WELOVEC_BOOTINFO_MAX_STORAGE; ++i) {
        EFI_BLOCK_IO_PROTOCOL *bio = 0;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &gEfiBlockIoProtocolGuid, (void **)&bio)) || !bio || !bio->Media) continue;
        EFI_BLOCK_IO_MEDIA *m = bio->Media;
        WelovecStorageDevice *d = &gStorage[gStorageCount++];
        memset_local(d, 0, sizeof(*d));
        d->media_id = m->MediaId; d->block_size = m->BlockSize; d->last_block = m->LastBlock;
        if (m->MediaPresent) d->flags |= 1u;
        if (m->RemovableMedia) d->flags |= 2u;
        if (m->LogicalPartition) d->flags |= 4u;
        if (m->ReadOnly) d->flags |= 8u;
    }
    if (handles) gBS->FreePool(handles);
}

static void print_memory_map(int verbose) {
    UINTN size = 0, key = 0, desc_size = 0; uint32_t version = 0;
    EFI_STATUS s = gBS->GetMemoryMap(&size, 0, &key, &desc_size, &version);
    if (s != EFI_BUFFER_TOO_SMALL || !desc_size) { out("UEFI no ofrece el mapa de memoria.\r\n"); return; }
    size += desc_size * 4;
    void *map = 0;
    s = alloc_pool(size, &map);
    if (EFI_ERROR(s)) { out("Sin memoria para consultar el mapa.\r\n"); return; }
    s = gBS->GetMemoryMap(&size, map, &key, &desc_size, &version);
    if (EFI_ERROR(s)) { gBS->FreePool(map); out("No se pudo leer el mapa.\r\n"); return; }
    uint64_t usable = 0, loader = 0, boot_services = 0; uint32_t count = (uint32_t)(size / desc_size);
    EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)map;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t bytes = d->NumberOfPages * PAGE_SIZE;
        if (d->Type == 7) usable += bytes; /* EfiConventionalMemory */
        if (d->Type == 1 || d->Type == 2) loader += bytes;
        if (d->Type == 3 || d->Type == 4) boot_services += bytes;
        if (verbose) {
            out("  tipo="); out_dec(d->Type); out(" inicio="); out_hex(d->PhysicalStart);
            out(" paginas="); out_dec(d->NumberOfPages); newline();
        }
        d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)d + desc_size);
    }
    out("Descriptores UEFI: "); out_dec(count); newline();
    out("Memoria convencional libre: "); out_dec(usable / (1024 * 1024)); out(" MiB\r\n");
    out("Memoria EFI Loader (reservar objetos de handoff): "); out_dec(loader / (1024 * 1024)); out(" MiB\r\n");
    out("Memoria Boot Services liberable tras ExitBootServices: ");
    out_dec(boot_services / (1024 * 1024)); out(" MiB\r\n");
    gBS->FreePool(map);
}

static void print_storage(void) {
    out("Dispositivos de bloques detectados: "); out_dec(gStorageCount); newline();
    for (uint32_t i = 0; i < gStorageCount; ++i) {
        WelovecStorageDevice *d = &gStorage[i];
        out("  Disco "); out_dec(i); out("  ");
        if (d->flags & 1) out("presente "); else out("sin medio ");
        if (d->flags & 2) out("extraible ");
        if (d->flags & 4) out("particion ");
        if (d->flags & 8) out("solo lectura ");
        out("bloque="); out_dec(d->block_size);
        out(" capacidad~="); out_dec((d->last_block + 1) * d->block_size / (1024ULL * 1024ULL)); out(" MiB\r\n");
    }
    out("Los volúmenes con Simple File System son los candidatos de arranque.\r\n");
}

static void list_kernels(void) {
    out("Entradas de arranque: "); out_dec(gEntryCount); newline();
    for (uint32_t i = 0; i < gEntryCount; ++i) {
        out("  ["); out_dec(i); out("] "); out(gEntries[i].title);
        out("  "); out(gEntries[i].kernel); newline();
    }
}

static void print_help(void) {
    out("Comandos: help, kernels, disks, mem, map, boot N, reboot, poweroff, clear, exit\r\n");
    out("mem muestra resumen; map lista descriptores UEFI. boot N carga la entrada N.\r\n");
}

static int read_line(char *buffer, UINTN cap) {
    UINTN n = 0;
    for (;;) {
        EFI_INPUT_KEY key;
        EFI_STATUS s = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
        if (s == EFI_NOT_READY) {
            UINTN index; gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &index); continue;
        }
        if (EFI_ERROR(s)) continue;
        if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            buffer[n] = 0; newline(); return (int)n;
        }
        if (key.UnicodeChar == 8) {
            if (n) { --n; out("\b \b"); }
            continue;
        }
        if (key.UnicodeChar >= 32 && key.UnicodeChar < 127 && n + 1 < cap) {
            buffer[n++] = (char)key.UnicodeChar; out_char((char)key.UnicodeChar);
        }
    }
}

static void shell(void) {
    char line[128];
    for (;;) {
        out("we-love-c> "); read_line(line, sizeof(line));
        while (*line == ' ' || *line == '\t') { for (int i=0; line[i]; ++i) line[i] = line[i+1]; }
        if (str_eq(line, "help")) print_help();
        else if (str_eq(line, "kernels")) list_kernels();
        else if (str_eq(line, "disks")) print_storage();
        else if (str_eq(line, "mem")) print_memory_map(0);
        else if (str_eq(line, "map")) print_memory_map(1);
        else if (str_eq(line, "clear")) clear_screen();
        else if (str_eq(line, "exit")) return;
        else if (str_eq(line, "reboot")) {
            gST->RuntimeServices->ResetSystem(1, EFI_SUCCESS, 0, 0);
        } else if (str_eq(line, "poweroff")) {
            gST->RuntimeServices->ResetSystem(2, EFI_SUCCESS, 0, 0);
        } else if (starts_with(line, "boot ")) {
            uint32_t index;
            if (parse_u32(line + 5, &index) && index < gEntryCount) {
                boot_entry(index);
            } else out("Indice de kernel no valido.\r\n");
        } else if (*line) { out("Comando desconocido; escribe help.\r\n"); }
    }
}

static void menu(void) {
    uint32_t selected = gDefaultEntry < gEntryCount ? gDefaultEntry : 0;
    uint32_t ticks = 0;
    for (;;) {
        clear_screen(); set_color(0x0b);
        out("WeLoveC UEFI Boot Manager"); newline();
        set_color(0x07); out("UEFI x86_64  |  discos: "); out_dec(gStorageCount); out("  |  volúmenes: "); out_dec(gFilesystemCount); newline();
        out("------------------------------------------------------------\r\n");
        if (!gEntryCount) {
            out("No hay kernels ELF configurados.\r\n");
            out("Instala un kernel y crea \\EFI\\WELOVEC\\boot.cfg.\r\n");
        }
        for (uint32_t i = 0; i < gEntryCount; ++i) {
            if (i == selected) { set_color(0x70); out(" > "); out(gEntries[i].title); out("  "); out(gEntries[i].kernel); out(" "); set_color(0x07); }
            else { out("   "); out(gEntries[i].title); out("  "); out(gEntries[i].kernel); }
            newline();
        }
        out("\r\nFlechas: elegir   Enter: arrancar   S: shell   M: memoria   D: discos   Q: salir\r\n");
        if (gEntryCount && gTimeout) { out("Arranque automatico en "); out_dec(gTimeout - ticks); out(" s\r\n"); }
        for (uint32_t tenth = 0; tenth < 10; ++tenth) {
            EFI_INPUT_KEY key;
            EFI_STATUS s = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
            if (!EFI_ERROR(s)) {
                if (key.ScanCode == SCAN_UP && gEntryCount) selected = selected ? selected - 1 : gEntryCount - 1;
                else if (key.ScanCode == SCAN_DOWN && gEntryCount) selected = (selected + 1) % gEntryCount;
                else if (key.UnicodeChar == '\r' && gEntryCount) { boot_entry(selected); }
                else if (key.UnicodeChar == 's' || key.UnicodeChar == 'S') { shell(); break; }
                else if (key.UnicodeChar == 'm' || key.UnicodeChar == 'M') { clear_screen(); print_memory_map(1); out("Pulsa una tecla para volver."); UINTN idx; gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &idx); gST->ConIn->ReadKeyStroke(gST->ConIn, &key); break; }
                else if (key.UnicodeChar == 'd' || key.UnicodeChar == 'D') { clear_screen(); print_storage(); out("Pulsa una tecla para volver."); UINTN idx; gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &idx); gST->ConIn->ReadKeyStroke(gST->ConIn, &key); break; }
                else if (key.UnicodeChar == 'q' || key.UnicodeChar == 'Q' || key.ScanCode == SCAN_ESC) { return; }
                break;
            }
            gBS->Stall(100000);
        }
        ++ticks;
        if (gEntryCount && gTimeout && ticks >= gTimeout) { boot_entry(selected); ticks = 0; }
    }
}

typedef struct __attribute__((packed)) {
    uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
    uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf64Header;
typedef struct __attribute__((packed)) {
    uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align;
} Elf64ProgramHeader;

static int is_canonical(uint64_t address) {
    if (gLa57) return (address >> 57) == 0 || (address >> 57) == 0x7f;
    return (address >> 48) == 0 || (address >> 48) == 0xffff;
}

static EFI_STATUS load_elf(uint64_t image, uint64_t image_size, ElfSegment *segments, uint32_t *segment_count,
                           uint64_t *entry, uint64_t *physical_min, uint64_t *physical_max) {
    if (image_size < sizeof(Elf64Header)) return EFI_LOAD_ERROR;
    Elf64Header *h = (Elf64Header *)(uintptr_t)image;
    if (h->ident[0] != 0x7f || h->ident[1] != 'E' || h->ident[2] != 'L' || h->ident[3] != 'F' ||
        h->ident[4] != 2 || h->ident[5] != 1 || h->ident[6] != 1 || h->machine != 62 || h->version != 1 ||
        h->type != 2 || h->ehsize < sizeof(Elf64Header) || h->phentsize < sizeof(Elf64ProgramHeader) ||
        h->phnum == 0 || h->phnum > MAX_ELF_PHDRS) return EFI_UNSUPPORTED;
    if (h->phoff > image_size || (uint64_t)h->phnum * h->phentsize > image_size - h->phoff) return EFI_LOAD_ERROR;
    uint32_t count = 0; uint64_t pmin = UINT64_MAX, pmax = 0; int entry_executable = 0;
    for (uint32_t i = 0; i < h->phnum; ++i) {
        Elf64ProgramHeader *ph = (Elf64ProgramHeader *)(uintptr_t)(image + h->phoff + (uint64_t)i * h->phentsize);
        if (ph->type != 1) continue;
        if (count >= WELOVEC_BOOTINFO_MAX_SEGMENTS || ph->filesz > ph->memsz || ph->offset > image_size ||
            ph->filesz > image_size - ph->offset || ph->memsz == 0 || ph->paddr > PT_WINDOW_END ||
            ph->memsz > PT_WINDOW_SIZE - ph->paddr || ph->vaddr > UINT64_MAX - ph->memsz ||
            !is_canonical(ph->vaddr) || !is_canonical(ph->vaddr + ph->memsz - 1) ||
            (ph->vaddr & (PAGE_SIZE - 1)) != (ph->paddr & (PAGE_SIZE - 1))) return EFI_LOAD_ERROR;
        uint64_t base = ph->paddr & PAGE_MASK;
        uint64_t end = (ph->paddr + ph->memsz + PAGE_SIZE - 1) & PAGE_MASK;
        if (end < ph->paddr || end > PT_WINDOW_SIZE) return EFI_LOAD_ERROR;
        for (uint32_t j = 0; j < count; ++j) {
            uint64_t other_base = segments[j].paddr & PAGE_MASK;
            uint64_t other_end = (segments[j].paddr + segments[j].memsz + PAGE_SIZE - 1) & PAGE_MASK;
            if (base < other_end && other_base < end) return EFI_UNSUPPORTED;
        }
        segments[count].paddr = ph->paddr;
        segments[count].vaddr = ph->vaddr;
        segments[count].offset = ph->offset;
        segments[count].filesz = ph->filesz;
        segments[count].memsz = ph->memsz;
        segments[count].flags = ph->flags;
        segments[count].alloc_base = base;
        segments[count].alloc_pages = (end - base) / PAGE_SIZE;
        if (base < pmin) pmin = base;
        if (end > pmax) pmax = end;
        if ((ph->flags & 1) && h->entry >= ph->vaddr && h->entry < ph->vaddr + ph->memsz) entry_executable = 1;
        ++count;
    }
    if (!count || !entry_executable) return EFI_LOAD_ERROR;
    *segment_count = count; *entry = h->entry; *physical_min = pmin; *physical_max = pmax;
    return EFI_SUCCESS;
}

static EFI_STATUS allocate_kernel_segments(uint64_t image, ElfSegment *segments, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t base = segments[i].alloc_base;
        EFI_STATUS s = gBS->AllocatePages(EfiAllocateAddress, EfiLoaderData, segments[i].alloc_pages, &base);
        if (EFI_ERROR(s)) {
            for (uint32_t j = 0; j < i; ++j) gBS->FreePages(segments[j].alloc_base, segments[j].alloc_pages);
            return s;
        }
        segments[i].alloc_base = base;
        memset_local((void *)(uintptr_t)base, 0, segments[i].alloc_pages * PAGE_SIZE);
        memcpy_local((void *)(uintptr_t)segments[i].paddr,
                     (void *)(uintptr_t)(image + segments[i].offset), segments[i].filesz);
    }
    return EFI_SUCCESS;
}

static EFI_STATUS alloc_pt_pages(uint64_t pages, uint64_t *address) {
    if (gPageTableAllocCount >= MAX_PAGE_TABLE_ALLOCS) return EFI_OUT_OF_RESOURCES;
    *address = PT_WINDOW_END;
    EFI_STATUS s = alloc_pages_below(EfiAllocateMaxAddress, pages, address);
    if (EFI_ERROR(s)) return s;
    if (*address >= PT_WINDOW_SIZE || pages > (PT_WINDOW_SIZE - *address) / PAGE_SIZE) {
        gBS->FreePages(*address, pages); return EFI_OUT_OF_RESOURCES;
    }
    memset_local((void *)(uintptr_t)*address, 0, pages * PAGE_SIZE);
    gPageTables[gPageTableAllocCount].address = *address;
    gPageTables[gPageTableAllocCount].pages = pages;
    ++gPageTableAllocCount;
    return EFI_SUCCESS;
}

static EFI_STATUS alloc_pt_page(uint64_t *address) { return alloc_pt_pages(1, address); }

static EFI_STATUS next_table(uint64_t *table, uint32_t index, uint64_t **next) {
    uint64_t ent = table[index];
    if (!(ent & PTE_PRESENT)) {
        uint64_t page;
        EFI_STATUS s = alloc_pt_page(&page);
        if (EFI_ERROR(s)) return s;
        table[index] = page | PTE_PRESENT | PTE_WRITE;
        ent = table[index];
    } else if (ent & PTE_LARGE) {
        return EFI_UNSUPPORTED;
    }
    *next = (uint64_t *)(uintptr_t)(ent & PTE_ADDR);
    return EFI_SUCCESS;
}

static EFI_STATUS walk_to_pd(uint64_t root_addr, uint64_t virtual_addr, uint64_t **pd_out) {
    uint64_t *root = (uint64_t *)(uintptr_t)root_addr, *pml4, *pdpt, *pd;
    EFI_STATUS s;
    if (gLa57) {
        s = next_table(root, (uint32_t)((virtual_addr >> 48) & 0x1ff), &pml4);
        if (EFI_ERROR(s)) return s;
    } else pml4 = root;
    uint32_t pml4_i = (uint32_t)((virtual_addr >> 39) & 0x1ff);
    uint32_t pdpt_i = (uint32_t)((virtual_addr >> 30) & 0x1ff);
    s = next_table(pml4, pml4_i, &pdpt); if (EFI_ERROR(s)) return s;
    s = next_table(pdpt, pdpt_i, &pd); if (EFI_ERROR(s)) return s;
    *pd_out = pd;
    return EFI_SUCCESS;
}

static EFI_STATUS map_large_page(uint64_t root_addr, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    uint64_t *pd;
    EFI_STATUS s = walk_to_pd(root_addr, virtual_addr, &pd);
    if (EFI_ERROR(s)) return s;
    uint32_t pd_i = (uint32_t)((virtual_addr >> 21) & 0x1ff);
    uint64_t wanted = (physical_addr & PDE_2M_ADDR) | PTE_PRESENT | PTE_WRITE | PTE_LARGE | (flags & PTE_NX);
    if (pd[pd_i] & PTE_PRESENT) {
        if (!(pd[pd_i] & PTE_LARGE)) return EFI_UNSUPPORTED;
        if ((pd[pd_i] & PDE_2M_ADDR) != (wanted & PDE_2M_ADDR)) return EFI_UNSUPPORTED;
    }
    pd[pd_i] = wanted;
    return EFI_SUCCESS;
}

static EFI_STATUS map_page(uint64_t root_addr, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    uint64_t *pd, *pt;
    EFI_STATUS s = walk_to_pd(root_addr, virtual_addr, &pd);
    if (EFI_ERROR(s)) return s;
    uint32_t pd_i = (uint32_t)((virtual_addr >> 21) & 0x1ff);
    uint32_t pt_i = (uint32_t)((virtual_addr >> 12) & 0x1ff);
    if (pd[pd_i] & PTE_LARGE) {
        uint64_t old = pd[pd_i], pt_page;
        s = alloc_pt_page(&pt_page); if (EFI_ERROR(s)) return s;
        uint64_t old_base = old & PDE_2M_ADDR;
        uint64_t leaf_flags = old & ~PDE_2M_ADDR;
        uint64_t *new_pt = (uint64_t *)(uintptr_t)pt_page;
        for (uint32_t i = 0; i < 512; ++i) new_pt[i] = old_base + (uint64_t)i * PAGE_SIZE | (leaf_flags & ~PTE_LARGE);
        pd[pd_i] = pt_page | PTE_PRESENT | PTE_WRITE;
    }
    s = next_table(pd, pd_i, &pt); if (EFI_ERROR(s)) return s;
    uint64_t wanted = (physical_addr & PAGE_MASK) | PTE_PRESENT |
                      (flags & (PTE_WRITE | PTE_WRITE_THROUGH | PTE_CACHE_DISABLE | PTE_NX));
    if (pt[pt_i] & PTE_PRESENT) {
        if ((pt[pt_i] & PTE_ADDR) != (wanted & PTE_ADDR)) return EFI_UNSUPPORTED;
        pt[pt_i] = wanted;
    } else pt[pt_i] = wanted;
    return EFI_SUCCESS;
}

static int memory_is_ram(uint32_t type) {
    return (type >= 1 && type <= 7) || type == 9 || type == 10 || type == 14;
}

static EFI_STATUS map_memory_descriptor(uint64_t root, const EFI_MEMORY_DESCRIPTOR *desc) {
    if (!memory_is_ram(desc->Type) || !desc->NumberOfPages || desc->PhysicalStart >= PT_WINDOW_SIZE) return EFI_SUCCESS;
    if (desc->NumberOfPages > UINT64_MAX / PAGE_SIZE) return EFI_LOAD_ERROR;
    uint64_t start = desc->PhysicalStart;
    uint64_t size = desc->NumberOfPages * PAGE_SIZE;
    uint64_t end = size > PT_WINDOW_SIZE - start ? PT_WINDOW_SIZE : start + size;
    uint64_t flags = gNxSupported ? PTE_NX : 0;
    while (start < end) {
        if ((start & ((2 * 1024 * 1024) - 1)) == 0 && end - start >= 2 * 1024 * 1024) {
            EFI_STATUS s = map_large_page(root, start, start, flags);
            if (!EFI_ERROR(s)) { start += 2 * 1024 * 1024; continue; }
            if (s != EFI_UNSUPPORTED) return s;
        }
        EFI_STATUS s = map_page(root, start, start, PTE_WRITE | flags);
        if (EFI_ERROR(s)) return s;
        start += PAGE_SIZE;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS map_framebuffer(uint64_t root, const WelovecFramebuffer *framebuffer) {
    if (!framebuffer || !framebuffer->base || !framebuffer->size ||
        framebuffer->pixel_format == 3) return EFI_SUCCESS;
    if (framebuffer->base >= PT_WINDOW_SIZE ||
        framebuffer->size > PT_WINDOW_SIZE - framebuffer->base) return EFI_LOAD_ERROR;

    const uint64_t start = framebuffer->base & PAGE_MASK;
    uint64_t end = framebuffer->base + framebuffer->size;
    if (end > UINT64_MAX - (PAGE_SIZE - 1)) return EFI_LOAD_ERROR;
    end = (end + PAGE_SIZE - 1) & PAGE_MASK;

    /* PAT entry 1 becomes write-combining in enter_kernel().  Keep the
       framebuffer UC on CPUs without PAT. */
    const uint64_t cache_flags = PTE_WRITE_THROUGH |
                                 (gPatSupported ? 0 : PTE_CACHE_DISABLE);
    const uint64_t flags = PTE_WRITE | cache_flags |
                           (gNxSupported ? PTE_NX : 0);
    for (uint64_t address = start; address < end; address += PAGE_SIZE) {
        EFI_STATUS s = map_page(root, address, address, flags);
        if (EFI_ERROR(s)) return s;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS build_page_tables(uint64_t *root_out, uint64_t trampoline, uint64_t fatal_handler,
                                    ElfSegment *segments, uint32_t count, uint64_t memory_map,
                                    uint64_t memory_map_size, uint64_t descriptor_size,
                                    const WelovecFramebuffer *framebuffer) {
    uint64_t pages_addr;
    uint64_t page_table_pages = gLa57 ? 515 : 514;
    EFI_STATUS s = alloc_pt_pages(page_table_pages, &pages_addr);
    if (EFI_ERROR(s)) return s;
    uint64_t *root = (uint64_t *)(uintptr_t)pages_addr;
    uint64_t *pml4 = gLa57 ? (uint64_t *)(uintptr_t)(pages_addr + PAGE_SIZE) : root;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pages_addr + (gLa57 ? 2 : 1) * PAGE_SIZE);
    uint64_t *pds = (uint64_t *)(uintptr_t)(pages_addr + (gLa57 ? 3 : 2) * PAGE_SIZE);
    if (gLa57) {
        root[0] = (uint64_t)(uintptr_t)pml4 | PTE_PRESENT | PTE_WRITE;
        root[511] = root[0]; /* identity map and canonical direct map */
        pml4[0] = (uint64_t)(uintptr_t)pdpt | PTE_PRESENT | PTE_WRITE;
        pml4[256] = pml4[0];
    } else {
        pml4[0] = (uint64_t)(uintptr_t)pdpt | PTE_PRESENT | PTE_WRITE;
        pml4[256] = pml4[0]; /* direct physical map at 0xffff800000000000 */
    }
    for (uint32_t i = 0; i < 512; ++i) {
        uint64_t *pd = pds + (uint64_t)i * 512;
        pdpt[i] = (uint64_t)(uintptr_t)pd | PTE_PRESENT | PTE_WRITE;
    }
    if (!descriptor_size || memory_map_size % descriptor_size) return EFI_LOAD_ERROR;
    uint64_t descriptor_count = memory_map_size / descriptor_size;
    for (uint64_t i = 0; i < descriptor_count; ++i) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)(memory_map + i * descriptor_size);
        s = map_memory_descriptor(pages_addr, d);
        if (EFI_ERROR(s)) return s;
    }
    /* The transition code is the only EFI code executed after CR3 changes. */
    s = map_page(pages_addr, trampoline, trampoline, PTE_PRESENT);
    if (EFI_ERROR(s)) return s;
    s = map_page(pages_addr, fatal_handler, fatal_handler, PTE_PRESENT);
    if (EFI_ERROR(s)) return s;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t vbase = segments[i].vaddr & PAGE_MASK;
        uint64_t pbase = segments[i].paddr & PAGE_MASK;
        uint64_t end = (segments[i].vaddr + segments[i].memsz + PAGE_SIZE - 1) & PAGE_MASK;
        uint64_t flags = (segments[i].flags & 2 ? PTE_WRITE : 0) | (gNxSupported && !(segments[i].flags & 1) ? PTE_NX : 0);
        for (uint64_t v = vbase, p = pbase; v < end; v += PAGE_SIZE, p += PAGE_SIZE) {
            /* Apply ELF permissions to the physical alias too, preserving W^X. */
            s = map_page(pages_addr, p, p, flags);
            if (EFI_ERROR(s)) return s;
            s = map_page(pages_addr, v, p, flags);
            if (EFI_ERROR(s)) return s;
        }
    }
    s = map_framebuffer(pages_addr, framebuffer);
    if (EFI_ERROR(s)) return s;
    *root_out = pages_addr;
    return EFI_SUCCESS;
}

static uint64_t find_config_table(EFI_GUID guid_a, EFI_GUID guid_b) {
    for (UINTN i = 0; i < gST->NumberOfTableEntries; ++i) {
        EFI_GUID *g = &gST->ConfigurationTable[i].VendorGuid;
        if ((g->Data1 == guid_a.Data1 && g->Data2 == guid_a.Data2 && g->Data3 == guid_a.Data3 && !memcmp_local(g->Data4, guid_a.Data4, 8)) ||
            (g->Data1 == guid_b.Data1 && g->Data2 == guid_b.Data2 && g->Data3 == guid_b.Data3 && !memcmp_local(g->Data4, guid_b.Data4, 8)))
            return (uint64_t)(uintptr_t)gST->ConfigurationTable[i].VendorTable;
    }
    return 0;
}

static uint32_t read_le32_local(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int valid_rsdp_bounded(const uint8_t *rsdp, uint32_t available,
                              uint32_t *length_out) {
    static const uint8_t signature[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
    if (!rsdp || available < 20 ||
        memcmp_local(rsdp, signature, sizeof(signature))) return 0;

    uint8_t checksum = 0;
    for (uint32_t i = 0; i < 20; ++i) checksum = (uint8_t)(checksum + rsdp[i]);
    if (checksum) return 0;

    uint32_t length = 20;
    if (rsdp[15] >= 2) {
        if (available < 36) return 0;
        length = read_le32_local(rsdp + 20);
        if (length < 36 || length > PAGE_SIZE || length > available) return 0;
        checksum = 0;
        for (uint32_t i = 0; i < length; ++i)
            checksum = (uint8_t)(checksum + rsdp[i]);
        if (checksum) return 0;
    }
    if (length_out) *length_out = length;
    return 1;
}

static int valid_rsdp(const uint8_t *rsdp, uint32_t *length_out) {
    return valid_rsdp_bounded(rsdp, PAGE_SIZE, length_out);
}

static uint64_t scan_rsdp_range(uint64_t start, uint64_t end) {
    /* RSDP 2.0 is 36 bytes; keep every candidate wholly inside the range. */
    if (end < start || end - start < 36) return 0;
    for (uint64_t address = (start + 15) & ~UINT64_C(15);
         address <= end - 36; address += 16) {
        const uint8_t *candidate = (const uint8_t *)(uintptr_t)address;
        const uint32_t available = (uint32_t)(end - address);
        if (valid_rsdp_bounded(candidate, available, 0)) return address;
    }
    return 0;
}

static uint64_t find_legacy_rsdp(uint32_t *source_out) {
    /* ACPI's legacy PC search checks the first KiB of the EBDA, then BIOS ROM. */
    const uint16_t ebda_segment =
        *(const volatile uint16_t *)(uintptr_t)UINT64_C(0x40e);
    const uint64_t ebda = (uint64_t)ebda_segment << 4;
    if (ebda >= 0x400 && ebda < 0xa0000) {
        uint64_t end = ebda + 1024;
        if (end > 0xa0000) end = 0xa0000;
        const uint64_t address = scan_rsdp_range(ebda, end);
        if (address) {
            if (source_out) *source_out = WELOVEC_ACPI_RSDP_SOURCE_EBDA;
            return address;
        }
    }
    const uint64_t address = scan_rsdp_range(0xe0000, 0x100000);
    if (address && source_out)
        *source_out = WELOVEC_ACPI_RSDP_SOURCE_BIOS_ROM;
    return address;
}

static uint64_t find_efi_config_rsdp(uint32_t *length_out) {
    if (!gST || !gST->ConfigurationTable) return 0;
    for (UINTN i = 0; i < gST->NumberOfTableEntries; ++i) {
        const uint8_t *candidate = (const uint8_t *)
            gST->ConfigurationTable[i].VendorTable;
        uint32_t length = 0;
        if (!candidate || !valid_rsdp(candidate, &length)) continue;
        if (length_out) *length_out = length;
        return (uint64_t)(uintptr_t)candidate;
    }
    return 0;
}

static uint64_t find_rsdp(uint32_t *length_out, uint32_t *source_out) {
    EFI_GUID acpi2 = {0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}};
    EFI_GUID acpi1 = {0xeb9d2d30,0x2d88,0x11d3,{0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
    uint64_t address = find_config_table(acpi2, acpi2);
    if (address && valid_rsdp((const uint8_t *)(uintptr_t)address, length_out)) {
        if (source_out) *source_out = WELOVEC_ACPI_RSDP_SOURCE_EFI_ACPI_2;
        return address;
    }
    address = find_config_table(acpi1, acpi1);
    if (address && valid_rsdp((const uint8_t *)(uintptr_t)address, length_out)) {
        if (source_out) *source_out = WELOVEC_ACPI_RSDP_SOURCE_EFI_ACPI_1;
        return address;
    }
    /* Some firmware publishes the ACPI table pointer with a nonstandard GUID.
       EFI configuration-table vendor pointers remain the authoritative list;
       accept only a signature- and checksum-validated RSDP from that list. */
    address = find_efi_config_rsdp(length_out);
    if (address) {
        if (source_out) *source_out = WELOVEC_ACPI_RSDP_SOURCE_EFI_CONFIG_SCAN;
        return address;
    }
    address = find_legacy_rsdp(source_out);
    if (address) (void)valid_rsdp((const uint8_t *)(uintptr_t)address, length_out);
    return address;
}

static int entropy_is_nonzero(const uint8_t *entropy, UINTN size) {
    uint8_t any = 0;
    for (UINTN i = 0; i < size; ++i) any |= entropy[i];
    return any != 0;
}

static int fill_entropy_from_cpu(uint8_t *entropy, UINTN size) {
    uint32_t max_leaf = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid" : "=a"(max_leaf), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0), "c"(0));
    if (max_leaf < 1) return 0;

    uint32_t eax = 0;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));
    const int has_rdrand = (ecx & (1u << 30)) != 0;
    int has_rdseed = 0;
    if (max_leaf >= 7) {
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(7), "c"(0));
        has_rdseed = (ebx & (1u << 18)) != 0;
    }
    if (!has_rdseed && !has_rdrand) return 0;

    for (UINTN offset = 0; offset < size;) {
        uint64_t sample = 0;
        int generated = 0;
        for (uint32_t attempt = 0; has_rdseed && attempt < 128; ++attempt) {
            uint8_t success = 0;
            __asm__ volatile("rdseed %0; setc %1"
                             : "=&r"(sample), "=qm"(success) : : "cc");
            if (success) { generated = 1; break; }
        }
        for (uint32_t attempt = 0; !generated && has_rdrand && attempt < 128;
             ++attempt) {
            uint8_t success = 0;
            __asm__ volatile("rdrand %0; setc %1"
                             : "=&r"(sample), "=qm"(success) : : "cc");
            if (success) generated = 1;
        }
        if (!generated) {
            memset_local(entropy, 0, size);
            return 0;
        }
        const UINTN remaining = size - offset;
        const UINTN copy_size = remaining < sizeof(sample) ? remaining : sizeof(sample);
        memcpy_local(entropy + offset, &sample, copy_size);
        offset += copy_size;
    }
    return entropy_is_nonzero(entropy, size);
}

static void fill_entropy(WelovecBootInfo *info) {
    EFI_RNG_PROTOCOL *rng = 0;
    if (!EFI_ERROR(gBS->LocateProtocol(&gEfiRngProtocolGuid, 0, (void **)&rng)) && rng &&
        !EFI_ERROR(rng->GetRNG(rng, 0, sizeof(info->entropy), info->entropy)) &&
        entropy_is_nonzero(info->entropy, sizeof(info->entropy))) {
        info->flags |= WELOVEC_BOOT_FLAG_RNG_VALID;
        return;
    }

    memset_local(info->entropy, 0, sizeof(info->entropy));
    if (fill_entropy_from_cpu(info->entropy, sizeof(info->entropy)))
        info->flags |= WELOVEC_BOOT_FLAG_RNG_VALID;
}

static EFI_STATUS finalize_memory_map(WelovecBootInfo *info, PageAllocation *map_alloc, uint64_t *map_size,
                                      uint64_t *map_key, uint64_t *desc_size, uint32_t *desc_version) {
    UINTN size = map_alloc->pages * PAGE_SIZE;
    EFI_STATUS s = gBS->GetMemoryMap(&size, (void *)(uintptr_t)map_alloc->address, (UINTN *)map_key,
                                     (UINTN *)desc_size, desc_version);
    if (EFI_ERROR(s)) return s;
    *map_size = size;
    info->memory_map_address = map_alloc->address;
    info->memory_map_size = size;
    info->memory_map_descriptor_size = *desc_size;
    info->memory_map_descriptor_version = *desc_version;
    info->memory_map_descriptor_count = (uint32_t)(size / *desc_size);
    return EFI_SUCCESS;
}

void enter_kernel(uint64_t root, uint64_t entry, WelovecBootInfo *info,
                  uint64_t cpu_features);
void fatal_halt(void);
static void boot_entry(uint32_t index) {
    if (index >= gEntryCount) return;
    BootEntry *entry = &gEntries[index];
    out("Cargando "); out(entry->title); out("...\r\n");
    PageAllocation image = {0}, initrd = {0}, fat32_image = {0}, info_pages = {0}, mmap_pages = {0}, cmdline = {0}, rsdp_pages = {0};
    gPageTableAllocCount = 0;
    uint64_t image_size = 0, initrd_size = 0, fat32_image_size = 0;
    EFI_STATUS s = read_whole_file(entry->filesystem, entry->kernel, MAX_ELF_FILE, &image, &image_size);
    if (EFI_ERROR(s)) { out("Error: no se pudo leer el kernel (EFI="); out_hex(s); out(").\r\n"); return; }
    ElfSegment segments[WELOVEC_BOOTINFO_MAX_SEGMENTS]; memset_local(segments, 0, sizeof(segments));
    uint32_t segment_count = 0; uint64_t kernel_entry = 0, pmin = 0, pmax = 0;
    s = load_elf(image.address, image_size, segments, &segment_count, &kernel_entry, &pmin, &pmax);
    if (EFI_ERROR(s)) { out("Error: ELF64 no compatible (se requiere ET_EXEC x86_64).\r\n"); free_pages(&image); return; }
    s = allocate_kernel_segments(image.address, segments, segment_count);
    free_pages(&image);
    if (EFI_ERROR(s)) { out("Error: no se pudo reservar la imagen ELF en sus direcciones fisicas.\r\n"); return; }
    if (entry->initrd[0]) {
        s = read_whole_file(entry->filesystem, entry->initrd, MAX_INITRD_FILE, &initrd, &initrd_size);
        if (EFI_ERROR(s)) {
            out("Error: no se pudo leer el initrd configurado.\r\n");
            for (uint32_t i=0; i<segment_count; ++i) gBS->FreePages(segments[i].alloc_base, segments[i].alloc_pages);
            return;
        }
    }
    if (entry->fat32_image[0]) {
        s = read_whole_file(entry->filesystem, entry->fat32_image,
                            MAX_FILESYSTEM_IMAGE, &fat32_image, &fat32_image_size);
        if (EFI_ERROR(s)) {
            out("Error: no se pudo leer la imagen FAT32 configurada.\r\n");
            goto boot_fail;
        }
    }
    s = alloc_any_pages((sizeof(WelovecBootInfo) + PAGE_SIZE - 1) / PAGE_SIZE, &info_pages);
    if (EFI_ERROR(s)) { out("Error: sin memoria para BootInfo.\r\n"); goto boot_fail; }
    WelovecBootInfo *info = (WelovecBootInfo *)(uintptr_t)info_pages.address;
    memset_local(info, 0, sizeof(*info));
    info->magic = WELOVEC_BOOTINFO_MAGIC; info->version = WELOVEC_BOOTINFO_VERSION; info->size = sizeof(*info);
    info->flags = WELOVEC_BOOT_FLAG_UEFI | WELOVEC_BOOT_FLAG_EXIT_BOOT_SERVICES |
                  WELOVEC_BOOT_FLAG_PAGING_ENABLED | WELOVEC_BOOT_FLAG_INTERRUPTS_DISABLED |
                  WELOVEC_BOOT_FLAG_GDT_READY | WELOVEC_BOOT_FLAG_IDT_READY;
    info->page_table_levels = gLa57 ? 5 : 4;
    info->kernel_entry_virtual = kernel_entry; info->kernel_physical_min = pmin; info->kernel_physical_max = pmax;
    info->kernel_segment_count = segment_count;
    for (uint32_t i = 0; i < segment_count; ++i) {
        info->kernel_segments[i].physical_address = segments[i].paddr;
        info->kernel_segments[i].virtual_address = segments[i].vaddr;
        info->kernel_segments[i].page_count = segments[i].alloc_pages;
        info->kernel_segments[i].flags = (uint32_t)segments[i].flags;
    }
    info->storage_device_count = gStorageCount;
    memcpy_local(info->storage_devices, gStorage, gStorageCount * sizeof(gStorage[0]));
    if (initrd.address) {
        WelovecModule *module = &info->modules[info->module_count++];
        module->address = initrd.address; module->size = initrd_size; module->kind = WELOVEC_MODULE_INITRD;
    }
    if (fat32_image.address && info->module_count < WELOVEC_BOOTINFO_MAX_MODULES) {
        WelovecModule *module = &info->modules[info->module_count++];
        module->address = fat32_image.address; module->size = fat32_image_size;
        module->kind = WELOVEC_MODULE_FAT32_IMAGE;
    }
    uint64_t command_line_address = 0;
    s = alloc_any_pages(1, &cmdline);
    if (EFI_ERROR(s)) { out("Error: sin memoria para la linea de comandos.\r\n"); goto boot_fail; }
    char *cmd_dst = (char *)(uintptr_t)cmdline.address;
    if (entry->command_line[0]) {
        str_copy(cmd_dst, PAGE_SIZE, entry->command_line);
        command_line_address = cmdline.address;
        info->command_line_size = str_len(entry->command_line) + 1;
    } else {
        cmd_dst[0] = 0; command_line_address = cmdline.address; info->command_line_size = 1;
    }
    info->command_line_address = command_line_address;
    EFI_GUID smbios = {0xeb9d2d31,0x2d88,0x11d3,{0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
    EFI_GUID smbios3 = {0xf2fd1544,0x9794,0x4a2c,{0x99,0x2e,0xe5,0xbb,0xcf,0x20,0xe3,0x94}};
    uint32_t rsdp_length = 0;
    uint32_t rsdp_source_kind = WELOVEC_ACPI_RSDP_SOURCE_NONE;
    const uint64_t rsdp_source = find_rsdp(&rsdp_length, &rsdp_source_kind);
    if (rsdp_source && rsdp_length &&
        !EFI_ERROR(alloc_any_pages(1, &rsdp_pages))) {
        memcpy_local((void *)(uintptr_t)rsdp_pages.address,
                     (const void *)(uintptr_t)rsdp_source, rsdp_length);
        info->acpi_rsdp = rsdp_pages.address;
        info->acpi_rsdp_source = rsdp_source_kind;
    } else if (rsdp_source) {
        info->acpi_rsdp_source = WELOVEC_ACPI_RSDP_SOURCE_COPY_FAILED;
    }
    info->smbios_entry = find_config_table(smbios3, smbios);
    if (info->acpi_rsdp) info->flags |= WELOVEC_BOOT_FLAG_ACPI_VALID;
    if (info->smbios_entry) info->flags |= WELOVEC_BOOT_FLAG_SMBIOS_VALID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    if (!EFI_ERROR(gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, 0, (void **)&gop)) && gop && gop->Mode && gop->Mode->Info) {
        info->framebuffer.base = gop->Mode->FrameBufferBase;
        info->framebuffer.size = gop->Mode->FrameBufferSize;
        info->framebuffer.width = gop->Mode->Info->HorizontalResolution;
        info->framebuffer.height = gop->Mode->Info->VerticalResolution;
        info->framebuffer.pixels_per_scanline = gop->Mode->Info->PixelsPerScanLine;
        info->framebuffer.pixel_format = gop->Mode->Info->PixelFormat;
        memcpy_local(info->framebuffer.pixel_masks, gop->Mode->Info->PixelInformation,
                     sizeof(info->framebuffer.pixel_masks));
        info->flags |= WELOVEC_BOOT_FLAG_GOP_VALID;
    }
    fill_entropy(info);
    if (gNxSupported) info->flags |= WELOVEC_BOOT_FLAG_NX_ENABLED;
    s = alloc_any_pages(MAX_MEMORY_MAP_BYTES / PAGE_SIZE, &mmap_pages);
    if (EFI_ERROR(s)) { out("Error: no se pudo reservar el mapa UEFI.\r\n"); goto boot_fail; }
    uint64_t map_size = 0, map_key = 0, desc_size = 0; uint32_t desc_version = 0;
    s = finalize_memory_map(info, &mmap_pages, &map_size, &map_key, &desc_size, &desc_version);
    if (EFI_ERROR(s)) { out("Error: no se pudo consultar el mapa UEFI para crear las tablas.\r\n"); goto boot_fail; }
    uint64_t page_root = 0;
    s = build_page_tables(&page_root, (uint64_t)(uintptr_t)&enter_kernel,
                          (uint64_t)(uintptr_t)&fatal_halt, segments, segment_count,
                          mmap_pages.address, map_size, desc_size, &info->framebuffer);
    if (EFI_ERROR(s)) { out("Error: no se pudieron crear las tablas de paginas.\r\n"); goto boot_fail; }
    info->page_table_root = page_root;
    for (unsigned retry = 0; retry < 4; ++retry) {
        s = finalize_memory_map(info, &mmap_pages, &map_size, &map_key, &desc_size, &desc_version);
        if (EFI_ERROR(s)) { out("Error: no se pudo obtener el mapa de memoria final.\r\n"); goto boot_fail; }
        s = gBS->ExitBootServices(gImage, map_key);
        if (s == EFI_SUCCESS) {
            const uint64_t cpu_features = (gNxSupported ? 1u : 0u) |
                                          (gPatSupported ? 2u : 0u);
            enter_kernel(page_root, kernel_entry, info, cpu_features);
            for (;;) __asm__ volatile("hlt");
        }
        if (s != EFI_INVALID_PARAMETER) break;
    }
    out("Error: ExitBootServices fallo (EFI="); out_hex(s); out(").\r\n");
boot_fail:
    if (mmap_pages.address) free_pages(&mmap_pages);
    if (rsdp_pages.address) free_pages(&rsdp_pages);
    if (cmdline.address) free_pages(&cmdline);
    if (info_pages.address) free_pages(&info_pages);
    if (initrd.address) free_pages(&initrd);
    if (fat32_image.address) free_pages(&fat32_image);
    for (uint32_t i = 0; i < segment_count; ++i) if (segments[i].alloc_base) gBS->FreePages(segments[i].alloc_base, segments[i].alloc_pages);
    for (uint32_t i = 0; i < gPageTableAllocCount; ++i) gBS->FreePages(gPageTables[i].address, gPageTables[i].pages);
    gPageTableAllocCount = 0;
}

static void detect_cpu_features(void) {
    uint32_t max = 0, a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(max), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(0), "c"(0));
    if (max >= 1) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(1), "c"(0));
        gPatSupported = (uint8_t)((d >> 16) & 1u);
    }
    max = 0;
    __asm__ volatile("cpuid" : "=a"(max), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000), "c"(0));
    if (max >= 0x80000001) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001), "c"(0));
        gNxSupported = (uint8_t)((d >> 20) & 1);
    }
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    gLa57 = (uint8_t)((cr4 >> 12) & 1);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table) {
    gST = system_table; gBS = system_table->BootServices; gImage = image;
    gST->ConOut->SetAttribute(gST->ConOut, 0x07);
    gBS->SetWatchdogTimer(0, 0, 0, 0);
    detect_cpu_features();
    EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
    if (!EFI_ERROR(gBS->HandleProtocol(image, &gEfiLoadedImageProtocolGuid, (void **)&loaded)) && loaded && loaded->DeviceHandle) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
        if (!EFI_ERROR(gBS->HandleProtocol(loaded->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (void **)&fs)) && fs) {
            EFI_FILE_PROTOCOL *root = 0;
            if (!EFI_ERROR(fs->OpenVolume(fs, &root)) && root) {
                gFilesystems[0].handle = loaded->DeviceHandle; gFilesystems[0].root = root; gFilesystemCount = 1;
            }
        }
    }
    /* Enumerate the remaining volumes without duplicating the boot volume. */
    EFI_HANDLE *handles = 0; UINTN count = 0;
    if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid, 0, &count, &handles))) {
        for (UINTN i = 0; i < count && gFilesystemCount < MAX_FILESYSTEMS; ++i) {
            int duplicate = 0;
            for (uint32_t j=0; j<gFilesystemCount; ++j) if (gFilesystems[j].handle == handles[i]) duplicate = 1;
            if (duplicate) continue;
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0; EFI_FILE_PROTOCOL *root = 0;
            if (EFI_ERROR(gBS->HandleProtocol(handles[i], &gEfiSimpleFileSystemProtocolGuid, (void **)&fs)) || !fs || EFI_ERROR(fs->OpenVolume(fs, &root))) continue;
            gFilesystems[gFilesystemCount].handle = handles[i]; gFilesystems[gFilesystemCount].root = root; ++gFilesystemCount;
        }
        if (handles) gBS->FreePool(handles);
    }
    for (uint32_t i = 0; i < gFilesystemCount; ++i) parse_config(i);
    if (!gEntryCount) for (uint32_t i = 0; i < gFilesystemCount; ++i) { scan_directory(i, "\\EFI\\WELOVEC"); scan_directory(i, "\\boot"); }
    if (gDefaultEntry >= gEntryCount) gDefaultEntry = 0;
    discover_storage();
    menu();
    return EFI_SUCCESS;
}

static int memcmp_local(const void *a, const void *b, UINTN n) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (UINTN i = 0; i < n; ++i) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}
