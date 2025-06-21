#include "keyboard_device.hpp"

extern "C" {
#include <tusb.h>
}

const DeviceInfo KeyboardDevice::device_info{
    .vid = 0xCafe,
    .pid = 0x4000,
    .bcd = 0x0100,
    .usb_bcd = 0x0200,
    .manufacturer_name = "Finger563",
    .product_name = "Keyboard",
    .serial_number = "0001",
};

static const uint8_t report_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07,
    0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00,
    0x29, 0x65, 0x81, 0x00, 0xC0
};

KeyboardDevice::KeyboardDevice() : GamepadDevice("Keyboard") {}

const DeviceInfo &KeyboardDevice::get_device_info() const { return device_info; }

std::vector<uint8_t> KeyboardDevice::get_report_descriptor() const {
  return std::vector<uint8_t>(report_desc, report_desc + sizeof(report_desc));
}

void KeyboardDevice::set_report_data(uint8_t, const uint8_t *data, size_t len) {
  if (len > sizeof(report_)) len = sizeof(report_);
  memcpy(report_, data, len);
}

std::vector<uint8_t> KeyboardDevice::get_report_data(uint8_t) const {
  return std::vector<uint8_t>(report_, report_ + sizeof(report_));
}

bool KeyboardDevice::send_report(const uint8_t *data, size_t len) {
  if (len < 8) return false;
  return tud_hid_keyboard_report(0, data[0], data + 2);
}
