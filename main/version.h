#pragma once
#ifndef CARMIND_VERSION
#define CARMIND_VERSION      "0.0.0-dev"   /* real value comes from version.txt via CMake */
#endif
#define CARMIND_NAME         "OpenCarMind"
#define CARMIND_FW_STRING    CARMIND_NAME " " CARMIND_VERSION      /* logs and session files */
/* Shown on the board's own 172 px screen, where the short name leaves room in the top bar. */
#define CARMIND_SCREEN_NAME  "CarMind"
