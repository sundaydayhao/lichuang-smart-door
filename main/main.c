#include <stdio.h>
#include "esp32_s3_szp.h"
#include "app_ui.h"
#include "doorlock_fingerprint.h"
#include "nvs_flash.h"
#include <esp_system.h>


EventGroupHandle_t my_event_group;

static const char *TAG = "MAIN";

static void fingerprint_probe_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(800));

    doorlock_fingerprint_status_t status = {0};
    esp_err_t ret = doorlock_fingerprint_probe(&status);

    if (ret == ESP_OK) {
        if (status.wiring == DOORLOCK_FINGERPRINT_WIRING_SWAPPED) {
            ESP_LOGW(TAG, "AS608 OK but RX/TX look swapped. Software now uses RX=IO11, TX=IO10, baud=%lu, templates=%u/%u",
                     (unsigned long)status.baudrate, status.template_count, status.capacity);
        } else {
            ESP_LOGI(TAG, "AS608 OK on RX=IO10, TX=IO11, baud=%lu, templates=%u/%u",
                     (unsigned long)status.baudrate, status.template_count, status.capacity);
        }
    } else {
        ESP_LOGW(TAG, "AS608 probe failed on IO10/IO11: %s. RX levels float/pulldown/pullup: IO10=%d/%d/%d IO11=%d/%d/%d. Check power, GND, baudrate, and RX/TX wiring",
                 esp_err_to_name(ret),
                 status.normal_rx_idle_level, status.normal_rx_pull_down_level, status.normal_rx_pull_up_level,
                 status.swapped_rx_idle_level, status.swapped_rx_pull_down_level, status.swapped_rx_pull_up_level);
        (void)doorlock_fingerprint_tx_diagnostic();
    }

    vTaskDelete(NULL);
}

// 打印内存使用情况
void displayMemoryUsage() {
    size_t totalDRAM = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t freeDRAM = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t usedDRAM = totalDRAM - freeDRAM;

    size_t totalPSRAM = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t freePSRAM = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t usedPSRAM = totalPSRAM - freePSRAM;

    size_t DRAM_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t PSRAM_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    float dramUsagePercentage = (float)usedDRAM / totalDRAM * 100;
    float psramUsagePercentage = (float)usedPSRAM / totalPSRAM * 100;

    ESP_LOGI(TAG, "DRAM Total: %zu bytes, Used: %zu bytes, Free: %zu bytes,  DRAM_Largest_block: %zu bytes", totalDRAM, usedDRAM, freeDRAM, DRAM_largest_block);
    ESP_LOGI(TAG, "DRAM Used: %.2f%%", dramUsagePercentage);
    ESP_LOGI(TAG, "PSRAM Total: %zu bytes, Used: %zu bytes, Free: %zu bytes, PSRAM_Largest_block: %zu bytes", totalPSRAM, usedPSRAM, freePSRAM, PSRAM_largest_block);
    ESP_LOGI(TAG, "PSRAM Used: %.2f%%", psramUsagePercentage);
} 

// 主界面 任务函数
static void main_page_task(void *pvParameters)
{
    // 等待开机音乐播放完成
    xEventGroupWaitBits(my_event_group, START_MUSIC_COMPLETED, pdFALSE, pdFALSE, portMAX_DELAY);
    // 进入主界面
    lv_main_page();

    vTaskDelete(NULL);
}

// 主函数
void app_main(void)
{
    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    bsp_i2c_init();  // I2C初始化
    pca9557_init();  // IO扩展芯片初始化
    doorlock_hw_init(); // 门锁控制输出初始化
    bsp_lvgl_start(); // 初始化液晶屏lvgl接口
    bsp_spiffs_mount(); // SPIFFS文件系统初始化
    bsp_codec_init(); // 音频初始化

    lv_gui_start(); // 显示开机界面

    my_event_group = xEventGroupCreate();

    xTaskCreatePinnedToCore(power_music_task, "power_music_task", 4*1024, NULL, 5, NULL, 1); // 播放开机音乐
    xTaskCreatePinnedToCore(main_page_task, "main_page_task", 4*1024, NULL, 5, NULL, 0); // 主界面任务
    xTaskCreatePinnedToCore(fingerprint_probe_task, "fingerprint_probe", 4*1024, NULL, 4, NULL, 1); // 检测AS608串口接线

    while (true) {
        displayMemoryUsage();
        vTaskDelay(pdMS_TO_TICKS(5 * 1000));
    }
}
