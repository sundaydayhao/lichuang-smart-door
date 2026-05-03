#include "doorlock_face.h"

#include <stdbool.h>
#include <string.h>
#include "esp32_s3_szp.h"
#include "esp_camera.h"
#include "nvs.h"

#define DOORLOCK_FACE_TEMPLATE_W          16
#define DOORLOCK_FACE_TEMPLATE_H          16
#define DOORLOCK_FACE_FEATURE_LEN         (DOORLOCK_FACE_TEMPLATE_W * DOORLOCK_FACE_TEMPLATE_H)
#define DOORLOCK_FACE_STORE_MAGIC         0x46414345
#define DOORLOCK_FACE_STORE_VERSION       1
#define DOORLOCK_FACE_NVS_NAMESPACE       "doorlock"
#define DOORLOCK_FACE_NVS_KEY             "face_set"
#define DOORLOCK_FACE_MIN_TEXTURE         6
#define DOORLOCK_FACE_WARMUP_FRAMES       2
#define DOORLOCK_FACE_CAPTURE_TRIES       8

typedef struct {
    uint8_t feature[DOORLOCK_FACE_FEATURE_LEN];
    uint8_t quality;
    uint8_t reserved[3];
} doorlock_face_template_t;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t count;
    uint8_t reserved[2];
    doorlock_face_template_t templates[DOORLOCK_FACE_MAX_TEMPLATES];
} doorlock_face_store_t;

static void doorlock_face_store_init_empty(doorlock_face_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->magic = DOORLOCK_FACE_STORE_MAGIC;
    store->version = DOORLOCK_FACE_STORE_VERSION;
}

static esp_err_t doorlock_face_store_load(doorlock_face_store_t *store)
{
    nvs_handle_t handle;
    size_t required_size = sizeof(*store);
    esp_err_t ret;

    doorlock_face_store_init_empty(store);

    ret = nvs_open(DOORLOCK_FACE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_blob(handle, DOORLOCK_FACE_NVS_KEY, store, &required_size);
    nvs_close(handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        doorlock_face_store_init_empty(store);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        doorlock_face_store_init_empty(store);
        return ret;
    }
    if (required_size != sizeof(*store) || store->magic != DOORLOCK_FACE_STORE_MAGIC ||
        store->version != DOORLOCK_FACE_STORE_VERSION || store->count > DOORLOCK_FACE_MAX_TEMPLATES) {
        doorlock_face_store_init_empty(store);
    }

    return ESP_OK;
}

static esp_err_t doorlock_face_store_save(doorlock_face_store_t *store)
{
    nvs_handle_t handle;
    esp_err_t ret;

    store->magic = DOORLOCK_FACE_STORE_MAGIC;
    store->version = DOORLOCK_FACE_STORE_VERSION;
    if (store->count > DOORLOCK_FACE_MAX_TEMPLATES) {
        store->count = DOORLOCK_FACE_MAX_TEMPLATES;
    }

    ret = nvs_open(DOORLOCK_FACE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_blob(handle, DOORLOCK_FACE_NVS_KEY, store, sizeof(*store));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    return ret;
}

static uint8_t doorlock_face_rgb565_to_gray(uint16_t pixel)
{
    uint8_t r = (pixel >> 11) & 0x1f;
    uint8_t g = (pixel >> 5) & 0x3f;
    uint8_t b = pixel & 0x1f;
    uint16_t r8 = (r << 3) | (r >> 2);
    uint16_t g8 = (g << 2) | (g >> 4);
    uint16_t b8 = (b << 3) | (b >> 2);

    return (uint8_t)((r8 * 30 + g8 * 59 + b8 * 11) / 100);
}

static uint8_t doorlock_face_clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }

    return (uint8_t)value;
}

static esp_err_t doorlock_face_build_template(const camera_fb_t *frame,
                                              doorlock_face_template_t *face_template)
{
    uint8_t raw[DOORLOCK_FACE_FEATURE_LEN];
    uint32_t total = 0;
    uint32_t deviation_sum = 0;
    uint32_t mean;
    uint32_t quality;

    if (frame == NULL || frame->buf == NULL || frame->format != PIXFORMAT_RGB565 ||
        frame->width < DOORLOCK_FACE_TEMPLATE_W || frame->height < DOORLOCK_FACE_TEMPLATE_H) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint16_t *pixels = (const uint16_t *)frame->buf;
    const uint16_t crop_w = frame->width / 2;
    const uint16_t crop_h = (frame->height * 3) / 4;
    const uint16_t crop_x = (frame->width - crop_w) / 2;
    const uint16_t crop_y = (frame->height - crop_h) / 2;

    for (uint16_t gy = 0; gy < DOORLOCK_FACE_TEMPLATE_H; gy++) {
        uint16_t y0 = crop_y + (gy * crop_h) / DOORLOCK_FACE_TEMPLATE_H;
        uint16_t y1 = crop_y + ((gy + 1) * crop_h) / DOORLOCK_FACE_TEMPLATE_H;

        for (uint16_t gx = 0; gx < DOORLOCK_FACE_TEMPLATE_W; gx++) {
            uint16_t x0 = crop_x + (gx * crop_w) / DOORLOCK_FACE_TEMPLATE_W;
            uint16_t x1 = crop_x + ((gx + 1) * crop_w) / DOORLOCK_FACE_TEMPLATE_W;
            uint32_t cell_sum = 0;
            uint32_t cell_count = 0;

            for (uint16_t y = y0; y < y1; y++) {
                const uint16_t *row = &pixels[y * frame->width];
                for (uint16_t x = x0; x < x1; x++) {
                    cell_sum += doorlock_face_rgb565_to_gray(row[x]);
                    cell_count++;
                }
            }

            uint8_t gray = cell_count > 0 ? (uint8_t)(cell_sum / cell_count) : 0;
            raw[gy * DOORLOCK_FACE_TEMPLATE_W + gx] = gray;
            total += gray;
        }
    }

    mean = total / DOORLOCK_FACE_FEATURE_LEN;
    for (size_t i = 0; i < DOORLOCK_FACE_FEATURE_LEN; i++) {
        deviation_sum += raw[i] > mean ? raw[i] - mean : mean - raw[i];
    }

    quality = deviation_sum / DOORLOCK_FACE_FEATURE_LEN;
    if (quality < DOORLOCK_FACE_MIN_TEXTURE) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (size_t i = 0; i < DOORLOCK_FACE_FEATURE_LEN; i++) {
        int normalized = 128 + (((int)raw[i] - (int)mean) * 32) / (int)quality;
        face_template->feature[i] = doorlock_face_clamp_u8(normalized);
    }
    face_template->quality = (uint8_t)(quality > 255 ? 255 : quality);
    memset(face_template->reserved, 0, sizeof(face_template->reserved));

    return ESP_OK;
}

static uint32_t doorlock_face_score(const doorlock_face_template_t *a,
                                    const doorlock_face_template_t *b)
{
    uint32_t diff_sum = 0;

    for (size_t i = 0; i < DOORLOCK_FACE_FEATURE_LEN; i++) {
        diff_sum += a->feature[i] > b->feature[i] ? a->feature[i] - b->feature[i] : b->feature[i] - a->feature[i];
    }

    return diff_sum / DOORLOCK_FACE_FEATURE_LEN;
}

static esp_err_t doorlock_face_capture_template(doorlock_face_template_t *face_template)
{
    esp_err_t ret = bsp_camera_init();
    camera_fb_t *frame = NULL;

    if (ret != ESP_OK) {
        dvp_pwdn(1);
        return ret;
    }

    for (uint8_t i = 0; i < DOORLOCK_FACE_CAPTURE_TRIES; i++) {
        frame = esp_camera_fb_get();
        if (frame == NULL) {
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        if (i < DOORLOCK_FACE_WARMUP_FRAMES) {
            esp_camera_fb_return(frame);
            frame = NULL;
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }

        ret = doorlock_face_build_template(frame, face_template);
        esp_camera_fb_return(frame);
        esp_camera_deinit();
        dvp_pwdn(1);
        return ret;
    }

    esp_camera_deinit();
    dvp_pwdn(1);
    return ESP_ERR_TIMEOUT;
}

esp_err_t doorlock_face_get_count(uint8_t *count)
{
    doorlock_face_store_t store;
    esp_err_t ret;

    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = doorlock_face_store_load(&store);
    if (ret != ESP_OK) {
        return ret;
    }

    *count = store.count;
    return ESP_OK;
}

esp_err_t doorlock_face_enroll(uint8_t *slot, uint32_t *quality)
{
    doorlock_face_store_t store;
    doorlock_face_template_t face_template;
    esp_err_t ret;

    ret = doorlock_face_store_load(&store);
    if (ret != ESP_OK) {
        return ret;
    }
    if (store.count >= DOORLOCK_FACE_MAX_TEMPLATES) {
        return ESP_ERR_NO_MEM;
    }

    ret = doorlock_face_capture_template(&face_template);
    if (ret != ESP_OK) {
        return ret;
    }

    store.templates[store.count] = face_template;
    if (slot != NULL) {
        *slot = store.count;
    }
    if (quality != NULL) {
        *quality = face_template.quality;
    }
    store.count++;

    return doorlock_face_store_save(&store);
}

esp_err_t doorlock_face_verify(uint8_t *matched_slot, uint32_t *score)
{
    doorlock_face_store_t store;
    doorlock_face_template_t face_template;
    uint32_t best_score = UINT32_MAX;
    uint8_t best_slot = 0;
    esp_err_t ret;

    ret = doorlock_face_store_load(&store);
    if (ret != ESP_OK) {
        return ret;
    }
    if (store.count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    ret = doorlock_face_capture_template(&face_template);
    if (ret != ESP_OK) {
        return ret;
    }

    for (uint8_t i = 0; i < store.count; i++) {
        uint32_t current_score = doorlock_face_score(&face_template, &store.templates[i]);
        if (current_score < best_score) {
            best_score = current_score;
            best_slot = i;
        }
    }

    if (score != NULL) {
        *score = best_score;
    }
    if (matched_slot != NULL) {
        *matched_slot = best_slot;
    }

    return best_score <= DOORLOCK_FACE_MATCH_THRESHOLD ? ESP_OK : ESP_FAIL;
}

esp_err_t doorlock_face_clear(void)
{
    doorlock_face_store_t store;

    doorlock_face_store_init_empty(&store);
    return doorlock_face_store_save(&store);
}
