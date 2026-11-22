/*
 * display_demo.c —— 点屏彩条实验（EXP_DISPLAY = 14）
 *
 * 初始化已抽到 display_port.c；这里只负责画 8 条彩条验证通路。
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"

#include "display_port.h"
#include "display_demo.h"

static const char *TAG = "LCD";

#define LCD_COLOR_BARS 8

static void display_task(void *arg)
{
    (void)arg;

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(display_port_init(&panel, &io));

    const size_t fb_bytes = DISPLAY_H_RES * DISPLAY_V_RES * 2;
    uint16_t *fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (fb == NULL) {
        ESP_LOGE(TAG, "PSRAM 分配 %u 字节失败", (unsigned)fb_bytes);
        vTaskDelete(NULL);
        return;
    }

    static const uint16_t bars[LCD_COLOR_BARS] = {
        0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0xFFE0, 0x07FF, 0xF81F,
    };
    const int bar_w = DISPLAY_H_RES / LCD_COLOR_BARS;
    for (int y = 0; y < DISPLAY_V_RES; y++) {
        for (int x = 0; x < DISPLAY_H_RES; x++) {
            fb[y * DISPLAY_H_RES + x] = bars[x / bar_w];
        }
    }

    ESP_LOGI(TAG, "画彩条 %dx%d …", DISPLAY_H_RES, DISPLAY_V_RES);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, DISPLAY_H_RES, DISPLAY_V_RES, fb));
    ESP_LOGI(TAG, "彩条已画完，看屏幕");

    vTaskDelete(NULL);
}

void display_demo_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(display_task, "display", 8192,
                                            NULL, 5, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 display 任务失败");
    }
}
