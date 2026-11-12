#pragma once
#ifndef CARMIND_VERSION
#define CARMIND_VERSION      "0.0.0-dev"   /* real value comes from version.txt via CMake */
#endif
#define CARMIND_NAME         "OpenCarMind"
#if CARMIND_VERSION_HAS_NAME
#define CARMIND_FW_STRING    CARMIND_VERSION                       /* the S31: "OpenCarMind EdgeOS 2.1.0" already names it */
#else
#define CARMIND_FW_STRING    CARMIND_NAME " " CARMIND_VERSION      /* logs and session files */
#endif
/* Shown on the board's own 172 px screen, where the short name leaves room in the top bar. */
#define CARMIND_SCREEN_NAME  "CarMind"
