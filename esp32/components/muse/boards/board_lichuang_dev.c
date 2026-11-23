/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Lichuang (LCKFB) "Szpi" ESP32-S3 development board (立创·实战派):
 * ESP32-S3R8 module (16 MB flash, 8 MB octal PSRAM), 2.0" ST7789 SPI LCD
 * (native 240x320 panel run as 320x240 landscape), FT6336 capacitive
 * touch at 0x38, ES8311 speaker DAC at 0x18 and ES7210 dual-mic ADC at
 * 0x41 sharing one I2S bus, PCA9557 IO expander at 0x19 (P0 = LCD chip
 * select, P1 = speaker amp enable, P2 = camera power), BOOT (GPIO0)
 * button, USB through a CH334F hub feeding both the S3's native USB
 * Serial/JTAG and a CH340 bridge.
 *
 * Pins and init follow xiaozhi-esp32's board
 * main/boards/lckfb/szpi-esp32s3 (config.h, lichuang_dev_board.cc),
 * cross-checked against the vendor schematic SCH_X9200-MB-V1.0A_20250828.
 * There is no vendor BSP in the ESP Component Registry, so like
 * board_fnk0104b.c this file drives esp_lcd, the codecs and the
 * expander directly.
 *
 * First-port notes (verify on hardware, in this order):
 * - Touch: FT6336 polled over I2C like the FNK0104B; the coordinate
 *   transform below applies xiaozhi's esp_lcd_touch settings
 *   (swap_xy + mirror_x). If taps land mirrored, this transform is the
 *   knob to turn.
 * - The ES7210's MIC3 input is the speaker reference xiaozhi uses for
 *   AEC; it is not selected here, Muse takes MIC1 (mic_slot 0).
 * - read_power is NULL for now: the SY6302 gauge on the I2C bus is a
 *   follow-up, so Settings shows no battery yet.
 * - power_off returns ESP_ERR_NOT_SUPPORTED: the board's power key is
 *   a hardware latch (AK8052), no software power-off is verified.
 */
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_H_RES 320
#define LCD_V_RES 240
#define LCD_HOST SPI3_HOST
#define LCD_SCLK GPIO_NUM_41
#define LCD_MOSI GPIO_NUM_40
#define LCD_CS GPIO_NUM_NC    /* as xiaozhi drives this board: no ESP-side CS */
#define LCD_DC GPIO_NUM_39
#define LCD_RST GPIO_NUM_NC   /* shared board reset line */
#define LCD_BL GPIO_NUM_42    /* backlight, inverted: low = on (xiaozhi) */
#define DRAW_BUF_LINES 16

#define I2C_SDA GPIO_NUM_1    /* codecs, touch, PCA9557, QMI8658, gauge */
#define I2C_SCL GPIO_NUM_2
#define TOUCH_ADDR 0x38       /* FT6336 (FT5x06 register map) */
#define TOUCH_REG_TD_STATUS 0x02
#define TOUCH_REG_P1 0x03     /* XH, XL, YH, YL; high nibble of XH/YH */

#define I2S_MCLK GPIO_NUM_38
#define I2S_BCLK GPIO_NUM_14
#define I2S_WS GPIO_NUM_13
#define I2S_DIN GPIO_NUM_12
#define I2S_DOUT GPIO_NUM_45
/* The schematic marks the ES7210 at 0x41 (AD0 high), not the esp_codec_dev
 * default of 0x40, so the address is written out in full. */
#define ES7210_ADDR 0x82   /* esp_codec_dev takes the 8-bit form; the chip's 7-bit address is 0x41 */

#define PCA9557_ADDR 0x19
#define PCA9557_REG_OUTPUT 0x01
#define PCA9557_REG_CONFIG 0x03
#define PCA9557_LCD_CS 0
#define PCA9557_PA_EN 1
#define PCA9557_CAM_PWR 2

#define TALK_GPIO GPIO_NUM_0  /* BOOT button */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_touch_dev;
static i2c_master_dev_handle_t s_pca_dev;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk;
/* PCA9557 output shadow, byte-identical to xiaozhi's Pca9557 ctor
 * (WriteReg(0x01, 0x03)): P0=1, P1=1, P2=0 at boot. P0 is pulled low
 * inside display_start, between panel reset and panel init, exactly
 * as xiaozhi's InitializeSt7789Display does. */
static uint8_t s_pca_out = 0x03;

static esp_err_t pca9557_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_pca_dev, buf, sizeof(buf), 50);
}

static esp_err_t pca9557_set(int pin, bool level)
{
    if (level) {
        s_pca_out |= (uint8_t)(1u << pin);
    } else {
        s_pca_out &= (uint8_t)~(1u << pin);
    }
    return pca9557_write(PCA9557_REG_OUTPUT, s_pca_out);
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");

    /* Expander first: display_start sequences one of its pins around the
     * panel reset, so it must be configured before the panel is touched. */
    const i2c_device_config_t pca_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9557_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &pca_cfg, &s_pca_dev), TAG, "pca9557 i2c");
    ESP_RETURN_ON_ERROR(pca9557_write(PCA9557_REG_OUTPUT, s_pca_out), TAG, "pca9557 output");
    /* P0, P1 and P2 are outputs (0); everything else stays an input. */
    ESP_RETURN_ON_ERROR(pca9557_write(PCA9557_REG_CONFIG, 0xF8), TAG, "pca9557 config");

    /* Touch controller: no reset or interrupt GPIO (the reset line is
     * shared with the panel), just add it to the bus and poll it. */
    const i2c_device_config_t touch_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &touch_cfg, &s_touch_dev), TAG, "touch i2c");

    return muse_gpio_button_init(&s_talk, TALK_GPIO);
}

static esp_err_t touch_read_regs(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_touch_dev, &reg, 1, buf, n, 50);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint8_t count = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    if (touch_read_regs(TOUCH_REG_TD_STATUS, &count, 1) != ESP_OK || (count & 0x0F) == 0) {
        return;
    }
    uint8_t p[4];
    if (touch_read_regs(TOUCH_REG_P1, p, sizeof(p)) != ESP_OK) {
        return;
    }
    /* The controller reports in the panel's native portrait frame
     * (x: 0..239, y: 0..319). Into the 320x240 landscape frame LVGL
     * draws: swap the axes, then mirror the new y axis. (The first cut
     * mirrored the new x instead, which lands 180 degrees off in this
     * stack — every swipe direction read reversed on the real board,
     * 2026-11-22.) */
    int raw_x = ((p[0] & 0x0F) << 8) | p[1];
    int raw_y = ((p[2] & 0x0F) << 8) | p[3];
    data->point.x = raw_y;
    data->point.y = (LCD_V_RES - 1) - raw_x;
    data->state = LV_INDEV_STATE_PRESSED;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1023,   /* inverted backlight: full duty = off */
    };
    if (ledc_timer_config(&bl_timer) != ESP_OK || ledc_channel_config(&bl_ch) != ESP_OK) {
        return NULL;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 2,   /* this panel latches data on the mode-2 edge (xiaozhi) */
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    pca9557_set(PCA9557_LCD_CS, false);   /* xiaozhi: SetOutputState(0, 0) at exactly this point */
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);   /* as xiaozhi drives this panel */
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, true, false);
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_H_RES,
            .ver_res = LCD_V_RES,
            .buffer_height = DRAW_BUF_LINES,
            .use_psram = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) {
        return NULL;
    }

    /* No esp_lcd_touch driver in this component set for the FT6336, so
     * register a plain LVGL pointer indev that polls it, as on the
     * FNK0104B. */
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, disp);
    lv_indev_set_read_cb(indev, touch_read_cb);
    *touch = indev;

    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    /* Inverted backlight (DISPLAY_BACKLIGHT_OUTPUT_INVERT in xiaozhi):
     * 100% brightness is duty 0. */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (100 - pct) * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);   /* SLPIN/SLPOUT; GRAM is kept */
}

/* ES8311 plays, ES7210 records, over one duplex I2S bus. The amp's
 * enable is not a GPIO: it is PCA9557 P1, driven from audio_power. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t dac_i2c = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    audio_codec_i2c_cfg_t adc_i2c = { .port = I2C_NUM_0, .addr = ES7210_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,   /* the amp enable is PCA9557 P1, see audio_power */
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&dac_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    es7210_codec_cfg_t adc_cfg = {
        .ctrl_if = adc_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *adc = es7210_codec_new(&adc_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void audio_power(bool on)
{
    /* NS4150B amp enable, as xiaozhi's EnableOutput does. */
    pca9557_set(PCA9557_PA_EN, on);
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk);
}

static esp_err_t power_off(void)
{
    /* The power key is a hardware latch through the AK8052 power chip;
     * no software power-off sequence is verified for this board. */
    return ESP_ERR_NOT_SUPPORTED;
}

static const muse_board_t s_board = {
    .name = "Lichuang Szpi ESP32-S3",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = false,
    .touch = true,
    .touch_talk = true,
    .diagonal_in = 2.0f,
    .talk_button = "boot",
    .talk_hint = { LV_ALIGN_TOP_LEFT, 8, 8 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .audio_power = audio_power,
    .mic_slot = 0,              /* MIC1; MIC2 is the second mic, MIC3 the unused reference */
    .poll_buttons = poll_buttons,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
