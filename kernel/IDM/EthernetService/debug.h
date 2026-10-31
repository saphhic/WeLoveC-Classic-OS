// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for debug in the ethernet service module.

//written by saphhic.

#ifndef LAWAI_ETHERNET_DEBUG_H
#define LAWAI_ETHERNET_DEBUG_H

#include <stdint.h>
#include "platform.h"

static inline void eth_dbg_print_hex_nibble(uint8_t value) {
    if (value < 10) {
        putchar(static_cast<char>('0' + value));
        return;
    }

    putchar(static_cast<char>('A' + (value - 10)));
}

static inline void eth_dbg_print_hex8(uint8_t value) {
    eth_dbg_print_hex_nibble(static_cast<uint8_t>((value >> 4) & 0x0F));
    eth_dbg_print_hex_nibble(static_cast<uint8_t>(value & 0x0F));
}

static inline void eth_dbg_print_hex16(uint16_t value) {
    eth_dbg_print_hex8(static_cast<uint8_t>((value >> 8) & 0xFF));
    eth_dbg_print_hex8(static_cast<uint8_t>(value & 0xFF));
}

static inline void eth_dbg_print_hex32(uint32_t value) {
    eth_dbg_print_hex16(static_cast<uint16_t>((value >> 16) & 0xFFFF));
    eth_dbg_print_hex16(static_cast<uint16_t>(value & 0xFFFF));
}

static inline void eth_dbg_print_dec(uint32_t value) {
    char buffer[10];
    int index = 0;

    if (value == 0) {
        putchar('0');
        return;
    }

    while (value > 0 && index < static_cast<int>(sizeof(buffer))) {
        buffer[index++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }

    while (index > 0) {
        putchar(buffer[--index]);
    }
}

static inline void eth_dbg_print_mac(const uint8_t mac[6]) {
    for (int index = 0; index < 6; ++index) {
        if (index != 0) {
            putchar(':');
        }

        eth_dbg_print_hex8(mac[index]);
    }
}

static inline void eth_dbg_print_ipv4(const uint8_t address[4]) {
    for (int index = 0; index < 4; ++index) {
        if (index != 0) {
            putchar('.');
        }

        eth_dbg_print_dec(address[index]);
    }
}

#endif // LAWAI_ETHERNET_DEBUG_H
