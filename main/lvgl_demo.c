/*
 * lvgl_demo.c —— LVGL 9 + GT911 触摸实验（EXP_LVGL = 15）
 *
 * 显示：JD9165 1024x600（display_port_init）
 * 触摸：GT911，I2C1，SCL=GPIO8 / SDA=GPIO7；实测地址是 0x14（备份地址），不是 0x5D
 *
 * 为什么不用 esp_lvgl_port 的 DSI 显示适配：
 *   IDF 5.5 的 esp_lcd_dpi_panel_register_event_callbacks 要求 on_color_trans_done
 *   回调位于 IRAM，而 esp_lvgl_port 2.8 的回调在 flash，注册被拒且返回值没检查，
 *   导致 full-refresh 的 flush 永远等不到信号、界面卡死。
 *   这里自己建 display：flush 里 draw_bitmap 后立即 lv_display_flush_ready。
 *   port 只保留 LVGL 任务/定时器/锁。
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"

#include "display_port.h"
#include "lvgl_demo.h"

static const char *TAG = "LVGL";

#define TOUCH_I2C_PORT  I2C_NUM_1
#define TOUCH_I2C_SCL   GPIO_NUM_8
#define TOUCH_I2C_SDA   GPIO_NUM_7

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_tp;
static lv_obj_t *s_count_label;
static int s_count;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
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

static void btn_clicked_cb(lv_event_t *e)
{
    (void)e;
    s_count++;
    if (s_count_label) {
        lv_label_set_text_fmt(s_count_label, "点击次数: %d", s_count);
    }
    ESP_LOGI(TAG, "按钮被点击，累计 %d 次", s_count);
}

static void lvgl_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "step 1: display_port_init");
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(display_port_init(&s_panel, &io));
    ESP_LOGI(TAG, "step 1 done");

    /* 触摸 I2C：和相机共用 I2C1 */
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_I2C_SDA,
        .scl_io_num = TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(i2c_bus, a, 5) == ESP_OK) {
            ESP_LOGI(TAG, "I2C1 应答: 0x%02X", a);
        }
    }
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
            .x_max = DISPLAY_H_RES,
            .y_max = DISPLAY_V_RES,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
            .levels = { .reset = 0, .interrupt = 0 },
            .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        };
        esp_err_t err = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_tp);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GT911 init 失败 0x%x，先跳过触摸", err);
            s_tp = NULL;
        } else {
            ESP_LOGI(TAG, "GT911 初始化完成");
        }
    } else {
        ESP_LOGW(TAG, "I2C1 上没扫到 GT911，跳过触摸");
    }

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 4;
    port_cfg.task_stack = 8192;
    port_cfg.timer_period_ms = 10;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));
    ESP_LOGI(TAG, "lvgl_port_init done");

    lvgl_port_lock(0);

    lv_display_t *disp = lv_display_create(DISPLAY_H_RES, DISPLAY_V_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    const size_t buf_bytes = DISPLAY_H_RES * DISPLAY_V_RES * 2;
    void *draw_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (draw_buf == NULL) {
        ESP_LOGE(TAG, "PSRAM 分配 LVGL 缓冲失败");
        lvgl_port_unlock();
        vTaskDelete(NULL);
        return;
    }
    lv_display_set_buffers(disp, draw_buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_FULL);
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
        ESP_LOGI(TAG, "LVGL 触摸输入设备已接入");
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESP32-P4  LVGL 9  JD9165");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    s_count_label = lv_label_create(scr);
    lv_label_set_text(s_count_label, "点击次数: 0");
    lv_obj_set_style_text_color(s_count_label, lv_color_hex(0x8FD3FF), 0);
    lv_obj_align(s_count_label, LV_ALIGN_TOP_MID, 0, 100);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 260, 96);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 30);
    lv_obj_add_event_cb(btn, btn_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "点我 +1");
    lv_obj_center(btn_label);

    lv_obj_invalidate(scr);
    lv_refr_now(disp);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "LVGL 界面就绪，点屏幕按钮试触摸");

    vTaskDelete(NULL);
}

void lvgl_demo_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(lvgl_task, "lvgl_demo", 8192,
                                            NULL, 5, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 lvgl 任务失败");
    }
}
