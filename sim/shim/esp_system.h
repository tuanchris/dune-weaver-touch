// Desktop shim: esp_system.h
//
// Only esp_restart() is used (page_control.c's theme toggle, ota.c). On the
// device it reboots; here the closest honest equivalent is to exit, so the
// sim visibly stops rather than pretending the restart happened and carrying
// on with stale state.
#pragma once

#include <stdio.h>
#include <stdlib.h>

static inline void esp_restart(void)
{
    fprintf(stderr, "[sim] esp_restart() -> exiting\n");
    exit(0);
}
