// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for rtl8139 in the ethernet service module.


//written by saphhic.
#ifndef LAWAI_RTL8139_H
#define LAWAI_RTL8139_H

#include <stdint.h>
#include "../pci.h"

constexpr uint16_t RTL8139_MAX_FRAME_SIZE = 1600;

struct rtl8139_stats {
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t tx_errors;
    uint32_t rx_errors;
    uint32_t tx_dropped;
    uint32_t rx_dropped;
    uint32_t resets;
};

struct rtl8139_info {
    bool present;
    bool initialized;
    bool link_up;
    uint16_t io_base;
    uint8_t mac[6];
    uint32_t queued_frames;
    eth_pci_device pci_device;
    rtl8139_stats stats;
};

struct rtl8139_raw_frame {
    uint16_t length;
    uint16_t status;
    uint8_t bytes[RTL8139_MAX_FRAME_SIZE];
};

bool rtl8139_init();
bool rtl8139_supports_device(const eth_pci_device& device);
bool rtl8139_init_device(const eth_pci_device& device);
bool rtl8139_is_ready();
void rtl8139_poll();
bool rtl8139_send_raw(const uint8_t* frame, uint16_t length);
bool rtl8139_receive_raw(rtl8139_raw_frame* out_frame);
void rtl8139_get_info(rtl8139_info* out_info);

#endif // LAWAI_RTL8139_H
