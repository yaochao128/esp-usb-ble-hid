#include <chrono>
#include <thread>

#include "logger.hpp"
#include "task.hpp"

#include "keyboard_device.hpp"
#include "xbox.hpp"

#include "ble.hpp"
#include "bsp.hpp"
#include "usb.hpp"
#include "keycodes.h"

// set to 1 to enable twirling the joysticks automatically (for testing) when
// there is no BLE device connected.
#define DEBUG_NO_BLE_TWIRL_JOYSTICKS 0
// set to 1 to enable pushing the buttons automatically (for testing) when there
// is no BLE device connected. Not recommended unless you want to annoy
// yourself.
#define DEBUG_NO_BLE_TEST_BUTTONS 0

using namespace std::chrono_literals;

/************* App Configuration ****************/

#if HAS_DISPLAY
static std::shared_ptr<Gui> gui;
#endif
static std::vector<uint8_t> hid_report_descriptor;
static std::shared_ptr<GamepadDevice> ble_gamepad;
static std::shared_ptr<KeyboardDevice> usb_keyboard;
static int battery_level_percent = 100;
static std::string serial_number = "";

struct KeyState {
  bool alt_pressed{false};
  bool ctrl_pressed{false};
  uint8_t last_keycode{0};
};
static KeyState key_state;

/********* BLE callbacks ***************/

/** Notification / Indication receiving handler callback */
void notifyCB(NimBLERemoteCharacteristic *pRemoteCharacteristic, uint8_t *pData, size_t length,
              bool isNotify) {
  // if it's the battery level characteristic, then store the battery level and
  // return.
  if (pRemoteCharacteristic->getUUID().equals(
          NimBLEUUID(espp::BatteryService::BATTERY_LEVEL_CHAR_UUID))) {
    battery_level_percent = pData[0];
    return;
  }
  // otherwise this is a HID input report

  // first check for keyboard combos
  if (length >= 8) {
    uint8_t modifiers = pData[0];
    uint8_t keycode = pData[2];

    key_state.alt_pressed = modifiers & (MOD_LEFT_ALT | MOD_RIGHT_ALT);
    key_state.ctrl_pressed = modifiers & (MOD_LEFT_CTRL | MOD_RIGHT_CTRL);
    key_state.last_keycode = keycode;

    if (key_state.alt_pressed && keycode == KEY_TAB) {
      printf("SEND F13\n");
      send_special_key(KEY_F13);
      return; // consume original keys
    }
    if (key_state.ctrl_pressed && keycode == KEY_SPACE) {
      printf("SEND F14\n");
      send_special_key(KEY_F14);
      return;
    }
    if (key_state.ctrl_pressed && keycode == KEY_ENTER) {
      printf("SEND F15\n");
      send_special_key(KEY_F15);
      return;
    }
  }
  // otherwise forward the keyboard report
  if (tud_mounted()) {
    usb_keyboard->send_report(pData, length);
    static auto &bsp = Bsp::get();
    static bool led_on = false;
    static auto on_color = espp::Rgb(0.0f, 0.0f, 1.0f); // use blue for BLE
    static auto off_color = espp::Rgb(0.0f, 0.0f, 0.0f);
    bsp.led(led_on ? on_color : off_color);
    led_on = !led_on;
  }
}

extern "C" void app_main(void) {
  espp::Logger logger({.tag = "ESP USB BLE HID", .level = espp::Logger::Verbosity::DEBUG});

  logger.info("Bootup");

  // MARK: BSP initialization
  auto &bsp = Bsp::get();

  // MARK: LED initialization
  bsp.initialize_led();
  bsp.led(espp::Rgb(0.0f, 0.0f, 0.0f));

  // MARK: Display initialization
#if HAS_DISPLAY
  logger.info("Display initialization");
  // initialize the LCD
  if (!bsp.initialize_lcd()) {
    logger.error("Failed to initialize LCD!");
    return;
  }
  // set the pixel buffer to be a full screen buffer
  static constexpr size_t pixel_buffer_size = bsp.lcd_width() * bsp.lcd_height();
  // initialize the LVGL display for the T-Dongle-S3
  if (!bsp.initialize_display(pixel_buffer_size)) {
    logger.error("Failed to initialize display!");
    return;
  }

  // initialize the gui
  logger.info("Making GUI");
  gui = std::make_shared<Gui>(Gui::Config{.log_level = espp::Logger::Verbosity::INFO});
  gui->set_label_text("");
#else  // HAS_DISPLAY
  logger.info("No display");
#endif // HAS_DISPLAY

  // MARK: BLE pairing timer (for use with button)
  espp::HighResolutionTimer ble_pairing_timer{
      {.name = "Pairing Timer", .callback = [&]() { start_ble_pairing_thread(notifyCB); }}};

  // MARK: Pairing button initialization
  // initialize the button, which we'll use to cycle the rotation of the display
  logger.info("Initializing the button");
  auto on_button_pressed = [&](const auto &event) {
    if (event.active) {
      // start ble pairing timer
      ble_pairing_timer.oneshot(3'000'000); // 3 seconds
    } else {
      // cancel the ble pairing timer
      ble_pairing_timer.stop();
    }
  };
  bsp.initialize_button(on_button_pressed);

  // MARK: Gamepad initialization
  usb_keyboard = std::make_shared<KeyboardDevice>();
  ble_gamepad = std::make_shared<Xbox>();

  // MARK: USB initialization
  logger.info("USB initialization");
#if DEBUG_USB
  set_gui(gui);
#endif // DEBUG_USB
  start_usb_keyboard(usb_keyboard);

  // MARK: BLE initialization
  logger.info("BLE initialization");
  std::string device_name = "Switch";
  init_ble(device_name);

  logger.info("Scanning for peripherals");
  start_ble_reconnection_thread(notifyCB);

  // Loop here until we find a device we want to connect to
  while (true) {
    // sleep for a bit
    std::this_thread::sleep_for(1s);

    // update the display if we have one
#if HAS_DISPLAY
    // show the usb icon if the USB is mounted
    gui->set_usb_connected(tud_mounted());
    // show the BLE icon if the BLE subsystem is subscribed (receiving data)
    gui->set_ble_connected(is_ble_subscribed());
#endif // HAS_DISPLAY

    // if we're subscribed, then don't do anything else
    if (is_ble_subscribed()) {
      // if we haven't gotten the serial number, then get that and save it
      if (serial_number.empty()) {
        serial_number = get_connected_client_serial_number();
#if HAS_DISPLAY
        gui->set_label_text(serial_number);
#endif // HAS_DISPLAY
      }
      continue;
    }
    // make sure to reset the connected device serial number
    serial_number = "";
#if HAS_DISPLAY
    gui->set_label_text(serial_number);
#endif // HAS_DISPLAY

#if DEBUG_NO_BLE_TWIRL_JOYSTICKS
    // otherwise, just twirl the joysticks
    static constexpr int num_segments = 16;
    static int index = 0;
    float angle = 2.0f * M_PI * (index % num_segments) / (num_segments);

    GamepadInputs inputs{};
    // joystick inputs are in the range [-1, 1] float
    inputs.left_joystick.x = sin(angle);
    inputs.left_joystick.y = cos(angle);
    inputs.right_joystick.x = cos(angle);
    inputs.right_joystick.y = sin(angle);

#if DEBUG_NO_BLE_TEST_BUTTONS
    // NOTE: this is not recommended since it's annoying when it works, but left
    // in for debugging when it doesn't work.
    static constexpr int num_buttons = 15;
    inputs.set_button(index % num_buttons, true);
#endif // DEBUG_NO_BLE_TEST_BUTTONS

    index++;

    uint8_t empty_report[8] = {0};
    if (tud_mounted()) {
      usb_keyboard->send_report(empty_report, sizeof(empty_report));
    } else {
      bsp.led(espp::Rgb(1.0f, 0.0f, 0.0f));
    }
#endif // DEBUG_NO_BLE_TWIRL_JOYSTICKS
  }
}
