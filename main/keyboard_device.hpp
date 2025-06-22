#pragma once

#include <vector>
#include "gamepad_device.hpp"

class KeyboardDevice : public GamepadDevice {
public:
  KeyboardDevice();

  const DeviceInfo &get_device_info() const override;
  uint8_t get_input_report_id() const override { return 1; }
  std::vector<uint8_t> get_report_descriptor() const override;
  void set_report_data(uint8_t report_id, const uint8_t *data, size_t len) override;
  std::vector<uint8_t> get_report_data(uint8_t report_id) const override;

  bool send_report(const uint8_t *data, size_t len);

private:
  static const DeviceInfo device_info;
  uint8_t report_[8] = {0};
};
