// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Manages file descriptors opened by processes.

#include "file_descriptors.hpp"

#include "arch/x86_64/mm/spinlock.hpp"

namespace welovec::filesystem {

namespace {
constexpr uint32_t kMaximumDescriptors = 128;
struct Descriptor {
    uint64_t process_id;
    uint64_t offset;
    uint32_t flags;
    int32_t number;
    bool used;
    char path[1024];
};

welovec::x86_64::mm::IrqSpinLock gLock;
Descriptor gDescriptors[kMaximumDescriptors]{};

void copy_string(char *destination, const char *source) {
    uint32_t i = 0;
    while (source[i] && i + 1 < 1024) {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}
}

int32_t open_descriptor(uint64_t process_id, const char *path, uint32_t flags) {
    if (!process_id || !path || !path[0]) return -1;
    const uint64_t irq = gLock.lock_irqsave();
    int32_t number = 3;
    for (; number < 32; ++number) {
        bool occupied = false;
        for (const Descriptor &entry : gDescriptors)
            if (entry.used && entry.process_id == process_id && entry.number == number) {
                occupied = true;
                break;
            }
        if (!occupied) break;
    }
    if (number == 32) {
        gLock.unlock_irqrestore(irq);
        return -1;
    }
    Descriptor *slot = nullptr;
    for (Descriptor &entry : gDescriptors) if (!entry.used) { slot = &entry; break; }
    if (!slot) {
        gLock.unlock_irqrestore(irq);
        return -1;
    }
    slot->process_id = process_id;
    slot->number = number;
    slot->offset = 0;
    slot->flags = flags;
    slot->used = true;
    copy_string(slot->path, path);
    gLock.unlock_irqrestore(irq);
    return number;
}

bool close_descriptor(uint64_t process_id, int32_t descriptor) {
    const uint64_t irq = gLock.lock_irqsave();
    for (Descriptor &entry : gDescriptors) {
        if (entry.used && entry.process_id == process_id && entry.number == descriptor) {
            entry.used = false;
            entry.path[0] = '\0';
            gLock.unlock_irqrestore(irq);
            return true;
        }
    }
    gLock.unlock_irqrestore(irq);
    return false;
}

bool get_descriptor(uint64_t process_id, int32_t descriptor, DescriptorInfo *out) {
    if (!out) return false;
    const uint64_t irq = gLock.lock_irqsave();
    for (const Descriptor &entry : gDescriptors) {
        if (!entry.used || entry.process_id != process_id || entry.number != descriptor) continue;
        copy_string(out->path, entry.path);
        out->offset = entry.offset;
        out->flags = entry.flags;
        gLock.unlock_irqrestore(irq);
        return true;
    }
    gLock.unlock_irqrestore(irq);
    return false;
}

bool set_descriptor_offset(uint64_t process_id, int32_t descriptor, uint64_t offset) {
    const uint64_t irq = gLock.lock_irqsave();
    for (Descriptor &entry : gDescriptors) {
        if (entry.used && entry.process_id == process_id && entry.number == descriptor) {
            entry.offset = offset;
            gLock.unlock_irqrestore(irq);
            return true;
        }
    }
    gLock.unlock_irqrestore(irq);
    return false;
}

void close_process_descriptors(uint64_t process_id) {
    if (!process_id) return;
    const uint64_t irq = gLock.lock_irqsave();
    for (Descriptor &entry : gDescriptors) {
        if (entry.used && entry.process_id == process_id) {
            entry.used = false;
            entry.path[0] = '\0';
        }
    }
    gLock.unlock_irqrestore(irq);
}

}
