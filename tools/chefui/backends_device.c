/* backends_device.c - the backends in libchefui.a (phone): fbdev only. */
#include "chefui_internal.h"

extern const struct cu_backend cu_backend_fbdev;

const struct cu_backend *const cu_backends[] = { &cu_backend_fbdev, NULL };
