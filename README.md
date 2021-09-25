ESP32 Bluetooth Speaker
======================

This project is based on the [ESP-IDF A2DP-SINK demo](https://github.com/espressif/esp-idf/tree/v4.3.1/examples/bluetooth/bluedroid/classic_bt/a2dp_sink) with a few small changes in order to allow using it
for a real bluetooth speaker.

The default settings assume the following I2S connections:

| ESP pin   | I2S signal   |
| :-------- | :----------- |
| GPIO22    | LRCK         |
| GPIO25    | DATA         |
| GPIO26    | BCK          |

This project is intended to be built using [PlatformIO](https://platformio.org/).

**Important:**

The currently selected and latest PlatformIO platform `espressif32@~3.3.2` uses ESP-IDF version 4.3.0,
which misses an important fix for volume control, which is only available starting with ESP-IDF 4.3.1. Please manually patch the respective files
according to [this commit](https://github.com/espressif/esp-idf/commit/fd83938f39759fc9843929f5dd4953d7bc0014d6).