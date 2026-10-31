// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for r8169 in the ethernet service module.

//written by saphhic.

#ifndef LAWAI_R8169_H
#define LAWAI_R8169_H

#include <stdint.h>
#include "../pci.h"

constexpr uint16_t R8169_MAX_FRAME_SIZE = 1600;

struct r8169_stats {
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t tx_errors;
    uint32_t rx_errors;
    uint32_t tx_dropped;
    uint32_t rx_dropped;
    uint32_t resets;
};

struct r8169_info {
    bool present;
    bool initialized;
    bool link_up;
    bool using_mmio;
    uint16_t io_base;
    uint32_t mmio_base;
    uint8_t mac[6];
    uint32_t queued_frames;
    eth_pci_device pci_device;
    r8169_stats stats;
};

struct r8169_raw_frame {
    uint16_t length;
    uint32_t status;
    uint8_t bytes[R8169_MAX_FRAME_SIZE];
};

bool r8169_init();
bool r8169_supports_device(const eth_pci_device& device);
bool r8169_init_device(const eth_pci_device& device);
bool r8169_is_ready();
void r8169_poll();
bool r8169_send_raw(const uint8_t* frame, uint16_t length);
bool r8169_receive_raw(r8169_raw_frame* out_frame);
void r8169_get_info(r8169_info* out_info);

#endif // LAWAI_R8169_H
