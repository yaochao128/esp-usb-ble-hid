#include <chrono>
#include <thread>

#include "logger.hpp"
#include "task.hpp"

#include "hid-rp-gamepad.hpp"
#include "hid_service.hpp"
#include <NimBLEDevice.h>

using namespace std::chrono_literals;

static uint32_t                      scanTimeMs = 5000; /** scan time in milliseconds, 0 = scan forever */

static NimBLEUUID service_uuid(espp::HidService::SERVICE_UUID);
static NimBLEUUID report_map_uuid(espp::HidService::REPORT_MAP_UUID);
static NimBLEUUID input_uuid(espp::HidService::REPORT_UUID);


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
static auto raw_descriptor = descriptor(usage_page<generic_desktop>(), usage(generic_desktop::GAMEPAD),
                                        collection::application(gamepad_input_report.get_descriptor(),
                                                                gamepad_rumble_report.get_descriptor(),
                                                                battery_input_report.get_descriptor(),
                                                                gamepad_leds_report.get_descriptor()));


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
    fmt::print("\t{}\n", gamepad_input_report);
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

  /** Set the callbacks to call when scan events occur, no duplicates */
  pScan->setScanCallbacks(&scanCallbacks);

  /** Set scan interval (how often) and window (how long) in milliseconds */
  pScan->setInterval(100);
  pScan->setWindow(100);

  /**
   * Active scan will gather scan response data from advertisers
   *  but will use more energy from both devices
   */
  pScan->setActiveScan(true);

  /** Start scanning for advertisers */
  pScan->start(scanTimeMs);
  logger.info("Scanning for peripherals");

  /** Loop here until we find a device we want to connect to */
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
                /** Send false as first argument to subscribe to indications instead of notifications */
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
  }
}
