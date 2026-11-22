#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#define DISPLAY_H_RES 1024
#define DISPLAY_V_RES 600

/* 初始化 JD9165 点屏链路（LDO/DSI/panel/背光），成功后可交给 LVGL 或直接画图。 */
esp_err_t display_port_init(esp_lcd_panel_handle_t *panel, esp_lcd_panel_io_handle_t *io);
