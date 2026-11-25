# Reference links

The board maker's material and community write-ups used while building this. Not copied here; follow the links.

- Elecrow's repository (pins, factory firmware and its source, examples, datasheets, schematic, enclosure models): <https://github.com/Elecrow-RD/CrowPanel-2.1inch-HMI-ESP32-Rotary-Display-480-480-IPS-Round-Touch-Knob-Screen>
- Product wiki (specs, ports): <https://www.elecrow.com/wiki/CrowPanel_2.1inch-HMI_ESP32_Rotary_Display_480_IPS_Round_Touch_Knob_Screen.html>
- Arduino course index: <https://www.elecrow.com/wiki/2.1_inch_Rotary_Display_Arduino_Course_Index.html>
  - Lesson 01, hello world (board settings): <https://www.elecrow.com/wiki/2.1_Lesson01-Print_Hello_World.html>
  - Lesson 02, encoder and brightness: <https://www.elecrow.com/wiki/2.1_Lesson02_Encoder_Brightness.html>
  - Lesson 04, serial port: <https://www.elecrow.com/wiki/2.1_Lesson04-Serial_port_usage.html>
  - Lesson 05, USB: <https://www.elecrow.com/wiki/2.1_Lesson05-USB2.0.html>
- Arduino LVGL rotary guide (display, touch, knob): <https://www.elecrow.com/wiki/2.1_Arduino_LVGL_Rotary_Guide.html>
- ESPHome course: <https://www.elecrow.com/wiki/RotaryScreen_2.1inch_ESPHome_5_Lesson_Course_Guide.html>
- Makerguides, getting started: <https://www.makerguides.com/getting-started-with-crowpanel-2-1inch-hmi-esp32-rotary-display/>
- espboards.dev review: <https://www.espboards.dev/blog/crowpanel-2-1-inch-rotary-display-review/>
- LVGL 9 docs: <https://docs.lvgl.io/9.6/>
- Open-Meteo forecast and geocoding APIs: <https://open-meteo.com/en/docs>

Arduino IDE settings from Elecrow's lesson 01, if you'd rather not use PlatformIO: board "ESP32S3 Dev Module", USB CDC On Boot enabled, USB Mode "Hardware CDC and JTAG", PSRAM "OPI PSRAM", partition scheme `elecrow_s3` (from their repo; the default 3 MB app partition is too small for this firmware).
