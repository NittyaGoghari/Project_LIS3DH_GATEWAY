#ifndef __ATCOMMAND_H__
#define __ATCOMMAND_H__

#include <stdint.h>
#include <stddef.h>

void init_gsm(void);
int gsm_network_connect(void);
int gsm_mqtt_publish(const char *topic,const char *payload,size_t payload_len,int qos,int timeout_ms);
int gsm_mqtt_subscribe(const char *topic, int timeout_ms);
int gsm_mqtt_check_messages(char *topic_out, size_t topic_len,
                            char *payload_out, size_t payload_len,
                            int timeout_ms);
void gsm_get_datetime(char *datetime_out, size_t len);
int gsm_time_fetch(uint8_t arr[10]);

uint32_t datetime_to_epoch(const char *dt_str);
void sim_status_checking();

void mqtt_rx_drain_locked(void);

#endif