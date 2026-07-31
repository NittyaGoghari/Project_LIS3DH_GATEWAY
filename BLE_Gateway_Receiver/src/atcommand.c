/* --- SYSTEM INCLUDES & CONFIGURATION --- */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <date_time.h>
#include "certs.h"
#include <time.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(atcmd, LOG_LEVEL_DBG);

#define UART_DEVICE_NAME "UART_2"
#define AT_CMD_RESPONSE_MAX_LEN 8192
#define AT_CMD_SMALL_LEN 256

/* --- GLOBAL VARIABLES & STATE --- */
static int mqtt_sub_id = 1;
static uint16_t mqtt_pub_id = 1;
static bool qos1_in_flight = false;
static int qos1_busy_count = 0;
static bool certs_uploaded = false;
static char stored_datetime[32] = {0};

/* Hardware Pointers & Buffers */
static const struct device *uart_dev;
static char at_response[AT_CMD_RESPONSE_MAX_LEN];
static size_t response_idx = 0;
static volatile bool line_seen = false;
static struct k_mutex uart_mutex;
static bool mqtt_connected = false;

/* Hardware LED mappings */
static const struct gpio_dt_spec gatewayled = GPIO_DT_SPEC_GET(DT_ALIAS(gatewayled), gpios);
static const struct gpio_dt_spec cloudled   = GPIO_DT_SPEC_GET(DT_ALIAS(cloudled), gpios);


/* --- LED CONTROL ABSTRACTION --- */
/* Forces Active-Low polarity to bypass Devicetree overlay issues. */

static void led_off(const struct gpio_dt_spec *led) {
    if (gpio_is_ready_dt(led)) {
        gpio_pin_configure_dt(led, GPIO_OUTPUT | GPIO_ACTIVE_LOW);
        gpio_pin_set_dt(led, 0);
    }
}

static void led_on(const struct gpio_dt_spec *led) {
    if (gpio_is_ready_dt(led)) {
        gpio_pin_configure_dt(led, GPIO_OUTPUT | GPIO_ACTIVE_LOW);
        gpio_pin_set_dt(led, 1);
    }
}

static void led_toggle(const struct gpio_dt_spec *led) {
    if (gpio_is_ready_dt(led)) {
        gpio_pin_configure_dt(led, GPIO_OUTPUT | GPIO_ACTIVE_LOW);
        gpio_pin_toggle_dt(led);
    }
}


/* --- UART SETUP & INTERRUPTS --- */

/* Clears the response buffer before sending a new command */
static void at_buf_clear_locked(void)
{
    response_idx = 0;
    at_response[0] = '\0';
    line_seen = false;
}

/* Background interrupt that catches data arriving from the modem */
static void uart_callback(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);
    uint8_t c;

    while (uart_fifo_read(dev, &c, 1) > 0)
    {
        if (response_idx < AT_CMD_RESPONSE_MAX_LEN - 1)
        {
            at_response[response_idx++] = (char)c;
            at_response[response_idx] = '\0';
        }
        if (c == '\n' || c == '\r')
        {
            line_seen = true;
        }
    }
}

/* Links the UART hardware to our callback function */
static int uart_initialize(void)
{
    uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart2));
    
    if (!device_is_ready(uart_dev))
    {
        LOG_ERR("UART device 'uart2' is not ready");
        return -ENODEV;
    }
    uart_irq_callback_user_data_set(uart_dev, uart_callback, NULL);
    uart_irq_rx_enable(uart_dev);

    k_mutex_init(&uart_mutex);

    LOG_INF("UART initialized (uart2)");
    return 0;
}

/* Sends raw byte data directly to the modem (used for certificates and payloads) */
static void uart_send_raw_m_locked(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; ++i)
    {
        uart_poll_out(uart_dev, buf[i]);
    }
}


/* --- AT COMMAND EXECUTION & PARSING --- */

/* Appends carriage return + newline and sends a standard string command */
static int send_at_command_locked(const char *cmd)
{
    if (!uart_dev)
        return -ENODEV;
    at_buf_clear_locked();
    LOG_DBG("AT> %s", cmd);
    for (size_t i = 0; i < strlen(cmd); ++i)
        uart_poll_out(uart_dev, (uint8_t)cmd[i]);
    uart_poll_out(uart_dev, '\r');
    uart_poll_out(uart_dev, '\n');
    return 0;
}

/* Reads the incoming buffer until it sees a specific token, "OK", or "ERROR" */
static int wait_for_response_token_locked(const char *token, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms)
    {
        if (token && strstr(at_response, token))
        {
            return 0;
        }
        if (!token)
        {
            if (strstr(at_response, "OK"))
                return 0;
            if (strstr(at_response, "ERROR") || strstr(at_response, "+CME ERROR"))
                return -EIO;
        }
        else
        {
            if (strstr(at_response, "ERROR") || strstr(at_response, "+CME ERROR"))
                return -EIO;
        }
        if (line_seen && (strstr(at_response, "OK") || strstr(at_response, "ERROR") || strstr(at_response, "+CME ERROR")))
        {
            return strstr(at_response, "OK") ? 0 : -EIO;
        }
        k_msleep(10);
        waited += 10;
    }
    LOG_DBG("wait_for_response_token timeout; buf=%s", at_response);
    return -ETIMEDOUT;
}

/* Helper to combine sending and waiting into one call */
static int at_cmd_and_wait_locked(const char *cmd, const char *token, int timeout_ms)
{
    int rc = send_at_command_locked(cmd);
    if (rc)
        return rc;
    return wait_for_response_token_locked(token, timeout_ms);
}

/* Waits for the '>' prompt indicating the modem is ready for raw file/message streaming */
static int wait_for_prompt_locked(int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms)
    {
        if (strchr(at_response, '>') || strstr(at_response, "CONNECT"))
            return 0;
        if (strstr(at_response, "+CME ERROR") || strstr(at_response, "ERROR"))
            return -EIO;
        k_msleep(10);
        waited += 10;
    }
    return -ETIMEDOUT;
}

/* Clears a stuck '>' prompt by sending a Ctrl+Z cancel command */
static int recover_stray_prompt_locked(void)
{
    if (strchr(at_response, '>'))
    {
        LOG_WRN("Stray '>' prompt present; sending Ctrl+Z to clear");
        uart_poll_out(uart_dev, 0x1A); /* Ctrl+Z */
        k_msleep(100);
        at_buf_clear_locked();
    }
    return 0;
}

/* Checks if the SIM card is physically present and readable */
void sim_status_checking()
{
    LOG_INF(" ------ Sim status checking :");

    k_mutex_lock(&uart_mutex, K_FOREVER);
    LOG_INF("Sending At command for checking sim_status :");
    send_at_command_locked("AT+CPIN?");
    if (wait_for_response_token_locked(NULL, 2000) == 0)
    {
        LOG_INF("SIM INSERTED ");
        k_msleep(10);
    }
    else
    {
        LOG_WRN("SIM NOT INSERTED");
        k_msleep(10);
    }
    k_mutex_unlock(&uart_mutex);
    return;
}


/* --- AWS CERTIFICATE UPLOADING --- */

/* Uploads a raw byte array (like a cert) into the modem's internal memory */
static int qfupl_upload_locked(const char *path, const uint8_t *data, size_t len, int timeout_s)
{
    char cmd[AT_CMD_SMALL_LEN];
    snprintf(cmd, sizeof(cmd), "AT+QFUPL=\"%s\",%lu,%d", path, len, timeout_s);
    int rc = send_at_command_locked(cmd);
    if (rc)
        return rc;

    rc = wait_for_prompt_locked((timeout_s + 5) * 1000);
    if (rc)
    {
        LOG_ERR("QFUPL: no upload prompt (> or CONNECT): %s", at_response);
        return rc;
    }

    LOG_INF("QFUPL: streaming %lu bytes to %s", len, path);

    uart_send_raw_m_locked(data, len);
    k_msleep(50);
    uart_poll_out(uart_dev, 0x1A);

    int waited = 0;
    int wait_ms = (timeout_s + 20) * 1000;
    while (waited < wait_ms)
    {
        if (strstr(at_response, "+QFUPL:") || strstr(at_response, "OK"))
        {
            LOG_DBG("QFUPL done: %s", at_response);
            return 0;
        }
        if (strstr(at_response, "+CME ERROR"))
        {
            LOG_ERR("QFUPL cme error: %s", at_response);
            return -EIO;
        }
        k_msleep(100);
        waited += 100;
    }
    return -ETIMEDOUT;
}

/* Attempts to upload certificates to permanent Flash (UFS). 
 * If it fails, falls back to ephemeral RAM storage. */
static const char *upload_certs_with_fallback_locked(void)
{
    static char storage[5] = "UFS";
    LOG_INF("Attempting UFS uploads first...");
    
    /* Delete old certificates */
    at_cmd_and_wait_locked("AT+QFDEL=\"UFS:cacert.pem\"", NULL, 2000);
    at_cmd_and_wait_locked("AT+QFDEL=\"UFS:client.pem\"", NULL, 2000);
    at_cmd_and_wait_locked("AT+QFDEL=\"UFS:client_key.pem\"", NULL, 2000);

    int rc = qfupl_upload_locked("UFS:cacert.pem", cacert_pem, cacert_pem_len, 120);
    if (rc < 0)
    {
        LOG_WRN("UFS cacert upload failed (%d), will fallback to RAM", rc);
        strcpy(storage, "RAM");
    }
    else
    {
        rc = qfupl_upload_locked("UFS:client.pem", client_pem, client_pem_len, 120);
        if (rc < 0)
        {
            strcpy(storage, "RAM");
        }
        else
        {
            rc = qfupl_upload_locked("UFS:client_key.pem", client_key_pem, client_key_pem_len, 120);
            if (rc < 0)
            {
                strcpy(storage, "RAM");
            }
        }
    }

    if (strcmp(storage, "RAM") == 0)
    {
        LOG_INF("Uploading certs to RAM (ephemeral)");
        qfupl_upload_locked("RAM:cacert.pem", cacert_pem, cacert_pem_len, 120);
        qfupl_upload_locked("RAM:client.pem", client_pem, client_pem_len, 120);
        qfupl_upload_locked("RAM:client_key.pem", client_key_pem, client_key_pem_len, 120);
    }
    else
    {
        LOG_INF("Certs uploaded to UFS");
    }
    return storage;
}


/* --- MQTT SUBSCRIBE & RECEIVE --- */

/* Subscribes the modem to a specific AWS MQTT Topic */
int gsm_mqtt_subscribe(const char *topic, int timeout_ms)
{
    if (!topic)
        return -EINVAL;

    int rc = 0;
    char subcmd[AT_CMD_SMALL_LEN];
    snprintf(subcmd, sizeof(subcmd),
             "AT+QMTSUB=0,%d,\"%s\",1",
             mqtt_sub_id++, topic);

    k_mutex_lock(&uart_mutex, K_FOREVER);

    if (!mqtt_connected)
    {
        LOG_ERR("MQTT not connected");
        k_mutex_unlock(&uart_mutex);
        return -ENOTCONN;
    }
    k_msleep(500);
    at_buf_clear_locked();
    rc = send_at_command_locked(subcmd);
    if (rc)
    {
        LOG_ERR("Subscribe command failed");
        k_mutex_unlock(&uart_mutex);
        return rc;
    }

    int waited = 0;
    while (waited < timeout_ms)
    {
        if (strstr(at_response, "+QMTSUB:"))
        {
            int client, msg_id, result;

            if (sscanf(at_response,
                       "+QMTSUB: %d,%d,%d",
                       &client, &msg_id, &result) == 3)
            {
                if (result == 0)
                {
                    LOG_INF("Subscribed to topic: %s (msgid=%d)", topic, msg_id);
                    rc = 0;
                }
                else
                {
                    LOG_ERR("Subscribe failed: result=%d", result);
                    rc = -EIO;
                }
            }
            break;
        }

        if (strstr(at_response, "+CME ERROR") ||
            strstr(at_response, "ERROR"))
        {
            LOG_ERR("Subscribe modem error: %s", at_response);
            rc = -EIO;
            break;
        }

        k_msleep(100);
        waited += 100;
    }
    
    if (rc != 0)
    {
        LOG_ERR("MQTT subscribe failed, MQTT state unstable");
        mqtt_connected = false;
        led_off(&cloudled);
    }

    if (waited >= timeout_ms)
    {
        LOG_ERR("Subscribe timeout");
        rc = -ETIMEDOUT;
    }

    k_mutex_unlock(&uart_mutex);
    return rc;
}

/* Scans the incoming buffer for messages pushed by the AWS Broker */
int gsm_mqtt_check_messages(char *topic_out, size_t topic_len,
                            char *payload_out, size_t payload_len,
                            int timeout_ms)
{
    int rc = -ETIMEDOUT;
    k_mutex_lock(&uart_mutex, K_FOREVER);

    int waited = 0;
    while (waited < timeout_ms)
    {
        if (strstr(at_response, "+QMTRECV:"))
        {
            char *start = strstr(at_response, "+QMTRECV:");
            if (start)
            {
                LOG_INF("MQTT message received: %s", start);

                /* Extract Topic */
                char *topic_start = strchr(start, '"');
                if (topic_start)
                {
                    topic_start++;
                    char *topic_end = strchr(topic_start, '"');
                    if (topic_end)
                    {
                        size_t len = topic_end - topic_start;
                        if (len < topic_len)
                        {
                            memcpy(topic_out, topic_start, len);
                            topic_out[len] = '\0';
                        }

                        /* Extract Payload */
                        char *payload_start = strchr(topic_end + 1, '"');
                        if (payload_start)
                        {
                            payload_start++;
                            char *payload_end = strrchr(payload_start, '"');
                            if (payload_end)
                            {
                                len = payload_end - payload_start;
                                if (len < payload_len)
                                {
                                    memcpy(payload_out, payload_start, len);
                                    payload_out[len] = '\0';
                                    rc = 0; 
                                }
                            }
                        }
                    }
                }
                at_buf_clear_locked();
                break;
            }
        }
        k_msleep(100);
        waited += 100;
    }

    k_mutex_unlock(&uart_mutex);
    return rc;
}


/* --- TIME & EPOCH CONVERSIONS --- */

/* Converts standard Date/Time structure into raw seconds since 1970 */
time_t my_timegm(struct tm *tm)
{
    static const int month_days[12] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    int year = tm->tm_year + 1900;
    int month = tm->tm_mon;
    int day = tm->tm_mday - 1;

    time_t days = (year - 1970) * 365 + (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400;

    for (int i = 0; i < month; i++)
    {
        days += month_days[i];
        if (i == 1)
        { 
            int leap = ((year % 4 == 0) &&
                        (year % 100 != 0 || year % 400 == 0));
            if (leap)
                days++;
        }
    }

    days += day;
    time_t seconds = days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;

    return seconds;
}

static bool read_int_n(const char **p, int digits, int *out)
{
    int val = 0;
    for (int i = 0; i < digits; i++)
    {
        char c = **p;
        if (c < '0' || c > '9') return false;
        val = val * 10 + (c - '0');
        (*p)++;
    }
    *out = val;
    return true;
}

/* Parses the raw modem time string (YYYY/MM/DD,HH:MM:SS) into Epoch format */
long convert_qlts_to_epoch(const char *dt)
{
    if (!dt) return -1;

    const char *p = dt;
    int year, month, day, hour, min, sec;
    int tz_qh = 0;
    char tz_sign = '+';

    if (!read_int_n(&p, 4, &year) || *p != '/') return -1; p++;
    if (!read_int_n(&p, 2, &month) || *p != '/') return -1; p++;
    if (!read_int_n(&p, 2, &day) || *p != ',') return -1; p++;
    if (!read_int_n(&p, 2, &hour) || *p != ':') return -1; p++;
    if (!read_int_n(&p, 2, &min) || *p != ':') return -1; p++;
    if (!read_int_n(&p, 2, &sec)) return -1;

    if (*p == '+' || *p == '-')
    {
        tz_sign = *p;
        p++;
        if (!read_int_n(&p, 2, &tz_qh)) return -1;

        if (*p == ',')
        {
            p++;
            int dummy;
            if (*p >= '0' && *p <= '9')
            {
                if (!read_int_n(&p, 1, &dummy)) return -1;
            }
        }
    }
    else
    {
        tz_qh = 0;
        tz_sign = '+';
    }

    int tz_offset_seconds = tz_qh * 15 * 60;
    if (tz_sign == '-') tz_offset_seconds = -tz_offset_seconds;

    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;

    time_t local_time = my_timegm(&t);
    time_t utc = local_time - tz_offset_seconds;

    return (long)utc;
}

uint32_t datetime_to_epoch(const char *dt_str)
{
    if (!dt_str) return 0;
    long epoch = convert_qlts_to_epoch(dt_str);
    return (epoch < 0) ? 0 : (uint32_t)epoch;
}

/* Asks the cell tower for the exact current network time */
int gsm_time_fetch(uint8_t arr[])
{
    int rc = 0;
    k_mutex_lock(&uart_mutex, K_FOREVER);
    at_buf_clear_locked();
    rc = send_at_command_locked("AT+QLTS=2");
    
    if (rc == 0)
    {
        rc = wait_for_response_token_locked("+QLTS:", 3000);
        if (rc == 0)
        {
            char *start = strstr(at_response, "+QLTS: \"");
            if (start)
            {
                start += 8;
                char *end = strchr(start, '"');
                if (end)
                {
                    size_t dt_len = end - start;
                    if (dt_len < sizeof(stored_datetime))
                    {
                        memcpy(stored_datetime, start, dt_len);
                        stored_datetime[dt_len] = '\0';
                        printf("Network DateTime: %s", stored_datetime);
                    }
                    else
                    {
                        LOG_ERR("Failed to parse stored_datetime");
                    }
                }
            }
        }
        else
        {
            LOG_WRN("QLTS query failed, keeping datetime as N/A");
        }
    }

    at_buf_clear_locked();
    k_mutex_unlock(&uart_mutex);
    long epoch_tm = convert_qlts_to_epoch(stored_datetime);

    if (epoch_tm < 0)
    {
        LOG_ERR("Failed to convert QLTS datetime to epoch");
        return -1;
    }

    int64_t divisor_epoch = 1000000000;

    for (int i = 0; i < 10; i++)
    {
        arr[i] = (uint8_t)((epoch_tm / divisor_epoch) % 10);
        divisor_epoch /= 10;
    }

    return 0;   
}


/* --- NETWORK CONNECTION ROUTINE --- */

/* 
 * The master sequence that completely boots the modem, connects to the 
 * cellular network, configures TLS Security, and logs into AWS via MQTT.
 */
int gsm_network_connect(void)
{
    int rc = 0;
    k_mutex_lock(&uart_mutex, K_FOREVER);

    LOG_INF("Starting GSM Network Connection...");

    /* Ensure LEDs are OFF before trying to connect */
    led_off(&gatewayled);
    led_off(&cloudled);

    /* Basic hardware check */
    at_buf_clear_locked();
    rc = at_cmd_and_wait_locked("AT", NULL, 5000);
    if (rc)
    {
        LOG_ERR("AT failed");
        goto out;
    }

    at_cmd_and_wait_locked("AT+CMEE=2", NULL, 5000);
    at_cmd_and_wait_locked("AT&D0", NULL, 2000);
    at_cmd_and_wait_locked("AT+CFUN=1", NULL, 10000);

    /* Wait for SIM card to be ready */
    int waited = 0;
    const int max_ms = 20000;
    for (;;)
    {
        /* Blinking indicates searching for SIM */
        led_toggle(&gatewayled);

        at_buf_clear_locked();
        rc = send_at_command_locked("AT+CPIN?");
        if (rc)
        {
            LOG_ERR("CPIN send fail");
            rc = -EIO;
            goto out;
        }
        rc = wait_for_response_token_locked(NULL, 2000);
        if (rc == 0 && strstr(at_response, "READY"))
        {
            LOG_INF("SIM is READY");
            break;
        }
        else if (rc == 0 && strstr(at_response, "SIM PIN"))
        {
            LOG_ERR("SIM requires PIN");
            rc = -EACCES;
            goto out;
        }
        else if (rc == 0 && strstr(at_response, "SIM not inserted"))
        {
            LOG_ERR("SIM not inserted");
            rc = -ENODEV;
            goto out;
        }
        
        if ((waited += 500) > max_ms)
        {
            LOG_ERR("SIM not ready after %dms; last: %s", max_ms, at_response);
            rc = -ETIMEDOUT;
            goto out;
        }
        k_msleep(500);
    }

    /* Probe device and network registration */
    at_cmd_and_wait_locked("AT+CGSN", NULL, 5000);
    at_cmd_and_wait_locked("AT+CIMI", NULL, 5000);
    at_cmd_and_wait_locked("AT+CREG?", NULL, 5000);
    at_cmd_and_wait_locked("AT+CEREG?", NULL, 5000);
    at_cmd_and_wait_locked("AT+CSQ", NULL, 5000);

    /* Verify UFS Storage and Upload Certificates */
    at_cmd_and_wait_locked("AT+QFLDS=\"UFS\"", NULL, 2000);
    at_cmd_and_wait_locked("AT+QFLST=\"UFS:*\"", NULL, 2000);
    const char *storage = "UFS";

    if (!certs_uploaded)
    {
        storage = upload_certs_with_fallback_locked();
        certs_uploaded = true;
    }
    
    at_cmd_and_wait_locked("AT+QFLST=\"UFS:*\"", NULL, 5000);

    /* Establish cellular data connection (PDP context) */
    at_cmd_and_wait_locked("AT+CGDCONT=1,\"IP\"", NULL, 5000);
    at_cmd_and_wait_locked("AT+CGATT=1", NULL, 60000);
    at_cmd_and_wait_locked("AT+QICSGP=1,1,\"internet\",\"\",\"\",1", NULL, 5000);
    at_cmd_and_wait_locked("AT+QIACT=1", NULL, 30000);
    at_cmd_and_wait_locked("AT+QIACT?", NULL, 5000);

    /* Connection established - Gateway LED becomes solid */
    led_on(&gatewayled);

    /* Configure TLS/MQTT parameters */
    at_cmd_and_wait_locked("AT+QMTCFG=\"clean_session\",0,0", NULL, 2000);
    at_cmd_and_wait_locked("AT+QMTCFG=\"recv/mode\",0,0,1", NULL, 5000);
    at_cmd_and_wait_locked("AT+QSSLCFG=\"ignorelocaltime\",2,1", NULL, 2000);
    at_cmd_and_wait_locked("AT+QSSLCFG=\"sslversion\",2,4", NULL, 2000);
    at_cmd_and_wait_locked("AT+QSSLCFG=\"seclevel\",2,2", NULL, 2000);
    at_cmd_and_wait_locked("AT+QSSLCFG=\"sni\",2,1", NULL, 2000);

    /* Inform modem of AWS Broker domain */
    {
        char qcmd[AT_CMD_SMALL_LEN];
        snprintf(qcmd, sizeof(qcmd), "AT+QSSLCFG=\"serverdomain\",2,\"a1tgjydixa0qkm-ats.iot.us-east-1.amazonaws.com\"");
        rc = at_cmd_and_wait_locked(qcmd, NULL, 2000);
        if (rc)
        {
            LOG_WRN("QSSLCFG serverdomain not accepted");
            at_buf_clear_locked();
        }
    }

    /* Assign previously uploaded certificates to TLS context */
    {
        char qcmd[AT_CMD_SMALL_LEN];
        snprintf(qcmd, sizeof(qcmd), "AT+QSSLCFG=\"cacert\",2,\"%s:cacert.pem\"", storage);
        at_cmd_and_wait_locked(qcmd, NULL, 2000);
        snprintf(qcmd, sizeof(qcmd), "AT+QSSLCFG=\"clientcert\",2,\"%s:client.pem\"", storage);
        at_cmd_and_wait_locked(qcmd, NULL, 2000);
        snprintf(qcmd, sizeof(qcmd), "AT+QSSLCFG=\"clientkey\",2,\"%s:client_key.pem\"", storage);
        at_cmd_and_wait_locked(qcmd, NULL, 2000);
        at_cmd_and_wait_locked("AT+QMTCFG=\"ssl\",0,1,2", NULL, 2000);
    }

    rc = at_cmd_and_wait_locked("AT+QIDNSCFG=1,8.8.8.8,1.1.1.1", NULL, 2000);
    if (rc)
    {
        LOG_WRN("QIDNSCFG rejected - using default DNS");
        at_buf_clear_locked();
    }

    /* Open the secure TCP connection to AWS on Port 8883 */
    at_buf_clear_locked();
    at_cmd_and_wait_locked("AT+QMTCFG=\"keepalive\",0,120", NULL, 2000);
    send_at_command_locked("AT+QMTOPEN=0,\"a1tgjydixa0qkm-ats.iot.us-east-1.amazonaws.com\",8883");

    waited = 0;
    bool open_ok = false;

    while (waited < 30000)
    {
        /* Blink Cloud LED during TLS negotiation */
        led_toggle(&cloudled);

        if (strstr(at_response, "+QMTOPEN: 0,0"))
        {
            open_ok = true;
            break;
        }
        if (strstr(at_response, "+QMTOPEN: 0,"))
        {
            LOG_ERR("QMTOPEN failed: %s", at_response);
            break;
        }
        k_msleep(100);
        waited += 100;
    }

    if (!open_ok)
    {
        LOG_ERR("QMTOPEN timeout, forcing modem recovery");
        at_cmd_and_wait_locked("AT+QMTCLOSE=0", NULL, 3000);
        at_cmd_and_wait_locked("AT+QIDEACT=1", NULL, 10000);
        rc = -EIO;
        goto out;
    }

    /* Send MQTT Login command */
    at_buf_clear_locked();
    send_at_command_locked("AT+QMTCONN=0,\"M26_0206\"");

    waited = 0;
    bool conn_ok = false;

    while (waited < 30000)
    {
        /* Continue blinking until login is fully confirmed */
        led_toggle(&cloudled);

        if (strstr(at_response, "+QMTCONN: 0,0,0"))
        {
            conn_ok = true;
            break;
        }
        if (strstr(at_response, "+QMTCONN: 0,"))
        {
            LOG_ERR("QMTCONN failed: %s", at_response);
            break;
        }
        k_msleep(100);
        waited += 100;
    }

    if (!conn_ok)
    {
        rc = -EIO;
        goto out;
    }

    qos1_in_flight = false;
    qos1_busy_count = 0;
    mqtt_sub_id = 1;
    mqtt_pub_id = 1;
    mqtt_connected = true;
    rc = 0;
    
    LOG_INF("MQTT/TLS connected (client 0)");

    /* MQTT Online - Cloud LED becomes solid */
    led_on(&cloudled);

    k_msleep(1000);

out:
    k_msleep(1500);
    at_buf_clear_locked();
    k_mutex_unlock(&uart_mutex);
    return rc;
}

void gsm_get_datetime(char *datetime_out, size_t len)
{
    if (datetime_out && len > 0)
    {
        strncpy(datetime_out, stored_datetime, len - 1);
        datetime_out[len - 1] = '\0';
    }
}


/* --- MQTT PUBLISHING --- */

/* Background task to handle network drops and incoming QOS messages */
static void mqtt_rx_drain_locked(void)
{
    /* If the modem reports a disconnected status */
    if (strstr(at_response, "+QMTSTAT: 0,1"))
    {
        LOG_WRN("MQTT connection lost (QMTSTAT)");

        mqtt_connected = false;
        qos1_in_flight = false;
        qos1_busy_count = 0;

        /* Turn Cloud LED off indicating loss of connection */
        led_off(&cloudled);

        at_buf_clear_locked();
        return;
    }

    /* Drain standard messages */
    if (strstr(at_response, "+QMTRECV") ||
        strstr(at_response, "+QMTPUBEX") ||
        strstr(at_response, "+QMTSTAT"))
    {
        LOG_DBG("Draining MQTT RX buffer");
        at_buf_clear_locked();
    }
}

/* 
 * Takes JSON string payloads from main.c and pushes them securely to AWS.
 * Monitors for timeouts and failure strikes to maintain stability. 
 */
int gsm_mqtt_publish(const char *topic, const char *payload, size_t payload_len, int qos, int timeout_ms)
{
    if (qos == 1 && qos1_in_flight)
    {
        LOG_WRN("QoS1 publish already in flight");
        return -EAGAIN;
    }

    if (!topic || (!payload && payload_len > 0))
        return -EINVAL;

    int rc = 0;
    char pubcmd[AT_CMD_SMALL_LEN];
    uint16_t msgid = mqtt_pub_id++;
    if (mqtt_pub_id == 0)
    {
        mqtt_pub_id = 1;
    }

    /* Prepare publish AT command */
    snprintf(pubcmd, sizeof(pubcmd),
             "AT+QMTPUBEX=0,%u,%d,0,\"%s\",%lu",
             msgid, qos, topic, payload_len);

    k_mutex_lock(&uart_mutex, K_FOREVER);

    mqtt_rx_drain_locked();
    recover_stray_prompt_locked();

    /* Ensure we are connected before trying to publish */
    if (!mqtt_connected)
    {
        LOG_ERR("MQTT not connected, reconnecting");
        k_mutex_unlock(&uart_mutex);

        rc = gsm_network_connect();
        if (rc)
            return rc;

        k_mutex_lock(&uart_mutex, K_FOREVER);
    }
    
    at_buf_clear_locked();
    k_msleep(50);
    if (qos == 1)
    {
        qos1_in_flight = true;
    }

    rc = send_at_command_locked(pubcmd);
    if (rc)
    {
        LOG_ERR("send_at_command failed");
        k_mutex_unlock(&uart_mutex);
        return rc;
    }

    /* Wait for modem to acknowledge command */
    rc = wait_for_prompt_locked(timeout_ms);
    if (rc == -ETIMEDOUT)
    {
        LOG_ERR("Publish: no '>' prompt: %s", at_response);
        recover_stray_prompt_locked();
        k_mutex_unlock(&uart_mutex);
        return -ETIMEDOUT;
    }
    else if (rc < 0)
    {
        if (qos == 1)
        {
            qos1_in_flight = false;
            qos1_busy_count++;

            LOG_WRN("QoS1 busy/error (%d): %s",
                    qos1_busy_count, at_response);

            /* If publish fails 3 times, assume dead connection and close sockets */
            if (qos1_busy_count >= 3)
            {
                LOG_ERR("QoS1 stuck, resetting MQTT");

                mqtt_connected = false;
                qos1_busy_count = 0;

                led_off(&cloudled);

                at_cmd_and_wait_locked("AT+QMTDISC=0", NULL, 5000);
                at_cmd_and_wait_locked("AT+QMTCLOSE=0", NULL, 5000);
            }

            at_buf_clear_locked();
            k_mutex_unlock(&uart_mutex);
            return -EAGAIN;
        }

        at_buf_clear_locked();
        k_mutex_unlock(&uart_mutex);
        return -EIO;
    }

    /* Stream the actual JSON string into the modem */
    LOG_DBG("Streaming %lu bytes payload", payload_len);
    if (payload_len > 0)
    {
        uart_send_raw_m_locked((const uint8_t *)payload, payload_len);
    }
    k_msleep(30);
    uart_poll_out(uart_dev, 0x1A); /* Send Ctrl+Z to finish stream */

    /* Wait for the AWS broker to send the QMTPUBEX delivery confirmation */
    int waited = 0;
    int wait_ms = timeout_ms;
    bool pub_ok = false;

    while (waited < wait_ms)
    {
        if (strstr(at_response, "+QMTPUBEX:"))
        {
            int client, msg_id, result;
            char *p = strstr(at_response, "+QMTPUBEX:");

            if (p &&
                sscanf(p, "+QMTPUBEX: %d,%d,%d",
                       &client, &msg_id, &result) == 3)
            {
                if (result == 0)
                {
                    pub_ok = true;
                }
                else
                {
                    LOG_ERR("Publish failed: client=%d msg_id=%d result=%d",
                            client, msg_id, result);

                    if (result == 2)
                    {
                        LOG_ERR("MQTT connection lost during publish");
                        mqtt_connected = false;
                        qos1_in_flight = false;
                        qos1_busy_count = 0;

                        led_off(&cloudled);
                    }
                }
            }
            break;
        }

        if (strstr(at_response, "+CME ERROR") ||
            strstr(at_response, "ERROR"))
        {
            LOG_ERR("Publish modem error: %s", at_response);
            break;
        }

        k_msleep(50);
        waited += 50;
    }

    if (!pub_ok)
    {
        LOG_ERR("Publish failed or timed out; buf=%s", at_response);

        if (qos == 1)
        {
            qos1_busy_count++;
            qos1_in_flight = false;
        }

        rc = -EIO;
    }
    else
    {
        LOG_INF("Publish OK");
        int drain_wait = 0;
        
        while (drain_wait < 2000)
        {
            if (strstr(at_response, "+QMTRECV"))
            {
                LOG_DBG("Drained loopback message");
                at_buf_clear_locked();
                break;
            }
            k_msleep(50);
            drain_wait += 50;
        }

        if (qos == 1)
        {
            qos1_in_flight = false;
            qos1_busy_count = 0;
        }

        mqtt_rx_drain_locked();
        rc = 0;
    }

    k_msleep(50);
    at_buf_clear_locked();
    k_mutex_unlock(&uart_mutex);
    return rc;
}


/* --- SYSTEM INITIALIZATION --- */

/* Called once on boot by main.c to prep hardware */
void init_gsm(void)
{
    LOG_INF("GSM init");

    led_off(&gatewayled);
    led_off(&cloudled);

    if (uart_initialize() < 0)
    {
        LOG_ERR("uart init failed");
        return;
    }

    k_msleep(200);
    k_mutex_lock(&uart_mutex, K_FOREVER);
    
    if (strstr(at_response, "RDY"))
    {
        LOG_INF("Modem RDY banner");
    }
    at_buf_clear_locked();
    k_mutex_unlock(&uart_mutex);
    k_msleep(20000); /* 20 second wait allows the EC200U modem to boot fully */
}