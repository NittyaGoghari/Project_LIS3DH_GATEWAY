/* --- SYSTEM INCLUDES & CONFIGURATION --- */
#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <ff.h> /* FatFS library */
#include <zephyr/logging/log.h>
#include <zephyr/drivers/rtc.h>
#include <stdio.h>
#include <string.h>
#include "sdcard.h"

LOG_MODULE_REGISTER(sdcard, LOG_LEVEL_INF);

/* Defines the internal name Zephyr uses for the SD slot, and the root folder path */
#define DISK_DRIVE_NAME "SD" 
#define MOUNT_POINT "/SD:"


/* --- GLOBAL VARIABLES & FILE SYSTEM SETUP --- */

/* 
 * The FAT file system object and mount configuration. 
 * This tells Zephyr how to interact with the raw memory on the SD card.
 */
static FATFS fat_fs;
static struct fs_mount_t mp = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
    .mnt_point = MOUNT_POINT,
};

/* Keeps track of whether the card is already ready to use */
static bool mounted = false;


/* --- DISK INITIALIZATION & MOUNTING --- */

/* 
 * Checks if the SD card is already mounted. If not, it wakes up the SD hardware 
 * and mounts the FAT file system so we can read and write files. 
 */
static int ensure_mounted(void)
{
    /* If already mounted from a previous loop, just skip and return success */
    if (mounted)
        return 0;

    /* Initialize the physical SD Card hardware */
    int rc = disk_access_init(DISK_DRIVE_NAME);
    if (rc)
    {
        LOG_ERR("Disk init failed: %d", rc);
        return rc;
    }

    /* Mount the FAT file system onto the initialized hardware */
    rc = fs_mount(&mp);
    if (rc)
    {
        LOG_ERR("SD mount failed: %d", rc);
        return rc;
    }
    
    LOG_INF("Initializing SD disk...");
    LOG_INF("SD card mounted at %s", MOUNT_POINT);

    /* Mark as successfully mounted so we don't repeat this process */
    mounted = true;
    return 0;
}


/* --- FILE WRITE OPERATIONS --- */

/* 
 * Generates the specific file path where we want to save our data.
 * Currently hardcoded to save everything in one master "data.json" file.
 */
static void make_filename(char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "/SD:/data.json");
}

/* 
 * The main public function. Takes the formatted JSON string, ensures the 
 * SD card is ready, and appends the new data to the bottom of the file.
 */
int sdcard_save_json(const char *json, int len)
{
    /* Make sure we actually have data to write */
    if (!json || len <= 0)
    {
        LOG_WRN("sdcard_save_json: nothing to write");
        return -EINVAL;
    }

    /* Make sure the SD card is mounted and ready */
    int rc = ensure_mounted();
    if (rc)
        return rc;

    /* Get the target file path */
    char path[64];
    make_filename(path, sizeof(path));

    /* Prepare a file object */
    struct fs_file_t f;
    fs_file_t_init(&f);

    /* 
     * Open the file. 
     * FS_O_CREATE makes the file if it doesn't exist yet.
     * FS_O_APPEND ensures we add to the bottom instead of overwriting old data.
     */
    rc = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
    if (rc)
    {
        LOG_ERR("Cannot open %s: %d", path, rc);
        return rc;
    }

    /* Write the actual JSON string into the file */
    ssize_t written = fs_write(&f, json, len);
    
    /* Append a newline so the file doesn't become one giant unreadable block of text */
    char newline = '\n';
    fs_write(&f, &newline, 1);

    /* Always close the file immediately to prevent corruption if power is lost */
    fs_close(&f);

    if (written < 0)
    {
        LOG_ERR("SD write failed: %d", (int)written);
        return (int)written;
    }

    LOG_INF("SD: saved %d bytes → %s", (int)written, path);
    return 0;
}