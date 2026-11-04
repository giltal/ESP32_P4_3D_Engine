/*
 * Display for the Guition JC1060P470C: JD9165 1024x600 MIPI-DSI panel on 2
 * lanes, double-buffered DPI, frames scaled up by the PPA.
 *
 * Adapted from the ESP32_P4_Launcher board support (boards/guition_p4_7),
 * whose init table and timing come from the ESP32_P4_Internet_Radio project.
 * This is the V1 panel; Guition's 2026 "V2" panel needs another init table
 * and resets on GPIO 0.
 */
#include "display.h"

#include <string.h>
#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_jd9165.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "display";

#define LCD_W          1024
#define LCD_H          600
#define LCD_BL_GPIO    23
#define LCD_RST_GPIO   27
#define DSI_LDO_CHAN   3
#define DSI_LDO_MV     2500
#define PCLK_MHZ       54

#define CMD(c, ...) { (c), (uint8_t[]){ __VA_ARGS__ }, sizeof((uint8_t[]){ __VA_ARGS__ }), 0 }
#define CMD_DELAY(c, ms) { (c), (uint8_t[]){ 0x00 }, 0, (ms) }

static const jd9165_lcd_init_cmd_t s_init[] = {
    CMD(0x30, 0x00), CMD(0xF7, 0x49, 0x61, 0x02, 0x00), CMD(0x30, 0x01), CMD(0x04, 0x0C), CMD(0x05, 0x00),
    CMD(0x06, 0x00), CMD(0x0B, 0x11), CMD(0x17, 0x00), CMD(0x20, 0x04), CMD(0x1F, 0x05), CMD(0x23, 0x00),
    CMD(0x25, 0x19), CMD(0x28, 0x18), CMD(0x29, 0x04), CMD(0x2A, 0x01), CMD(0x2B, 0x04), CMD(0x2C, 0x01),
    CMD(0x30, 0x02), CMD(0x01, 0x22), CMD(0x03, 0x12), CMD(0x04, 0x00), CMD(0x05, 0x64), CMD(0x0A, 0x08),
    CMD(0x0B, 0x0A, 0x1A, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x08, 0x1F, 0x1D),
    CMD(0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x0D, 0x16, 0x1B, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x09, 0x1E, 0x1C),
    CMD(0x0E, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x0F, 0x16, 0x1B, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1C, 0x1E, 0x09, 0x07),
    CMD(0x10, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x11, 0x0A, 0x1A, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1D, 0x1F, 0x08, 0x06),
    CMD(0x12, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x14, 0x00, 0x00, 0x11, 0x11), CMD(0x18, 0x99), CMD(0x30, 0x06),
    CMD(0x12, 0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29),
    CMD(0x13, 0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29),
    CMD(0x30, 0x0A), CMD(0x02, 0x4F), CMD(0x0B, 0x40), CMD(0x12, 0x3E), CMD(0x13, 0x78), CMD(0x30, 0x0D),
    CMD(0x0D, 0x04), CMD(0x10, 0x0C), CMD(0x11, 0x0C), CMD(0x12, 0x0C), CMD(0x13, 0x0C), CMD(0x30, 0x00),
    CMD_DELAY(0x11, 120),   /* sleep out */
    CMD_DELAY(0x29, 20),    /* display on */
};

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb[2];              /* the panel driver's own two frame buffers */
static int s_visible;
static SemaphoreHandle_t s_vsync;      /* given at the end of each panel refresh */
static ppa_client_handle_t s_ppa;
static int s_fw, s_fh, s_ox, s_oy;
static float s_scale;
static SemaphoreHandle_t s_go, s_idle;
static const uint16_t *volatile s_frame;

static bool IRAM_ATTR on_refresh_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *ed, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync, &woken);
    return woken == pdTRUE;
}

static void backlight(int percent)
{
    ledc_timer_config_t t = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                              .timer_num = LEDC_TIMER_0, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&t);
    ledc_channel_config_t c = { .gpio_num = LCD_BL_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
                                .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0,
                                .duty = 1023 * percent / 100 };
    ledc_channel_config(&c);
}

/* Scale one frame into the hidden panel buffer, then flip to it at the next refresh */
static void present_one(const uint16_t *frame)
{
    int back = s_visible ^ 1;
    ppa_srm_oper_config_t op = {
        .in = { .buffer = frame, .pic_w = s_fw, .pic_h = s_fh, .block_w = s_fw, .block_h = s_fh,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .out = { .buffer = s_fb[back], .buffer_size = LCD_W * LCD_H * 2, .pic_w = LCD_W, .pic_h = LCD_H,
                 .block_offset_x = s_ox, .block_offset_y = s_oy, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = s_scale,
        .scale_y = s_scale,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(s_ppa, &op) != ESP_OK)
        return;
    /* drawing one of the driver's own buffers makes it scan out from it */
    xSemaphoreTake(s_vsync, 0);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb[back]);
    s_visible = back;
    /* wait for the flip, so the next frame does not scale into the buffer on screen */
    xSemaphoreTake(s_vsync, pdMS_TO_TICKS(50));
}

static void present_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        present_one(s_frame);
        xSemaphoreGive(s_idle);
    }
}

void display_present(const uint16_t *frame)
{
    xSemaphoreTake(s_idle, portMAX_DELAY);   /* the previous frame is scaled */
    s_frame = frame;
    xSemaphoreGive(s_go);
}

uint16_t *display_alloc_frame(void)
{
    /* 64-byte aligned for the PPA's DMA and the cache line */
    return heap_caps_aligned_calloc(64, (size_t)s_fw * s_fh, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

esp_err_t display_init(int frame_w, int frame_h)
{
    s_fw = frame_w;
    s_fh = frame_h;
    backlight(0);

    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = DSI_LDO_CHAN, .voltage_mv = DSI_LDO_MV };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "DSI PHY power");

    esp_lcd_dsi_bus_handle_t bus;
    esp_lcd_dsi_bus_config_t bus_cfg = JD9165_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "DSI bus");
    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dbi_cfg = JD9165_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &io), TAG, "DBI io");

    esp_lcd_dpi_panel_config_t dpi = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = PCLK_MHZ,
        .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 2,
        .video_timing = { .h_size = LCD_W, .v_size = LCD_H,
                          .hsync_pulse_width = 40, .hsync_back_porch = 160, .hsync_front_porch = 160,
                          .vsync_pulse_width = 10, .vsync_back_porch = 23, .vsync_front_porch = 12 },
    };
    jd9165_vendor_config_t vendor = {
        .init_cmds = s_init,
        .init_cmds_size = sizeof(s_init) / sizeof(s_init[0]),
        .mipi_config = { .dsi_bus = bus, .dpi_config = &dpi },
    };
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9165(io, &dev, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(120));     /* the JD9165 needs 120 ms after reset */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");

    void *fb0, *fb1;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1), TAG, "frame buffers");
    s_fb[0] = fb0;
    s_fb[1] = fb1;
    for (int i = 0; i < 2; i++) {       /* black, including the borders the frame leaves */
        memset(s_fb[i], 0, LCD_W * LCD_H * 2);
        esp_cache_msync(s_fb[i], LCD_W * LCD_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    s_vsync = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t cbs = { .on_refresh_done = on_refresh_done };
    esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL);

    /* The largest scale that fits, in the PPA's 1/16 steps, centred */
    float sx = (float)LCD_W / frame_w, sy = (float)LCD_H / frame_h;
    s_scale = (float)(int)((sx < sy ? sx : sy) * 16) / 16;
    s_ox = (LCD_W - (int)(frame_w * s_scale)) / 2;
    s_oy = (LCD_H - (int)(frame_h * s_scale)) / 2;
    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    ESP_RETURN_ON_ERROR(ppa_register_client(&pc, &s_ppa), TAG, "PPA");

    s_go = xSemaphoreCreateBinary();
    s_idle = xSemaphoreCreateBinary();
    xSemaphoreGive(s_idle);
    /* core 0 at a high priority: the PPA work is short, and core 1 is the engine's */
    xTaskCreatePinnedToCore(present_task, "present", 4096, NULL, configMAX_PRIORITIES - 2, NULL, 0);

    backlight(100);
    ESP_LOGI(TAG, "JD9165 %dx%d, frame %dx%d scaled %.4g at %d,%d", LCD_W, LCD_H, frame_w, frame_h,
             (double)s_scale, s_ox, s_oy);
    return ESP_OK;
}
