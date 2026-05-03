#pragma once

#include <stdint.h>
#include "esp_err.h"

#define DOORLOCK_FACE_MAX_TEMPLATES       4
#define DOORLOCK_FACE_MATCH_THRESHOLD     42

esp_err_t doorlock_face_get_count(uint8_t *count);
esp_err_t doorlock_face_enroll(uint8_t *slot, uint32_t *quality);
esp_err_t doorlock_face_verify(uint8_t *matched_slot, uint32_t *score);
esp_err_t doorlock_face_clear(void);
