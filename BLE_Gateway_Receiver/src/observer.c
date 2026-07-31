/* --- SYSTEM INCLUDES & CONFIGURATION --- */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <stdio.h>
#include "observer.h"
#include "atcommand.h"

/* Shared memory lock */
extern struct k_mutex test_mutex;

#define STACKSIZE 32768
#define THREAD0_PRIORITY 7
#define SPI_FLASH_SECTOR_SIZE 4096

/* Flash tracking variables from main.c */
extern int flashLimit, writeIndex, readIndex;
extern bool memFilled;

/* BLE status LED */
static const struct gpio_dt_spec bleled = GPIO_DT_SPEC_GET(DT_ALIAS(bleled), gpios);


/* --- BLUETOOTH DATA STRUCTURES & QUEUES --- */

/* Structure to hold a single incoming Bluetooth packet */
struct ble_pkt
{
    uint8_t data[241];
    uint8_t len;
    /* RSSI removed */
};

/* 
 * A message queue to safely pass packets from the fast Bluetooth interrupt 
 * to the slower Flash Writer background thread. Holds up to 64 packets. 
 */
K_MSGQ_DEFINE(ble_msgq, sizeof(struct ble_pkt), 64, 4);
static atomic_t drop_counter;

/* Semaphore used to wake up the main.c loop when data is saved to Flash */
K_SEM_DEFINE(data_ready_sem, 0, 1);

#if defined(CONFIG_BT_EXT_ADV)

/* Buffers used to drop duplicate packets sent in the same burst */
static uint8_t last_packet_buffer[256] = {0};
static size_t last_packet_len = 0;


/* --- BLUETOOTH DATA PARSING & FILTERING --- */

/* 
 * Analyzes the raw Bluetooth payload. Extracts only our specific 
 * 241-byte manufacturer data and drops exact duplicates.
 */
static bool data_cb(struct bt_data *data, void *user_data)
{
    if (data->type != BT_DATA_MANUFACTURER_DATA) {
        return true; /* Not manufacturer data, keep looking */
    }

    if (data->data_len != 241) {
        return true; /* Not our 241-byte sensor packet, keep looking */
    }

    /* Single-node deduplication check: Did we just see this exact packet? */
    if (data->data_len == last_packet_len &&
        memcmp(data->data, last_packet_buffer, data->data_len) == 0)
    {
        /* Duplicate burst from our node. Drop silently. */
        return false;
    }

    /* New unique packet! Save it to the tracking buffer for future duplicate checks. */
    last_packet_len = MIN(data->data_len, sizeof(last_packet_buffer));
    memcpy(last_packet_buffer, data->data, last_packet_len);

    /* Copy the validated data into our packet structure */
    struct ble_pkt *pkt = (struct ble_pkt *)user_data;
    pkt->len = MIN(data->data_len, 241);
    memcpy(pkt->data, data->data, pkt->len);

    return false; /* We found what we need, stop parsing this broadcast */
}

/* Grabs the current network time from the GSM modem */
static void update_epoch_array(uint8_t *epochArr)
{
    (void)gsm_time_fetch(epochArr);
}


/* --- FLASH WRITING BACKGROUND THREAD --- */

/* 
 * This background task waits for valid Bluetooth packets to arrive in the queue.
 * When one arrives, it appends the current network time, handles memory limits, 
 * and writes the final combined block to the SPI Flash.
 */
void flash_writer_thread(void)
{
    struct ble_pkt pkt;
    uint8_t epochArr[10] = {0};
    uint8_t writeArray[CONFIG_BLE_RECEIVING_PAYLOAD_SIZE];
    
    const struct device *flash_dev;
    const size_t arrayLen = CONFIG_BLE_RECEIVING_PAYLOAD_SIZE;
    
    static int64_t last_log_time = 0;
    static int64_t last_epoch_update = 0;
    static uint32_t last_erased_sector = 0xFFFFFFFF;

    flash_dev = DEVICE_DT_GET(DT_ALIAS(spi_flash0));
    if (!device_is_ready(flash_dev))
    {
        printk("Flash device not ready\n");
        return;
    }

    /* Get initial time on startup */
    update_epoch_array(epochArr);

    while (1)
    {
        /* Print queue health stats every 5 seconds */
        if (k_uptime_get() - last_log_time > 5000)
        {
            printk("BLE stats: queue=%u drops=%ld writeIndex=0x%x readIndex=0x%x\n",
                   k_msgq_num_used_get(&ble_msgq),
                   atomic_get(&drop_counter),
                   writeIndex, readIndex);

            last_log_time = k_uptime_get();
        }

        /* Wait here forever until a packet arrives in the queue */
        if (k_msgq_get(&ble_msgq, &pkt, K_FOREVER) == 0)
        {     
            memset(writeArray, 0, sizeof(writeArray));

            /* Refresh the network time every 5 seconds */
            if (k_uptime_get() - last_epoch_update > 5000)
            {
                update_epoch_array(epochArr);
                last_epoch_update = k_uptime_get();
            }

            /* 1. Copy the 241-byte sensor data */
            size_t copy_len = MIN(pkt.len, 241);
            memcpy(writeArray, pkt.data, copy_len);

            /* 2. Pad unused bytes (241-245) */
            writeArray[241] = 0x00;
            writeArray[242] = 0x00;
            writeArray[243] = 0x00;
            writeArray[244] = 0x00;
            writeArray[245] = 0x00;

            /* 3. Append the 10-byte time array at the end (246-255) */
            for (size_t epochIdx = 0; epochIdx < 10; epochIdx++)
            {
                writeArray[246 + epochIdx] = (uint8_t)epochArr[epochIdx];
            }

            /* --- Lock Flash for Writing --- */
            k_mutex_lock(&test_mutex, K_FOREVER);

            /* Calculate where the next write will go to see if we wrap around */
            uint32_t next_write = (uint32_t)writeIndex + (uint32_t)CONFIG_BLE_RECEIVING_PAYLOAD_SIZE;
            bool will_wrap = (next_write >= (uint32_t)flashLimit);
            if (will_wrap) {
                next_write = 0x102000;
            }

            /* If flash is full, push the read pointer forward (dropping oldest data) */
            if (memFilled &&
                ((writeIndex < readIndex && (int)next_write >= readIndex) ||
                 (will_wrap && readIndex <= writeIndex)))
            {
                int old_read = readIndex;
                readIndex += CONFIG_BLE_RECEIVING_PAYLOAD_SIZE;
                if (readIndex >= flashLimit) {
                    readIndex = 0x102000;
                }
                printk("FIFO full: dropping oldest record at 0x%x -> new readIndex 0x%x\n",
                       old_read, readIndex);
            }

            /* Erase the 4KB flash sector if we just crossed into a new one */
            uint32_t current_sector = writeIndex / SPI_FLASH_SECTOR_SIZE;
            if (current_sector != last_erased_sector)
            {
                int ret = flash_erase(flash_dev, current_sector * SPI_FLASH_SECTOR_SIZE, SPI_FLASH_SECTOR_SIZE);
                if (ret == 0)
                {
                    printk("Erased flash sector starting at 0x%x\n", current_sector * SPI_FLASH_SECTOR_SIZE);
                }
                last_erased_sector = current_sector;
            }

            /* Write the final 256-byte chunk to memory */
            int rc = flash_write(flash_dev, writeIndex, writeArray, arrayLen);

            if (rc != 0)
            {
                printf("Flash write failed! %d\n", rc);
                k_mutex_unlock(&test_mutex);
                continue;
            }
            else
            {
                /* Success! Move write index forward */
                writeIndex = writeIndex + CONFIG_BLE_RECEIVING_PAYLOAD_SIZE;
            }

            /* Wrap write index if it hits the physical flash limit */
            if (writeIndex >= flashLimit)
            {
                writeIndex = 0x102000;
                memFilled = true;
            }

            /* --- Unlock Flash --- */
            k_mutex_unlock(&test_mutex);
            
            /* Wake up main.c so it knows new data is ready to be uploaded */
            k_sem_give(&data_ready_sem);
        }
    }
}


/* --- BLUETOOTH SCANNING CALLBACKS --- */

/* 
 * This is triggered by the hardware every time a packet is detected in the air. 
 * It toggles the LED, passes the data to the parser, and drops it into the queue.
 */
static void scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf)
{
    /* Toggle BLE LED to show a packet was received */
    gpio_pin_toggle_dt(&bleled);

    struct ble_pkt pkt;
    memset(&pkt, 0, sizeof(pkt));

    /* Parse the payload using our data_cb filter */
    bt_data_parse(buf, data_cb, &pkt);

    /* If data_cb accepted it, put it in the queue for the flash writer thread */
    if (pkt.len > 0)
    {
        if (k_msgq_put(&ble_msgq, &pkt, K_NO_WAIT) != 0)
        {
            atomic_inc(&drop_counter); /* Track if the queue is full and dropping packets */
        }
    }
}

static struct bt_le_scan_cb scan_callbacks = {
    .recv = scan_recv,
};
#endif /* CONFIG_BT_EXT_ADV */


/* --- OBSERVER INITIALIZATION --- */

/* Starts the Bluetooth radio and configures it to only listen to our specific sensor */
int observer_start(void)
{
    printk("observer Started\n");

    /* Ensure BLE LED starts completely OFF */
    if (gpio_is_ready_dt(&bleled)) {
        gpio_pin_configure_dt(&bleled, GPIO_OUTPUT_INACTIVE);
    } else {
        printk("bleled GPIO not ready\n");
    }

    int err = bt_enable(NULL);
    if (err)
    {
        printk("Bluetooth init failed (err %d)\n", err);
        return err;
    }

    /* Set up Hardware Filter Accept List (Whitelist) */
    bt_addr_le_t addr1;
    bt_addr_le_from_str(CONFIG_BLE_ADD, "random", &addr1);

    err = bt_le_filter_accept_list_add(&addr1);
    if (err == 0)
    {
        printk("Successfully added the address to Filter Accept List..\n");
    }
    else
    {
        printk("Failed to update Filter Accept List..\n");
    }

    /* Configure Scanning: Passive, Coded PHY (Long Range), and Whitelist enabled */
    struct bt_le_scan_param scan_param = {
        .type       = BT_LE_SCAN_TYPE_PASSIVE,
        .options    = BT_LE_SCAN_OPT_CODED | BT_LE_SCAN_OPT_FILTER_ACCEPT_LIST, 
        .interval   = 0x00A0,
        .window     = 0x00A0,
    };

    /* Register our callback and start listening */
    bt_le_scan_cb_register(&scan_callbacks);
    err = bt_le_scan_start(&scan_param, NULL);

    if (err)
    {
        printk("Start scanning failed (err %d)\n", err);
        return err;
    }
    printk("Started scanning for single node %s...\n", CONFIG_BLE_ADD);
    return 0;
}

/* Start the background flash writer thread automatically */
K_THREAD_DEFINE(thread0_id, STACKSIZE, flash_writer_thread, NULL, NULL, NULL,
                THREAD0_PRIORITY, 0, 20000);