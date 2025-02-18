#include <chrono>
#include <thread>

#include "logger.hpp"
#include "qtpy.hpp"
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

    .idVendor = 0x045E, // Microsoft
    .idProduct = 0x0B13, // Xbox One Controller (model 1708)
    .bcdDevice = 0x0100, // 1.0

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
      static auto &qtpy = espp::QtPy::get();
      static bool led_on = false;
      static auto on_color = espp::Rgb(0.0f, 0.0f, 1.0f); // use blue for BLE
      static auto off_color = espp::Rgb(0.0f, 0.0f, 0.0f);
      qtpy.led(led_on ? on_color : off_color);
      led_on = !led_on;
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
            // set the connection parameters
            NimBLEDevice::getClientByHandle(connInfo.getConnHandle())->updateConnParams(12, 12, 0, 400);
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

            // set the connection parameters before we connect
            pClient->setConnectionParams(12, 12, 0, 400);
            // and set our callbacks
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
  auto &qtpy = espp::QtPy::get();
  qtpy.initialize_led();
  qtpy.led(espp::Rgb(0.0f, 0.0f, 0.0f));

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
        qtpy.led(color);
      } else {
        qtpy.led(espp::Rgb(0.0f, 0.0f, 0.0f));
      }
    }
  }
}
