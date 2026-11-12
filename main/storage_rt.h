#pragma once
/* Storage reset-injection test, CONFIG_OCM_STORAGE_RESET_TEST (test builds only): checks the flash FAT after the previous
   reset, then writes and resets the board again at a random moment. A no-op when the option is off. Call after sdlog_init(). */
void storage_rt_run(void);
