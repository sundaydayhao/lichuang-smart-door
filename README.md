# Lichuang Smart Door

基于 ESP-IDF 的 ESP32-S3 触摸屏应用工程。当前工程名为 `handheld`，面向立创实战派 ESP32-S3 开发板一类的硬件组合，集成 LVGL 图形界面、摄像头预览、姿态监测、音乐播放、TF 卡浏览、WiFi 连接和 BLE HID 音量控制。

## 功能概览

- 开机界面：显示立创 LOGO 动画，并播放内置 `sword.pcm` 开机音效。
- 主界面：320x240 触摸屏应用入口，包含 6 个应用图标。
- 运动监测：读取 QMI8658 姿态传感器，显示 X/Y/Z 倾角和运动状态。
- 音乐播放器：从 SPIFFS 的 `/spiffs` 目录扫描音频文件，支持播放、暂停、上一首、下一首和音量调节。
- TF 卡浏览：挂载 SDMMC 1-bit TF 卡，按文件类型显示文件列表，并支持进入/返回目录。
- 摄像头：初始化 DVP 摄像头，以 RGB565/QVGA 格式在 LCD 上实时显示。
- 门锁控制：支持本地人脸模板录入/刷脸解锁、NFC 卡片录入/刷卡解锁，解锁后 8 秒自动上锁。
- WiFi 设置：扫描附近 WLAN，输入密码连接，连接成功后通过 SNTP 同步时间。
- 蓝牙控制器：作为 BLE HID 设备广播，提供音量加/减控制。

## 硬件与引脚

主要硬件配置集中在 `main/esp32_s3_szp.h`：

- 芯片目标：ESP32-S3
- Flash：16 MB
- PSRAM：Octal PSRAM，80 MHz
- LCD：ST7789，320x240，SPI
- 触摸：FT5x06，I2C
- 姿态传感器：QMI8658，I2C 地址 `0x6A`
- IO 扩展：PCA9557，I2C 地址 `0x19`
- 音频 Codec：ES8311 输出、ES7210 输入，I2S
- 摄像头：DVP，RGB565/QVGA
- TF 卡：SDMMC 1-bit

关键引脚：

| 模块 | 引脚 |
| --- | --- |
| I2C | SDA: GPIO1, SCL: GPIO2 |
| LCD SPI | MOSI: GPIO40, CLK: GPIO41, DC: GPIO39, BL: GPIO42 |
| Audio I2S | MCLK: GPIO38, BCLK: GPIO14, LRCK: GPIO13, DOUT: GPIO45, DIN: GPIO12 |
| TF Card | CMD: GPIO48, CLK: GPIO47, D0: GPIO21 |
| Camera | XCLK: GPIO5, PCLK: GPIO7, VSYNC: GPIO3, HREF: GPIO46, D0-D7: GPIO16/18/8/17/15/6/4/9 |

门锁电控输出默认不占用 GPIO：`main/esp32_s3_szp.h` 中 `DOORLOCK_CTRL_GPIO` 默认为 `GPIO_NUM_NC`。如需接继电器或电控锁，请改成实际控制引脚，并通过 `DOORLOCK_UNLOCK_LEVEL` 配置开锁有效电平。

## 软件依赖

工程使用 ESP-IDF Component Manager 管理依赖，清单在 `main/idf_component.yml`，锁定版本在 `dependencies.lock`。

当前锁定的主要版本：

- ESP-IDF: `5.5.3`
- `espressif/esp32-camera`: `2.1.4`
- `lvgl/lvgl`: `8.3.11`
- `espressif/esp_lvgl_port`: `1.4.0`
- `espressif/esp_lcd_touch_ft5x06`: `1.0.7`
- `chmorgan/esp-audio-player`: `1.0.7`
- `chmorgan/esp-file-iterator`: `1.0.0`
- `espressif/esp_codec_dev`: `1.3.6`
- `espressif/esp-sr`: `1.6.1`

`managed_components/` 是 ESP-IDF 自动下载生成的依赖目录，不提交到 Git。

## 分区与资源

分区表在 `partitions.csv`：

| 分区 | 类型 | 大小 | 用途 |
| --- | --- | --- | --- |
| `nvs` | data/nvs | 24 KB | NVS 参数存储 |
| `phy_init` | data/phy | 4 KB | PHY 初始化数据 |
| `factory` | app/factory | 8 MB | 应用固件 |
| `storage` | data/spiffs | 3 MB | SPIFFS 资源 |

`spiffs/` 会被打包进 `storage` 分区，当前包含：

- `little_changes.mp3`
- `the_beginning.mp3`
- `they_say.mp3`

`main/sword.pcm` 通过 `EMBED_FILES` 编译进固件，用作开机音效。

## 目录结构

```text
.
├── CMakeLists.txt
├── dependencies.lock
├── main
│   ├── main.c              # app_main 入口
│   ├── app_ui.c/.h         # LVGL 界面与应用逻辑
│   ├── esp32_s3_szp.c/.h   # 板级外设初始化与驱动封装
│   ├── bt/                 # BLE HID 相关实现
│   ├── assets/             # LVGL 图片和字体资源
│   ├── idf_component.yml   # 组件依赖声明
│   └── sword.pcm           # 开机音效
├── partitions.csv
├── sdkconfig
├── sdkconfig.defaults
└── spiffs                  # SPIFFS 文件系统资源
```

## 构建与烧录

先进入 ESP-IDF 环境：

```bash
. $IDF_PATH/export.sh
```

首次构建建议确认目标芯片：

```bash
idf.py set-target esp32s3
```

构建：

```bash
idf.py build
```

烧录并打开串口监视器：

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

如果串口不是 `/dev/ttyUSB0`，请替换为实际设备，例如 `/dev/ttyACM0`。

由于 `main/CMakeLists.txt` 使用了：

```cmake
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
```

执行 `idf.py flash` 时会同时生成并烧录 SPIFFS 镜像。

## 常用命令

```bash
# 清理构建产物
idf.py clean

# 完全清理后重新构建
idf.py fullclean
idf.py build

# 仅打开串口监视器
idf.py -p /dev/ttyUSB0 monitor

# 退出 monitor
Ctrl+]
```
