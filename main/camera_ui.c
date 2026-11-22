/*
 * camera_ui.c —— 相机直绘 + LVGL 小区域叠加（EXP_CAM_UI = 17）
 *
 * 优化点：
 *   之前把整帧 canvas 交给 LVGL 合成，LVGL 软件重绘 1024x600 要 76ms，只有 12fps。
 *   现在相机帧直接 draw_bitmap 到面板（y=64..600），LVGL 只负责顶部状态条和
 *   右下角按钮这些小区域，用 PARTIAL 模式 + 256KB 小缓冲，不再合成整屏。
 *
 * 同步：
 *   面板的 DMA2D 是共享资源，相机绘制和 LVGL flush 都用 s_panel_mutex 串行。
 *   相机每画一帧会盖掉右下角按钮，所以画完 invalidate 按钮，让 LVGL 补画。
 *
 * 共享 I2C1：相机 SCCB（0x30）+ GT911（0x14），只建一条总线。
 */

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"

#include "display_port.h"
#include "camera_ui.h"

static const char *TAG = "CAMUI";

#define I2C_PORT        I2C_NUM_1
#define I2C_SCL         GPIO_NUM_8
#define I2C_SDA         GPIO_NUM_7
#define SCCB_FREQ_HZ    100000
#define PREVIEW_W       DISPLAY_H_RES
#define PREVIEW_H       DISPLAY_V_RES
#define PREVIEW_BYTES   (PREVIEW_W * PREVIEW_H * 2)
#define CAM_BUF_COUNT   3
#define TOP_BAR_H       64
#define UI_BUF_LINES    128

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_tp;
static lv_obj_t *s_bar;
static lv_obj_t *s_btn;
static lv_obj_t *s_fps_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_freeze_label;
static SemaphoreHandle_t s_panel_mutex;
static volatile bool s_paused;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (s_panel_mutex) {
        xSemaphoreTake(s_panel_mutex, portMAX_DELAY);
    }
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
    if (s_panel_mutex) {
        xSemaphoreGive(s_panel_mutex);
    }
    lv_display_flush_ready(disp);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (s_tp == NULL) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    esp_lcd_touch_read_data(s_tp);
    esp_lcd_touch_point_data_t point[1] = {0};
    uint8_t count = 0;
    esp_err_t err = esp_lcd_touch_get_data(s_tp, point, &count, 1);
    if (err == ESP_OK && count > 0) {
        data->point.x = point[0].x;
        data->point.y = point[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void freeze_clicked_cb(lv_event_t *e)
{
    (void)e;
    s_paused = !s_paused;
    if (s_freeze_label) {
        lv_label_set_text(s_freeze_label, s_paused ? "继续" : "冻结");
    }
    if (s_status_label) {
        lv_label_set_text(s_status_label, s_paused ? "已冻结" : "预览中");
    }
    ESP_LOGI(TAG, "预览%s", s_paused ? "冻结" : "继续");
}

static void camera_stream_task(void *arg)
{
    (void)arg;

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
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON 失败");
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "相机流已启动 %dx%d", PREVIEW_W, PREVIEW_H);

    uint32_t frames = 0;
    int64_t t0 = esp_timer_get_time();

    for (;;) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_DQBUF 失败: errno=%d", errno);
            break;
        }

        if (!s_paused && buffers[buf.index] != NULL) {
            int64_t t_draw0 = esp_timer_get_time();

            /* 相机直绘：跳过顶部 64 行（留给 UI），其余整宽送屏 */
            if (s_panel_mutex) {
                xSemaphoreTake(s_panel_mutex, portMAX_DELAY);
            }
            esp_lcd_panel_draw_bitmap(s_panel, 0, TOP_BAR_H, PREVIEW_W, PREVIEW_H,
                                      buffers[buf.index] + TOP_BAR_H * PREVIEW_W * 2);
            if (s_panel_mutex) {
                xSemaphoreGive(s_panel_mutex);
            }

            frames++;
            if ((frames % 30) == 0 && s_fps_label) {
                int64_t t1 = esp_timer_get_time();
                int fps = (int)(30.0f * 1000000.0f / (float)(t1 - t0));
                lvgl_port_lock(0);
                lv_label_set_text_fmt(s_fps_label, "%d fps", fps);
                lv_obj_invalidate(s_bar);
                lvgl_port_unlock();
                t0 = t1;
                ESP_LOGI(TAG, "显示 %d fps, draw=%lldus", fps,
                         (long long)(esp_timer_get_time() - t_draw0));
            }
        }

        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    ioctl(fd, VIDIOC_STREAMOFF, &type);
    close(fd);
    vTaskDelete(NULL);
}

static void camera_ui_task(void *arg)
{
    (void)arg;

    s_panel_mutex = xSemaphoreCreateMutex();

    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(display_port_init(&s_panel, &io));

    /* 共享 I2C1：相机 SCCB + GT911 */
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
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
            .freq = SCCB_FREQ_HZ,
        },
        .reset_pin = GPIO_NUM_NC,
        .pwdn_pin = GPIO_NUM_NC,
        .dont_init_ldo = false,
    };
    esp_video_init_config_t video_cfg = { .csi = &csi_cfg };
    ESP_ERROR_CHECK(esp_video_init(&video_cfg));
    ESP_LOGI(TAG, "相机初始化完成");

    uint8_t tp_addr = 0;
    if (i2c_master_probe(i2c_bus, 0x5D, 50) == ESP_OK) {
        tp_addr = 0x5D;
    } else if (i2c_master_probe(i2c_bus, 0x14, 50) == ESP_OK) {
        tp_addr = 0x14;
    }
    ESP_LOGI(TAG, "GT911 地址: 0x%02X", tp_addr);
    if (tp_addr != 0) {
        esp_lcd_panel_io_handle_t tp_io = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        tp_io_cfg.dev_addr = tp_addr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io));
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = PREVIEW_W,
            .y_max = PREVIEW_H,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
            .levels = { .reset = 0, .interrupt = 0 },
            .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        };
        if (esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_tp) != ESP_OK) {
            ESP_LOGW(TAG, "GT911 初始化失败，界面只能看不能点");
            s_tp = NULL;
        }
    }

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 4;
    port_cfg.task_stack = 8192;
    port_cfg.timer_period_ms = 10;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_lock(0);

    lv_display_t *disp = lv_display_create(PREVIEW_W, PREVIEW_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    /* PARTIAL + 小缓冲：只装得下顶部条/按钮，不装整屏 */
    const size_t ui_buf_bytes = PREVIEW_W * UI_BUF_LINES * 2;
    void *ui_buf = heap_caps_malloc(ui_buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ui_buf == NULL) {
        ESP_LOGE(TAG, "PSRAM 分配 UI 缓冲失败");
        lvgl_port_unlock();
        vTaskDelete(NULL);
        return;
    }
    lv_display_set_buffers(disp, ui_buf, NULL, ui_buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_theme_t *theme = lv_theme_default_init(disp,
                                              lv_palette_main(LV_PALETTE_BLUE),
                                              lv_palette_main(LV_PALETTE_RED),
                                              false, LV_FONT_DEFAULT);
    lv_display_set_theme(disp, theme);

    if (s_tp != NULL) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read_cb);
        lv_indev_set_display(indev, disp);
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_bar = lv_obj_create(scr);
    lv_obj_set_size(s_bar, PREVIEW_W, TOP_BAR_H);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);

    lv_obj_t *title = lv_label_create(s_bar);
    lv_label_set_text(title, "SC2336 1024x600");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 20, 0);

    s_status_label = lv_label_create(s_bar);
    lv_label_set_text(s_status_label, "预览中");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x8FD3FF), 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 0);

    s_fps_label = lv_label_create(s_bar);
    lv_label_set_text(s_fps_label, "-- fps");
    lv_obj_set_style_text_color(s_fps_label, lv_color_white(), 0);
    lv_obj_align(s_fps_label, LV_ALIGN_RIGHT_MID, -170, 0);

    /* 按钮放进顶栏：相机不画 y<64，按钮不会被盖掉，不会闪 */
    s_btn = lv_button_create(s_bar);
    lv_obj_set_size(s_btn, 140, 48);
    lv_obj_align(s_btn, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_event_cb(s_btn, freeze_clicked_cb, LV_EVENT_CLICKED, NULL);
    s_freeze_label = lv_label_create(s_btn);
    lv_label_set_text(s_freeze_label, "冻结");
    lv_obj_center(s_freeze_label);

    lv_obj_invalidate(scr);
    lv_refr_now(disp);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "界面就绪，启动相机流");

    xTaskCreatePinnedToCore(camera_stream_task, "cam_stream", 8192, NULL, 6, NULL, 1);
    vTaskDelete(NULL);
}

void camera_ui_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(camera_ui_task, "cam_ui", 8192,
                                            NULL, 5, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 cam_ui 任务失败");
    }
}
