/* The Bluetooth link states (g_status.ble). Kept apart from status.h so pure, host-tested rules can use them. */
#pragma once

typedef enum {
    BLE_ST_IDLE = 0,
    BLE_ST_SCANNING,
    BLE_ST_CONNECTING,
    BLE_ST_DISCOVERING,
    BLE_ST_ELM_INIT,
    BLE_ST_LOGGING,
    BLE_ST_LOCKED,        /* adapter answered "NA": LELink security enabled */
} ble_state_t;
