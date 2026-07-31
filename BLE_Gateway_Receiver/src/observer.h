#ifndef __OBSERVER_H__
#define __OBSERVER_H__

#include <zephyr/kernel.h>

int observer_start(void);

/* Given by flash_writer_thread() each time a new record is committed to
 * flash, so main() can publish it immediately instead of only polling. */
extern struct k_sem data_ready_sem;

#endif