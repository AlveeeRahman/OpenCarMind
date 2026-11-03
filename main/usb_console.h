/* Settings console on the USB port. One command per line, answers end with a line "OK" or "ERR <reason>".
 * Log lines from the firmware start with "I (", "W (" or "E (" and can be ignored by a program reading the answers.
 * Changing settings is refused while the car link is up, so the console never competes with logging. */
#pragma once

void usb_console_start(void);
