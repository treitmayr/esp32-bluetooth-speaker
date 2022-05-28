ESP32 Bluetooth Speaker
======================

**NOTE ON THIS BRANCH:** Unfortunately the ESP32 has too little memory to support OTA for the
Bluetooth A2DP application. Therefore this branch just exists for saving the (abendoned) work
already done into that direction.

This project is based on the [ESP-IDF A2DP-SINK demo](https://github.com/espressif/esp-idf/tree/v4.3.1/examples/bluetooth/bluedroid/classic_bt/a2dp_sink) with a few small changes in order to allow using it
for a real bluetooth speaker.

The default settings assume the following I2S connections:

| ESP pin   | I2S signal   |
| :-------- | :----------- |
| GPIO22    | LRCK         |
| GPIO25    | DATA         |
| GPIO26    | BCK          |

This project is intended to be built using [PlatformIO](https://platformio.org/).
