/*
 * display_port.c —— JD9165 1024x600 点屏公共初始化
 *
 * 硬件：沃乐康 W700BH018I-30Z（JD9165BA，2-lane，pclk 51.2MHz）
 *       RST=GPIO27，背光=GPIO26
 * 要点：LDO chan3 2.5V、DSI 2-lane 600Mbps、DPI 51.2MHz、RGB565
 *       厂商驱动初始化里读 ID 会死等，已在 components/esp_lcd_jd9165 里跳过
 */

#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_jd9165.h"

#include "display_port.h"

static const char *TAG = "LCD";

#define LCD_RST_GPIO       GPIO_NUM_27
#define LCD_BL_GPIO        GPIO_NUM_26
#define LCD_LDO_CHAN       3
#define LCD_LDO_MV         2500
#define LCD_LANE_MBPS      600.0f
#define LCD_DPI_MHZ        51.2f

static esp_ldo_channel_handle_t s_ldo;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;

esp_err_t display_port_init(esp_lcd_panel_handle_t *panel, esp_lcd_panel_io_handle_t *io_out)
{
    esp_err_t ret;

    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = LCD_LDO_CHAN,
        .voltage_mv = LCD_LDO_MV,
    };
    ret = esp_ldo_acquire_channel(&ldo_cfg, &s_ldo);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LDO acquire: 0x%x", ret);
        return ret;
    }

    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_XTAL,
        .lane_bit_rate_mbps = LCD_LANE_MBPS,
    };
    ret = esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "new DSI bus: 0x%x", ret);
        return ret;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = JD9165BA_PANEL_IO_DBI_CONFIG();
    ret = esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "new DBI IO: 0x%x", ret);
        return ret;
    }

    esp_lcd_dpi_panel_config_t dpi_cfg =
        JD9165BA_1024_600_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi_cfg.dpi_clock_freq_mhz = LCD_DPI_MHZ;

    jd9165_vendor_config_t vendor_cfg = {
        .init_cmds = NULL,
        .init_cmds_size = 0,
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };

    esp_lcd_panel_handle_t panel_handle = NULL;
    ret = esp_lcd_new_panel_jd9165(io, &panel_cfg, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "new panel: 0x%x", ret);
        return ret;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    gpio_config_t bl_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << LCD_BL_GPIO,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(LCD_BL_GPIO, 1);

    ESP_LOGI(TAG, "panel ready: %dx%d, 2 lanes %.0fMbps, dpi %.1fMHz",
             DISPLAY_H_RES, DISPLAY_V_RES, (double)LCD_LANE_MBPS, (double)LCD_DPI_MHZ);

    *panel = panel_handle;
    *io_out = io;
    return ESP_OK;
}
