#include "desktop/include/desktop.h"

extern const struct app_ops app_sysinfo;

const struct app_ops *desktop_app_target(void) { return &app_sysinfo; }
