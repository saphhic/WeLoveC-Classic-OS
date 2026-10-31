// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements PIO access to IDE-compatible ATA disks.

#include "arch/common/Block/block_device.hpp"

#include "arch/x86_64/io/io.h"
#include "arch/x86_64/scheduler/task_lock.hpp"

namespace welovec::block::ata_pio {

namespace {
using welovec::x86_64::scheduler::TaskLock;

constexpr uint16_t kPrimaryBase = 0x1f0;
constexpr uint16_t kPrimaryControl = 0x3f6;
constexpr uint16_t kSecondaryBase = 0x170;
constexpr uint16_t kSecondaryControl = 0x376;
constexpr uint32_t kPollLimit = 1000000;

struct Channel {
    uint16_t base;
    uint16_t control;
    TaskLock lock;
};

struct Drive {
    Device block;
    Channel *channel;
    uint16_t identify[256];
    uint32_t words_per_sector;
    uint8_t slave;
    bool lba48;
};

Channel gChannels[2] = {
    {kPrimaryBase, kPrimaryControl, {}},
    {kSecondaryBase, kSecondaryControl, {}}
};
Drive gDrives[4]{};
uint32_t gDriveCount;

void delay_400ns(Channel *channel) {
    (void)inb(channel->control);
    (void)inb(channel->control);
    (void)inb(channel->control);
    (void)inb(channel->control);
}

bool wait_not_busy(Channel *channel) {
    for (uint32_t i = 0; i < kPollLimit; ++i)
        if (!(inb(channel->base + 7) & 0x80)) return true;
    return false;
}

bool wait_data_request(Channel *channel) {
    for (uint32_t i = 0; i < kPollLimit; ++i) {
        const uint8_t status = inb(channel->base + 7);
        if (!status || (status & 0x21)) return false; // no device, ERR, or DF
        if (!(status & 0x80) && (status & 0x08)) return true;
    }
    return false;
}

void select_drive(Drive *drive) {
    outb(drive->channel->base + 6,
         static_cast<uint8_t>(0xa0 | (drive->slave << 4)));
    delay_400ns(drive->channel);
}

bool read_identify(Drive *drive) {
    outb(drive->channel->control, 0x02); // This driver polls; suppress ATA device IRQs.
    select_drive(drive);
    outb(drive->channel->base + 2, 0);
    outb(drive->channel->base + 3, 0);
    outb(drive->channel->base + 4, 0);
    outb(drive->channel->base + 5, 0);
    outb(drive->channel->base + 7, 0xec);
    const uint8_t initial_status = inb(drive->channel->base + 7);
    if (!initial_status || initial_status == 0xff || !wait_not_busy(drive->channel)) return false;
    if (inb(drive->channel->base + 4) || inb(drive->channel->base + 5)) return false;
    if (!wait_data_request(drive->channel)) return false;
    for (uint32_t i = 0; i < 256; ++i)
        drive->identify[i] = inw(drive->channel->base);

    if (!(drive->identify[49] & (1u << 9))) return false;
    drive->lba48 = (drive->identify[83] & 0xc000) == 0x4000 &&
                   (drive->identify[83] & (1u << 10)) != 0;
    uint64_t sectors = drive->lba48
        ? static_cast<uint64_t>(drive->identify[100]) |
          (static_cast<uint64_t>(drive->identify[101]) << 16) |
          (static_cast<uint64_t>(drive->identify[102]) << 32) |
          (static_cast<uint64_t>(drive->identify[103]) << 48)
        : static_cast<uint64_t>(drive->identify[60]) |
          (static_cast<uint64_t>(drive->identify[61]) << 16);
    if (!sectors) return false;

    drive->words_per_sector = 256;
    const uint16_t sector_info = drive->identify[106];
    if ((sector_info & 0xc000) == 0x4000 && (sector_info & (1u << 12))) {
        const uint32_t words = static_cast<uint32_t>(drive->identify[117]) |
                               (static_cast<uint32_t>(drive->identify[118]) << 16);
        if (words < 256 || words > 2048 || (words & 1)) return false;
        drive->words_per_sector = words;
    }
    drive->block.name[0] = 'a'; drive->block.name[1] = 't'; drive->block.name[2] = 'a';
    drive->block.name[3] = static_cast<char>('0' + gDriveCount);
    drive->block.name[4] = '\0';
    drive->block.context = drive;
    drive->block.block_count = sectors;
    drive->block.block_size = drive->words_per_sector * 2;
    drive->block.read_only = false;
    return true;
}

void select_lba28(Drive *drive, uint64_t lba, uint8_t count) {
    Channel *channel = drive->channel;
    outb(channel->base + 6,
         static_cast<uint8_t>(0xe0 | (drive->slave << 4) | ((lba >> 24) & 0x0f)));
    outb(channel->base + 1, 0);
    outb(channel->base + 2, count);
    outb(channel->base + 3, static_cast<uint8_t>(lba));
    outb(channel->base + 4, static_cast<uint8_t>(lba >> 8));
    outb(channel->base + 5, static_cast<uint8_t>(lba >> 16));
}

void select_lba48(Drive *drive, uint64_t lba, uint16_t count) {
    Channel *channel = drive->channel;
    outb(channel->base + 6, static_cast<uint8_t>(0x40 | (drive->slave << 4)));
    outb(channel->base + 1, 0);
    outb(channel->base + 2, static_cast<uint8_t>(count >> 8));
    outb(channel->base + 3, static_cast<uint8_t>(lba >> 24));
    outb(channel->base + 4, static_cast<uint8_t>(lba >> 32));
    outb(channel->base + 5, static_cast<uint8_t>(lba >> 40));
    outb(channel->base + 2, static_cast<uint8_t>(count));
    outb(channel->base + 3, static_cast<uint8_t>(lba));
    outb(channel->base + 4, static_cast<uint8_t>(lba >> 8));
    outb(channel->base + 5, static_cast<uint8_t>(lba >> 16));
}

bool transfer(Drive *drive, uint64_t lba, uint32_t count, void *buffer, bool write) {
    if (!drive || !buffer || !count || lba >= drive->block.block_count ||
        count > drive->block.block_count - lba) return false;
    if (!drive->lba48 && (lba >= (UINT64_C(1) << 28) ||
        count > (UINT64_C(1) << 28) - lba)) return false;

    const uint64_t irq = drive->channel->lock.lock();
    outb(drive->channel->control, 0x02);
    auto *bytes = static_cast<uint8_t *>(buffer);
    uint32_t remaining = count;
    bool ok = true;
    while (remaining && ok) {
        const uint16_t batch = remaining > 128 ? 128 : static_cast<uint16_t>(remaining);
        if (drive->lba48) select_lba48(drive, lba, batch);
        else select_lba28(drive, lba, static_cast<uint8_t>(batch));
        outb(drive->channel->base + 7,
             drive->lba48 ? (write ? 0x34 : 0x24) : (write ? 0x30 : 0x20));

        for (uint16_t sector = 0; sector < batch && ok; ++sector) {
            if (!wait_data_request(drive->channel)) { ok = false; break; }
            uint8_t *sector_buffer = bytes + static_cast<uint64_t>(sector) * drive->block.block_size;
            if (write) {
                for (uint32_t i = 0; i < drive->words_per_sector; ++i) {
                    const uint16_t word = static_cast<uint16_t>(sector_buffer[i * 2]) |
                                          (static_cast<uint16_t>(sector_buffer[i * 2 + 1]) << 8);
                    outw(drive->channel->base, word);
                }
            } else {
                for (uint32_t i = 0; i < drive->words_per_sector; ++i) {
                    const uint16_t word = inw(drive->channel->base);
                    sector_buffer[i * 2] = static_cast<uint8_t>(word);
                    sector_buffer[i * 2 + 1] = static_cast<uint8_t>(word >> 8);
                }
            }
        }
        if (ok) ok = wait_not_busy(drive->channel) && !(inb(drive->channel->base + 7) & 0x21);
        bytes += static_cast<uint64_t>(batch) * drive->block.block_size;
        lba += batch;
        remaining -= batch;
    }
    drive->channel->lock.unlock(irq);
    return ok;
}

bool ata_read(Device *device, uint64_t lba, uint32_t count, void *buffer) {
    return transfer(static_cast<Drive *>(device->context), lba, count, buffer, false);
}

bool ata_write(Device *device, uint64_t lba, uint32_t count, const void *buffer) {
    return transfer(static_cast<Drive *>(device->context), lba, count,
                    const_cast<void *>(buffer), true);
}

bool ata_flush(Device *device) {
    auto *drive = static_cast<Drive *>(device->context);
    if (!drive) return false;
    const uint64_t irq = drive->channel->lock.lock();
    outb(drive->channel->control, 0x02);
    select_drive(drive);
    outb(drive->channel->base + 7, drive->lba48 ? 0xea : 0xe7);
    const bool ok = wait_not_busy(drive->channel) && !(inb(drive->channel->base + 7) & 0x21);
    drive->channel->lock.unlock(irq);
    return ok;
}

}

uint32_t initialize() {
    uint32_t found = 0;
    for (uint32_t channel_index = 0; channel_index < 2; ++channel_index) {
        Channel *channel = &gChannels[channel_index];
        for (uint8_t slave = 0; slave < 2; ++slave) {
            if (gDriveCount >= 4) return found;
            Drive &drive = gDrives[gDriveCount];
            drive.channel = channel;
            drive.slave = slave;
            if (!read_identify(&drive)) continue;
            drive.block.read = ata_read;
            drive.block.write = ata_write;
            drive.block.flush = ata_flush;
            if (register_device(&drive.block)) {
                ++gDriveCount;
                ++found;
            }
        }
    }
    return found;
}

}
