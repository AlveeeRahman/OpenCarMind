/* User-defined PIDs loaded from the SD card (EngineLink/Torque-style CSV). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CUSTOM_PID_MAX 8

typedef struct {
    char cmd[12];       /* e.g. "0105", "222813" (mode + PID/DID hex) */
    char prefix[12];    /* expected reply prefix: mode+0x40 then the PID hex, e.g. "4105", "622813" */
    char name[16];
    char unit[8];
    char formula[64];   /* A, B, ... with + - * / ( ) and {A:3} bit extraction */
    char header[8];     /* "" or "0" = functional; else e.g. "7E0" (11-bit CAN only) */
} custom_pid_t;

/* Parse CONFIG path; returns number loaded (0 if the file is missing). Writes an example file if none exists. */
size_t custom_pids_load(const char *path, custom_pid_t *out, size_t max);
/* Evaluate formula against the data bytes. Returns false on a syntax error or missing byte. */
bool   custom_pid_eval(const char *formula, const uint8_t *bytes, int n, float *result);
