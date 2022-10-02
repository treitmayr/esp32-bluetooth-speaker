ESP32 Bluetooth Speaker
======================

This project is based on the [ESP-IDF A2DP-SINK demo](https://github.com/espressif/esp-idf/tree/v4.3.1/examples/bluetooth/bluedroid/classic_bt/a2dp_sink) with a number of changes:

* Over-the-air updates with configurable update server (currently only via http, not https)
* Send log out to syslog server
* Dithering to improve audio quality at lower volumes (kicking in at half the maximum volume)
* Volume control (but initial volume still needs fixes)

The first two items are intended for putting the ESP32+DAC inside a closed speaker, but still
be able to update it and observe its operation.

The default settings assume the following I2S connections:

| ESP pin   | I2S signal   |
| :-------- | :----------- |
| GPIO22    | LRCK         |
| GPIO25    | DATA         |
| GPIO26    | BCK          |

This project is intended to be built using [PlatformIO](https://platformio.org/).
