// SPDX-License-Identifier: EUPL-1.2
// Copyright (c) 2026 the Lumen authors. Part of a modified version of WLED; see NOTICE.md.
#pragma once
#ifdef GLOWLINE_IMPROV_BLE

#include <Arduino.h>
#include <functional>

// Wi-Fi setup over Bluetooth LE: Improv Wi-Fi BLE (https://www.improv-wifi.com/ble/) plus Lumen's
// encrypted Wi-Fi command (lumen-shared contracts/firmware.md "BLE provisioning (Improv)", REQ-131).
// The usermod starts it only while the setup hotspot is up and releases it for good (until the next
// reboot) before the first TLS connect: BLE takes ~78 KB of internal heap that TLS needs (LESSONS).
// Every BLE callback only copies bytes; all work happens in loop(), on WLED's main task.
namespace GlowlineBle {

struct Identity {
  String advName;     // "Lumen-xxxx", the setup hotspot's name
  String deviceId;    // Improv device-info slot 4; "" on a blank board
  String fwVersion;
  String passphrase;  // the hotspot passphrase; "" = none, then plain Improv Wi-Fi is accepted
};

// Both run on WLED's main task, from loop().
typedef std::function<void(const char* ssid, const char* password)> WifiHandler;
typedef std::function<void()> IdentifyHandler;

bool start(const Identity& id, WifiHandler onWifi, IdentifyHandler onIdentify);
void loop();
bool active();    // up: advertising or a phone connected
bool released();  // was up this boot and has been released (no restart until reboot)
void reportConnected(const char* url);  // the Wi-Fi a phone sent is up
void reportFailed(const char* reason);  // it didn't connect; reason as in /json/info lumen.wifi.reason
void release(const char* why);
const char* stateName();  // for /json/info: "off", "on", "released"

}  // namespace GlowlineBle

#endif  // GLOWLINE_IMPROV_BLE
