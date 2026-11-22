/*
 * camera_preview.c —— SC2336 → JD9165 实时预览（EXP_CAM_PREVIEW = 16）
 *
 * 链路：SC2336 RAW8 1024x600 → CSI → ISP → RGB565 → /dev/video0
 *       → esp_lcd_panel_draw_bitmap(0,0,1024,600) → 屏
 *
 * 选 1024x600 是为了和屏 1:1，不做缩放。
 * SCCB：I2C1，SCL=GPIO8，SDA=GPIO7（与触摸共用，但本实验不跑触摸）。
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
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"

#include "display_port.h"
#include "camera_preview.h"

static const char *TAG = "PREVIEW";

#define CAM_I2C_PORT     I2C_NUM_1
#define CAM_I2C_SCL      GPIO_NUM_8
#define CAM_I2C_SDA      GPIO_NUM_7
#define CAM_SCCB_FREQ_HZ 100000
#define CAM_BUF_COUNT    2
#define PREVIEW_W        DISPLAY_H_RES
#define PREVIEW_H        DISPLAY_V_RES

static void preview_task(void *arg)
{
    (void)arg;

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(display_port_init(&panel, &io));
    ESP_LOGI(TAG, "屏就绪，初始化相机");

    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = CAM_I2C_PORT,
        .sda_io_num = CAM_I2C_SDA,
        .scl_io_num = CAM_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

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

    esp_err_t err = esp_video_init(&video_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init: 0x%x（相机没接好？）", err);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "相机初始化完成");

    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);    if (fd < 0) {
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
    ESP_LOGI(TAG, "相机默认: %ux%u fmt=0x%08x stride=%u",
             (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height,
             (unsigned)fmt.fmt.pix.pixelformat, (unsigned)fmt.fmt.pix.bytesperline);

    fmt.fmt.pix.width = PREVIEW_W;
    fmt.fmt.pix.height = PREVIEW_H;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT(%dx%d RGB565) 失败", PREVIEW_W, PREVIEW_H);
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "设置后: %ux%u stride=%u",
             (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height,
             (unsigned)fmt.fmt.pix.bytesperline);

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
        buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
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
    ESP_LOGI(TAG, "开始预览：%dx%d RGB565", PREVIEW_W, PREVIEW_H);

    uint32_t frames = 0;    int64_t t0 = esp_timer_get_time();
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

        if (frames == 1) {
            ESP_LOGI(TAG, "第一帧：index=%u bytes=%u", (unsigned)buf.index,
                     (unsigned)lengths[buf.index]);
        }

        /* 相机缓冲直接送屏：整屏 draw_bitmap，DMA2D 同步拷贝 */
        esp_lcd_panel_draw_bitmap(panel, 0, 0, PREVIEW_W, PREVIEW_H, buffers[buf.index]);

        if ((frames % 300) == 0) {
            int64_t t1 = esp_timer_get_time();
            ESP_LOGI(TAG, "第 %u 帧，%.1f fps，%u 字节/帧",
                     (unsigned)frames, 30.0f * 1000000.0f / (float)(t1 - t0),
                     (unsigned)lengths[buf.index]);
            t0 = t1;
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

void camera_preview_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(preview_task, "preview", 8192,
                                            NULL, 6, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 preview 任务失败");
    }
}
