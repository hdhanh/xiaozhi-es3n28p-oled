#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_ssd1306.h>
#include <esp_lcd_panel_sh1106.h>
#include <wifi_station.h>
#include "application.h"
#include "codecs/es8311_audio_codec.h"
#include "button.h"
#include "display/oled_display.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "system_reset.h"
#include "wifi_board.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "config.h"
#ifdef CONFIG_SD_CARD_MMC_INTERFACE
#include "sdmmc.h"
#elif defined(CONFIG_SD_CARD_SPI_INTERFACE)
#include "sdspi.h"
#endif

#define TAG "XiaozhiAIIoTEs3n28p"

class XiaozhiAIIoTEs3n28p : public WifiBoard {
 private:
  Button boot_button_;
  Display* display_;
  i2c_master_bus_handle_t i2c_bus_;  // Single shared I2C bus for both Audio and Display
  esp_lcd_panel_io_handle_t panel_io_ = nullptr;
  esp_lcd_panel_handle_t panel_ = nullptr;

  void InitializeI2c() {
    // Single I2C bus shared by Audio Codec (ES8311) and OLED Display (SH1106)
    i2c_master_bus_config_t i2c_bus_cfg = {
      .i2c_port = DISPLAY_I2C_NUM,
      .sda_io_num = DISPLAY_SDA_PIN,
      .scl_io_num = DISPLAY_SCL_PIN,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .intr_priority = 0,
      .trans_queue_depth = 0,
      .flags = {
        .enable_internal_pullup = 1,
      },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    ESP_LOGI(TAG, "I2C bus initialized: SDA=%d, SCL=%d", DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
    
    // Scan I2C bus to find all devices
    ESP_LOGI(TAG, "Scanning I2C bus for devices...");
    bool found_audio = false;
    bool found_display = false;
    
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
      i2c_master_dev_handle_t dev;
      i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
      };
      if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev) == ESP_OK) {
        uint8_t data;
        if (i2c_master_receive(dev, &data, 1, 100) == ESP_OK) {
          ESP_LOGI(TAG, "✓ Found I2C device at 0x%02X", addr);
          if (addr == 0x18) {
            ESP_LOGI(TAG, "  → ES8311 Audio Codec detected");
            found_audio = true;
          } else if (addr == 0x3C || addr == 0x3D) {
            ESP_LOGI(TAG, "  → OLED Display detected");
            found_display = true;
          }
        }
        i2c_master_bus_rm_device(dev);
      }
    }
    
    if (!found_audio) {
      ESP_LOGW(TAG, "⚠ ES8311 Audio Codec NOT found at 0x18!");
    }
    if (!found_display) {
      ESP_LOGW(TAG, "⚠ OLED Display NOT found at 0x3C!");
    }
  }

  void InitializeOledDisplay() {
    ESP_LOGI(TAG, "Initializing OLED Display SH1106 128x64");
    
    // I2C panel IO config
    esp_lcd_panel_io_i2c_config_t io_config = {
      .dev_addr = 0x3C,
      .on_color_trans_done = nullptr,
      .user_ctx = nullptr,
      .control_phase_bytes = 1,
      .dc_bit_offset = 6,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .flags = {
        .dc_low_on_data = 0,
        .disable_control_phase = 0,
      },
      .scl_speed_hz = 400 * 1000,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(i2c_bus_, &io_config, &panel_io_));

    ESP_LOGI(TAG, "Install SH1106 driver");
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.bits_per_pixel = 1;

    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
      .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
    };
    panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
    ESP_LOGI(TAG, "SH1106 driver installed");
#else
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
    ESP_LOGI(TAG, "SSD1306 driver installed");
#endif

    // Reset and initialize display
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
    if (esp_lcd_panel_init(panel_) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to initialize display");
      return;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

    // Turn display on
    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    ESP_LOGI(TAG, "✓ OLED Display initialized successfully!");
  }

  void InitializeButtons() {
    boot_button_.OnMultipleClick([this]() {
      ResetWifiConfiguration();
    }, 5);

    boot_button_.OnClick([this]() {
      auto &app = Application::GetInstance();
      if (app.GetDeviceState() == kDeviceStateStarting &&
          !WifiStation::GetInstance().IsConnected()) {
        ResetWifiConfiguration();
      }
      app.ToggleChatState();
    });
  }

  void InitializeTools() {
    static LampController lamp(BUILTIN_LED_GPIO);
  }

 public:
  XiaozhiAIIoTEs3n28p(): boot_button_(BOOT_BUTTON_GPIO)
  {
    InitializeI2c();
    InitializeOledDisplay();
    InitializeButtons();
    InitializeTools();
  }

  virtual Led *GetLed() override {
    static SingleLed led(BUILTIN_LED_GPIO);
    return &led;
  }

  virtual AudioCodec* GetAudioCodec() override {
    static Es8311AudioCodec audio_codec(i2c_bus_, AUDIO_CODEC_I2C_NUM,
      AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK,
      AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
      AUDIO_CODEC_ES8311_ADDR, true, true);
    return &audio_codec;
  }

  virtual Display *GetDisplay() override { return display_; }

#ifdef CONFIG_SD_CARD_MMC_INTERFACE
  virtual SdCard* GetSdCard() override {
#ifdef CARD_SDMMC_BUS_WIDTH_4BIT
    static SdMMC sdmmc(CARD_SDMMC_CLK_GPIO,
                       CARD_SDMMC_CMD_GPIO,
                       CARD_SDMMC_D0_GPIO,
                       CARD_SDMMC_D1_GPIO,
                       CARD_SDMMC_D2_GPIO,
                       CARD_SDMMC_D3_GPIO);
#else
    static SdMMC sdmmc(CARD_SDMMC_CLK_GPIO,
                       CARD_SDMMC_CMD_GPIO,
                       CARD_SDMMC_D0_GPIO);
#endif
    return &sdmmc;
  }
#endif
#ifdef CONFIG_SD_CARD_SPI_INTERFACE
  virtual SdCard* GetSdCard() override {
    static SdSPI sdspi(CARD_SPI_MISO_GPIO,
                       CARD_SPI_MOSI_GPIO,
                       CARD_SPI_SCLK_GPIO,
                       CARD_SPI_CS_GPIO);
    return &sdspi;
  }
#endif

};

DECLARE_BOARD(XiaozhiAIIoTEs3n28p);
