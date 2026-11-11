#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Samples FreeRTOS run-time statistics once a second and publishes per-core load and free heap into g_status. */
void cpu_monitor_start(void);
/* Model inference time (E.V / R.E.V passes, A.E.V steps), added by the ML task; any task. The monitor turns it into
   the AI share of both cores per second (g_status.ai_pct). */
void cpu_monitor_add_ai_us(uint32_t us);
#ifdef __cplusplus
}
#endif
