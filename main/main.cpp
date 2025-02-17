#include <chrono>
#include <thread>

#include <driver/gpio.h>

#include "color.hpp"
#include "logger.hpp"
#include "rmt.hpp"
#include "task.hpp"

extern "C" {
#include <tinyusb.h>
#include <class/hid/hid_device.h>
#include <tusb.h>
}

#include "hid-rp-gamepad.hpp"
#include "hid_service.hpp"
#include <NimBLEDevice.h>

using namespace std::chrono_literals;

static uint32_t scanTimeMs = 5000; // scan time in milliseconds, 0 = scan forever

static NimBLEUUID service_uuid(espp::HidService::SERVICE_UUID);
static NimBLEUUID report_map_uuid(espp::HidService::REPORT_MAP_UUID);
static NimBLEUUID input_uuid(espp::HidService::REPORT_UUID);

/************* Gamepad Configuration ****************/

static constexpr uint8_t input_report_id = 1;
static constexpr size_t num_buttons = 15;
static constexpr int joystick_min = 0;
static constexpr int joystick_max = 65535;
static constexpr int trigger_min = 0;
static constexpr int trigger_max = 1023;
using GamepadInput =
  espp::GamepadInputReport<num_buttons, std::uint16_t, std::uint16_t, joystick_min,
                           joystick_max, trigger_min, trigger_max, input_report_id>;
static GamepadInput gamepad_input_report;

static constexpr uint8_t battery_report_id = 4;
using BatteryReport = espp::XboxBatteryInputReport<battery_report_id>;
static BatteryReport battery_input_report;

static constexpr uint8_t led_output_report_id = 2;
static constexpr size_t num_leds = 4;
using GamepadLeds = espp::GamepadLedOutputReport<num_leds, led_output_report_id>;
static GamepadLeds gamepad_leds_report;

static constexpr uint8_t rumble_output_report_id = 3;
using RumbleReport = espp::XboxRumbleOutputReport<rumble_output_report_id>;
static RumbleReport gamepad_rumble_report;

using namespace hid::page;
using namespace hid::rdf;
// @brief HID report descriptor
static const auto hid_report_descriptor = descriptor(usage_page<generic_desktop>(), usage(generic_desktop::GAMEPAD),
                                              collection::application(gamepad_input_report.get_descriptor(),
                                                                      gamepad_rumble_report.get_descriptor(),
                                                                      battery_input_report.get_descriptor(),
                                                                      gamepad_leds_report.get_descriptor()));

/************* TinyUSB descriptors ****************/

#define TUSB_DESC_TOTAL_LEN      (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_INOUT_DESC_LEN)
static_assert(CFG_TUD_HID >= 1, "CFG_TUD_HID must be at least 1");

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
static tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0000,
    // Use Interface Association Descriptor (IAD) for CDC
    // As required by USB Specs IAD's subclass must be common class (2) and protocol must be IAD (1)
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,

    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = 0x0000,
    .idProduct = 0x000,
    .bcdDevice = 0x0100,

    // Index of manufacturer description string
    .iManufacturer = 0x01,
    // Index of product description string
    .iProduct = 0x02,
    // Index of serial number description string
    .iSerialNumber = 0x03,
    // Number of configurations
    .bNumConfigurations = 0x01};

// @brief String descriptor
const char* hid_string_descriptor[5] = {
    // array of pointer to string descriptors
    (char[]){0x09, 0x04},     // 0: is supported language is English (0x0409)
    "Finger563",              // 1: Manufacturer
    "ESP USB BLE HID",        // 2: Product
    "1337",                   // 3: Serials, should use chip ID
    "USB HID interface",      // 4: HID
};

// @brief Configuration descriptor
// This is a simple configuration descriptor that defines 1 configuration and 1
// HID interface
static const uint8_t hid_configuration_descriptor[] = {
    // Configuration number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, 0x00, 100),

    // Interface number, string index, boot protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_INOUT_DESCRIPTOR(0, 4, false,
                       hid_report_descriptor.size(),
                       0x01, // out EP
                       0x81, // in EP
                       CFG_TUD_HID_EP_BUFSIZE,
                       1),
};

/********* TinyUSB HID callbacks ***************/

// Invoked when received GET HID REPORT DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
extern "C" uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    // We use only one interface and one HID report descriptor, so we can ignore parameter 'instance'
    return hid_report_descriptor.data();
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
extern "C" uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    (void) instance;
    (void) report_type;
    (void) reqlen;

    switch (report_type) {
    case HID_REPORT_TYPE_INPUT: {
      const auto report_data = gamepad_input_report.get_report();
      // copy the report data (vector) into the buffer
      std::copy(report_data.begin(), report_data.end(), buffer);
      // return the size of the report
      return report_data.size();
    }
    default:
      return 0;
    }
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
extern "C" void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
}

// Invoked when sent REPORT successfully to host
// Application can use this to send the next report
// Note: For composite reports, report[0] is report ID
extern "C" void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *reprot, uint16_t len) {
}

/********* BLE callbacks ***************/

/** Notification / Indication receiving handler callback */
void notifyCB(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    std::string str  = (isNotify == true) ? "Notification" : "Indication";
    str             += " from ";
    str             += pRemoteCharacteristic->getClient()->getPeerAddress().toString();
    str             += ": Service = " + pRemoteCharacteristic->getRemoteService()->getUUID().toString();
    str             += ", Characteristic = " + pRemoteCharacteristic->getUUID().toString();
    // str             += ", Value = " + std::string((char*)pData, length);
    fmt::print("{}\n", str);
    std::vector<uint8_t> data(pData, pData + length);
    gamepad_input_report.set_data(data);
    // fmt::print("\t{}\n", gamepad_input_report);
    // send the report via tiny usb
    if (tud_mounted()) {
      auto report = gamepad_input_report.get_report();
      tud_hid_report(input_report_id, report.data(), report.size());
    }
}

class ClientCallbacks : public NimBLEClientCallbacks {
  espp::Logger logger = espp::Logger({.tag = "BLE Client Callbacks", .level = espp::Logger::Verbosity::INFO});
    void onConnect(NimBLEClient* pClient) override {
        logger.info("connected to: {}", pClient->getPeerAddress().toString());
        static constexpr bool async = true;
        pClient->secureConnection(async);
    }

    void onDisconnect(NimBLEClient* pClient, int reason) override {
        logger.info("{} Disconnected, reason = {} - Starting scan", pClient->getPeerAddress().toString(), reason);
        NimBLEDevice::getScan()->start(scanTimeMs);
    }

    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        if (!connInfo.isEncrypted()) {
            logger.error("Encrypt connection failed - disconnecting");
            /** Find the client with the connection handle provided in connInfo */
            NimBLEDevice::getClientByHandle(connInfo.getConnHandle())->disconnect();
            return;
        } else {
            logger.info("Encryption successful!");
        }
    }
} clientCallbacks;

class ScanCallbacks : public NimBLEScanCallbacks {
  espp::Logger logger = espp::Logger({.tag = "BLE Scan Callbacks", .level = espp::Logger::Verbosity::INFO});
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        logger.info("Advertised Device found: {}", advertisedDevice->toString());
        if (advertisedDevice->isAdvertisingService(service_uuid)) {
            logger.info("Found Our Device");

            /** Async connections can be made directly in the scan callbacks */
            auto pClient = NimBLEDevice::getDisconnectedClient();
            if (!pClient) {
                pClient = NimBLEDevice::createClient(advertisedDevice->getAddress());
                if (!pClient) {
                    logger.error("Failed to create client");
                    return;
                }
            }

            pClient->setClientCallbacks(&clientCallbacks, false);
            if (!pClient->connect(true, true, false)) { // delete attributes, async connect, no MTU exchange
                NimBLEDevice::deleteClient(pClient);
                logger.error("Failed to connect");
                return;
            }
        }
    }

    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        printf("Scan Ended\n");
        NimBLEDevice::getScan()->start(scanTimeMs);
    }
} scanCallbacks;

extern "C" void app_main(void) {
  espp::Logger logger({.tag = "ESP USB BLE HID", .level = espp::Logger::Verbosity::DEBUG});

  logger.info("Bootup");

  // MARK: LED initialization
  static constexpr int led_power_pin = 38; // Neopixel power pin on QtPy ESP32s3
  gpio_config_t power_pin_config = {
    .pin_bit_mask = (1ULL << led_power_pin),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&power_pin_config);
  // turn on the power
  gpio_set_level((gpio_num_t)led_power_pin, 1);

  int led_encoder_state = 0;
  static constexpr int WS2812_FREQ_HZ = 10000000;
  static constexpr int MICROS_PER_SEC = 1000000;
  auto led_encoder = std::make_unique<espp::RmtEncoder>(espp::RmtEncoder::Config{
      // NOTE: since we're using the 10MHz RMT clock, we can use the pre-defined
      //       ws2812_10mhz_bytes_encoder_config
      .bytes_encoder_config = espp::RmtEncoder::ws2812_10mhz_bytes_encoder_config,
        .encode = [&led_encoder_state](auto channel, auto *copy_encoder, auto *bytes_encoder,
                                       const void *data, size_t data_size,
                                       rmt_encode_state_t *ret_state) -> size_t {
          // divide by 2 since we have both duration0 and duration1 in the reset code
          static uint16_t reset_ticks =
            WS2812_FREQ_HZ / MICROS_PER_SEC * 50 / 2; // reset code duration defaults to 50us
          static rmt_symbol_word_t led_reset_code = (rmt_symbol_word_t){
            .duration0 = reset_ticks,
            .level0 = 0,
            .duration1 = reset_ticks,
            .level1 = 0,
          };
          rmt_encode_state_t session_state = RMT_ENCODING_RESET;
          int state = RMT_ENCODING_RESET;
          size_t encoded_symbols = 0;
          switch (led_encoder_state) {
          case 0: // send RGB data
            encoded_symbols +=
              bytes_encoder->encode(bytes_encoder, channel, data, data_size, &session_state);
            if (session_state & RMT_ENCODING_COMPLETE) {
              led_encoder_state = 1; // switch to next state when current encoding session finished
            }
            if (session_state & RMT_ENCODING_MEM_FULL) {
              state |= RMT_ENCODING_MEM_FULL;
              goto out; // yield if there's no free space for encoding artifacts
            }
            // fall-through
          case 1: // send reset code
            encoded_symbols += copy_encoder->encode(copy_encoder, channel, &led_reset_code,
                                                    sizeof(led_reset_code), &session_state);
            if (session_state & RMT_ENCODING_COMPLETE) {
              led_encoder_state = RMT_ENCODING_RESET; // back to the initial encoding session
              state |= RMT_ENCODING_COMPLETE;
            }
            if (session_state & RMT_ENCODING_MEM_FULL) {
              state |= RMT_ENCODING_MEM_FULL;
              goto out; // yield if there's no free space for encoding artifacts
            }
          }
      out:
          *ret_state = static_cast<rmt_encode_state_t>(state);
          return encoded_symbols;
        },
        .del = [](auto *base_encoder) -> esp_err_t {
          // we don't have any extra resources to free, so just return ESP_OK
          return ESP_OK;
        },
        .reset = [&led_encoder_state](auto *base_encoder) -> esp_err_t {
          // all we have is some extra state to reset
          led_encoder_state = 0;
          return ESP_OK;
        },
        });

  // create the rmt object
  espp::Rmt rmt(espp::Rmt::Config{
      .gpio_num = 39, // Neopixel data pin on QtPy ESP32s3
      .resolution_hz = WS2812_FREQ_HZ,
      .log_level = espp::Logger::Verbosity::INFO,
    });

  // tell the RMT object to use the led_encoder (espp::RmtEncoder) that's
  // defined above
  rmt.set_encoder(std::move(led_encoder));

  auto led_fn = [&rmt](const espp::Rgb &rgb) {
    uint8_t green = std::clamp(int(rgb.g * 255), 0, 255);
    uint8_t blue = std::clamp(int(rgb.b * 255), 0, 255);
    uint8_t red = std::clamp(int(rgb.r * 255), 0, 255);
    // NOTE: we only have one LED so we only need to send one set of RGB data
    uint8_t data[3] = {red, green, blue};
    // now we can send the data to the WS2812B LED
    rmt.transmit(data, sizeof(data));
  };

  // MARK: USB initialization
  logger.info("USB initialization");
  const tinyusb_config_t tusb_cfg = {
    .device_descriptor = &desc_device,
    .string_descriptor = hid_string_descriptor,
    .string_descriptor_count = sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]),
    .external_phy = false,
    .configuration_descriptor = hid_configuration_descriptor,
    .self_powered = false
  };

  if (tinyusb_driver_install(&tusb_cfg)) {
    logger.error("Failed to install tinyusb driver");
    return;
  }
  logger.info("USB initialization DONE");

  // MARK: BLE initialization
  logger.info("BLE initialization");
  NimBLEDevice::init("ESP-USB-BLE-HID");

  // // and some i/o config
  // auto io_capabilities = BLE_HS_IO_NO_INPUT_OUTPUT;
  // NimBLEDevice::setSecurityIOCap(io_capabilities);

  // // set security parameters
  // bool bonding = true;
  // bool mitm = false;
  // bool secure_connections = true;
  // NimBLEDevice::setSecurityAuth(bonding, mitm, secure_connections);

  NimBLEScan* pScan = NimBLEDevice::getScan();

  // Set the callbacks to call when scan events occur, no duplicates
  pScan->setScanCallbacks(&scanCallbacks);

  // Set scan interval (how often) and window (how long) in milliseconds
  pScan->setInterval(100);
  pScan->setWindow(100);

  // Active scan will gather scan response data from advertisers
  // but will use more energy from both devices
  pScan->setActiveScan(true);

  // Start scanning for advertisers
  pScan->start(scanTimeMs);
  logger.info("Scanning for peripherals");

  // Loop here until we find a device we want to connect to
  bool subscribed = false;
  while (true) {
    std::this_thread::sleep_for(1s);
    auto pClients = NimBLEDevice::getConnectedClients();
    for (auto& pClient : pClients) {
      if (!subscribed) {
        logger.info("{}", pClient->toString());
        // subscribe to notifications for the HID input report
        NimBLERemoteService*        pSvc = nullptr;
        pSvc = pClient->getService(service_uuid);
        // get all characteristics
        if (pSvc) {
          logger.info("Found HID service");
          const auto& chars = pSvc->getCharacteristics(true);
          for (auto& pChr : chars) {
            logger.info("Characteristic: {}", pChr->getUUID().toString());
            // if it matches the report uuid, subscribe to it
            if (pChr->getUUID() == input_uuid) {
              logger.info("Found HID input characteristic");
              if (pChr->canRead()) {
                logger.info("{} Value: {}", pChr->getUUID().toString(), pChr->readValue());
              }

              if (pChr->canNotify()) {
                if (!pChr->subscribe(true, notifyCB)) {
                  pClient->disconnect();
                } else {
                  logger.info("Subscribed to notifications");
                }
              } else if (pChr->canIndicate()) {
                // Send false as first argument to subscribe to indications instead of notifications
                if (!pChr->subscribe(false, notifyCB)) {
                  pClient->disconnect();
                } else {
                  logger.info("Subscribed to notifications");
                }
              }
              subscribed = true;
            }
          }
        }
      }
    }

    if (pClients.size()) {
      continue;
    }

    subscribed = false;

    if (!NimBLEDevice::getScan()->isScanning()) {
      NimBLEDevice::getScan()->start(scanTimeMs);
    }

    static int button_index = 0;
    // if we're not subscribed, then twirl the joysticks
    // use the button index to set the position of the right joystick
    float angle = 2.0f * M_PI * button_index / num_buttons;

    gamepad_input_report.reset();
    // gamepad_input_report.set_hat(hat);
    // gamepad_input_report.set_button(button_index, true);

    // joystick inputs are in the range [-1, 1] float
    gamepad_input_report.set_right_joystick(cos(angle), sin(angle));
    gamepad_input_report.set_left_joystick(sin(angle), cos(angle));

    // trigger inputs are in the range [0, 1] float
    // gamepad_input_report.set_accelerator(std::abs(sin(angle)));
    // gamepad_input_report.set_brake(std::abs(cos(angle)));

    // static bool consumer_record = false;
    // gamepad_input_report.set_consumer_record(consumer_record);
    // consumer_record = !consumer_record;

    button_index = (button_index % num_buttons) + 1;

    // send an input report
    auto report = gamepad_input_report.get_report();

    if (tud_mounted()) {
      bool success = tud_hid_report(input_report_id, report.data(), report.size());
      espp::Rgb color = success ? espp::Rgb(0.0f, 1.0f, 0.0f) : espp::Rgb(1.0f, 0.0f, 0.0f);
      // toggle the LED each send, so mod 2
      if (button_index % 2 == 0) {
        led_fn(color);
      } else {
        led_fn(espp::Rgb(0.0f, 0.0f, 0.0f));
      }
    }
  }
}
