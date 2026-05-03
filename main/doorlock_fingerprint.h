#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define DOORLOCK_FINGERPRINT_NORMAL_RX_GPIO     10
#define DOORLOCK_FINGERPRINT_NORMAL_TX_GPIO     11
#define DOORLOCK_FINGERPRINT_DEFAULT_CAPACITY   300

typedef enum {
    DOORLOCK_FINGERPRINT_WIRING_UNKNOWN = 0,
    DOORLOCK_FINGERPRINT_WIRING_NORMAL,
    DOORLOCK_FINGERPRINT_WIRING_SWAPPED,
} doorlock_fingerprint_wiring_t;

typedef struct {
    doorlock_fingerprint_wiring_t wiring;
    uint32_t baudrate;
    uint16_t template_count;
    uint16_t capacity;
    int8_t normal_rx_idle_level;
    int8_t normal_rx_pull_down_level;
    int8_t normal_rx_pull_up_level;
    int8_t swapped_rx_idle_level;
    int8_t swapped_rx_pull_down_level;
    int8_t swapped_rx_pull_up_level;
    uint8_t last_code;
} doorlock_fingerprint_status_t;

esp_err_t doorlock_fingerprint_probe(doorlock_fingerprint_status_t *status);
esp_err_t doorlock_fingerprint_tx_diagnostic(void);
esp_err_t doorlock_fingerprint_get_count(uint16_t *count, uint16_t *capacity);
esp_err_t doorlock_fingerprint_enroll(uint16_t *slot);
esp_err_t doorlock_fingerprint_verify(uint16_t *matched_slot, uint16_t *score);
esp_err_t doorlock_fingerprint_clear(void);
uint8_t doorlock_fingerprint_get_last_code(void);
