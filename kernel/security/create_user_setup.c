// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the setup screen for the first administrator account.

#include "../IDM/DisplayService/framebuffer.hpp"
#include "../arch/x86_64/input/keyboard.h"
#include "../arch/x86_64/scheduler/scheduler.h"
#include "../include/user/security_types.h"
#include "createuser.h"
#include "privileges.h"

#define SETUP_MIN_WIDTH 500
#define SETUP_MIN_HEIGHT 360

static int setup_panel_x;
static int setup_panel_y;
static int setup_panel_width;
static int setup_panel_height;
static int setup_label_x;
static int setup_field_x;
static int setup_field_width;
static int setup_user_y;
static int setup_password_y;

static void clear_secret(char *secret, uint32_t capacity) {
    volatile char *cursor = secret;
    while (capacity--) *cursor++ = '\0';
}

static uint32_t string_length(const char *text) {
    uint32_t length = 0;
    if (!text) return 0;
    while (text[length]) ++length;
    return length;
}

static bool same_text(const char *left, const char *right) {
    uint32_t index = 0;
    while (left[index] && right[index] && left[index] == right[index]) ++index;
    return left[index] == right[index];
}

static void draw_small_text(int x, int y, const char *text, uint32_t color) {
    if (!text) return;
    while (*text) {
        draw_char_font(x, y, *text++, color, 0, 1,
                       WELOVEC_FB_FONT_SYSTEM8);
        x += 8;
    }
}

static void draw_text_16(int x, int y, const char *text, uint32_t color) {
    if (!text) return;
    while (*text) {
        draw_char_font(x, y, *text++, color, 0, 1,
                       WELOVEC_FB_FONT_SYSTEM16);
        x += 16;
    }
}

static void draw_error(const char *message) {
    if (!message) return;
    const int start_x = setup_label_x;
    const int panel_right = setup_panel_x + setup_panel_width - 24;
    const int max_columns = (panel_right - start_x) / 8;
    int x = start_x;
    int y = setup_password_y + 34;
    int column = 0;
    while (*message) {
        if (*message == '\n' || column >= max_columns) {
            x = start_x;
            y += 10;
            column = 0;
            if (*message == '\n') {
                ++message;
                continue;
            }
        }
        draw_char_font(x, y, *message++, FB_RED, 0, 1,
                       WELOVEC_FB_FONT_SYSTEM8);
        x += 8;
        ++column;
    }
}

static void draw_field(int y, const char *text, bool hide_text) {
    char visible[WELOVEC_USER_PASSWORD_MAX + 1];
    const uint32_t length = string_length(text);
    const int visible_columns = (setup_field_width - 16) / 16;
    const uint32_t first = length > (uint32_t)visible_columns
        ? length - (uint32_t)visible_columns : 0;
    uint32_t output = 0;

    draw_rect(setup_field_x, y, setup_field_width, 26, FB_WHITE);
    draw_rect_outline(setup_field_x, y, setup_field_width, 26, FB_CYAN);
    for (uint32_t index = first; index < length && output < sizeof(visible) - 1;
         ++index)
        visible[output++] = hide_text ? '*' : text[index];
    visible[output] = '\0';
    draw_text_16(setup_field_x + 8, y + 5, visible, FB_BLACK);
    clear_secret(visible, sizeof(visible));
}

static void draw_setup_screen(const char *username, const char *password,
                              const char *error) {
    const int title_x = setup_panel_x + (setup_panel_width - 304) / 2;
    const int title_y = setup_panel_y - 34;

    fb_clear_screen_with_color(FB_BLUE);
    draw_rect(setup_panel_x + 8, setup_panel_y + 8,
              setup_panel_width + 5, setup_panel_height + 5, FB_GRAY);
    draw_rect(setup_panel_x - 2, setup_panel_y - 2,
              setup_panel_width + 5, setup_panel_height + 5, FB_CYAN);
    draw_rect(setup_panel_x, setup_panel_y, setup_panel_width,
              setup_panel_height, FB_WHITE);
    draw_text_16(title_x + 3, title_y + 3,
                 "CREATE A ADMIN USER", FB_GRAY);
    draw_text_16(title_x, title_y, "CREATE A ADMIN USER", FB_WHITE);

    draw_rect(setup_label_x, setup_user_y, 150, 26, FB_CYAN);
    draw_text_16(setup_label_x + 5, setup_user_y + 5,
                 "USERNAME:", FB_BLUE);
    draw_field(setup_user_y, username, false);

    draw_rect(setup_label_x, setup_password_y, 150, 26, FB_CYAN);
    draw_text_16(setup_label_x + 5, setup_password_y + 5,
                 "PASSWORD:", FB_BLUE);
    draw_field(setup_password_y, password, true);

    draw_error(error);
    draw_small_text(setup_label_x,
                    setup_panel_y + setup_panel_height - 18,
                    "ENTER continue  <- erase", FB_DARK_GRAY);
}

static int read_setup_key(char *key) {
    if (!key) return 0;
    for (;;) {
        const int64_t count = welovec_keyboard_read(key, 1, 0);
        if (count == 1) return 1;
        if (count < 0) return 0;
        /* Startup runs on the scheduler's boot task, which cannot sleep. */
        scheduler_yield();
    }
}

static int read_field(char *output, uint32_t capacity, bool hide_text,
                      int field_y, int *overflowed) {
    uint32_t length = 0;
    char key = 0;
    if (!output || capacity < 2 || !overflowed) return 0;
    output[0] = '\0';
    *overflowed = 0;
    draw_field(field_y, output, hide_text);

    for (;;) {
        if (!read_setup_key(&key)) return 0;
        if (key == '\n' || key == '\r') {
            output[length] = '\0';
            return 1;
        }
        if (key == '\b' || (uint8_t)key == 0x7f) {
            if (length) output[--length] = '\0';
            *overflowed = 0;
            draw_field(field_y, output, hide_text);
            continue;
        }
        if (key < 0x20 || key > 0x7e) continue;
        if (length + 1 < capacity) {
            output[length++] = key;
            output[length] = '\0';
        } else *overflowed = 1;
        draw_field(field_y, output, hide_text);
    }
}

static const char *validate_setup_input(const char *username,
                                       const char *password,
                                       int username_overflow,
                                       int password_overflow) {
    const uint32_t username_length = string_length(username);
    const uint32_t password_length = string_length(password);
    if (username_overflow || username_length > WELOVEC_USER_NAME_MAX - 1)
        return "Username cannot exceed 31 characters.";
    if (!username_length) return "Username is required.";
    for (uint32_t index = 0; index < username_length; ++index) {
        const char character = username[index];
        const bool allowed = (character >= 'a' && character <= 'z') ||
                             (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') ||
                             character == '_' || character == '-' ||
                             character == '.';
        if (!allowed)
            return "Use letters, numbers, period, hyphen, or underscore only.";
    }
    if (same_text(username, ".") || same_text(username, ".."))
        return "Username cannot be '.' or '..'.";
    if (same_text(username, "root") || same_text(username, "guest"))
        return "That username is reserved.";
    if (password_overflow || password_length > WELOVEC_USER_PASSWORD_MAX)
        return "Password cannot exceed 128 characters.";
    if (password_length < WELOVEC_USER_PASSWORD_MIN)
        return "Password must be at least 7 characters.";
    return 0;
}

static const char *creation_error(int64_t result) {
    switch (result) {
        case -5: return "Could not save the account to disk.";
        case -16: return "Another account operation is in progress.";
        case -17: return "The username exists or an admin was already created.";
        case -28: return "The user database is full.";
        case -38: return "Password hashing failed; account was not created.";
        default: return "Could not create the account. Check the entered data.";
    }
}

static void wait_for_enter(void) {
    char key = 0;
    while (read_setup_key(&key))
        if (key == '\n' || key == '\r') return;
}

static void discard_pending_input(void) {
    char key = 0;
    while (welovec_keyboard_read(&key, 1, 0) == 1) {}
}

static int configure_panel(void) {
    const int screen_width = (int)FRAMEBUFFER_WIDTH;
    const int screen_height = (int)FRAMEBUFFER_HEIGHT;
    if (!framebuffer_available() || screen_width < SETUP_MIN_WIDTH ||
        screen_height < SETUP_MIN_HEIGHT) return 0;

    setup_panel_width = screen_width * 70 / 100;
    setup_panel_height = screen_height * 70 / 100;
    setup_panel_x = (screen_width - setup_panel_width) / 2;
    setup_panel_y = (screen_height - setup_panel_height) / 2;
    setup_label_x = setup_panel_x + 97;
    setup_field_x = setup_panel_x + 260;
    setup_field_width = setup_panel_width - 284;
    setup_user_y = setup_panel_y + 42;
    setup_password_y = setup_panel_y + 98;
    return setup_field_width >= 64 && setup_panel_height >= 250;
}

int welovec_create_user_setup(void) {
    char username[WELOVEC_USER_NAME_MAX];
    char password[WELOVEC_USER_PASSWORD_MAX + 1];
    int username_overflow = 0;
    int password_overflow = 0;

    if (!configure_panel()) {
        printf_fb_color("[security] Screen is too small to create an administrator.\n",
                        FB_RED);
        return 0;
    }
    if (!welovec_keyboard_available()) {
        printf_fb_color("[security] No keyboard detected; startup is stopping.\n",
                        FB_RED);
        return 0;
    }
    discard_pending_input();

    for (;;) {
        const char *error = 0;
        clear_secret(username, sizeof(username));
        clear_secret(password, sizeof(password));
        draw_setup_screen(username, password, 0);

        if (!read_field(username, sizeof(username), false, setup_user_y,
                        &username_overflow) ||
            !read_field(password, sizeof(password), true, setup_password_y,
                        &password_overflow)) {
            clear_secret(username, sizeof(username));
            clear_secret(password, sizeof(password));
            return 0;
        }

        error = validate_setup_input(username, password,
                                     username_overflow, password_overflow);
        if (!error) {
            const int64_t result =
                welovec_security_create_initial_admin(username, password);
            if (result >= 0) {
                clear_secret(password, sizeof(password));
                clear_secret(username, sizeof(username));
                fb_clear_screen();
                printf_fb_color("[security] Administrator created; continuing startup.\n",
                                FB_GREEN);
                return 1;
            }
            error = creation_error(result);
        }

        clear_secret(password, sizeof(password));
        draw_setup_screen(username, "", error);
        wait_for_enter();
        clear_secret(username, sizeof(username));
    }
}
