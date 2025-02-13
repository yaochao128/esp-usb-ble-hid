# ESP USB BLE HID

This repository contains example code for using an ESP32s3 to act as a USB-BLE
HID bridge. You would run this code for instance on a QtPy ESP32s3, connected to
a computer or other device which is a USB HID host. The QtPy / this code would
then start a BLE GATT Client to connect to a BLE HID device (this example
targets a gamepad), and will allow the wireless HID device (gamepad) to talk to
the HID Host.

## Cloning

Since this repo contains a submodule, you need to make sure you clone it
recursively, e.g. with:

``` sh
git clone --recurse-submodules https://github.com/finger563/esp-usb-ble-hid
```

Alternatively, you can always ensure the submodules are up to date after cloning
(or if you forgot to clone recursively) by running:

``` sh
git submodule update --init --recursive
```

## Build and Flash

Build the project and flash it to the board, then run monitor tool to view serial output:

```
idf.py -p PORT flash monitor
```

(Replace PORT with the name of the serial port to use.)

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.

## Output

