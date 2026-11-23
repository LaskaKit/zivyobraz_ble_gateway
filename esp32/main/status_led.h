#pragma once

typedef enum { STATUS_BOOT, STATUS_NO_NET, STATUS_OK, STATUS_BUSY } status_t;

void status_led_init(void);
void status_led_set(status_t s);
