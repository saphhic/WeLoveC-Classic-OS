// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Interprets HID reports from touchpads.

#include "hid_touchpad.hpp"

#include "mouse.hpp"

namespace {
using welovec::x86_64::input::hid_touchpad::Field;
using welovec::x86_64::input::hid_touchpad::State;

constexpr uint32_t kMaximumContacts = 5;
constexpr uint32_t kMaximumCollectionDepth = 16;
constexpr uint32_t kMaximumGlobalDepth = 8;
constexpr uint32_t kMaximumLocalUsages = 64;
constexpr uint8_t kPageGenericDesktop = 0x01;
constexpr uint8_t kPageButton = 0x09;
constexpr uint8_t kPageDigitizers = 0x0d;

struct GlobalState {
    uint32_t usage_page;
    int32_t logical_minimum;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t report_id;
};

struct CollectionState {
    bool touchpad;
    bool mouse;
    bool finger;
    uint8_t finger_slot;
};

struct LocalState {
    uint32_t usages[kMaximumLocalUsages];
    uint32_t usage_count;
    uint32_t usage_minimum;
    uint32_t usage_maximum;
    bool have_usage_minimum;
    bool have_usage_maximum;
};

uint32_t item_unsigned(const uint8_t *bytes, uint32_t size) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < size; ++i)
        value |= static_cast<uint32_t>(bytes[i]) << (i * 8);
    return value;
}

int32_t item_signed(const uint8_t *bytes, uint32_t size) {
    const uint32_t value = item_unsigned(bytes, size);
    if (size == 1) return static_cast<int8_t>(value);
    if (size == 2) return static_cast<int16_t>(value);
    return static_cast<int32_t>(value);
}

uint32_t local_usage(uint32_t value, uint32_t size, uint32_t page) {
    if (size == 4) return value;
    return ((page & 0xffffu) << 16) | (value & 0xffffu);
}

uint32_t usage_at(const LocalState &local, uint32_t index,
                  uint32_t page) {
    if (index < local.usage_count) return local.usages[index];
    if (local.have_usage_minimum && local.have_usage_maximum &&
        local.usage_maximum >= local.usage_minimum) {
        const uint32_t value = local.usage_minimum + index;
        if (value <= local.usage_maximum)
            return local_usage(value, 2, page);
    }
    if (local.usage_count) return local.usages[local.usage_count - 1];
    return 0;
}

void reset_local(LocalState *local) {
    if (!local) return;
    local->usage_count = 0;
    local->usage_minimum = 0;
    local->usage_maximum = 0;
    local->have_usage_minimum = false;
    local->have_usage_maximum = false;
}

void add_field(State *state, const GlobalState &global, uint16_t bit_offset,
               uint32_t usage, uint8_t contact_slot, uint8_t input_flags,
               bool in_touchpad) {
    if (!state || state->field_count == 80 || !usage) return;
    const uint8_t page = static_cast<uint8_t>(usage >> 16);
    const uint16_t id = static_cast<uint16_t>(usage);
    const bool pointer_field = page == kPageGenericDesktop &&
                               (id == 0x30 || id == 0x31 || id == 0x38);
    const bool touch_field = page == kPageDigitizers &&
        (id == 0x42 || id == 0x51 || id == 0x54);
    const bool button_field = page == kPageButton && id >= 1 && id <= 5;
    if (!pointer_field && !touch_field && !button_field) return;
    state->fields[state->field_count++] = Field{
        bit_offset, id, global.logical_minimum, global.report_id,
        static_cast<uint8_t>(global.report_size), page,
        static_cast<uint8_t>(contact_slot < kMaximumContacts
            ? contact_slot : 0xff),
        static_cast<uint8_t>((input_flags & 0x04u) != 0)
    };
    if (in_touchpad &&
        (pointer_field || (page == kPageDigitizers && (id == 0x42 || id == 0x51)))) {
        state->touch_report[global.report_id] = 1;
    }
    if (page == kPageDigitizers && id == 0x42)
        state->has_tip_switch[global.report_id] = 1;
    if (page == kPageDigitizers && id == 0x51)
        state->has_contact_id[global.report_id] = 1;
}

uint32_t read_bits(const uint8_t *data, uint32_t bit_offset, uint32_t bit_size) {
    if (!data || !bit_size || bit_size > 32) return 0;
    uint32_t value = 0;
    for (uint32_t bit = 0; bit < bit_size; ++bit) {
        const uint32_t absolute = bit_offset + bit;
        value |= static_cast<uint32_t>((data[absolute / 8] >> (absolute % 8)) & 1u)
                 << bit;
    }
    return value;
}

int32_t decode_value(uint32_t value, uint32_t size, int32_t logical_minimum) {
    if (logical_minimum >= 0 || !size || size >= 32) return static_cast<int32_t>(value);
    const uint32_t sign = 1u << (size - 1);
    if (value & sign) value |= (~0u << size);
    return static_cast<int32_t>(value);
}

int32_t clamp_i64(int64_t value) {
    if (value < INT32_MIN) return INT32_MIN;
    if (value > INT32_MAX) return INT32_MAX;
    return static_cast<int32_t>(value);
}

uint32_t hid_buttons(uint32_t hid) {
    switch (hid) {
        case 1: return WELOVEC_POINTER_BUTTON_LEFT;
        case 2: return WELOVEC_POINTER_BUTTON_RIGHT;
        case 3: return WELOVEC_POINTER_BUTTON_MIDDLE;
        case 4: return WELOVEC_POINTER_BUTTON_BACK;
        case 5: return WELOVEC_POINTER_BUTTON_FORWARD;
        default: return 0;
    }
}

struct Contact {
    int32_t x;
    int32_t y;
    int32_t id;
    bool have_x;
    bool have_y;
    bool tip;
    bool have_tip;
    bool have_id;
};

int32_t abs32(int32_t value) { return value < 0 ? -value : value; }

void emit(welovec::x86_64::input::hid_touchpad::GestureState *gesture,
          int32_t dx, int32_t dy, int32_t wheel, uint32_t buttons,
          welovec::x86_64::input::pointer::Source source) {
    if (!gesture || (dx == 0 && dy == 0 && wheel == 0 &&
                     buttons == gesture->buttons)) return;
    gesture->buttons = buttons;
    welovec::x86_64::input::pointer::submit_relative(
        dx, dy, wheel, buttons,
        source);
}

void process_contacts(State *state, Contact contacts[kMaximumContacts],
                      uint32_t buttons,
                      welovec::x86_64::input::pointer::Source source) {
    auto &gesture = state->gesture;
    uint8_t active_slots[2]{};
    uint8_t count = 0;
    for (uint8_t i = 0; i < kMaximumContacts; ++i) {
        const bool active = contacts[i].have_x && contacts[i].have_y &&
            (contacts[i].have_tip ? contacts[i].tip :
             contacts[i].have_id ? contacts[i].id != 0 : false);
        if (active) {
            if (count < 2) active_slots[count] = i;
            ++count;
        }
    }

    int32_t dx = 0, dy = 0, wheel = 0;
    if (count == 1) {
        Contact &contact = contacts[active_slots[0]];
        if (gesture.previous_contacts == 1) {
            const int32_t raw_x = contact.x - gesture.previous_x[0];
            const int32_t raw_y = contact.y - gesture.previous_y[0];
            gesture.motion_remainder_x += raw_x;
            gesture.motion_remainder_y += raw_y;
            dx = gesture.motion_remainder_x / 16;
            dy = gesture.motion_remainder_y / 16;
            gesture.motion_remainder_x -= dx * 16;
            gesture.motion_remainder_y -= dy * 16;
            if (abs32(raw_x) + abs32(raw_y) > 16) gesture.tap_candidate = false;
            if (gesture.tap_frames < 255) ++gesture.tap_frames;
        } else {
            gesture.motion_remainder_x = 0;
            gesture.motion_remainder_y = 0;
            gesture.tap_frames = 0;
            gesture.tap_candidate = gesture.previous_contacts == 0;
            gesture.scroll_remainder = 0;
        }
        gesture.previous_x[0] = contact.x;
        gesture.previous_y[0] = contact.y;
    } else if (count == 2) {
        Contact &first = contacts[active_slots[0]];
        Contact &second = contacts[active_slots[1]];
        if (gesture.previous_contacts == 2) {
            const int32_t center_y = (first.y + second.y) / 2;
            const int32_t center_x = (first.x + second.x) / 2;
            const int32_t previous_center_y =
                (gesture.previous_y[0] + gesture.previous_y[1]) / 2;
            const int32_t previous_center_x =
                (gesture.previous_x[0] + gesture.previous_x[1]) / 2;
            const int32_t raw_x = center_x - previous_center_x;
            const int32_t raw_y = center_y - previous_center_y;
            gesture.scroll_remainder += raw_y;
            wheel = -(gesture.scroll_remainder / 80);
            gesture.scroll_remainder += wheel * 80;
            if (abs32(raw_x) + abs32(raw_y) > 16)
                gesture.tap_candidate = false;
            if (gesture.tap_frames < 255) ++gesture.tap_frames;
        } else {
            gesture.tap_candidate = gesture.previous_contacts == 0;
            gesture.tap_frames = 0;
            gesture.scroll_remainder = 0;
            gesture.motion_remainder_x = 0;
        }
        gesture.previous_x[0] = first.x;
        gesture.previous_y[0] = first.y;
        gesture.previous_x[1] = second.x;
        gesture.previous_y[1] = second.y;
    } else if (count == 0 && gesture.previous_contacts != 0) {
        if (gesture.tap_candidate && gesture.tap_frames <= 20 && !buttons) {
            const uint32_t tap_button = gesture.previous_contacts == 2
                ? WELOVEC_POINTER_BUTTON_RIGHT : WELOVEC_POINTER_BUTTON_LEFT;
            emit(&gesture, 0, 0, 0, tap_button, source);
            emit(&gesture, 0, 0, 0, 0, source);
        }
        gesture.tap_candidate = false;
        gesture.tap_frames = 0;
        gesture.motion_remainder_x = 0;
        gesture.motion_remainder_y = 0;
        gesture.scroll_remainder = 0;
    } else if (count > 2) {
        gesture.tap_candidate = false;
    }

    emit(&gesture, dx, dy, wheel, buttons, source);
    gesture.previous_contacts = count;
}
}

namespace welovec::x86_64::input::hid_touchpad {

bool parse_report_descriptor(State *state, const uint8_t *descriptor,
                             uint32_t length) {
    if (!state || !descriptor || !length || length > 4096) return false;
    *state = {};
    GlobalState global{};
    GlobalState global_stack[kMaximumGlobalDepth]{};
    CollectionState collections[kMaximumCollectionDepth]{};
    LocalState local{};
    uint16_t report_offsets[256]{};
    uint8_t next_finger_slot = 0;
    uint8_t collection_depth = 0;
    uint8_t global_depth = 0;
    bool touchpad_found = false;
    bool mouse_found = false;

    for (uint32_t cursor = 0; cursor < length;) {
        const uint8_t prefix = descriptor[cursor++];
        if (prefix == 0xfe) {
            if (cursor + 2 > length) return false;
            const uint8_t skip = descriptor[cursor];
            cursor += 2;
            if (skip > length - cursor) return false;
            cursor += skip;
            continue;
        }
        uint32_t size = prefix & 3u;
        if (size == 3) size = 4;
        const uint8_t type = (prefix >> 2) & 3u;
        const uint8_t tag = prefix >> 4;
        if (size > length - cursor) return false;
        const uint32_t raw = item_unsigned(descriptor + cursor, size);
        cursor += size;

        if (type == 1) { // Global items.
            switch (tag) {
                case 0: global.usage_page = raw; break;
                case 1: global.logical_minimum = item_signed(descriptor + cursor - size, size); break;
                case 7: global.report_size = raw; break;
                case 8:
                    if (!raw || raw > 255) return false;
                    global.report_id = static_cast<uint8_t>(raw);
                    state->uses_report_ids = 1;
                    break;
                case 9: global.report_count = raw; break;
                case 10:
                    if (global_depth == kMaximumGlobalDepth) return false;
                    global_stack[global_depth++] = global;
                    break;
                case 11:
                    if (!global_depth) return false;
                    global = global_stack[--global_depth];
                    break;
                default: break;
            }
        } else if (type == 2) { // Local items.
            const uint32_t usage = local_usage(raw, size, global.usage_page);
            switch (tag) {
                case 0:
                    if (local.usage_count < kMaximumLocalUsages)
                        local.usages[local.usage_count++] = usage;
                    break;
                case 1:
                    local.usage_minimum = usage;
                    local.have_usage_minimum = true;
                    break;
                case 2:
                    local.usage_maximum = usage;
                    local.have_usage_maximum = true;
                    break;
                default: break;
            }
        } else if (type == 0) { // Main items.
            if (tag == 10) { // Collection.
                if (collection_depth == kMaximumCollectionDepth) return false;
                const uint32_t usage = usage_at(local, 0, global.usage_page);
                const uint8_t usage_page = static_cast<uint8_t>(usage >> 16);
                const uint16_t usage_id = static_cast<uint16_t>(usage);
                const CollectionState parent = collection_depth
                    ? collections[collection_depth - 1] : CollectionState{};
                const bool is_application = (raw & 0xffu) == 1;
                const bool is_touchpad = is_application &&
                    usage_page == kPageDigitizers && usage_id == 0x05;
                const bool is_mouse = is_application &&
                    usage_page == kPageGenericDesktop &&
                    (usage_id == 0x01 || usage_id == 0x02);
                const bool is_finger = usage_page == kPageDigitizers &&
                                       usage_id == 0x22;
                CollectionState current{
                    parent.touchpad || is_touchpad,
                    parent.mouse || is_mouse,
                    parent.finger || (parent.touchpad && is_finger),
                    parent.finger_slot
                };
                if (is_finger && parent.touchpad && !parent.finger) {
                    if (next_finger_slot < kMaximumContacts)
                        current.finger_slot = next_finger_slot++;
                    else current.finger_slot = 0xff;
                }
                collections[collection_depth++] = current;
                touchpad_found |= current.touchpad;
                mouse_found |= current.mouse;
            } else if (tag == 12) { // End Collection.
                if (!collection_depth) return false;
                --collection_depth;
            } else if (tag == 8) { // Input.
                if (global.report_size > 32 || global.report_count > 80 ||
                    !global.report_size || !global.report_count) return false;
                const uint32_t bits = global.report_size * global.report_count;
                const uint8_t id = global.report_id;
                if (bits > 8192 || report_offsets[id] > 8192 - bits) return false;
                const bool in_touchpad = collection_depth &&
                    collections[collection_depth - 1].touchpad;
                const bool in_mouse = collection_depth &&
                    collections[collection_depth - 1].mouse;
                const bool in_finger = in_touchpad &&
                    collections[collection_depth - 1].finger;
                if (!(raw & 1u) && (in_touchpad || in_mouse ||
                    (global.usage_page == kPageButton && collection_depth &&
                     collections[collection_depth - 1].touchpad))) {
                    if (in_mouse) state->mouse_report[id] = 1;
                    for (uint32_t index = 0; index < global.report_count; ++index) {
                        const uint32_t usage = usage_at(local, index, global.usage_page);
                        uint8_t contact = in_finger
                            ? collections[collection_depth - 1].finger_slot : 0xff;
                        if (in_finger && global.report_count > 1 && contact != 0xff)
                            contact = static_cast<uint8_t>(contact + index);
                        add_field(state, global,
                            static_cast<uint16_t>(report_offsets[id] +
                                index * global.report_size), usage, contact,
                            static_cast<uint8_t>(raw), in_touchpad);
                    }
                }
                report_offsets[id] = static_cast<uint16_t>(report_offsets[id] + bits);
            }
            reset_local(&local);
        }
    }

    if ((!touchpad_found && !mouse_found) || !state->field_count) return false;
    bool has_x = false;
    bool has_y = false;
    for (uint32_t i = 0; i < state->field_count; ++i) {
        has_x |= state->fields[i].usage_page == kPageGenericDesktop &&
                 state->fields[i].usage == 0x30;
        has_y |= state->fields[i].usage_page == kPageGenericDesktop &&
                 state->fields[i].usage == 0x31;
    }
    if (!has_x || !has_y) return false;
    for (uint32_t id = 0; id < 256; ++id) {
        if (!report_offsets[id]) continue;
        const uint32_t bytes = (report_offsets[id] + 7) / 8 +
                               (state->uses_report_ids ? 1 : 0);
        if (bytes > kMaximumReportBytes) return false;
        state->report_bytes[id] = static_cast<uint16_t>(bytes);
        if (bytes > state->maximum_report_bytes)
            state->maximum_report_bytes = static_cast<uint16_t>(bytes);
    }
    return state->maximum_report_bytes != 0;
}

uint16_t maximum_report_size(const State *state) {
    return state ? state->maximum_report_bytes : 0;
}

bool process_report(State *state, const uint8_t *report, uint32_t length,
                    pointer::Source source) {
    if (!state || !report || !length) return false;
    const uint8_t report_id = state->uses_report_ids ? report[0] : 0;
    const uint16_t expected = state->report_bytes[report_id];
    if (!expected || length < expected) return false;
    const uint32_t bit_base = state->uses_report_ids ? 8 : 0;
    if (!state->touch_report[report_id] && state->mouse_report[report_id]) {
        int64_t dx = 0, dy = 0, wheel = 0;
        const uint32_t previous_buttons = state->gesture.buttons;
        uint32_t buttons = state->gesture.buttons;
        for (uint32_t i = 0; i < state->field_count; ++i) {
            const Field &field = state->fields[i];
            if (field.report_id != report_id || !field.report_size) continue;
            const int32_t value = decode_value(
                read_bits(report, bit_base + field.bit_offset, field.report_size),
                field.report_size, field.logical_minimum);
            if (field.usage_page == kPageButton) {
                const uint32_t mask = hid_buttons(field.usage);
                if (mask) {
                    if (value) buttons |= mask;
                    else buttons &= ~mask;
                }
            } else if (field.usage_page == kPageGenericDesktop) {
                int64_t *axis = nullptr;
                bool *axis_valid = nullptr;
                int32_t *previous = nullptr;
                if (field.usage == 0x30) {
                    axis = &dx;
                    axis_valid = &state->gesture.mouse_x_valid;
                    previous = &state->gesture.previous_x[0];
                } else if (field.usage == 0x31) {
                    axis = &dy;
                    axis_valid = &state->gesture.mouse_y_valid;
                    previous = &state->gesture.previous_y[0];
                } else if (field.usage == 0x38) {
                    axis = &wheel;
                }
                if (!axis) continue;
                if (field.relative) {
                    *axis += value;
                } else if (previous && axis_valid) {
                    if (*axis_valid) *axis += static_cast<int64_t>(value) - *previous;
                    *previous = value;
                    *axis_valid = true;
                }
            }
        }
        if (dx || dy || wheel || buttons != previous_buttons) {
            state->gesture.buttons = buttons;
            welovec::x86_64::input::pointer::submit_relative(
                clamp_i64(dx), clamp_i64(dy), clamp_i64(wheel), buttons, source);
        }
        return true;
    }
    Contact contacts[kMaximumContacts]{};
    int32_t reported_count = -1;
    uint32_t buttons = state->gesture.buttons;
    bool button_report = false;
    for (uint32_t i = 0; i < state->field_count; ++i) {
        const Field &field = state->fields[i];
        if (field.report_id != report_id || !field.report_size) continue;
        const int32_t value = decode_value(
            read_bits(report, bit_base + field.bit_offset, field.report_size),
            field.report_size, field.logical_minimum);
        if (field.usage_page == kPageButton) {
            const uint32_t mask = hid_buttons(field.usage);
            if (mask) {
                button_report = true;
                if (value) buttons |= mask;
                else buttons &= ~mask;
            }
            continue;
        }
        if (field.usage_page == kPageDigitizers && field.usage == 0x54) {
            reported_count = value;
            continue;
        }
        if (field.contact_slot >= kMaximumContacts) continue;
        Contact &contact = contacts[field.contact_slot];
        if (field.usage_page == kPageGenericDesktop && field.usage == 0x30) {
            contact.x = value;
            contact.have_x = true;
        } else if (field.usage_page == kPageGenericDesktop && field.usage == 0x31) {
            contact.y = value;
            contact.have_y = true;
        } else if (field.usage_page == kPageDigitizers && field.usage == 0x42) {
            contact.tip = value != 0;
            contact.have_tip = true;
        } else if (field.usage_page == kPageDigitizers && field.usage == 0x51) {
            contact.id = value;
            contact.have_id = true;
        }
    }
    if (!state->touch_report[report_id]) {
        if (!button_report) return false;
        if (buttons == state->gesture.buttons) return true;
        state->gesture.buttons = buttons;
        welovec::x86_64::input::pointer::submit_relative(
            0, 0, 0, buttons,
            source);
        return true;
    }
    if (!state->has_tip_switch[report_id]) {
        if (reported_count >= 0) {
            for (uint32_t i = 0; i < kMaximumContacts; ++i) {
                contacts[i].tip = i < static_cast<uint32_t>(reported_count);
                contacts[i].have_tip = true;
            }
        } else if (state->has_contact_id[report_id]) {
            for (uint32_t i = 0; i < kMaximumContacts; ++i) {
                if (!contacts[i].have_id) continue;
                contacts[i].tip = contacts[i].id != 0;
                contacts[i].have_tip = true;
            }
        }
    }
    process_contacts(state, contacts, buttons, source);
    return true;
}

}
