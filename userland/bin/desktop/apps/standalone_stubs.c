// Description: Supplies desktop-only interfaces unused by embedded apps.

#include "desktop/include/desktop.h"

void menu_init(void) {}
int menu_is_open(void) { return 0; }
void menu_toggle(void) {}
void menu_close(void) {}
void menu_draw(rect_t area) { (void)area; }
int menu_mouse(const struct mouse_event *event) { (void)event; return 0; }
int menu_key(int key) { (void)key; return 0; }
void icons_draw(rect_t area) { (void)area; }
int icons_mouse(const struct mouse_event *event) { (void)event; return 0; }
int icons_drag_active(void) { return 0; }
