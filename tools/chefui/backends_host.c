/* backends_host.c - the backends in libchefui-host.a (PC): SDL only. */
#include "chefui_internal.h"

extern const struct cu_backend cu_backend_sdl;

const struct cu_backend *const cu_backends[] = { &cu_backend_sdl, NULL };
