#include "desktop/include/desktop.h"

extern const struct app_ops app_tasks;

const struct app_ops *desktop_app_target(void) { return &app_tasks; }
