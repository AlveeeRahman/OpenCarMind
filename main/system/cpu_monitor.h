#pragma once
#include <stdint.h>
/* Samples FreeRTOS run-time statistics once a second and publishes per-core load and free heap into g_status. */
void cpu_monitor_start(void);
