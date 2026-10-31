// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the shared queue for mouse and pointer device events.

#include "mouse.hpp"

#include "arch/x86_64/mm/spinlock.hpp"

namespace welovec::x86_64::input::pointer {

namespace {
constexpr uint32_t kQueueCapacity = 128;
welovec::x86_64::mm::IrqSpinLock gLock;
welovec_pointer_event gQueue[kQueueCapacity]{};
uint32_t gRead;
uint32_t gWrite;
uint32_t gSources;

void push_locked(const welovec_pointer_event &event) {
    const uint32_t next = (gWrite + 1) % kQueueCapacity;
    if (next == gRead) gRead = (gRead + 1) % kQueueCapacity;
    gQueue[gWrite] = event;
    gWrite = next;
}

int32_t normalize_axis(int32_t value, int32_t minimum, int32_t maximum) {
    if (maximum <= minimum) return 0;
    if (value <= minimum) return 0;
    if (value >= maximum) return 65535;
    const int64_t offset = static_cast<int64_t>(value) - minimum;
    const int64_t range = static_cast<int64_t>(maximum) - minimum;
    return static_cast<int32_t>((offset * 65535) / range);
}
}

void register_source(Source source) {
    const uint64_t irq = gLock.lock_irqsave();
    gSources |= static_cast<uint32_t>(source);
    gLock.unlock_irqrestore(irq);
}

uint32_t detected_sources() {
    const uint64_t irq = gLock.lock_irqsave();
    const uint32_t sources = gSources;
    gLock.unlock_irqrestore(irq);
    return sources;
}

void submit_relative(int32_t delta_x, int32_t delta_y, int32_t wheel,
                     uint32_t buttons, Source source) {
    const welovec_pointer_event event{
        delta_x, delta_y, wheel, 0, 0, buttons,
        WELOVEC_POINTER_EVENT_RELATIVE, static_cast<uint32_t>(source), 0
    };
    const uint64_t irq = gLock.lock_irqsave();
    push_locked(event);
    gLock.unlock_irqrestore(irq);
}

void submit_absolute(int32_t x, int32_t y, int32_t min_x, int32_t max_x,
                     int32_t min_y, int32_t max_y, uint32_t buttons,
                     Source source) {
    const welovec_pointer_event event{
        0, 0, 0, normalize_axis(x, min_x, max_x), normalize_axis(y, min_y, max_y),
        buttons, WELOVEC_POINTER_EVENT_ABSOLUTE,
        static_cast<uint32_t>(source), 0
    };
    const uint64_t irq = gLock.lock_irqsave();
    push_locked(event);
    gLock.unlock_irqrestore(irq);
}

int64_t read_events(welovec_pointer_event *events, uint64_t capacity) {
    if (!events || !capacity) return 0;
    const uint64_t irq = gLock.lock_irqsave();
    uint64_t count = 0;
    while (count < capacity && gRead != gWrite) {
        events[count++] = gQueue[gRead];
        gRead = (gRead + 1) % kQueueCapacity;
    }
    gLock.unlock_irqrestore(irq);
    return static_cast<int64_t>(count);
}

}

extern "C" int64_t welovec_pointer_read(welovec_pointer_event *events,
                                         uint64_t capacity) {
    return welovec::x86_64::input::pointer::read_events(events, capacity);
}

extern "C" uint32_t welovec_pointer_sources(void) {
    return welovec::x86_64::input::pointer::detected_sources();
}
