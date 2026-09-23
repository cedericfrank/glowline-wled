// BENCH ONLY -- DO NOT SHIP. See readme.md. Everything is behind LUMEN_BLE_HEAP_PROBE so the
// `custom_usermods = *` envs (no BLE library on their platform) compile this file as empty.
#ifdef LUMEN_BLE_HEAP_PROBE

#include "wled.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <esp_heap_caps.h>

#ifndef PROBE_BLE_HOLD_MS
#define PROBE_BLE_HOLD_MS (3UL * 60UL * 1000UL) // BLE stays up this long after boot (power-on window)
#endif
#define PROBE_TICK_MS 10000UL

// Improv BLE service layout (https://www.improv-wifi.com/ble/) -- same GATT footprint, no protocol.
#define IMPROV_SVC  "00467768-6228-2272-4663-277478268000"
#define IMPROV_STATE "00467768-6228-2272-4663-277478268001"
#define IMPROV_ERR   "00467768-6228-2272-4663-277478268002"
#define IMPROV_RPC   "00467768-6228-2272-4663-277478268003"
#define IMPROV_RES   "00467768-6228-2272-4663-277478268004"
#define IMPROV_CAPS  "00467768-6228-2272-4663-277478268005"

static volatile bool centralConnected = false;
static volatile bool centralDisconnected = false;

class ProbeServerCallbacks : public BLEServerCallbacks {
  // NimBLE host task -- only set flags here, log from loop()
  void onConnect(BLEServer *) override { centralConnected = true; }
  void onDisconnect(BLEServer *) override { centralDisconnected = true; }
};

class BleHeapProbe : public Usermod {
  private:
    bool bleOn = false;
    bool wifiSeen = false;
    unsigned long lastTick = 0;
    const char *stage = "S0";

    void logHeap(const char *label) {
      Serial.printf_P(PSTR("bleprobe %s t=%lums int_free=%u int_largest=%u int_min=%u psram_free=%u leds=%u fx=%u ble=%d wifi=%d\n"),
        label, millis(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)strip.getLengthTotal(),
        (unsigned)strip.getMainSegment().mode,
        (int)bleOn, (int)WLED_CONNECTED);
    }

  public:
    void setup() override {
      Serial.println(F("################################################################"));
      Serial.println(F("# BENCH BUILD -- BLE heap probe, DO NOT SHIP                   #"));
      Serial.println(F("################################################################"));
      logHeap("S0 boot");

      BLEDevice::init("Lumen-probe");
      BLEServer *server = BLEDevice::createServer();
      server->setCallbacks(new ProbeServerCallbacks());
      BLEService *svc = server->createService(IMPROV_SVC);
      BLECharacteristic *state = svc->createCharacteristic(IMPROV_STATE, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
      svc->createCharacteristic(IMPROV_ERR, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
      svc->createCharacteristic(IMPROV_RPC, BLECharacteristic::PROPERTY_WRITE);
      svc->createCharacteristic(IMPROV_RES, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
      BLECharacteristic *caps = svc->createCharacteristic(IMPROV_CAPS, BLECharacteristic::PROPERTY_READ);
      uint8_t authorized = 0x02, noCaps = 0x00;
      state->setValue(&authorized, 1);
      caps->setValue(&noCaps, 1);
      svc->start();
      BLEAdvertising *adv = BLEDevice::getAdvertising();
      adv->addServiceUUID(IMPROV_SVC);
      adv->start();
      bleOn = true;
      stage = "S1";
      logHeap("S1 ble_up");
    }

    void loop() override {
      unsigned long now = millis();

      if (centralConnected) { centralConnected = false; logHeap("S2b central_connected"); }
      if (centralDisconnected) {
        centralDisconnected = false;
        logHeap("S2b central_disconnected");
        if (bleOn) BLEDevice::startAdvertising();
      }

      if (bleOn && !wifiSeen && WLED_CONNECTED) {
        wifiSeen = true;
        stage = "S2";
        logHeap("S2 ble+wifi");
      }

      if (bleOn && now >= PROBE_BLE_HOLD_MS) {
        BLEDevice::deinit(true); // releases BT memory; BLE cannot restart until reboot
        bleOn = false;
        stage = "S3";
        logHeap("S3 ble_released");
        if (WLED_CONNECTED) {
          forceReconnect = true; // WLED reconnects Wi-Fi -> glowline redoes its TLS handshake
          stage = "S4";
          logHeap("S4 reconnect");
        }
      }

      if (now - lastTick >= PROBE_TICK_MS) {
        lastTick = now;
        char label[12];
        snprintf_P(label, sizeof(label), PSTR("tick %s"), stage);
        logHeap(label);
      }
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

static BleHeapProbe ble_heap_probe;
REGISTER_USERMOD(ble_heap_probe);

#endif // LUMEN_BLE_HEAP_PROBE
