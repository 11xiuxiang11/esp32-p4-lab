/*
 * camera_demo.c —— SC2336 + MIPI-CSI 采集实验（ESP32-P4X）
 *
 * 编译验证：见下方构建记录
 * 配置    ：EXP_CAMERA (13)
 * 日期    ：2026-11-21
 *
 * 硬件：P4X-Function-EV-Board + 2MP SC2336（MIPI-CSI，摄像头转接板）
 * SCCB：SCL=GPIO8，SDA=GPIO7（板级定义）；reset/pwdn 未接
 * 数据流：SC2336 RAW8 → CSI → ISP → RGB565 → /dev/video0 → mmap 缓冲
 */

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"

#include "camera_demo.h"

static const char *TAG = "CAM";

#define CAM_I2C_PORT       I2C_NUM_1   /* 板级 I2C1（BSP 的 CONFIG_BSP_I2C_NUM 默认 1） */
#define CAM_I2C_SCL        GPIO_NUM_8
#define CAM_I2C_SDA        GPIO_NUM_7
#define CAM_SCCB_FREQ_HZ   100000   /* 先用 100kHz：BSP 的 I2C 默认也是 100k */
#define CAM_BUF_COUNT      2
#define CAM_TASK_STACK     4096
#define CAM_TASK_PRIO      6
#define CAM_LOG_EVERY_N    30
#define CAM_SAMPLE_STEP    257

static void camera_task(void *arg)
{
    (void)arg;

    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = CAM_I2C_PORT,
        .sda_io_num = CAM_I2C_SDA,
        .scl_io_num = CAM_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;

    /* 先按输入读一下总线空闲电平：SDA/SCL 都应被上拉到 1。
     * 读到 0 说明线被拉死（排线插反/短路/设备没供电）。 */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CAM_I2C_SDA) | (1ULL << CAM_I2C_SCL),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    vTaskDelay(pdMS_TO_TICKS(10));
    int sda_idle = gpio_get_level(CAM_I2C_SDA);
    int scl_idle = gpio_get_level(CAM_I2C_SCL);

    esp_err_t err = i2c_new_master_bus(&i2c_cfg, &i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: 0x%x", err);
        vTaskDelete(NULL);
        return;
    }

    esp_video_init_csi_config_t csi_cfg = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus,
            .freq = CAM_SCCB_FREQ_HZ,
        },
        .reset_pin = GPIO_NUM_NC,
        .pwdn_pin = GPIO_NUM_NC,
        .dont_init_ldo = false,
    };
    esp_video_init_config_t video_cfg = { .csi = &csi_cfg };

    ESP_LOGI(TAG, "扫描 SCCB 总线（SCL=GPIO8, SDA=GPIO7）...");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(i2c_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  I2C 设备应答: 0x%02X", addr);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "SCCB 总线上没有任何设备：先查排线方向和 3.3V 供电");
    }
    /* 放在扫描之后打印：避开复位时 USB 重新枚举，保证能看到 */
    ESP_LOGI(TAG, "扫描前总线电平: SDA=%d SCL=%d（都应为 1）", sda_idle, scl_idle);

    err = esp_video_init(&video_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init: 0x%x（相机没接好或 sensor 没识别？）", err);
        vTaskDelete(NULL);
        return;
    }

    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "open %s 失败: errno=%d", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, errno);
        vTaskDelete(NULL);
        return;
    }

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT 失败");
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "sensor 默认格式: %ux%u fmt=0x%08x",
             (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height,
             (unsigned)fmt.fmt.pix.pixelformat);

    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT(RGB565) 失败");
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    struct v4l2_requestbuffers req = {
        .count = CAM_BUF_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS 失败");
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    uint8_t *buffers[CAM_BUF_COUNT] = {0};
    size_t lengths[CAM_BUF_COUNT] = {0};
    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = (uint32_t)i,
        };
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF(%d) 失败", i);
            close(fd);
            vTaskDelete(NULL);
            return;
        }
        buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, buf.m.offset);
        if (buffers[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap(%d) 失败", i);
            close(fd);
            vTaskDelete(NULL);
            return;
        }
        lengths[i] = buf.length;
        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF(%d) 失败", i);
            close(fd);
            vTaskDelete(NULL);
            return;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON 失败");
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "开始采集：%ux%u RGB565，%d 个缓冲",
             (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height, CAM_BUF_COUNT);

    uint32_t frames = 0;
    for (;;) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_DQBUF 失败: errno=%d", errno);
            break;
        }
        frames++;

        if ((frames % CAM_LOG_EVERY_N) == 0) {
            const uint8_t *p = buffers[buf.index];
            uint32_t n = buf.bytesused ? buf.bytesused : (uint32_t)lengths[buf.index];
            uint64_t sum = 0;
            uint32_t samples = 0;
            for (uint32_t off = 0; off < n; off += CAM_SAMPLE_STEP) {
                sum += p[off];
                samples++;
            }
            ESP_LOGI(TAG, "第 %u 帧: %u 字节, 平均亮度 %u",
                     (unsigned)frames, (unsigned)n,
                     (unsigned)(samples ? (sum / samples) : 0));
        }

        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF 失败");
            break;
        }
    }

    ioctl(fd, VIDIOC_STREAMOFF, &type);
    close(fd);
    vTaskDelete(NULL);
}

void camera_demo_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(camera_task, "camera", CAM_TASK_STACK,
                                            NULL, CAM_TASK_PRIO, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 camera 任务失败");
    }
}
