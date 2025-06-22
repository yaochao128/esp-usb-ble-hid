#pragma once

#include <cstdint>
#include <vector>

#include "logger.hpp"

#include "gamepad_device.hpp"
#include "keyboard_device.hpp"

#include "bsp.hpp"

extern "C" {
#include <class/hid/hid_device.h>
#include <tinyusb.h>
#include <tusb.h>
}

void start_usb_keyboard(const std::shared_ptr<KeyboardDevice> &keyboard_device);
bool send_hid_report(uint8_t report_id, const std::vector<uint8_t> &report);
bool send_special_key(uint8_t code);
void stop_usb_keyboard();

// debugging

#if HAS_DISPLAY

// Set this to 1 to turn on debugging for USB using the GUI
#define DEBUG_USB 0

#if DEBUG_USB
#include "gui.hpp"
void set_gui(std::shared_ptr<Gui> gui_ptr);
#endif // DEBUG_USB

#endif // HAS_DISPLAY
