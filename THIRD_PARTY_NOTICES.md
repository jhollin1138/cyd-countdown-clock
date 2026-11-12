# Third-party software

The Countdown Clock firmware is licensed under the PolyForm Noncommercial License 1.0.0 (see `LICENSE.md`). That license covers only this project's own code and documentation. Built firmware also contains the following components, which keep their own licenses. All of them allow use in this project, including in noncommercial binary releases.

| Component | License | Source |
|---|---|---|
| Arduino core for ESP32 / ESP-IDF | LGPL-2.1 (Arduino core), Apache-2.0 (ESP-IDF) | https://github.com/espressif/arduino-esp32 |
| TFT_eSPI | FreeBSD (BSD-2-Clause) and MIT parts | https://github.com/Bodmer/TFT_eSPI |
| TJpg_Decoder (includes TJpgDec by ChaN) | FreeBSD-style / ChaN's permissive license | https://github.com/Bodmer/TJpg_Decoder |
| WiFiManager | MIT | https://github.com/tzapu/WiFiManager |
| ArduinoJson | MIT | https://github.com/bblanchon/ArduinoJson |
| minimp3 (bundled in `lib/minimp3`) | CC0 1.0 (public domain) | https://github.com/lieff/minimp3 |

Weather and place search data come from [Open-Meteo](https://open-meteo.com) under CC BY 4.0. Open-Meteo's free API is for non-commercial use.

The full license texts are included with each library's source. PlatformIO downloads them into `.pio/libdeps/` when you build. Because this project's complete source is published, you can rebuild the firmware with modified versions of the LGPL-licensed Arduino core.
