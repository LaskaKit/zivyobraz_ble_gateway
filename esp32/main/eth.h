#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* Start Ethernet (DHCP). The network interface and event loop must exist. */
esp_err_t eth_start(const char *hostname);
bool eth_has_ip(void);
bool eth_wait_ip(int timeout_ms);
/* "192.168.1.50", or what is missing ("no Ethernet link", ...). */
void eth_status(char *out, size_t size);
