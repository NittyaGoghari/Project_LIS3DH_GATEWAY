#ifndef SDCARD_H
#define SDCARD_H

/**
 * Save a JSON string to the SD card.
 * Returns 0 on success, negative errno on failure.
 */
int sdcard_save_json(const char *json, int len);

#endif /* SDCARD_H */