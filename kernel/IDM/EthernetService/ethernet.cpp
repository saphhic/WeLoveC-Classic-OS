// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Manages the Ethernet interface and packets.

//written by saphhic.

#include "ethernet.h"
#include "debug.h"
#include "pci.h"
#include "Drivers/r8169.h"
#include "Drivers/rtl8139.h"

extern "C" void* memcpy(void* destination, const void* source, unsigned long long size);
extern "C" void* memset(void* destination, int value, unsigned long long size);

namespace {

static bool g_ethernet_probed = false;
static uint8_t g_active_driver = ETHERNET_DRIVER_NONE;

constexpr uint8_t kPciClassNetwork = 0x02;
constexpr uint8_t kPciSubclassEthernet = 0x00;

struct ethernet_probe_context {
    bool saw_ethernet;
    bool saw_supported;
    bool loaded;
    uint8_t failed_driver;
    eth_pci_device first_ethernet;
    eth_pci_device failed_device;
};

static uint16_t host_to_network16(uint16_t value) {
    return static_cast<uint16_t>((value >> 8) | (value << 8));
}

static const char* ethernet_driver_name(uint8_t driver) {
    if (driver == ETHERNET_DRIVER_R8169) {
        return "r8169";
    }

    if (driver == ETHERNET_DRIVER_RTL8139) {
        return "RTL8139";
    }

    return "none";
}

static void print_pci_location(const eth_pci_device& device) {
    eth_dbg_print_hex8(device.bus);
    putchar(':');
    eth_dbg_print_hex8(device.slot);
    putchar('.');
    eth_dbg_print_hex8(device.function);
}

static bool is_ethernet_controller(const eth_pci_device& device) {
    return device.class_code == kPciClassNetwork &&
           device.subclass == kPciSubclassEthernet;
}

static void print_unsupported_network_controller(const ethernet_probe_context& context) {
    if (!context.saw_ethernet) {
        printf("[ethernet] no network controller detected.\n");
        return;
    }

    printf("[ethernet] no compatible driver for ethernet controller PCI ");
    print_pci_location(context.first_ethernet);
    printf(" vendor=0x");
    eth_dbg_print_hex16(context.first_ethernet.vendor_id);
    printf(" device=0x");
    eth_dbg_print_hex16(context.first_ethernet.device_id);
    printf("; no driver loaded.\n");
}

static void print_driver_load_failed(const ethernet_probe_context& context) {
    printf("[ethernet] ");
    printf(ethernet_driver_name(context.failed_driver));
    printf(" matched PCI ");
    print_pci_location(context.failed_device);
    printf(" vendor=0x");
    eth_dbg_print_hex16(context.failed_device.vendor_id);
    printf(" device=0x");
    eth_dbg_print_hex16(context.failed_device.device_id);
    printf(" but failed to initialize; no driver loaded.\n");
}

static void print_r8169_ready() {
    r8169_info info = {};
    r8169_get_info(&info);

    printf("[ethernet] r8169 ready on PCI ");
    print_pci_location(info.pci_device);
    if (info.using_mmio) {
        printf(" mmio=0x");
        eth_dbg_print_hex32(info.mmio_base);
    } else {
        printf(" io=0x");
        eth_dbg_print_hex16(info.io_base);
    }
    printf(" mac=");
    eth_dbg_print_mac(info.mac);
    printf(" link=");
    printf(info.link_up ? "up" : "down");
    printf(".\n");
}

static void print_rtl8139_ready() {
    rtl8139_info info = {};
    rtl8139_get_info(&info);

    printf("[ethernet] RTL8139 ready on PCI ");
    print_pci_location(info.pci_device);
    printf(" io=0x");
    eth_dbg_print_hex16(info.io_base);
    printf(" mac=");
    eth_dbg_print_mac(info.mac);
    printf(" link=");
    printf(info.link_up ? "up" : "down");
    printf(".\n");
}

static bool active_driver_is_ready() {
    if (g_active_driver == ETHERNET_DRIVER_R8169) {
        return r8169_is_ready();
    }

    if (g_active_driver == ETHERNET_DRIVER_RTL8139) {
        return rtl8139_is_ready();
    }

    return false;
}

static void active_driver_poll() {
    if (g_active_driver == ETHERNET_DRIVER_R8169) {
        r8169_poll();
    } else if (g_active_driver == ETHERNET_DRIVER_RTL8139) {
        rtl8139_poll();
    }
}

static bool active_driver_send_raw(const uint8_t* frame, uint16_t length) {
    if (g_active_driver == ETHERNET_DRIVER_R8169) {
        return r8169_send_raw(frame, length);
    }

    if (g_active_driver == ETHERNET_DRIVER_RTL8139) {
        return rtl8139_send_raw(frame, length);
    }

    return false;
}

static bool decode_raw_frame(ethernet_packet* out_packet, const uint8_t* bytes, uint16_t length) {
    if (out_packet == nullptr || bytes == nullptr || length < 14) {
        return false;
    }

    const uint16_t payload_length = static_cast<uint16_t>(length - 14);
    if (payload_length > ETHERNET_MAX_PAYLOAD_SIZE) {
        return false;
    }

    memcpy(out_packet->destination, &bytes[0], ETHERNET_ADDRESS_LENGTH);
    memcpy(out_packet->source, &bytes[6], ETHERNET_ADDRESS_LENGTH);
    out_packet->ether_type = static_cast<uint16_t>(
        (static_cast<uint16_t>(bytes[12]) << 8) |
        static_cast<uint16_t>(bytes[13])
    );
    out_packet->payload_length = payload_length;
    if (payload_length > 0) {
        memcpy(out_packet->payload, &bytes[14], payload_length);
    }

    return true;
}

static bool active_driver_receive_raw(ethernet_packet* out_packet) {
    if (g_active_driver == ETHERNET_DRIVER_R8169) {
        r8169_raw_frame raw_frame = {};
        if (!r8169_receive_raw(&raw_frame)) {
            return false;
        }

        return decode_raw_frame(out_packet, raw_frame.bytes, raw_frame.length);
    }

    if (g_active_driver == ETHERNET_DRIVER_RTL8139) {
        rtl8139_raw_frame raw_frame = {};
        if (!rtl8139_receive_raw(&raw_frame)) {
            return false;
        }

        return decode_raw_frame(out_packet, raw_frame.bytes, raw_frame.length);
    }

    return false;
}

static bool probe_ethernet_device(const eth_pci_device& device, void* raw_context) {
    ethernet_probe_context* context = static_cast<ethernet_probe_context*>(raw_context);
    if (context == nullptr || !is_ethernet_controller(device)) {
        return false;
    }

    if (!context->saw_ethernet) {
        context->saw_ethernet = true;
        context->first_ethernet = device;
    }

    if (r8169_supports_device(device)) {
        context->saw_supported = true;
        if (r8169_init_device(device)) {
            g_active_driver = ETHERNET_DRIVER_R8169;
            context->loaded = true;
            return true;
        }

        context->failed_driver = ETHERNET_DRIVER_R8169;
        context->failed_device = device;
        return false;
    }

    if (rtl8139_supports_device(device)) {
        context->saw_supported = true;
        if (rtl8139_init_device(device)) {
            g_active_driver = ETHERNET_DRIVER_RTL8139;
            context->loaded = true;
            return true;
        }

        context->failed_driver = ETHERNET_DRIVER_RTL8139;
        context->failed_device = device;
        return false;
    }

    return false;
}

} // namespace

bool ethernet_init() {
    if (g_ethernet_probed) {
        return active_driver_is_ready();
    }

    g_ethernet_probed = true;
    g_active_driver = ETHERNET_DRIVER_NONE;
    printf("[ethernet] probing PCI NICs...\n");

    ethernet_probe_context context = {};
    eth_pci_for_each_device(probe_ethernet_device, &context);

    if (g_active_driver == ETHERNET_DRIVER_R8169) {
        printf("\n");
        print_r8169_ready();
        printf("\n");
        return true;
    }

    if (g_active_driver == ETHERNET_DRIVER_RTL8139) {
        printf("\n");
        print_rtl8139_ready();
        printf("\n");
        return true;
    }

    if (context.saw_supported) {
        printf("\n");
        print_driver_load_failed(context);
        printf("\n");
        return false;
    }
    
    printf("\n");
    print_unsupported_network_controller(context);
    printf("\n");
    return false;
}

bool ethernet_is_ready() {
    return active_driver_is_ready();
}

void ethernet_poll() {
    active_driver_poll();
}

bool ethernet_send(const uint8_t destination[ETHERNET_ADDRESS_LENGTH], uint16_t ether_type, const void* payload, uint16_t payload_length) {
    if (!active_driver_is_ready() || destination == nullptr) {
        return false;
    }

    if (payload_length > ETHERNET_MAX_PAYLOAD_SIZE) {
        return false;
    }

    ethernet_info info = {};
    ethernet_get_info(&info);
    if (!info.ready) {
        return false;
    }

    uint8_t frame[14 + ETHERNET_MAX_PAYLOAD_SIZE];
    memset(frame, 0, sizeof(frame));

    memcpy(&frame[0], destination, ETHERNET_ADDRESS_LENGTH);
    memcpy(&frame[6], info.mac, ETHERNET_ADDRESS_LENGTH);

    const uint16_t network_ether_type = host_to_network16(ether_type);
    memcpy(&frame[12], &network_ether_type, sizeof(network_ether_type));

    if (payload_length > 0 && payload != nullptr) {
        memcpy(&frame[14], payload, payload_length);
    }

    return active_driver_send_raw(frame, static_cast<uint16_t>(14 + payload_length));
}

bool ethernet_receive(ethernet_packet* out_packet) {
    if (out_packet == nullptr || !active_driver_is_ready()) {
        return false;
    }

    active_driver_poll();
    return active_driver_receive_raw(out_packet);
}

void ethernet_get_info(ethernet_info* out_info) {
    if (out_info == nullptr) {
        return;
    }

    memset(out_info, 0, sizeof(ethernet_info));
    out_info->driver = g_active_driver;

    if (g_active_driver == ETHERNET_DRIVER_R8169) {
        r8169_info info = {};
        r8169_get_info(&info);

        out_info->ready = info.initialized;
        out_info->link_up = info.link_up;
        out_info->using_mmio = info.using_mmio;
        out_info->pci_vendor_id = info.pci_device.vendor_id;
        out_info->pci_device_id = info.pci_device.device_id;
        out_info->io_base = info.io_base;
        out_info->mmio_base = info.mmio_base;
        out_info->tx_packets = info.stats.tx_packets;
        out_info->rx_packets = info.stats.rx_packets;
        out_info->tx_errors = info.stats.tx_errors;
        out_info->rx_errors = info.stats.rx_errors;
        out_info->tx_dropped = info.stats.tx_dropped;
        out_info->rx_dropped = info.stats.rx_dropped;
        out_info->resets = info.stats.resets;
        out_info->queued_frames = info.queued_frames;
        memcpy(out_info->mac, info.mac, ETHERNET_ADDRESS_LENGTH);
        return;
    }

    if (g_active_driver == ETHERNET_DRIVER_RTL8139) {
        rtl8139_info info = {};
        rtl8139_get_info(&info);

        out_info->ready = info.initialized;
        out_info->link_up = info.link_up;
        out_info->using_mmio = false;
        out_info->pci_vendor_id = info.pci_device.vendor_id;
        out_info->pci_device_id = info.pci_device.device_id;
        out_info->io_base = info.io_base;
        out_info->tx_packets = info.stats.tx_packets;
        out_info->rx_packets = info.stats.rx_packets;
        out_info->tx_errors = info.stats.tx_errors;
        out_info->rx_errors = info.stats.rx_errors;
        out_info->tx_dropped = info.stats.tx_dropped;
        out_info->rx_dropped = info.stats.rx_dropped;
        out_info->resets = info.stats.resets;
        out_info->queued_frames = info.queued_frames;
        memcpy(out_info->mac, info.mac, ETHERNET_ADDRESS_LENGTH);
    }
}

void ethernet_debug_print_status() {
    ethernet_info info = {};
    ethernet_get_info(&info);

    if (!info.ready) {
        uprint("[ethernet] driver not ready.\n");
        return;
    }

    uprint("[ethernet] driver=");
    uprint(ethernet_driver_name(info.driver));
    putchar(' ');
    if (info.using_mmio) {
        uprint("mmio=0x");
        eth_dbg_print_hex32(info.mmio_base);
    } else {
        uprint("io=0x");
        eth_dbg_print_hex16(info.io_base);
    }
    uprint(" mac=");
    eth_dbg_print_mac(info.mac);
    uprint(" link=");
    uprint(info.link_up ? "up" : "down");
    uprint(" tx=");
    eth_dbg_print_dec(info.tx_packets);
    uprint(" rx=");
    eth_dbg_print_dec(info.rx_packets);
    uprint(" queued=");
    eth_dbg_print_dec(info.queued_frames);
    uprint(" txerr=");
    eth_dbg_print_dec(info.tx_errors);
    uprint(" rxerr=");
    eth_dbg_print_dec(info.rx_errors);
    uprint(" drops=");
    eth_dbg_print_dec(info.tx_dropped + info.rx_dropped);
    uprint(" resets=");
    eth_dbg_print_dec(info.resets);
    uprint(".\n");
}

void ethernet_debug_drain_frames() {
    if (!active_driver_is_ready()) {
        printf("[ethernet] driver not ready.\n");
        return;
    }

    ethernet_poll();

    uint32_t drained = 0;
    ethernet_packet packet = {};
    while (ethernet_receive(&packet) && drained < 16) {
        printf("[ethernet] rx type=0x");
        eth_dbg_print_hex16(packet.ether_type);
        printf(" len=");
        eth_dbg_print_dec(packet.payload_length);
        printf(" dst=");
        eth_dbg_print_mac(packet.destination);
        printf(" src=");
        eth_dbg_print_mac(packet.source);
        printf(".\n");
        ++drained;
    }

    if (drained == 0) {
        printf("[ethernet] no queued frames.\n");
    }
}
