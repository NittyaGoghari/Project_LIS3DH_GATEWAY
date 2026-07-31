/* --- SYSTEM INCLUDES & CONFIGURATION --- */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdint.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <stdio.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/data/json.h>
#include "sdcard.h"
#include <stdlib.h>
#include "observer.h"
#include "atcommand.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* Define the size of one Bluetooth packet and Flash sector sizes */
#define NODE_PACKET_SIZE CONFIG_BLE_RECEIVING_PAYLOAD_SIZE
#define SPI_FLASH_SECTOR_SIZE 4096

/* MQTT and AWS Configuration */
#define AWS_TOPIC "gateway_data/20"
#define PUBLISH_INTERVAL_MS 1000

/* --- GLOBAL VARIABLES & STATE --- */
/* Memory pointers for circular Flash storage */
int readIndex = 0x102000;
int writeIndex = 0x102000;
int flashLimit = 0x800000;

bool memFilled = false;
bool Upload_Status = true;
struct k_mutex test_mutex;
int Total_Node = 0;


/* --- MQTT PUBLISHING ROUTINE --- */

/* 
 * Packages the formatted JSON string and sends it to the modem.
 * If the upload fails, it "rewinds" the flash read pointer so the data 
 * isn't lost and will be retried on the next loop.
 */
int mqtt_upload(char *line)
{
    char payload[CONFIG_AWS_IOT_SAMPLE_JSON_MESSAGE_SIZE_MAX];
    
    /* Safely copy the JSON string into our payload buffer */
    snprintf(payload, sizeof(payload), "%s", line);
    if (payload[0] == '\0')
    {
        printk("Failed to build payload\n");
        return -1;
    }

    printk("Sending JSON payload (%d bytes)...\n", strlen(payload));
    
    /* Send to the GSM modem driver (QoS 1, 5-second timeout) */
    int status = gsm_mqtt_publish(AWS_TOPIC, payload, strlen(payload), 1, 5000);

    if (status == 0)
    {
        printk("Published successfully|\n");
    }
    else
    {
        /* Upload failed - Roll back the read index so we don't lose this data */
        Upload_Status = false;
        printk("Publish failed..\n");

        k_mutex_lock(&test_mutex, K_FOREVER);
        readIndex -= Total_Node * NODE_PACKET_SIZE;
        
        /* Prevent the index from rewinding past the start of our memory boundary */
        if (readIndex < 0x102000) {
            readIndex = 0x102000;
        }
        k_mutex_unlock(&test_mutex);
    }
    
    /* Brief pause between publishes to prevent flooding the network */
    k_sleep(K_MSEC(PUBLISH_INTERVAL_MS));
    return status;
}


/* --- MAIN APPLICATION LOOP --- */

int main(void)
{
    int rc;
    int temp_read_index, temp_write_index;
    const struct device *flash_dev;
    int len = sizeof(int);

    k_mutex_init(&test_mutex);

    /* 1. Initialize the SPI Flash memory chip */
    flash_dev = DEVICE_DT_GET(DT_ALIAS(spi_flash0));
    if (!device_is_ready(flash_dev))
    {
        printk("%s: flash device not ready.\n", flash_dev->name);
        return 0;
    }

    /* 2. Recover previous state: Load the last Read Index from memory */
    rc = flash_read(flash_dev, CONFIG_FLASH_READ_ADD, &temp_read_index, len);
    if (rc == 0)
    {
        printk("Successfully read Read_index : %d\n", temp_read_index);
        
        /* Validate that the saved index is within our safe memory boundaries */
        if (temp_read_index > 0x102000 && temp_read_index < flashLimit)
        {
            readIndex = temp_read_index;
        }
        else
        {
            printk("It does not have stored read memory\n");
        }
    }
    else
    {
        printk("Failed to read Read_index\n");
    }
    
    /* 3. Recover previous state: Load the last Write Index from memory */
    rc = flash_read(flash_dev, CONFIG_FLASH_WRITE_ADD, &temp_write_index, len);
    if (rc == 0)
    {
        printk("Successfully read write_index : %d\n", temp_write_index);
        if (temp_write_index > 0x102000 && temp_write_index < flashLimit)
        {
            writeIndex = temp_write_index;
        }
        else
        {
            printk("It does not have stored write memory\n");
        }
    }
    else
    {
        printk("Failed to read write_index\n");
    }
    
    /* If memory indices are corrupted or backwards, reset them to the beginning */
    if (writeIndex < readIndex)
    {
        readIndex = 0x102000;
        writeIndex = 0x102000;
        printk("Read & write resets\n");
    }

    printf("Board Name: %s\n", CONFIG_BOARD);
    
    /* 4. Boot up peripheral subsystems */
    init_gsm();            /* Turn on UART and power up modem */
    observer_start();      /* Start listening for Bluetooth packets */
    
    /* 5. Connect to the cellular network and AWS */
    rc = gsm_network_connect();
    if (rc != 0)
    {
        LOG_ERR("GSM network connect failed (%d)", rc);
        k_sleep(K_SECONDS(10));
        return 0;
    }

    int64_t now_time = k_uptime_get() / 1000;
    printk("System Uptime: %lld seconds\n", now_time);

    /* 6. Main Infinite Processing Loop */
    while (1)
    {
        uint8_t buf[CONFIG_CLOUD_UPLOAD_PAYLOAD_SIZE] = {0};

        /* Wait here until the Bluetooth thread signals that new data has arrived */
        int sem_result = k_sem_take(&data_ready_sem, K_MSEC(1000));

        /* Wait an extra 500ms so we can batch multiple packets together */
        if (sem_result == 0) {
            k_msleep(500);
        }

        /* Safely grab the current memory positions */
        k_mutex_lock(&test_mutex, K_FOREVER);
        int cur_write = writeIndex;
        int cur_read = readIndex;
        bool cur_filled = memFilled;
        k_mutex_unlock(&test_mutex);

        /* 
         * Check if it's time to upload: 
         * Do we have enough data? Is memory full? Has the upload timer expired? 
         */
        if (((cur_write - cur_read) >= CONFIG_CLOUD_UPLOAD_PAYLOAD_SIZE) || cur_filled ||
            k_uptime_get() / 1000 - now_time >= CONFIG_UPLOAD_INTERVAL)
        {
            now_time = k_uptime_get() / 1000;

            k_mutex_lock(&test_mutex, K_FOREVER);
            
            /* Wrap the read index back to the start if we hit the limit */
            if (readIndex >= flashLimit)
            {
                readIndex = 0x102000;
                memFilled = false;
            }
            int local_read_index = readIndex;

            /* Calculate exactly how many packets we need to read from Flash */
            int new_entries = 0;
            int diff = cur_write - local_read_index;
            if (diff > 0 && diff < CONFIG_CLOUD_UPLOAD_PAYLOAD_SIZE)
            {
                new_entries = diff / NODE_PACKET_SIZE;
            }
            else if (diff >= CONFIG_CLOUD_UPLOAD_PAYLOAD_SIZE)
            {
                new_entries = CONFIG_CLOUD_UPLOAD_PAYLOAD_SIZE / NODE_PACKET_SIZE;
            }
            Total_Node = new_entries;
            k_mutex_unlock(&test_mutex);

            /* Read the raw binary data block from Flash memory */
            int rc2 = flash_read(flash_dev, local_read_index, buf, Total_Node * NODE_PACKET_SIZE);
            if (rc2 != 0)
            {
                printf("Flash read failed! %d\n", rc2);
            }

            /* --- Convert Binary Data to JSON --- */
            char msg[CONFIG_AWS_IOT_SAMPLE_JSON_MESSAGE_SIZE_MAX] = {0};
            int pos = 0;

            /* Start the JSON structure */
            pos += snprintk(&msg[pos], sizeof(msg) - pos, "{\"counter\": [");

            int total_bytes = Total_Node * NODE_PACKET_SIZE;

            /* Loop through every byte and append it as a number in the JSON array */
            for (int i = 0; i < total_bytes; i++)
            {
                pos += snprintk(&msg[pos], sizeof(msg) - pos, "%d%s",
                                buf[i],
                                (i < total_bytes - 1) ? "," : "");
            }

            /* Close the JSON structure */
            pos += snprintk(&msg[pos], sizeof(msg) - pos, "]}");

            /* If we actually read data, save and transmit it */
            if (Total_Node > 0)
            {
                sim_status_checking();

                /* Advance the read pointer assuming success (we will roll it back if it fails) */
                k_mutex_lock(&test_mutex, K_FOREVER);
                readIndex += Total_Node * NODE_PACKET_SIZE;
                k_mutex_unlock(&test_mutex);

                /* Save a backup copy of the JSON payload to the SD Card */
                int sd_rc = sdcard_save_json(msg, pos);
                if (sd_rc != 0)
                {
                    LOG_WRN("SD card save failed (%d), continuing without it", sd_rc);
                }

                /* Publish the payload to AWS over Cellular */
                mqtt_upload(msg);
            }
        }
    }

    return 0;
}