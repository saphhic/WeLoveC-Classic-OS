// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for ethernet in the ethernet service module.

//written by saphhic.

#ifndef LAWAI_ETHERNET_H
#define LAWAI_ETHERNET_H

#include <stdint.h>

constexpr uint16_t ETHERNET_ADDRESS_LENGTH = 6;
constexpr uint16_t ETHERNET_MAX_PAYLOAD_SIZE = 1500;
constexpr uint8_t ETHERNET_DRIVER_NONE = 0;
constexpr uint8_t ETHERNET_DRIVER_RTL8139 = 1;
constexpr uint8_t ETHERNET_DRIVER_R8169 = 2;

struct ethernet_packet {
    uint8_t destination[ETHERNET_ADDRESS_LENGTH];
    uint8_t source[ETHERNET_ADDRESS_LENGTH];
    uint16_t ether_type;
    uint16_t payload_length;
    uint8_t payload[ETHERNET_MAX_PAYLOAD_SIZE];
};

struct ethernet_info {
    bool ready;
    bool link_up;
    bool using_mmio;
    uint8_t driver;
    uint8_t mac[ETHERNET_ADDRESS_LENGTH];
    uint16_t pci_vendor_id;
    uint16_t pci_device_id;
    uint16_t io_base;
    uint32_t mmio_base;
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t tx_errors;
    uint32_t rx_errors;
    uint32_t tx_dropped;
    uint32_t rx_dropped;
    uint32_t resets;
    uint32_t queued_frames;
};

bool ethernet_init();
bool ethernet_is_ready();
void ethernet_poll();
bool ethernet_send(const uint8_t destination[ETHERNET_ADDRESS_LENGTH], uint16_t ether_type, const void* payload, uint16_t payload_length);
bool ethernet_receive(ethernet_packet* out_packet);
void ethernet_get_info(ethernet_info* out_info);
void ethernet_debug_print_status();
void ethernet_debug_drain_frames();

#endif // LAWAI_ETHERNET_H
