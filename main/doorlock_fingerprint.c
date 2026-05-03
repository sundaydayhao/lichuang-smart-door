#include "doorlock_fingerprint.h"

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define AS608_UART_NUM                 UART_NUM_1
#define AS608_UART_DEFAULT_BAUDRATE    57600
#define AS608_UART_BUF_SIZE            256
#define AS608_DEFAULT_ADDRESS          0xffffffff
#define AS608_PACKET_HEADER            0xef01
#define AS608_PACKET_COMMAND           0x01
#define AS608_PACKET_ACK               0x07
#define AS608_MAX_PAYLOAD              64

#define AS608_CMD_GET_IMAGE            0x01
#define AS608_CMD_IMAGE_2_TZ           0x02
#define AS608_CMD_SEARCH               0x04
#define AS608_CMD_REG_MODEL            0x05
#define AS608_CMD_STORE                0x06
#define AS608_CMD_EMPTY                0x0d
#define AS608_CMD_VERIFY_PASSWORD      0x13
#define AS608_CMD_READ_SYS_PARAM       0x0f
#define AS608_CMD_TEMPLATE_COUNT       0x1d
#define AS608_CMD_READ_INDEX_TABLE     0x1f

#define AS608_CODE_OK                  0x00
#define AS608_CODE_NO_FINGER           0x02
#define AS608_CODE_NOT_FOUND           0x09

#define AS608_PROBE_TIMEOUT_MS         400
#define AS608_COMMAND_TIMEOUT_MS       1200
#define AS608_ENROLL_WAIT_MS           12000
#define AS608_VERIFY_WAIT_MS           5000
#define AS608_REMOVE_WAIT_MS           5000
#define AS608_TX_DIAG_PULSE_COUNT      50
#define AS608_IO10_GPIO                GPIO_NUM_10
#define AS608_IO11_GPIO                GPIO_NUM_11

static const char *TAG = "AS608";

static const uint8_t fpm10a_pack_head[6] = {0xef, 0x01, 0xff, 0xff, 0xff, 0xff};
static const uint8_t fpm10a_get_device[10] = {0x01, 0x00, 0x07, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1b};

static SemaphoreHandle_t s_as608_mutex;
static bool s_uart_driver_installed;
static bool s_as608_online;
static doorlock_fingerprint_wiring_t s_wiring = DOORLOCK_FINGERPRINT_WIRING_UNKNOWN;
static uint32_t s_baudrate = AS608_UART_DEFAULT_BAUDRATE;
static uint32_t s_address = AS608_DEFAULT_ADDRESS;
static uint16_t s_template_count;
static uint16_t s_capacity = DOORLOCK_FINGERPRINT_DEFAULT_CAPACITY;
static uint8_t s_last_code = 0xff;

static esp_err_t as608_write_packet(uint8_t packet_id, const uint8_t *payload, size_t payload_len);
static esp_err_t as608_code_to_err(uint8_t code);

static const char *as608_gpio_label(gpio_num_t gpio)
{
    if (gpio == AS608_IO10_GPIO) {
        return "IO10";
    }
    if (gpio == AS608_IO11_GPIO) {
        return "IO11";
    }
    return "GPIO?";
}

static esp_err_t as608_ensure_mutex(void)
{
    if (s_as608_mutex == NULL) {
        s_as608_mutex = xSemaphoreCreateMutex();
        if (s_as608_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static esp_err_t as608_uart_config(gpio_num_t rx_gpio, gpio_num_t tx_gpio, uint32_t baudrate)
{
    uart_config_t uart_config = {
        .baud_rate = (int)baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret;

    if (!s_uart_driver_installed) {
        ret = uart_driver_install(AS608_UART_NUM, AS608_UART_BUF_SIZE, AS608_UART_BUF_SIZE, 0, NULL, 0);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            return ret;
        }
        s_uart_driver_installed = true;
    }

    ret = uart_param_config(AS608_UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = uart_set_pin(AS608_UART_NUM, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        return ret;
    }

    uart_flush_input(AS608_UART_NUM);
    s_baudrate = baudrate;
    return ESP_OK;
}

static void as608_uart_release_pins(void)
{
    if (s_uart_driver_installed) {
        (void)uart_wait_tx_done(AS608_UART_NUM, pdMS_TO_TICKS(50));
        (void)uart_driver_delete(AS608_UART_NUM);
        s_uart_driver_installed = false;
    }

    gpio_reset_pin(AS608_IO10_GPIO);
    gpio_set_direction(AS608_IO10_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(AS608_IO10_GPIO, GPIO_FLOATING);

    gpio_reset_pin(AS608_IO11_GPIO);
    gpio_set_direction(AS608_IO11_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(AS608_IO11_GPIO, GPIO_FLOATING);
}

static int8_t as608_sample_level(gpio_num_t gpio, gpio_pull_mode_t pull_mode)
{
    uint8_t high_count = 0;
    uint8_t low_count = 0;

    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(gpio, pull_mode);
    vTaskDelay(pdMS_TO_TICKS(5));

    for (uint8_t i = 0; i < 16; i++) {
        if (gpio_get_level(gpio)) {
            high_count++;
        } else {
            low_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return high_count >= low_count ? 1 : 0;
}

static void as608_sample_pin_levels(gpio_num_t gpio, int8_t *idle_level,
                                    int8_t *pull_down_level, int8_t *pull_up_level)
{
    if (idle_level != NULL) {
        *idle_level = as608_sample_level(gpio, GPIO_FLOATING);
    }
    if (pull_down_level != NULL) {
        *pull_down_level = as608_sample_level(gpio, GPIO_PULLDOWN_ONLY);
    }
    if (pull_up_level != NULL) {
        *pull_up_level = as608_sample_level(gpio, GPIO_PULLUP_ONLY);
    }
}

static void as608_log_raw_response(const char *label, uint32_t baudrate)
{
    uint8_t raw[48] = {0};
    size_t total = 0;
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(160);

    uart_flush_input(AS608_UART_NUM);
    if (uart_write_bytes(AS608_UART_NUM, fpm10a_pack_head, sizeof(fpm10a_pack_head)) != sizeof(fpm10a_pack_head) ||
        uart_write_bytes(AS608_UART_NUM, fpm10a_get_device, sizeof(fpm10a_get_device)) != sizeof(fpm10a_get_device) ||
        uart_wait_tx_done(AS608_UART_NUM, pdMS_TO_TICKS(200)) != ESP_OK) {
        return;
    }

    while ((xTaskGetTickCount() - start_tick) <= timeout_ticks && total < sizeof(raw)) {
        int read_len = uart_read_bytes(AS608_UART_NUM, &raw[total], sizeof(raw) - total, pdMS_TO_TICKS(20));
        if (read_len > 0) {
            total += read_len;
        }
    }

    if (total > 0) {
        char hex[sizeof(raw) * 3 + 1] = {0};
        size_t pos = 0;

        for (size_t i = 0; i < total && pos + 3 < sizeof(hex); i++) {
            int written = snprintf(&hex[pos], sizeof(hex) - pos, "%02X%s", raw[i], i + 1 == total ? "" : " ");
            if (written <= 0) {
                break;
            }
            pos += written;
        }
        ESP_LOGW(TAG, "AS608 raw UART bytes on %s, baud=%lu, addr=0x%08lX: %s",
                 label, (unsigned long)baudrate, (unsigned long)s_address, hex);
    }
}

static esp_err_t fpm10a_example_check_locked(uint8_t *raw_response, size_t raw_response_size,
                                             size_t *raw_response_len)
{
    uint8_t response[12] = {0};
    size_t total = 0;
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(AS608_PROBE_TIMEOUT_MS);

    uart_flush_input(AS608_UART_NUM);
    if (uart_write_bytes(AS608_UART_NUM, fpm10a_pack_head, sizeof(fpm10a_pack_head)) != sizeof(fpm10a_pack_head) ||
        uart_write_bytes(AS608_UART_NUM, fpm10a_get_device, sizeof(fpm10a_get_device)) != sizeof(fpm10a_get_device)) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(uart_wait_tx_done(AS608_UART_NUM, pdMS_TO_TICKS(200)), TAG, "FPM10A check tx");

    while ((xTaskGetTickCount() - start_tick) <= timeout_ticks && total < sizeof(response)) {
        int read_len = uart_read_bytes(AS608_UART_NUM, &response[total], sizeof(response) - total, pdMS_TO_TICKS(20));
        if (read_len > 0) {
            total += read_len;
        }
    }

    if (raw_response_len != NULL) {
        *raw_response_len = total;
    }
    if (raw_response != NULL && raw_response_size > 0) {
        size_t copy_len = total < raw_response_size ? total : raw_response_size;
        memcpy(raw_response, response, copy_len);
    }

    if (total == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (total < sizeof(response)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(response, fpm10a_pack_head, sizeof(fpm10a_pack_head)) != 0 || response[6] != AS608_PACKET_ACK) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return as608_code_to_err(response[9]);
}

static void as608_log_example_response(const char *label, uint32_t baudrate,
                                       const uint8_t *raw_response, size_t raw_response_len)
{
    if (raw_response_len == 0) {
        return;
    }

    char hex[12 * 3 + 1] = {0};
    size_t pos = 0;

    for (size_t i = 0; i < raw_response_len && pos + 3 < sizeof(hex); i++) {
        int written = snprintf(&hex[pos], sizeof(hex) - pos, "%02X%s",
                               raw_response[i], i + 1 == raw_response_len ? "" : " ");
        if (written <= 0) {
            break;
        }
        pos += written;
    }
    ESP_LOGW(TAG, "FPM10A example response on %s, baud=%lu: %s",
             label, (unsigned long)baudrate, hex);
}

static esp_err_t as608_read_byte(uint8_t *byte, TickType_t start_tick, TickType_t timeout_ticks)
{
    while ((xTaskGetTickCount() - start_tick) <= timeout_ticks) {
        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        TickType_t wait_ticks = elapsed < timeout_ticks ? timeout_ticks - elapsed : 1;

        if (uart_read_bytes(AS608_UART_NUM, byte, 1, wait_ticks) == 1) {
            return ESP_OK;
        }
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t as608_read_bytes(uint8_t *data, size_t len, TickType_t start_tick, TickType_t timeout_ticks)
{
    for (size_t i = 0; i < len; i++) {
        esp_err_t ret = as608_read_byte(&data[i], start_tick, timeout_ticks);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

static esp_err_t as608_write_packet(uint8_t packet_id, const uint8_t *payload, size_t payload_len)
{
    uint8_t frame[11 + AS608_MAX_PAYLOAD];
    uint16_t length = payload_len + 2;
    uint16_t checksum = packet_id + (length >> 8) + (length & 0xff);
    size_t idx = 0;

    if (payload_len > AS608_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    frame[idx++] = (AS608_PACKET_HEADER >> 8) & 0xff;
    frame[idx++] = AS608_PACKET_HEADER & 0xff;
    frame[idx++] = (s_address >> 24) & 0xff;
    frame[idx++] = (s_address >> 16) & 0xff;
    frame[idx++] = (s_address >> 8) & 0xff;
    frame[idx++] = s_address & 0xff;
    frame[idx++] = packet_id;
    frame[idx++] = (length >> 8) & 0xff;
    frame[idx++] = length & 0xff;

    for (size_t i = 0; i < payload_len; i++) {
        frame[idx++] = payload[i];
        checksum += payload[i];
    }

    frame[idx++] = (checksum >> 8) & 0xff;
    frame[idx++] = checksum & 0xff;

    int written = uart_write_bytes(AS608_UART_NUM, frame, idx);
    if (written != (int)idx) {
        return ESP_FAIL;
    }

    return uart_wait_tx_done(AS608_UART_NUM, pdMS_TO_TICKS(200));
}

static esp_err_t as608_read_packet(uint8_t expected_packet_id, uint8_t *payload, size_t payload_size,
                                   size_t *payload_len, uint32_t timeout_ms)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    uint8_t previous = 0;
    uint8_t current = 0;
    uint8_t meta[7] = {0};
    uint8_t packet_data[AS608_MAX_PAYLOAD + 2] = {0};
    uint16_t length;
    uint16_t checksum;
    uint16_t received_checksum;
    uint8_t packet_id;

    if (timeout_ticks == 0) {
        timeout_ticks = 1;
    }

    while ((xTaskGetTickCount() - start_tick) <= timeout_ticks) {
        esp_err_t ret = as608_read_byte(&current, start_tick, timeout_ticks);
        if (ret != ESP_OK) {
            return ret;
        }
        if (previous == ((AS608_PACKET_HEADER >> 8) & 0xff) && current == (AS608_PACKET_HEADER & 0xff)) {
            break;
        }
        previous = current;
    }

    ESP_RETURN_ON_ERROR(as608_read_bytes(meta, sizeof(meta), start_tick, timeout_ticks), TAG, "read packet meta");

    packet_id = meta[4];
    length = ((uint16_t)meta[5] << 8) | meta[6];
    if (length < 2 || length > sizeof(packet_data)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (expected_packet_id != 0 && packet_id != expected_packet_id) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_RETURN_ON_ERROR(as608_read_bytes(packet_data, length, start_tick, timeout_ticks), TAG, "read packet payload");

    checksum = packet_id + meta[5] + meta[6];
    for (uint16_t i = 0; i < length - 2; i++) {
        checksum += packet_data[i];
    }

    received_checksum = ((uint16_t)packet_data[length - 2] << 8) | packet_data[length - 1];
    if (checksum != received_checksum) {
        return ESP_ERR_INVALID_CRC;
    }

    if (payload != NULL && payload_size > 0) {
        size_t copy_len = length - 2;
        if (copy_len > payload_size) {
            copy_len = payload_size;
        }
        memcpy(payload, packet_data, copy_len);
    }
    if (payload_len != NULL) {
        *payload_len = length - 2;
    }

    return ESP_OK;
}

static esp_err_t as608_code_to_err(uint8_t code)
{
    s_last_code = code;

    switch (code) {
    case AS608_CODE_OK:
        return ESP_OK;
    case AS608_CODE_NO_FINGER:
        return ESP_ERR_NOT_FOUND;
    case AS608_CODE_NOT_FOUND:
        return ESP_ERR_NOT_FOUND;
    case 0x0b:
        return ESP_ERR_INVALID_ARG;
    case 0x03:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x0a:
    case 0x15:
        return ESP_ERR_INVALID_RESPONSE;
    case 0x13:
        return ESP_ERR_INVALID_STATE;
    default:
        return ESP_FAIL;
    }
}

static esp_err_t as608_command(const uint8_t *command, size_t command_len, uint8_t *response,
                               size_t response_size, size_t *response_len, uint32_t timeout_ms)
{
    esp_err_t ret;
    size_t len = 0;

    uart_flush_input(AS608_UART_NUM);
    ret = as608_write_packet(AS608_PACKET_COMMAND, command, command_len);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = as608_read_packet(AS608_PACKET_ACK, response, response_size, &len, timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    if (response_len != NULL) {
        *response_len = len;
    }
    if (response != NULL && len > 0) {
        s_last_code = response[0];
    }

    return ESP_OK;
}

static esp_err_t as608_expect_ok(const uint8_t *response, size_t response_len)
{
    if (response_len < 1) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return as608_code_to_err(response[0]);
}

static esp_err_t as608_read_sys_param_locked(void)
{
    const uint8_t command[] = {AS608_CMD_READ_SYS_PARAM};
    uint8_t response[24] = {0};
    size_t response_len = 0;
    esp_err_t ret = as608_command(command, sizeof(command), response, sizeof(response), &response_len,
                                  AS608_COMMAND_TIMEOUT_MS);

    if (ret != ESP_OK) {
        return ret;
    }
    ret = as608_expect_ok(response, response_len);
    if (ret != ESP_OK) {
        return ret;
    }
    if (response_len >= 7) {
        uint16_t capacity = ((uint16_t)response[5] << 8) | response[6];
        if (capacity > 0) {
            s_capacity = capacity;
        }
    }

    return ESP_OK;
}

static esp_err_t as608_get_template_count_locked(uint16_t *count)
{
    const uint8_t command[] = {AS608_CMD_TEMPLATE_COUNT};
    uint8_t response[8] = {0};
    size_t response_len = 0;
    esp_err_t ret = as608_command(command, sizeof(command), response, sizeof(response), &response_len,
                                  AS608_COMMAND_TIMEOUT_MS);

    if (ret != ESP_OK) {
        return ret;
    }
    ret = as608_expect_ok(response, response_len);
    if (ret != ESP_OK) {
        return ret;
    }
    if (response_len < 3) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_template_count = ((uint16_t)response[1] << 8) | response[2];
    if (count != NULL) {
        *count = s_template_count;
    }

    return ESP_OK;
}

static esp_err_t as608_get_image_code_locked(uint8_t *code)
{
    const uint8_t command[] = {AS608_CMD_GET_IMAGE};
    uint8_t response[4] = {0};
    size_t response_len = 0;
    esp_err_t ret = as608_command(command, sizeof(command), response, sizeof(response), &response_len,
                                  AS608_COMMAND_TIMEOUT_MS);

    if (ret != ESP_OK) {
        return ret;
    }
    if (response_len < 1) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *code = response[0];
    s_last_code = response[0];
    return ESP_OK;
}

static esp_err_t as608_wait_image_locked(uint32_t timeout_ms)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    if (timeout_ticks == 0) {
        timeout_ticks = 1;
    }

    while ((xTaskGetTickCount() - start_tick) <= timeout_ticks) {
        uint8_t code = 0;
        esp_err_t ret = as608_get_image_code_locked(&code);
        if (ret != ESP_OK) {
            return ret;
        }
        if (code == AS608_CODE_OK) {
            return ESP_OK;
        }
        if (code != AS608_CODE_NO_FINGER) {
            return as608_code_to_err(code);
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    s_last_code = AS608_CODE_NO_FINGER;
    return ESP_ERR_TIMEOUT;
}

static esp_err_t as608_wait_no_finger_locked(uint32_t timeout_ms)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    if (timeout_ticks == 0) {
        timeout_ticks = 1;
    }

    while ((xTaskGetTickCount() - start_tick) <= timeout_ticks) {
        uint8_t code = 0;
        esp_err_t ret = as608_get_image_code_locked(&code);
        if (ret != ESP_OK) {
            return ret;
        }
        if (code == AS608_CODE_NO_FINGER) {
            return ESP_OK;
        }
        if (code != AS608_CODE_OK) {
            return as608_code_to_err(code);
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t as608_image_2_tz_locked(uint8_t buffer_id)
{
    const uint8_t command[] = {AS608_CMD_IMAGE_2_TZ, buffer_id};
    uint8_t response[4] = {0};
    size_t response_len = 0;
    esp_err_t ret = as608_command(command, sizeof(command), response, sizeof(response), &response_len,
                                  AS608_COMMAND_TIMEOUT_MS);

    if (ret != ESP_OK) {
        return ret;
    }

    return as608_expect_ok(response, response_len);
}

static esp_err_t as608_reg_model_locked(void)
{
    const uint8_t command[] = {AS608_CMD_REG_MODEL};
    uint8_t response[4] = {0};
    size_t response_len = 0;
    esp_err_t ret = as608_command(command, sizeof(command), response, sizeof(response), &response_len,
                                  AS608_COMMAND_TIMEOUT_MS);

    if (ret != ESP_OK) {
        return ret;
    }

    return as608_expect_ok(response, response_len);
}

static esp_err_t as608_store_model_locked(uint8_t buffer_id, uint16_t page_id)
{
    const uint8_t command[] = {
        AS608_CMD_STORE,
        buffer_id,
        (uint8_t)(page_id >> 8),
        (uint8_t)(page_id & 0xff),
    };
    uint8_t response[4] = {0};
    size_t response_len = 0;
    esp_err_t ret = as608_command(command, sizeof(command), response, sizeof(response), &response_len,
                                  AS608_COMMAND_TIMEOUT_MS);

    if (ret != ESP_OK) {
        return ret;
    }

    return as608_expect_ok(response, response_len);
}

static esp_err_t as608_empty_locked(void)
{
    const uint8_t command[] = {AS608_CMD_EMPTY};
    uint8_t response[4] = {0};
    size_t response_len = 0;
    esp_err_t ret = as608_command(command, sizeof(command), response, sizeof(response), &response_len,
                                  AS608_COMMAND_TIMEOUT_MS);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = as608_expect_ok(response, response_len);
    if (ret == ESP_OK) {
        s_template_count = 0;
    }

    return ret;
}

static esp_err_t as608_search_locked(uint8_t buffer_id, uint16_t start_id, uint16_t count,
                                     uint16_t *matched_id, uint16_t *score)
{
    const uint8_t command[] = {
        AS608_CMD_SEARCH,
        buffer_id,
        (uint8_t)(start_id >> 8),
        (uint8_t)(start_id & 0xff),
        (uint8_t)(count >> 8),
        (uint8_t)(count & 0xff),
    };
    uint8_t response[8] = {0};
    size_t response_len = 0;
    esp_err_t ret = as608_command(command, sizeof(command), response, sizeof(response), &response_len,
                                  AS608_COMMAND_TIMEOUT_MS);

    if (ret != ESP_OK) {
        return ret;
    }
    ret = as608_expect_ok(response, response_len);
    if (ret != ESP_OK) {
        return ret;
    }
    if (response_len < 5) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (matched_id != NULL) {
        *matched_id = ((uint16_t)response[1] << 8) | response[2];
    }
    if (score != NULL) {
        *score = ((uint16_t)response[3] << 8) | response[4];
    }

    return ESP_OK;
}

static esp_err_t as608_find_free_slot_locked(uint16_t *slot)
{
    uint16_t table_count = (s_capacity + 255) / 256;

    if (slot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint16_t table = 0; table < table_count; table++) {
        const uint8_t command[] = {AS608_CMD_READ_INDEX_TABLE, (uint8_t)table};
        uint8_t response[40] = {0};
        size_t response_len = 0;
        esp_err_t ret = as608_command(command, sizeof(command), response, sizeof(response), &response_len,
                                      AS608_COMMAND_TIMEOUT_MS);

        if (ret != ESP_OK || as608_expect_ok(response, response_len) != ESP_OK || response_len < 33) {
            break;
        }

        for (uint8_t byte_idx = 0; byte_idx < 32; byte_idx++) {
            for (uint8_t bit = 0; bit < 8; bit++) {
                uint16_t id = table * 256 + byte_idx * 8 + bit;
                if (id >= s_capacity) {
                    return ESP_ERR_NO_MEM;
                }
                if ((response[1 + byte_idx] & BIT(bit)) == 0) {
                    *slot = id;
                    return ESP_OK;
                }
            }
        }
    }

    if (s_template_count >= s_capacity) {
        return ESP_ERR_NO_MEM;
    }

    uint16_t fallback = s_template_count + 1;
    if (fallback >= s_capacity) {
        fallback = s_template_count;
    }
    *slot = fallback;
    return ESP_OK;
}

static esp_err_t as608_probe_locked(doorlock_fingerprint_status_t *status)
{
    typedef struct {
        doorlock_fingerprint_wiring_t wiring;
        gpio_num_t rx_gpio;
        gpio_num_t tx_gpio;
        const char *label;
    } probe_pin_t;

    const probe_pin_t probes[] = {
        {
            .wiring = DOORLOCK_FINGERPRINT_WIRING_NORMAL,
            .rx_gpio = AS608_IO10_GPIO,
            .tx_gpio = AS608_IO11_GPIO,
            .label = "normal: module TX->IO10, module RX->IO11",
        },
        {
            .wiring = DOORLOCK_FINGERPRINT_WIRING_SWAPPED,
            .rx_gpio = AS608_IO11_GPIO,
            .tx_gpio = AS608_IO10_GPIO,
            .label = "swapped: module TX->IO11, module RX->IO10",
        },
    };
    const uint32_t baudrates[] = {57600, 9600, 115200, 38400, 19200};
    int8_t normal_rx_idle_level = -1;
    int8_t normal_rx_pull_down_level = -1;
    int8_t normal_rx_pull_up_level = -1;
    int8_t swapped_rx_idle_level = -1;
    int8_t swapped_rx_pull_down_level = -1;
    int8_t swapped_rx_pull_up_level = -1;
    esp_err_t last_ret = ESP_ERR_TIMEOUT;

    as608_uart_release_pins();

    as608_sample_pin_levels(AS608_IO10_GPIO, &normal_rx_idle_level,
                            &normal_rx_pull_down_level, &normal_rx_pull_up_level);
    as608_sample_pin_levels(AS608_IO11_GPIO, &swapped_rx_idle_level,
                            &swapped_rx_pull_down_level, &swapped_rx_pull_up_level);

    bool normal_rx_looks_driven = normal_rx_pull_down_level == 1;
    bool swapped_rx_looks_driven = swapped_rx_pull_down_level == 1;

    if (normal_rx_looks_driven != swapped_rx_looks_driven) {
        ESP_LOGI(TAG, "AS608 TX candidate: %s (IO10=%d/%d/%d IO11=%d/%d/%d)",
                 normal_rx_looks_driven ? "IO10" : "IO11",
                 normal_rx_idle_level, normal_rx_pull_down_level, normal_rx_pull_up_level,
                 swapped_rx_idle_level, swapped_rx_pull_down_level, swapped_rx_pull_up_level);
    }

    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        int8_t rx_idle_level = -1;
        int8_t rx_pull_down_level = -1;
        int8_t rx_pull_up_level = -1;

        if (normal_rx_looks_driven != swapped_rx_looks_driven) {
            bool probe_uses_driven_rx =
                (probes[i].wiring == DOORLOCK_FINGERPRINT_WIRING_NORMAL && normal_rx_looks_driven) ||
                (probes[i].wiring == DOORLOCK_FINGERPRINT_WIRING_SWAPPED && swapped_rx_looks_driven);

            if (!probe_uses_driven_rx) {
                ESP_LOGW(TAG, "Skip %s to avoid driving the pin that looks like module TX", probes[i].label);
                continue;
            }
        }

        if (probes[i].wiring == DOORLOCK_FINGERPRINT_WIRING_NORMAL) {
            rx_idle_level = normal_rx_idle_level;
            rx_pull_down_level = normal_rx_pull_down_level;
            rx_pull_up_level = normal_rx_pull_up_level;
        } else {
            rx_idle_level = swapped_rx_idle_level;
            rx_pull_down_level = swapped_rx_pull_down_level;
            rx_pull_up_level = swapped_rx_pull_up_level;
        }

        for (size_t baud_idx = 0; baud_idx < sizeof(baudrates) / sizeof(baudrates[0]); baud_idx++) {
            uint32_t baudrate = baudrates[baud_idx];
            uint8_t example_response[12] = {0};
            size_t example_response_len = 0;
            esp_err_t ret = as608_uart_config(probes[i].rx_gpio, probes[i].tx_gpio, baudrate);
            if (ret != ESP_OK) {
                last_ret = ret;
                continue;
            }

            s_address = AS608_DEFAULT_ADDRESS;
            vTaskDelay(pdMS_TO_TICKS(30));
            ret = fpm10a_example_check_locked(example_response, sizeof(example_response), &example_response_len);
            if (ret == ESP_OK) {
                s_as608_online = true;
                s_wiring = probes[i].wiring;
                s_baudrate = baudrate;
                s_address = AS608_DEFAULT_ADDRESS;
                (void)as608_read_sys_param_locked();
                (void)as608_get_template_count_locked(&s_template_count);

                ESP_LOGI(TAG, "FPM10A example check ok (%s, baud=%lu, rx=%d/%d/%d), templates=%u/%u",
                         probes[i].label, (unsigned long)baudrate,
                         rx_idle_level, rx_pull_down_level, rx_pull_up_level, s_template_count, s_capacity);
                if (status != NULL) {
                    status->wiring = s_wiring;
                    status->baudrate = s_baudrate;
                    status->template_count = s_template_count;
                    status->capacity = s_capacity;
                    status->normal_rx_idle_level = normal_rx_idle_level;
                    status->normal_rx_pull_down_level = normal_rx_pull_down_level;
                    status->normal_rx_pull_up_level = normal_rx_pull_up_level;
                    status->swapped_rx_idle_level = swapped_rx_idle_level;
                    status->swapped_rx_pull_down_level = swapped_rx_pull_down_level;
                    status->swapped_rx_pull_up_level = swapped_rx_pull_up_level;
                    status->last_code = s_last_code;
                }
                return ESP_OK;
            }

            as608_log_example_response(probes[i].label, baudrate, example_response, example_response_len);
            if (example_response_len == 0) {
                as608_log_raw_response(probes[i].label, baudrate);
            }
            ESP_LOGW(TAG, "FPM10A example check failed on %s, baud=%lu, rx=%d/%d/%d: %s",
                     probes[i].label, (unsigned long)baudrate,
                     rx_idle_level, rx_pull_down_level, rx_pull_up_level, esp_err_to_name(ret));
            last_ret = ret;
        }
    }

    as608_uart_release_pins();
    s_as608_online = false;
    s_wiring = DOORLOCK_FINGERPRINT_WIRING_UNKNOWN;
    s_baudrate = AS608_UART_DEFAULT_BAUDRATE;
    s_address = AS608_DEFAULT_ADDRESS;
    if (status != NULL) {
        status->wiring = s_wiring;
        status->baudrate = s_baudrate;
        status->template_count = 0;
        status->capacity = s_capacity;
        status->normal_rx_idle_level = normal_rx_idle_level;
        status->normal_rx_pull_down_level = normal_rx_pull_down_level;
        status->normal_rx_pull_up_level = normal_rx_pull_up_level;
        status->swapped_rx_idle_level = swapped_rx_idle_level;
        status->swapped_rx_pull_down_level = swapped_rx_pull_down_level;
        status->swapped_rx_pull_up_level = swapped_rx_pull_up_level;
        status->last_code = s_last_code;
    }

    return last_ret;
}

static esp_err_t as608_prepare_locked(void)
{
    if (s_as608_online) {
        return ESP_OK;
    }

    return as608_probe_locked(NULL);
}

esp_err_t doorlock_fingerprint_probe(doorlock_fingerprint_status_t *status)
{
    esp_err_t ret = as608_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_as608_mutex, portMAX_DELAY);
    ret = as608_probe_locked(status);
    xSemaphoreGive(s_as608_mutex);

    return ret;
}

esp_err_t doorlock_fingerprint_tx_diagnostic(void)
{
    const uint32_t baudrates[] = {57600, 9600};
    int8_t io10_idle_level = -1;
    int8_t io10_pull_down_level = -1;
    int8_t io10_pull_up_level = -1;
    int8_t io11_idle_level = -1;
    int8_t io11_pull_down_level = -1;
    int8_t io11_pull_up_level = -1;
    esp_err_t ret = ESP_ERR_TIMEOUT;

    ret = as608_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_as608_mutex, portMAX_DELAY);

    as608_uart_release_pins();
    as608_sample_pin_levels(AS608_IO10_GPIO, &io10_idle_level,
                            &io10_pull_down_level, &io10_pull_up_level);
    as608_sample_pin_levels(AS608_IO11_GPIO, &io11_idle_level,
                            &io11_pull_down_level, &io11_pull_up_level);

    bool io10_looks_module_tx = io10_pull_down_level == 1;
    bool io11_looks_module_tx = io11_pull_down_level == 1;
    if (io10_looks_module_tx == io11_looks_module_tx) {
        ESP_LOGW(TAG, "AS608 TX diagnostic skipped: no single module TX candidate "
                 "(IO10=%d/%d/%d IO11=%d/%d/%d)",
                 io10_idle_level, io10_pull_down_level, io10_pull_up_level,
                 io11_idle_level, io11_pull_down_level, io11_pull_up_level);
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

    gpio_num_t module_tx_gpio = io10_looks_module_tx ? AS608_IO10_GPIO : AS608_IO11_GPIO;
    gpio_num_t esp_tx_gpio = io10_looks_module_tx ? AS608_IO11_GPIO : AS608_IO10_GPIO;
    gpio_num_t esp_rx_gpio = module_tx_gpio;

    ESP_LOGW(TAG, "AS608 TX diagnostic: %s looks like module TX; only pulsing %s as ESP TX. "
             "Measure %s or the AS608 RX pad now.",
             as608_gpio_label(module_tx_gpio), as608_gpio_label(esp_tx_gpio),
             as608_gpio_label(esp_tx_gpio));

    as608_uart_release_pins();
    gpio_reset_pin(esp_tx_gpio);
    gpio_set_direction(esp_tx_gpio, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(esp_tx_gpio, GPIO_FLOATING);
    gpio_set_level(esp_tx_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t low_readback_count = 0;
    uint8_t high_readback_count = 0;
    for (uint8_t i = 0; i < AS608_TX_DIAG_PULSE_COUNT; i++) {
        gpio_set_level(esp_tx_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(80));
        if (gpio_get_level(esp_tx_gpio) == 0) {
            low_readback_count++;
        }
        gpio_set_level(esp_tx_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(80));
        if (gpio_get_level(esp_tx_gpio) == 1) {
            high_readback_count++;
        }
    }

    ESP_LOGI(TAG, "AS608 TX diagnostic pulse done on %s; readback low/high=%u/%u of %u; "
             "starting UART frame burst on %s->module RX, RX=%s",
             as608_gpio_label(esp_tx_gpio), low_readback_count, high_readback_count,
             AS608_TX_DIAG_PULSE_COUNT,
             as608_gpio_label(esp_tx_gpio), as608_gpio_label(esp_rx_gpio));

    for (size_t baud_idx = 0; baud_idx < sizeof(baudrates) / sizeof(baudrates[0]); baud_idx++) {
        uint32_t baudrate = baudrates[baud_idx];

        ret = as608_uart_config(esp_rx_gpio, esp_tx_gpio, baudrate);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "AS608 TX diagnostic UART config failed: TX=%s RX=%s baud=%lu: %s",
                     as608_gpio_label(esp_tx_gpio), as608_gpio_label(esp_rx_gpio),
                     (unsigned long)baudrate, esp_err_to_name(ret));
            continue;
        }

        ESP_LOGI(TAG, "AS608 TX diagnostic: sending FPM10A check frame 3 times, TX=%s RX=%s baud=%lu",
                 as608_gpio_label(esp_tx_gpio), as608_gpio_label(esp_rx_gpio),
                 (unsigned long)baudrate);

        for (uint8_t repeat = 0; repeat < 3; repeat++) {
            uint8_t response[12] = {0};
            size_t response_len = 0;
            esp_err_t check_ret = fpm10a_example_check_locked(response, sizeof(response), &response_len);

            as608_log_example_response("TX diagnostic", baudrate, response, response_len);
            if (check_ret == ESP_OK) {
                ESP_LOGI(TAG, "AS608 TX diagnostic got ACK: TX=%s RX=%s baud=%lu",
                         as608_gpio_label(esp_tx_gpio), as608_gpio_label(esp_rx_gpio),
                         (unsigned long)baudrate);
                ret = ESP_OK;
                goto done;
            }

            ESP_LOGW(TAG, "AS608 TX diagnostic frame %u failed: TX=%s RX=%s baud=%lu len=%u: %s",
                     repeat + 1, as608_gpio_label(esp_tx_gpio), as608_gpio_label(esp_rx_gpio),
                     (unsigned long)baudrate, (unsigned)response_len, esp_err_to_name(check_ret));
            ret = check_ret;
            vTaskDelay(pdMS_TO_TICKS(80));
        }
    }

done:
    as608_uart_release_pins();
    xSemaphoreGive(s_as608_mutex);
    return ret;
}

esp_err_t doorlock_fingerprint_get_count(uint16_t *count, uint16_t *capacity)
{
    esp_err_t ret = as608_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_as608_mutex, portMAX_DELAY);
    ret = as608_prepare_locked();
    if (ret == ESP_OK) {
        (void)as608_read_sys_param_locked();
        ret = as608_get_template_count_locked(count);
    }
    if (ret != ESP_OK && ret == ESP_ERR_TIMEOUT) {
        s_as608_online = false;
    }
    if (capacity != NULL) {
        *capacity = s_capacity;
    }
    xSemaphoreGive(s_as608_mutex);

    return ret;
}

esp_err_t doorlock_fingerprint_enroll(uint16_t *slot)
{
    uint16_t target_slot = 0;
    esp_err_t ret = as608_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_as608_mutex, portMAX_DELAY);
    ret = as608_prepare_locked();
    if (ret != ESP_OK) {
        goto done;
    }

    (void)as608_read_sys_param_locked();
    ret = as608_get_template_count_locked(&s_template_count);
    if (ret != ESP_OK) {
        goto done;
    }
    if (s_template_count >= s_capacity) {
        ret = ESP_ERR_NO_MEM;
        goto done;
    }

    ret = as608_find_free_slot_locked(&target_slot);
    if (ret != ESP_OK) {
        goto done;
    }

    ret = as608_wait_image_locked(AS608_ENROLL_WAIT_MS);
    if (ret != ESP_OK) {
        goto done;
    }
    ret = as608_image_2_tz_locked(1);
    if (ret != ESP_OK) {
        goto done;
    }
    ret = as608_wait_no_finger_locked(AS608_REMOVE_WAIT_MS);
    if (ret != ESP_OK) {
        goto done;
    }
    ret = as608_wait_image_locked(AS608_ENROLL_WAIT_MS);
    if (ret != ESP_OK) {
        goto done;
    }
    ret = as608_image_2_tz_locked(2);
    if (ret != ESP_OK) {
        goto done;
    }
    ret = as608_reg_model_locked();
    if (ret != ESP_OK) {
        goto done;
    }
    ret = as608_store_model_locked(1, target_slot);
    if (ret != ESP_OK) {
        goto done;
    }

    (void)as608_get_template_count_locked(&s_template_count);
    if (slot != NULL) {
        *slot = target_slot;
    }

done:
    if (ret == ESP_ERR_TIMEOUT) {
        s_as608_online = false;
    }
    xSemaphoreGive(s_as608_mutex);
    return ret;
}

esp_err_t doorlock_fingerprint_verify(uint16_t *matched_slot, uint16_t *score)
{
    esp_err_t ret = as608_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_as608_mutex, portMAX_DELAY);
    ret = as608_prepare_locked();
    if (ret != ESP_OK) {
        goto done;
    }

    (void)as608_read_sys_param_locked();
    ret = as608_get_template_count_locked(&s_template_count);
    if (ret != ESP_OK) {
        goto done;
    }
    if (s_template_count == 0) {
        ret = ESP_ERR_NOT_FOUND;
        goto done;
    }

    ret = as608_wait_image_locked(AS608_VERIFY_WAIT_MS);
    if (ret != ESP_OK) {
        goto done;
    }
    ret = as608_image_2_tz_locked(1);
    if (ret != ESP_OK) {
        goto done;
    }
    ret = as608_search_locked(1, 0, s_capacity, matched_slot, score);

done:
    if (ret == ESP_ERR_TIMEOUT) {
        s_as608_online = false;
    }
    xSemaphoreGive(s_as608_mutex);
    return ret;
}

esp_err_t doorlock_fingerprint_clear(void)
{
    esp_err_t ret = as608_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_as608_mutex, portMAX_DELAY);
    ret = as608_prepare_locked();
    if (ret == ESP_OK) {
        ret = as608_empty_locked();
    }
    if (ret == ESP_ERR_TIMEOUT) {
        s_as608_online = false;
    }
    xSemaphoreGive(s_as608_mutex);

    return ret;
}

uint8_t doorlock_fingerprint_get_last_code(void)
{
    return s_last_code;
}
