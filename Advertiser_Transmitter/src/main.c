/* 
 * Sensor to BLE Gateway Node
 * Reads motion data, stores 80 samples, and broadcasts over Long-Range Bluetooth. 
 */

/* System Includes */
#include <zephyr/sys/reboot.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/pm/pm.h>
#include <hal/nrf_gpio.h>
#include <zephyr/drivers/sensor.h>

/* Configuration & Defines */
#define SENSOR_TYPE     0x1E
#define TOTAL_PACKETS   1
#define CHUNK_SAMPLES   80 /* Number of accelerometer readings per batch */
#define MFG_DATA_LEN (1 + (CHUNK_SAMPLES * 3)) /* 241 bytes total: 1 ID + 240 data bytes */

/* Global Variables & State */
void repeated_adv(struct k_work *work);
void fetch_acc_data(struct k_work *work);

struct k_work my_work, acc_work;
struct k_timer my_timer, acc_timer;
struct bt_le_ext_adv *adv;
const struct device *sensor = DEVICE_DT_GET_ANY(st_lis2dh);
uint8_t *acc_ptr;

/* Structure to hold one X, Y, Z reading */
typedef struct {
    uint8_t x_lsb;
    uint8_t y_lsb;
    uint8_t z_lsb;
} motion_data;

motion_data Motion_data[CHUNK_SAMPLES];

static uint32_t time, last_time;
static uint32_t button_counter = 0;
int sleep_flag = 1;
int acc_fetch_flag = 0; /* Tracks number of collected samples */
int err;
static int count = 0;
int i;
int j = 0;

/* Bluetooth Radio Configuration */
/* 
 * Configured for Extended Advertising (Long Range / Coded PHY).
 * BT_LE_ADV_OPT_REQUIRE_S8_CODING ensures max range (125kbps).
 */
struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
    BT_LE_ADV_OPT_NONE | BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_USE_IDENTITY |
    BT_LE_ADV_OPT_CODED | BT_LE_ADV_OPT_REQUIRE_S8_CODING,
    0xC8,
    0xC8,
    NULL);

struct bt_le_ext_adv_start_param ext_adv_param = BT_LE_EXT_ADV_START_PARAM_INIT(0, 0);


/* Utility Functions */

/* Rounds floating-point sensor numbers to nearest integer */
double round_off(float value)
{
    if (value > 0) {
        if ((int)(value * 10) % 10 <= 4)
            return (uint8_t)(value);
        else
            return (uint8_t)(value + 1);
    } else {
        return (int8_t)(value);
    }
}

/* Hardcodes the MAC Address so the Gateway recognizes this device */
static void set_random_static_address(void)
{
    int err;
    int err1;
    printk("Starting iBeacon Demo\n");

    bt_addr_le_t addr;

    err = bt_addr_le_from_str("CE:BD:BE:AF:BA:11", "random", &addr);
    if (err) {
        printk("Invalid BT address (err %d)\n", err);
    }

    err1 = bt_id_create(&addr, NULL);
    if (err1 < 0) {
        printk("Creating new ID failed (err %d)\n", err1);
    }
    printk("Created new address\n");
}


/* Bluetooth Initialization */

/* Creates the advertising set with the Long Range parameters */
void adv_param_init(void)
{
    int err;

    err = bt_le_ext_adv_create(&adv_param, NULL, &adv);

    if (err) {
        printk("Failed to create advertising set (err %d)\n", err);
        return;
    }
    printk("Created extended advertising set (Coded PHY / Long Range, S=8)\n");
}


/* Sensor Data Collection */

/* Wakes up repeatedly to grab accelerometer data and triggers broadcast when full */
void fetch_acc_data(struct k_work *work)
{
    struct sensor_value accel[3];

    if (sensor == NULL) {
        printf("No device found\n");
        return;
    }
    if (!device_is_ready(sensor)) {
        printf("Device %s is not ready\n", sensor->name);
        return;
    }

    int rc = sensor_sample_fetch(sensor);
    if (rc == 0) {
        rc = sensor_channel_get(sensor, SENSOR_CHAN_ACCEL_XYZ, accel);
    }

    if (rc < 0) {
        printf("ERROR: Update failed: %d\n", rc);
    } else {
        count++;
        printf(" %d ", count);
        printf("x= %f, y= %f, z= %f\n",
               sensor_value_to_double(&accel[0]), sensor_value_to_double(&accel[1]),
               sensor_value_to_double(&accel[2]));
    }

    /* Convert float data into 8-bit format */
    Motion_data[acc_fetch_flag].x_lsb = (int8_t)(round_off(accel[0].val1 * 6.4 + accel[0].val2 * 0.0000064));
    Motion_data[acc_fetch_flag].y_lsb = (int8_t)(round_off(accel[1].val1 * 6.4 + accel[1].val2 * 0.0000064));
    Motion_data[acc_fetch_flag].z_lsb = (int8_t)(round_off(accel[2].val1 * 6.4 + accel[2].val2 * 0.0000064));
    
    printf("x= %u, y= %u, z=%u\n", 
           Motion_data[acc_fetch_flag].x_lsb, 
           Motion_data[acc_fetch_flag].y_lsb, 
           Motion_data[acc_fetch_flag].z_lsb);

    acc_fetch_flag++;
    
    /* Stop gathering and broadcast when 80 samples are collected */
    if (acc_fetch_flag == CHUNK_SAMPLES) {
        acc_fetch_flag = 0;
        k_timer_stop(&acc_timer);
        repeated_adv(NULL);
    }
}


/* Bluetooth Transmission */

/* Blasts the payload over BLE for 5 seconds */
void start_adv(void)
{
    printk("Start Extended Advertising (Long Range)...");
    err = bt_le_ext_adv_start(adv, &ext_adv_param);

    if (err) {
        printk("Failed to start extended advertising (err %d)\n", err);
        return;
    }
    printk("done.\n");

    k_msleep(5000);
    
    bt_le_ext_adv_stop(adv);
    printk("Stopped advertising..!!\n");

    /* Resume collecting data */
    k_timer_start(&acc_timer, K_MSEC(500), K_MSEC(500));
}

/* Packages 80 samples into a 241-byte flat array for BLE transmission */
void repeated_adv(struct k_work *work)
{
    int err;

    static uint8_t mfg_data[MFG_DATA_LEN];
    static const struct bt_data ad[] = {BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, MFG_DATA_LEN)};

    /* Byte 0 is the Device ID */
    mfg_data[0] = 50;  
    printf("%d ", mfg_data[0]);

    acc_ptr = &Motion_data[0].x_lsb;

    /* Flatten the XYZ data into bytes 1 to 240 */
    for (i = 1; i <= (CHUNK_SAMPLES * 3); i++) {
        mfg_data[i] = *(acc_ptr + (i - 1));
        printf("%d ", mfg_data[i]);
    }

    k_msleep(2000);

    err = bt_le_ext_adv_set_data(adv, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        printk("Failed to set advertising data (err %d) - skipping this batch\n", err);
        k_timer_start(&acc_timer, K_MSEC(500), K_MSEC(500));
        return;
    }
    
    start_adv();
}


/* Timers & Work Queues */

/* Advertising Work Queue */
K_WORK_DEFINE(my_work, repeated_adv);
void my_expiry_function(struct k_timer *timer_id)
{
    k_work_submit(&my_work);
};
K_TIMER_DEFINE(my_timer, my_expiry_function, NULL);

/* Sensor Reading Work Queue */
K_WORK_DEFINE(acc_work, fetch_acc_data);
void acc_expiry_function(struct k_timer *timer_id)
{
    k_work_submit(&acc_work);
};
K_TIMER_DEFINE(acc_timer, acc_expiry_function, NULL);


/* Main Initialization & Loop */

int main(void)
{
    /* Setup LEDs and Button */
    nrf_gpio_cfg_output(DT_GPIO_PIN(DT_NODELABEL(led3), gpios));
    nrf_gpio_pin_set(DT_GPIO_PIN(DT_NODELABEL(led3), gpios));

    nrf_gpio_cfg_input(DT_GPIO_PIN(DT_NODELABEL(button0), gpios),
                       NRF_GPIO_PIN_PULLUP);
    nrf_gpio_cfg_sense_set(DT_GPIO_PIN(DT_NODELABEL(button0), gpios),
                           NRF_GPIO_PIN_SENSE_LOW);

    int status = nrf_gpio_pin_read(DT_GPIO_PIN(DT_NODELABEL(button0), gpios));
    printk("button status=%d\n", status);

    /* Startup LED blinking sequence */
    while (j <= 5)
    {
        nrf_gpio_cfg_output(DT_GPIO_PIN(DT_NODELABEL(led0), gpios));
        nrf_gpio_pin_toggle(DT_GPIO_PIN(DT_NODELABEL(led0), gpios));
        k_msleep(50);
        j++;
    }
    nrf_gpio_pin_set(DT_GPIO_PIN(DT_NODELABEL(led0), gpios));

    /* Initialize Bluetooth */
    set_random_static_address();

    int err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return 0;
    }

    adv_param_init();
    
    /* Start sampling sensor data */
    k_timer_start(&acc_timer, K_MSEC(500), K_MSEC(500));

    /* Main loop checks for physical button resets */
    while (1) {
        
        if (nrf_gpio_pin_read(DT_GPIO_PIN(DT_NODELABEL(button0), gpios)) == 0) {

            uint32_t press_start = k_uptime_get_32();
            bool held_long_enough = false;

            /* Check if held down for 5 seconds */
            while (nrf_gpio_pin_read(DT_GPIO_PIN(DT_NODELABEL(button0), gpios)) == 0) {
                if (k_uptime_get_32() - press_start >= 5000) {
                    held_long_enough = true;
                    break;
                }
                k_msleep(50);
            }

            /* Trigger Cold Reboot */
            if (held_long_enough) {
                
                for (int blink = 0; blink < 10; blink++) {
                    nrf_gpio_pin_toggle(DT_GPIO_PIN(DT_NODELABEL(led0), gpios));
                    k_msleep(50);
                }
                nrf_gpio_pin_set(DT_GPIO_PIN(DT_NODELABEL(led0), gpios));

                k_timer_stop(&acc_timer);
                k_timer_stop(&my_timer);
                bt_le_ext_adv_stop(adv);

                sys_reboot(SYS_REBOOT_COLD);
            }
        }
        k_msleep(100);
    }

    return 0;
}