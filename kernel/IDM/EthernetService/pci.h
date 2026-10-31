// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for pci in the ethernet service module.


//written by saphhic.
#ifndef LAWAI_ETHERNET_PCI_H
#define LAWAI_ETHERNET_PCI_H

#include <stdint.h>

struct eth_pci_device {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint8_t revision_id;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t cache_line_size;
    uint8_t latency_timer;
    uint8_t header_type;
    uint8_t bist;
    uint32_t bar[6];
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
};

typedef bool (*eth_pci_device_visitor)(const eth_pci_device& device, void* context);

uint32_t eth_pci_read_dword(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint16_t eth_pci_read_word(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint8_t eth_pci_read_byte(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
void eth_pci_write_dword(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);
void eth_pci_write_word(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value);
bool eth_pci_read_device(uint8_t bus, uint8_t slot, uint8_t function, eth_pci_device* out_device);
bool eth_pci_for_each_device(eth_pci_device_visitor visitor, void* context);
bool eth_pci_find_device(uint16_t vendor_id, uint16_t device_id, eth_pci_device* out_device);
bool eth_pci_find_class(uint8_t class_code, uint8_t subclass, eth_pci_device* out_device);
void eth_pci_set_command_bits(const eth_pci_device* device, uint16_t command_bits);
uint32_t eth_pci_get_io_bar_base(const eth_pci_device* device, uint8_t bar_index);
uint32_t eth_pci_get_memory_bar_base(const eth_pci_device* device, uint8_t bar_index);

#endif // LAWAI_ETHERNET_PCI_H
