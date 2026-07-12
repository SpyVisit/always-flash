// LGFX_Config.h — LovyanGFX panel config for Waveshare ESP32-S3-LCD-1.47
// Panel: ST7789, 172x320 (portrait). Pins per Waveshare wiki.
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;

public:
  LGFX() {
    { // SPI bus
      auto c = _bus.config();
      c.spi_host   = SPI2_HOST;
      c.spi_mode   = 0;
      c.freq_write = 40000000;
      c.freq_read  = 16000000;
      c.pin_sclk   = 40;   // SCLK
      c.pin_mosi   = 45;   // MOSI
      c.pin_miso   = -1;   // not used
      c.pin_dc     = 41;   // LCD_DC
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    { // panel
      auto c = _panel.config();
      c.pin_cs   = 42;     // LCD_CS
      c.pin_rst  = 39;     // LCD_RST
      c.pin_busy = -1;
      c.panel_width   = 172;
      c.panel_height  = 320;
      c.offset_x      = 34;   // 172-wide ST7789 is centered in 240 → (240-172)/2
      c.offset_y      = 0;
      c.offset_rotation = 0;
      c.readable   = false;
      c.invert     = true;    // ST7789 usually needs inversion
      c.rgb_order  = false;
      c.dlen_16bit = false;
      c.bus_shared = false;   // SD is on SDMMC, not this SPI bus
      _panel.config(c);
    }
    { // backlight (LCD_BL = GPIO48)
      auto c = _light.config();
      c.pin_bl      = 48;
      c.invert      = false;
      c.freq        = 44100;
      c.pwm_channel = 7;
      _light.config(c);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};
