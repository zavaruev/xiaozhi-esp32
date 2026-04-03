#include "wifi_board.h"
#include "esp_wifi.h"
#include <cstring>
#include "settings.h"
#include "codecs/es8311_audio_codec.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "display/emote_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/circular_strip.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_ili9341.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>

#define TAG "FreenoveBoard"

// Init ili9341 by custom cmd
static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0xC8, (uint8_t []){0xFF, 0x93, 0x42}, 3, 0},
    {0xC0, (uint8_t []){0x0E, 0x0E}, 2, 0},
    {0xC5, (uint8_t []){0xD0}, 1, 0},
    {0xC1, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F}, 15, 0},
    {0xE1, (uint8_t []){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F}, 15, 0},
    {0xB1, (uint8_t []){00, 0x1B}, 2, 0},
    {0x36, (uint8_t []){0x08}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0xB7, (uint8_t []){0x06}, 1, 0},
    {0x11, (uint8_t []){0}, 0x80, 0},
    {0x29, (uint8_t []){0}, 0x80, 0},
    {0, (uint8_t []){0}, 0xff, 0},
};

class Ft6336 : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };
    
    Ft6336(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        uint8_t chip_id = ReadReg(0xA3);
        ESP_LOGI("Ft6336", "Get chip ID: 0x%02X", chip_id);
    }

    void UpdateTouchPoint() {
        uint8_t buffer[6];
        ReadRegs(0x02, buffer, 6);
        tp_.num = buffer[0] & 0x0F;
        tp_.x = ((buffer[1] & 0x0F) << 8) | buffer[2];
        tp_.y = ((buffer[3] & 0x0F) << 8) | buffer[4];
    }

    inline const TouchPoint_t& GetTouchPoint() { return tp_; }

private:
    TouchPoint_t tp_;
};

class FreenoveBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Display* display_ = nullptr;
    CircularStrip* led_ring_ = nullptr;
    Ft6336* ft6336_ = nullptr;
    esp_timer_handle_t touchpad_timer_;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    bool usb_detected_ = false;

    void InitializeAdc() {
        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = ADC_UNIT_1,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));

        adc_oneshot_chan_cfg_t config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        // Freenove S3 2.8" board uses GPIO 9 (ADC1_CH8) for battery sensing
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_8, &config));
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = { .enable_internal_pullup = 1 },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // I2C Scanner to log all devices
        ESP_LOGI(TAG, "I2C scanning...");
        for (int addr = 1; addr < 127; addr++) {
            if (i2c_master_probe(i2c_bus_, addr, 100) == ESP_OK) {
                ESP_LOGI(TAG, "Found device at 0x%02X", addr);
            }
        }
        
        // Manual Init for ES7243 (Address 0x13) to ensure hearing works
        uint8_t es7243_addr = 0x13;
        i2c_device_config_t dev_cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = es7243_addr, .scl_speed_hz = 100000 };
        i2c_master_dev_handle_t es7243_handle;
        if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &es7243_handle) == ESP_OK) {
            uint8_t es7243_init[] = { 0x01, 0x00 }; // Power up
            i2c_master_transmit(es7243_handle, es7243_init, 2, 1000);
            ESP_LOGI(TAG, "ES7243 mic initialized");
        }
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_SPI_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 24 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        const ili9341_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_SPI_RESET_PIN;
        panel_config.flags.reset_active_high = 0;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void *)&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
        
#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT, GetBacklight(), GetAudioCodec());
#else
        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
    }

    void InitializeTouch() {
        ft6336_ = new Ft6336(i2c_bus_, TOUCH_I2C_ADDRESS);
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto tp = (Ft6336*)arg;
                tp->UpdateTouchPoint();
                auto& touch = tp->GetTouchPoint();
                if (touch.num > 0) {
                    Application::GetInstance().ToggleChatState();
                }
            },
            .arg = ft6336_,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "touch_timer",
        };
        esp_timer_create(&timer_args, &touchpad_timer_);
        esp_timer_start_periodic(touchpad_timer_, 100 * 1000);
    }

public:
    FreenoveBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeAdc();
        InitializeDisplay();
        InitializeTouch();
        GetBacklight()->RestoreBrightness();
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        int r = 0;
        adc_oneshot_read(adc_handle_, ADC_CHANNEL_8, &r);

        // Freenove S3 2.8" uses a 2:1 divider. 12dB atten (~3.1V FS) gives ~1.51mV/bit with 2:1 divider.
        // We'll use a slightly safer 0.00175f multiplier based on empirical evidence for this board.
        float voltage = r * 0.00175f;
        ESP_LOGI("FreenoveBoard", "Battery ADC: %d, Voltage: %.2fV", r, voltage);
        
        // USB Detection: If voltage > 4.3V, it's typically powered by USB/Charging
        if (voltage > 4.3f) {
            usb_detected_ = true;
            charging = true;
            discharging = false;
            level = 100;
        } else {
            usb_detected_ = false;
            charging = false;
            discharging = true;
            
            // Battery voltage range: 3.3V (0%) to 4.2V (100%)
            if (voltage > 4.2f) level = 100;
            else if (voltage < 3.3f) level = 0;
            else level = (int)((voltage - 3.4f) * 125.0f); // 3.4V-4.2V range for better accuracy
        }
        return true;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Led* GetLed() override {
        if (led_ring_ == nullptr) {
            led_ring_ = new CircularStrip(BUILTIN_LED_GPIO, 1); // Maintain original CircularStrip beauty
        }
        return led_ring_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, true);
        return &audio_codec;
    }
};

DECLARE_BOARD(FreenoveBoard);