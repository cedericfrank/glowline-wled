#include "wled.h"
#include <WiFi.h>
#include <base64.h>
#include <Update.h>
#include <Preferences.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "mbedtls/pk.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"

#if __has_include(<WiFiClientSecure.h>)
  #include <WiFiClientSecure.h>
#else
  #error "glowline needs a WiFiClientSecure/NetworkClientSecure-capable platform for wss:// -- e.g. pioarduino/platform-espressif32 (see usermods/glowline/platformio_override.ini.sample). The default tasmota-sourced espressif32 platform ships TLS compiled out."
#endif

// This build's self-reported firmware version -- sent as &fw= on the /ws handshake (docs/OTA.md
// Section 3) and used as the /ota/check query and the locally-persisted "known bad" marker.
// Set per release via build_flags, e.g. -D GLOWLINE_FW_VERSION=\"1.2.0\" -- never hand-edit this
// default and ship it, or every unit reports the same version forever.
#ifndef GLOWLINE_FW_VERSION
#define GLOWLINE_FW_VERSION "0.0.0-dev"
#endif

// How long a freshly-flashed image has to prove (via a successful WebSocket CONNECT -- see
// pollHandshake()) that it can actually reach the backend before the bootloader rolls it back.
// 15 minutes in any build that ships -- overridden to 2 minutes ONLY by the
// *_BENCH_2MIN_ROLLBACK_DO_NOT_SHIP env in platformio_override.ini, which also makes setup() print
// an unmissable warning on every boot when this isn't the real value.
#ifndef OTA_VERIFY_TIMEOUT_MS
#define OTA_VERIFY_TIMEOUT_MS (15UL * 60UL * 1000UL)
#endif

/*
 * Diagnostic + control usermod: bridges WLED to a Glowline backend over
 * WebSocket, applying whatever state it's sent.
 *
 * - Prints a heartbeat and the current free heap to Serial every 5 seconds.
 * - Maintains a TLS (wss://) WebSocket connection to
 *   wss://host:port/ws?device=<deviceId>&token=<token>, where host, port,
 *   deviceId and token are all configurable in Settings -> Usermods (nothing
 *   hardcoded). Sends a hello message on connect, logs every message
 *   received, and reconnects with exponential backoff (1s doubling up to a
 *   60s cap) on disconnect. Every state transition is logged, and free heap
 *   is logged before, during and after the TLS handshake.
 * - Received JSON is applied directly to WLED's own state (same path as the
 *   JSON API), so a message can turn the strip on, change color, effects,
 *   brightness, etc.
 * - While connected, sends a ping every 30s and requires *something* back
 *   (a pong, or any other inbound frame) within 90s -- a TCP socket can
 *   report connected() == true while the actual path is dead, and this is
 *   what actually catches that and forces a reconnect instead of sitting on
 *   a socket that looks fine but never delivers anything again.
 * - The first time a config change (a Settings -> Usermods save, not just a
 *   plain reboot) leads to a successful connection, plays an unmistakable
 *   "setup worked" cue: brief full-brightness green, then solid white at
 *   50%. Lets someone confirm setup succeeded without checking their phone.
 *
 * TLS is via setInsecure() -- the connection is encrypted but the server's
 * certificate is NOT validated (no chain-of-trust check), so this is still
 * vulnerable to a MITM presenting any certificate. That's known debt to
 * replace with setCACert()/a pinned cert before this ships for real.
 *
 * The WebSocket client (handshake + RFC 6455 framing) is otherwise
 * hand-rolled on top of the TCP client rather than using a third-party
 * library, since every WebSocket client library available for this core
 * unconditionally pulls in its own TLS assumptions. This only builds on
 * platforms that actually ship a working WiFiClientSecure/NetworkClientSecure
 * (see the #error above) -- confirmed working on pioarduino/platform-esp32,
 * NOT on the tasmota-sourced platform (or the official PlatformIO registry
 * platform, which resolves to the same tasmota source), both of which ship
 * mbedtls with TLS compiled out.
 */
class GlowlineUsermod : public Usermod {
  private:
    // Heartbeat
    unsigned long lastTime_ = 0;
    static const unsigned long INTERVAL_MS = 5000;

    // Boot cue: replaces WLED's default "on, orange" power-up state with a
    // branded status sequence, drawn directly via handleOverlayDraw() (raw
    // pixel writes every frame, right before strip.show()) rather than
    // through WLED's effect engine, since effects have no precise
    // speed-to-duration mapping and this needs exact timing.
    //   1. Fill (white) from the first to the last LED over 1s.
    //   2. Hold fully lit (white) while waiting for a WiFi connection.
    //   3. WiFi connects  -> solid green for 1s, then off.
    //      WiFi times out -> solid red for 5s, then off.
    // Runs once per physical boot (driven by setup(), not by config saves),
    // at a fixed 50% brightness throughout.
    enum class BootCuePhase : uint8_t { CUE_FILL, CUE_WAIT_WIFI, CUE_GREEN, CUE_RED, CUE_DONE };
    static const uint8_t BOOT_CUE_BRI = 128; // 50%
    static const unsigned long BOOT_CUE_FILL_MS = 1000;
    static const unsigned long BOOT_CUE_WIFI_WAIT_MS = 10000; // budget after the fill before declaring failure
    static const unsigned long BOOT_CUE_GREEN_MS = 1000;
    static const unsigned long BOOT_CUE_RED_MS = 5000;
    BootCuePhase bootCuePhase = BootCuePhase::CUE_FILL;
    unsigned long bootCuePhaseStartedAt = 0;

    // WebSocket config (Settings -> Usermods)
    String wsHost = "";
    uint16_t wsPort = 0;
    String wsDeviceId = "";
    String wsToken = "";

    // TLS connection
    WiFiClientSecure client;

    enum class WsState : uint8_t { DISCONNECTED, CONNECTING, CONNECTED };
    WsState wsState = WsState::DISCONNECTED;

    static const unsigned long BACKOFF_MIN_MS = 1000;
    static const unsigned long BACKOFF_MAX_MS = 60000;
    unsigned long backoffMs = BACKOFF_MIN_MS;
    unsigned long lastBackoffMs = BACKOFF_MIN_MS; // delay actually being waited for the upcoming retry, for accurate fire-time logging
    bool reconnectDue = false;
    unsigned long nextAttemptAt = 0;

    // Handshake
    static const unsigned long HANDSHAKE_TIMEOUT_MS = 5000;
    unsigned long connectStartedAt = 0;
    String handshakeStatusLine = "";

    // Liveness: a TCP socket can stay "connected" while the actual path is
    // dead (nothing arrives, nothing gets a response) -- ping periodically
    // and require *something* back within the timeout, or treat it as dead
    // and fall into the normal reconnect/backoff path.
    static const unsigned long PING_INTERVAL_MS = 30000;
    static const unsigned long LIVENESS_TIMEOUT_MS = 90000;
    unsigned long lastPingSentAt = 0;
    unsigned long lastInboundAt = 0;

    // First-connection success signal: an unmistakable "setup worked" cue --
    // brief green, then settle to solid white at 50% -- played once on the
    // first successful connect after a config change (not on every
    // reconnect), so someone doesn't have to check their phone to know it's
    // set up right.
    static const unsigned long SUCCESS_SIGNAL_GREEN_MS = 1500;
    bool successSignalPending = false; // armed by a config save, cleared once played
    bool successSignalPlaying = false; // currently in the green phase
    unsigned long successSignalStartedAt = 0;
    // readFromConfig() runs once at boot (loading whatever's already on
    // disk) and again on every live Settings->Usermods save -- only the
    // latter is an actual "config change", so the signal is armed starting
    // from the second call onward, not the first.
    bool hasLoadedConfigOnce = false;

    // Incoming frame parser (streaming, byte-at-a-time; small fixed payload
    // buffer -- fine for a diagnostic hello/heartbeat-style protocol)
    enum class FrameState : uint8_t { HEADER1, HEADER2, EXT_LEN, MASK_KEY, PAYLOAD };
    FrameState frameState = FrameState::HEADER1;
    uint8_t frameOpcode = 0;
    bool frameMasked = false;
    uint64_t framePayloadLen = 0;
    uint8_t extLenBytesNeeded = 0;
    uint8_t extLenIdx = 0;
    uint8_t extLenBuf[8];
    uint8_t maskKey[4];
    uint8_t maskIdx = 0;
    static const size_t MAX_FRAME_PAYLOAD = 512;
    uint8_t frameBuf[MAX_FRAME_PAYLOAD];
    uint64_t payloadIdx = 0;

    // OTA update (docs/OTA.md Section 5). The check-and-download sequence is intentionally
    // blocking rather than threaded through the wsState machine above: it runs once per WS
    // CONNECTED transition, a plain 204 resolves in well under a second, and even a real download
    // (a couple of MB over TLS) is a rare, deliberate event, not something the rest of the usermod
    // needs to stay responsive through. WLED_WATCHDOG_TIMEOUT=0 for this board env
    // (platformio.ini's esp32s3dev_16MB_opi) means this can't trip WLED's own watchdog.
    static const unsigned long OTA_CHECK_COOLDOWN_MS = 60000; // shared by "just checked" and the 429 case below
    unsigned long otaCheckBlockedUntil = 0;

    // Set in setup() if esp_ota_get_state_partition() reports the running image as
    // PENDING_VERIFY -- only true right after a fresh OTA flash, never after a USB/factory flash
    // or an already-confirmed reboot. Cleared the moment a WS CONNECTED proves this image can
    // reach the backend (pollHandshake()); if that never happens within OTA_VERIFY_TIMEOUT_MS,
    // loop() rolls the device back to its previous image.
    bool otaPendingVerify = false;
    unsigned long otaBootTime = 0;

    // NVS ("ota" namespace), read once at boot, surviving the reboot a rollback itself causes:
    //   failedVer -- a version known to roll back on this device. Refused locally, before any
    //                network/crypto work, independent of whether the server still offers it as a
    //                target. Cleared ONLY when a *different* version's mark-valid succeeds (see
    //                pollHandshake()) -- deliberately outlives the one-shot rolled_back report
    //                below, so clearing it can't race the server's own processing of that report
    //                on the very next /ota/check in the same connect cycle.
    //   rbPending  -- true if the rolled_back frame for failedVer hasn't been sent yet. Read once
    //                 at boot; cleared the moment that frame actually sends.
    String otaKnownBadVersion = "";
    bool otaRollbackReportPending = false;

    // A check/download attempt failed without ever rebooting (bad signature, hash mismatch, HTTP
    // error, dropped connection) -- the wss link may have been closed for the download, so this is
    // held in RAM and reported once the normal reconnect brings CONNECTED back, not sent inline.
    bool otaFailurePending = false;
    String otaFailureReason = "";

    static const uint8_t OTA_CUE_BRI = 96; // dim, but unmistakably not "off" or "frozen"

    // Both PEMs exactly as OpenSSL emits them (openssl ec -pubout). The manifest's keyId selects
    // which one verifies its signature -- "primary" or "spare". The spare exists so a leaked
    // primary key doesn't require physically reflashing every unit already in the field.
    static constexpr const char* kOtaKeyPrimary =
      "-----BEGIN PUBLIC KEY-----\n"
      "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEBuUddsyboiSmr7v/QyjBwGKBRrc4\n"
      "7Ih/H9JG+Rxcc0onmxJgH4+K7haVW+8TO9xthwDcjpcsGSn5R3YnVBA6/w==\n"
      "-----END PUBLIC KEY-----\n";
    static constexpr const char* kOtaKeySpare =
      "-----BEGIN PUBLIC KEY-----\n"
      "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEaTv+HkVrT04aSjnNHdNVusWt49JW\n"
      "E1U86cd5LS7pN3jyfLiCO6IiGBjBW3UbDe8Nc6Veay0J3aMwOPzGsdBLCQ==\n"
      "-----END PUBLIC KEY-----\n";

    static const char* stateName(WsState s) {
      switch (s) {
        case WsState::DISCONNECTED: return "DISCONNECTED";
        case WsState::CONNECTING:   return "CONNECTING";
        case WsState::CONNECTED:    return "CONNECTED";
      }
      return "?";
    }

    void setState(WsState newState) {
      if (newState == wsState) return;
      wsState = newState;
      Serial.print(F("glowline ws: state -> "));
      Serial.println(stateName(newState));
    }

    void resetFrameParser() {
      frameState = FrameState::HEADER1;
      payloadIdx = 0;
      maskIdx = 0;
      extLenIdx = 0;
    }

    // Drop any existing connection and arm an immediate reconnect attempt
    // with backoff reset to the minimum. Used at startup and whenever the
    // host/port config is (re)loaded.
    void resetAndScheduleImmediateConnect() {
      if (client.connected()) client.stop();
      resetFrameParser();
      setState(WsState::DISCONNECTED);
      backoffMs = BACKOFF_MIN_MS;
      nextAttemptAt = millis();
      reconnectDue = true;
    }

    void onDisconnected(const __FlashStringHelper* reason) {
      client.stop();
      setState(WsState::DISCONNECTED);
      lastBackoffMs = backoffMs;
      nextAttemptAt = millis() + backoffMs;
      reconnectDue = true;
      Serial.print(F("glowline ws: disconnected ("));
      Serial.print(reason);
      Serial.print(F("), retrying in "));
      Serial.print(backoffMs);
      Serial.println(F(" ms"));
      backoffMs = (backoffMs * 2 > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : backoffMs * 2;
    }

    String makeWsKey() {
      uint8_t raw[16];
      for (int i = 0; i < 16; i++) raw[i] = (uint8_t)random(0, 256);
      return base64::encode(raw, 16);
    }

    static String urlEncode(const String& s) {
      String out;
      char buf[4];
      for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
          out += c;
        } else {
          snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
          out += buf;
        }
      }
      return out;
    }

    void beginConnect() {
      setState(WsState::CONNECTING);
      Serial.print(F("glowline ws: connecting to "));
      Serial.print(wsHost);
      Serial.print(':');
      Serial.println(wsPort);

      resetFrameParser();
      handshakeStatusLine = "";
      connectStartedAt = millis();

      // TODO(security debt): setInsecure() accepts any certificate the server
      // presents, with no chain-of-trust check -- encrypted, but still
      // vulnerable to a MITM. Replace with setCACert() (or a pinned cert)
      // before this ships for real.
      client.setInsecure();
      // Bound both blocking calls below explicitly: the library defaults (30s
      // TCP connect, 120s TLS handshake) are far longer than our 60s backoff
      // cap assumes, so a single hung attempt against a network that silently
      // drops packets (rather than cleanly refusing) could otherwise block
      // for up to ~150s before a retry is even scheduled.
      client.setHandshakeTimeout(10); // seconds
      // Postpone the TLS handshake so the TCP connect and the handshake are
      // two separate, individually-timed/logged steps below.
      client.setPlainStart();

      Serial.print(F("glowline ws: free heap before TLS handshake: "));
      Serial.println(ESP.getFreeHeap());

      if (!client.connect(wsHost.c_str(), wsPort, 10000)) { // 10s TCP connect timeout
        onDisconnected(F("tcp connect failed"));
        return;
      }

      Serial.print(F("glowline ws: tcp connected, starting TLS handshake, free heap: "));
      Serial.println(ESP.getFreeHeap());

      if (!client.startTLS()) {
        onDisconnected(F("TLS handshake failed"));
        return;
      }

      Serial.print(F("glowline ws: TLS handshake complete, free heap: "));
      Serial.println(ESP.getFreeHeap());

      client.print(F("GET /ws?device="));
      client.print(urlEncode(wsDeviceId));
      client.print(F("&token="));
      client.print(urlEncode(wsToken));
      client.print(F("&fw="));
      client.print(urlEncode(String(F(GLOWLINE_FW_VERSION))));
      client.print(F(" HTTP/1.1\r\n"));
      client.print(F("Host: "));
      client.print(wsHost);
      client.print(':');
      client.print(wsPort);
      client.print(F("\r\n"));
      client.print(F("Upgrade: websocket\r\n"));
      client.print(F("Connection: Upgrade\r\n"));
      client.print(F("Sec-WebSocket-Key: "));
      client.print(makeWsKey());
      client.print(F("\r\n"));
      client.print(F("Sec-WebSocket-Version: 13\r\n"));
      client.print(F("\r\n"));
    }

    // Returns true once the handshake has concluded (success or failure).
    void pollHandshake() {
      while (client.available()) {
        String line = client.readStringUntil('\n');
        while (line.length() && (line[line.length() - 1] == '\r' || line[line.length() - 1] == '\n')) {
          line.remove(line.length() - 1);
        }
        if (line.length() == 0) {
          // Blank line: end of headers.
          if (handshakeStatusLine.indexOf(F("101")) < 0) {
            Serial.print(F("glowline ws: handshake rejected: "));
            Serial.println(handshakeStatusLine);
            onDisconnected(F("handshake rejected"));
            return;
          }
          setState(WsState::CONNECTED);
          backoffMs = BACKOFF_MIN_MS; // reset backoff after a successful connect
          lastInboundAt = millis();
          lastPingSentAt = millis();
          Serial.print(F("glowline ws: connected, free heap: "));
          Serial.println(ESP.getFreeHeap());
          sendFrame(0x1, (const uint8_t*)"hello from glowline", 20);
          if (successSignalPending) {
            successSignalPending = false;
            startSuccessSignal();
          }

          // Mark-valid happens ONLY here, on a proven CONNECTED -- never in setup(), never on
          // WiFi association alone. A bad wss host/port baked into a new image would still pass a
          // WiFi-only check and roll back nothing; reaching this exact point is what actually
          // proves the new firmware can talk to the backend.
          if (otaPendingVerify) {
            esp_ota_mark_app_valid_cancel_rollback();
            otaPendingVerify = false;
            Serial.println(F("glowline ota: image marked valid (WS connected)"));
            // This confirms a different, working image -- whatever local block existed for a
            // previously-failed version no longer applies. See otaKnownBadVersion's own comment
            // for why this is the only place that clears it.
            if (otaKnownBadVersion.length() > 0) {
              otaKnownBadVersion = "";
              Preferences prefs;
              prefs.begin("ota", false);
              prefs.remove("failedVer");
              prefs.end();
            }
          }

          if (otaRollbackReportPending) sendOtaRolledBackFrame();
          if (otaFailurePending) sendOtaFailedFrame();

          checkForOtaUpdate(); // blocking; respects its own cooldown internally
          return;
        }
        if (handshakeStatusLine.length() == 0) handshakeStatusLine = line;
      }
      if (!client.connected()) {
        onDisconnected(F("closed during handshake"));
        return;
      }
      if (millis() - connectStartedAt > HANDSHAKE_TIMEOUT_MS) {
        onDisconnected(F("handshake timed out"));
      }
    }

    void sendFrame(uint8_t opcode, const uint8_t* payload, size_t len) {
      uint8_t header[14];
      size_t hlen = 0;
      header[hlen++] = 0x80 | (opcode & 0x0F); // FIN + opcode
      if (len < 126) {
        header[hlen++] = 0x80 | (uint8_t)len;
      } else if (len < 65536) {
        header[hlen++] = 0x80 | 126;
        header[hlen++] = (uint8_t)((len >> 8) & 0xFF);
        header[hlen++] = (uint8_t)(len & 0xFF);
      } else {
        header[hlen++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) header[hlen++] = (uint8_t)((len >> (8 * i)) & 0xFF);
      }
      uint8_t mask[4];
      for (int i = 0; i < 4; i++) mask[i] = (uint8_t)random(0, 256);
      memcpy(&header[hlen], mask, 4);
      hlen += 4;
      client.write(header, hlen);

      uint8_t buf[64];
      size_t sent = 0;
      while (sent < len) {
        size_t chunk = (len - sent < sizeof(buf)) ? (len - sent) : sizeof(buf);
        for (size_t i = 0; i < chunk; i++) buf[i] = payload[sent + i] ^ mask[(sent + i) % 4];
        client.write(buf, chunk);
        sent += chunk;
      }
    }

    // Hands the payload to the same deserializeState() path the JSON API
    // (POST /json/state), WebSocket server, and UDP sync all use -- see
    // ws.cpp's WS_EVT_DATA handler and udp.cpp's incoming-packet handler for
    // the reference pattern this mirrors.
    void applyJsonState(const uint8_t* payload, size_t len) {
      if (!requestJSONBufferLock(JSON_LOCK_UNKNOWN)) {
        Serial.println(F("glowline ws: JSON buffer busy, not applied"));
        return;
      }
      DeserializationError error = deserializeJson(*pDoc, payload, len);
      JsonObject root = pDoc->as<JsonObject>();
      if (error || root.isNull()) {
        Serial.println(F("glowline ws: not valid JSON, not applied"));
        releaseJSONBufferLock();
        return;
      }
      deserializeState(root);
      releaseJSONBufferLock();
      Serial.println(F("glowline ws: applied to WLED state"));
    }

    // Unmistakable "setup worked" cue: brief full-brightness green, then
    // settle to solid white at 50%. Goes through the same applyJsonState()
    // path as everything else that changes WLED state.
    void startSuccessSignal() {
      successSignalPlaying = true;
      successSignalStartedAt = millis();
      Serial.println(F("glowline: first connect after config change -- playing success signal (green)"));
      static const char kGreen[] = "{\"on\":true,\"bri\":255,\"seg\":[{\"fx\":0,\"col\":[[0,255,0]]}]}";
      applyJsonState((const uint8_t*)kGreen, strlen(kGreen));
    }

    void finishSuccessSignal() {
      successSignalPlaying = false;
      Serial.println(F("glowline: success signal settling to solid white 50%"));
      static const char kWhite[] = "{\"on\":true,\"bri\":128,\"seg\":[{\"fx\":0,\"col\":[[255,255,255]]}]}";
      applyJsonState((const uint8_t*)kWhite, strlen(kWhite));
    }

    void finishFrame() {
      lastInboundAt = millis(); // any complete frame counts as proof the connection is alive
      size_t len = (size_t)((payloadIdx < MAX_FRAME_PAYLOAD) ? payloadIdx : MAX_FRAME_PAYLOAD);
      switch (frameOpcode) {
        case 0x1: // text
        case 0x0: // continuation (treated as text here)
          Serial.print(F("glowline ws: received: "));
          Serial.write(frameBuf, len);
          if (payloadIdx > MAX_FRAME_PAYLOAD) Serial.print(F(" ...[truncated]"));
          Serial.println();
          applyJsonState(frameBuf, len);
          break;
        case 0x8: // close
          Serial.println(F("glowline ws: received close frame"));
          onDisconnected(F("closed by peer"));
          break;
        case 0x9: // ping -> reply with pong
          Serial.println(F("glowline ws: received ping"));
          sendFrame(0xA, frameBuf, len);
          break;
        case 0xA: // pong
          Serial.println(F("glowline ws: received pong"));
          break;
        default:
          Serial.print(F("glowline ws: received frame, opcode="));
          Serial.println(frameOpcode);
          break;
      }
    }

    void pollFrames() {
      while (client.available()) {
        uint8_t b = (uint8_t)client.read();
        switch (frameState) {
          case FrameState::HEADER1:
            frameOpcode = b & 0x0F;
            frameState = FrameState::HEADER2;
            break;

          case FrameState::HEADER2: {
            frameMasked = b & 0x80;
            uint8_t len7 = b & 0x7F;
            maskIdx = 0;
            if (len7 < 126) {
              framePayloadLen = len7;
              payloadIdx = 0;
              frameState = frameMasked ? FrameState::MASK_KEY : FrameState::PAYLOAD;
              if (framePayloadLen == 0 && !frameMasked) { finishFrame(); frameState = FrameState::HEADER1; }
            } else if (len7 == 126) {
              extLenBytesNeeded = 2; extLenIdx = 0; frameState = FrameState::EXT_LEN;
            } else {
              extLenBytesNeeded = 8; extLenIdx = 0; frameState = FrameState::EXT_LEN;
            }
            break;
          }

          case FrameState::EXT_LEN:
            extLenBuf[extLenIdx++] = b;
            if (extLenIdx == extLenBytesNeeded) {
              framePayloadLen = 0;
              for (int i = 0; i < extLenBytesNeeded; i++) framePayloadLen = (framePayloadLen << 8) | extLenBuf[i];
              payloadIdx = 0;
              frameState = frameMasked ? FrameState::MASK_KEY : FrameState::PAYLOAD;
              if (framePayloadLen == 0) { finishFrame(); frameState = FrameState::HEADER1; }
            }
            break;

          case FrameState::MASK_KEY:
            maskKey[maskIdx++] = b;
            if (maskIdx == 4) {
              payloadIdx = 0;
              frameState = FrameState::PAYLOAD;
              if (framePayloadLen == 0) { finishFrame(); frameState = FrameState::HEADER1; }
            }
            break;

          case FrameState::PAYLOAD: {
            uint8_t decoded = frameMasked ? (uint8_t)(b ^ maskKey[payloadIdx % 4]) : b;
            if (payloadIdx < MAX_FRAME_PAYLOAD) frameBuf[payloadIdx] = decoded;
            payloadIdx++;
            if (payloadIdx >= framePayloadLen) {
              finishFrame();
              frameState = FrameState::HEADER1;
              if (wsState != WsState::CONNECTED) return; // finishFrame() may have disconnected us
            }
            break;
          }
        }
      }
    }

    // Sends a periodic ping and requires *something* back (a pong, or any
    // other inbound frame) within LIVENESS_TIMEOUT_MS. A TCP socket can
    // report connected() == true while the actual path is dead -- this is
    // what actually detects that and forces a reconnect.
    void checkLiveness() {
      unsigned long now = millis();
      if (now - lastInboundAt >= LIVENESS_TIMEOUT_MS) {
        Serial.print(F("glowline ws: no inbound frame in "));
        Serial.print(LIVENESS_TIMEOUT_MS / 1000);
        Serial.println(F("s, treating connection as dead"));
        onDisconnected(F("liveness timeout"));
        return;
      }
      if (now - lastPingSentAt >= PING_INTERVAL_MS) {
        lastPingSentAt = now;
        Serial.println(F("glowline ws: sending ping"));
        sendFrame(0x9, nullptr, 0);
      }
    }

    // Ends the boot cue and hands the strip back to normal operation, off.
    // Without this, handleOverlayDraw() simply stops drawing and whatever
    // WLED's own default state was (orange, per beginStrip()) would show
    // through underneath -- exactly what this whole cue exists to avoid.
    void finishBootCue() {
      bootCuePhase = BootCuePhase::CUE_DONE;
      Serial.println(F("glowline: boot cue finished"));
      static const char kOff[] = "{\"on\":false}";
      applyJsonState((const uint8_t*)kOff, strlen(kOff));
    }

    // Advances the boot cue's phase based on elapsed time and WiFi state.
    // Call once per loop() tick; the actual pixel painting happens in
    // handleOverlayDraw() so it stays exactly in sync with what's shown.
    void advanceBootCue() {
      if (bootCuePhase == BootCuePhase::CUE_DONE) return;
      unsigned long elapsed = millis() - bootCuePhaseStartedAt;
      switch (bootCuePhase) {
        case BootCuePhase::CUE_FILL:
          if (elapsed >= BOOT_CUE_FILL_MS) {
            bootCuePhase = BootCuePhase::CUE_WAIT_WIFI;
            bootCuePhaseStartedAt = millis();
          }
          break;
        case BootCuePhase::CUE_WAIT_WIFI:
          if (WLED_CONNECTED) {
            Serial.println(F("glowline: boot cue -- WiFi connected"));
            bootCuePhase = BootCuePhase::CUE_GREEN;
            bootCuePhaseStartedAt = millis();
          } else if (elapsed >= BOOT_CUE_WIFI_WAIT_MS) {
            Serial.println(F("glowline: boot cue -- WiFi did not connect in time"));
            bootCuePhase = BootCuePhase::CUE_RED;
            bootCuePhaseStartedAt = millis();
          }
          break;
        case BootCuePhase::CUE_GREEN:
          if (elapsed >= BOOT_CUE_GREEN_MS) finishBootCue();
          break;
        case BootCuePhase::CUE_RED:
          if (elapsed >= BOOT_CUE_RED_MS) finishBootCue();
          break;
        case BootCuePhase::CUE_DONE:
          break;
      }
    }

    // ---- OTA update helpers (docs/OTA.md Section 5) ----

    static const char* selectOtaKey(const String& keyId) {
      if (keyId == "primary") return kOtaKeyPrimary;
      if (keyId == "spare") return kOtaKeySpare;
      return nullptr;
    }

    // ota_failed's error field is hand-embedded into a JSON string, not run through a serializer --
    // strip anything that would break that (quotes, control chars) rather than properly escaping
    // it, since this is a short diagnostic string, not structured data. Matters because the HTTP
    // 400+ path embeds a real response body here, which is externally-influenced content.
    static String sanitizeForJsonString(const String& in, size_t maxLen = 120) {
      String out;
      for (size_t i = 0; i < in.length() && out.length() < maxLen; i++) {
        char c = in[i];
        if (c == '"' || c == '\\' || (uint8_t)c < 0x20) continue;
        out += c;
      }
      return out;
    }

    // https://<host>[:port]/<path-and-query> -> host/port/path. Only https:// is ever expected;
    // the manifest's url is otherwise opaque -- whatever's after the host is forwarded as-is.
    static bool parseHttpsUrl(const String& url, String& host, uint16_t& port, String& path) {
      if (!url.startsWith("https://")) return false;
      int hostStart = 8; // strlen("https://")
      int pathStart = url.indexOf('/', hostStart);
      String hostPort = (pathStart < 0) ? url.substring(hostStart) : url.substring(hostStart, pathStart);
      path = (pathStart < 0) ? "/" : url.substring(pathStart);

      int colonIdx = hostPort.indexOf(':');
      if (colonIdx < 0) {
        host = hostPort;
        port = 443;
      } else {
        host = hostPort.substring(0, colonIdx);
        port = (uint16_t)hostPort.substring(colonIdx + 1).toInt();
      }
      return host.length() > 0;
    }

    // Exponential backoff on SSL/connection failures specifically (500ms, 1s, 2s -- max 3 tries),
    // never on HTTP-level errors (those fail immediately below, no retry). Credited: this exact
    // schedule, and the reasoning (ESP32 TLS hardware-accelerator flakiness under load), come from
    // the reference OTA implementation docs/OTA.md Section 2 compares against. Reuses the same
    // `client` member as the wss link -- by the time this is ever called, that link has already
    // been closed (see performOtaDownload()), so only one TLS session is ever live at a time.
    bool connectHttpsWithRetry(const String& host, uint16_t port) {
      static const unsigned long kDelaysMs[] = {500, 1000, 2000};
      for (int attempt = 0; attempt < 3; attempt++) {
        client.setInsecure(); // same known debt as the wss connection -- see file header comment
        client.setHandshakeTimeout(10);
        client.setPlainStart();
        if (client.connect(host.c_str(), port, 10000) && client.startTLS()) {
          return true;
        }
        Serial.print(F("glowline ota: connect/TLS attempt "));
        Serial.print(attempt + 1);
        Serial.println(F(" failed"));
        client.stop();
        if (attempt < 2) delay(kDelaysMs[attempt]);
      }
      return false;
    }

    // For a response body that comfortably fits in RAM -- the manifest JSON, or an HTTP error
    // body. Never used for the firmware binary itself (see performOtaDownload(), which streams
    // straight into Update.write() without ever buffering the whole thing). Uses
    // "Connection: close" so end-of-body is just end-of-stream, no chunked-transfer handling
    // needed for these small, server-controlled responses.
    bool httpGetSmall(const String& host, uint16_t port, const String& path, int& status, String& body, int& retryAfterSec) {
      status = 0;
      body = "";
      retryAfterSec = -1;
      if (!connectHttpsWithRetry(host, port)) return false;

      client.print(F("GET "));
      client.print(path);
      client.print(F(" HTTP/1.1\r\nHost: "));
      client.print(host);
      client.print(F("\r\nConnection: close\r\n\r\n"));

      String statusLine = "";
      bool headersDone = false;
      unsigned long startedAt = millis();
      while (millis() - startedAt < 10000) {
        while (client.available()) {
          String line = client.readStringUntil('\n');
          while (line.length() && (line[line.length() - 1] == '\r' || line[line.length() - 1] == '\n')) line.remove(line.length() - 1);
          if (!headersDone) {
            if (statusLine.length() == 0) {
              statusLine = line;
              int sp1 = line.indexOf(' ');
              int sp2 = line.indexOf(' ', sp1 + 1);
              if (sp1 > 0 && sp2 > sp1) status = line.substring(sp1 + 1, sp2).toInt();
            } else if (line.length() == 0) {
              headersDone = true;
            } else if (line.startsWith("Retry-After:") || line.startsWith("retry-after:")) {
              retryAfterSec = line.substring(line.indexOf(':') + 1).toInt();
            }
          } else {
            body += line;
            body += '\n';
          }
        }
        if (!client.connected() && !client.available()) break;
      }
      client.stop();
      return statusLine.length() > 0;
    }

    // Rebuilds the exact signed byte string (docs/OTA.md Section 3's addendum) and verifies it
    // against whichever embedded key `keyId` selects. `url` is deliberately excluded from this --
    // it's untrusted transport, policed instead by the SHA-256 comparison after download.
    bool verifyManifestSignature(const String& version, uint32_t size, const String& sha256, const String& keyId, const String& signatureB64) {
      const char* pem = selectOtaKey(keyId);
      if (!pem) {
        Serial.print(F("glowline ota: unknown keyId: "));
        Serial.println(keyId);
        return false;
      }

      String signedBytes = version + "\n" + String(size) + "\n" + sha256 + "\n" + keyId;

      uint8_t hash[32];
      mbedtls_sha256_context shaCtx;
      mbedtls_sha256_init(&shaCtx);
      mbedtls_sha256_starts(&shaCtx, 0 /* SHA-256, not SHA-224 */);
      mbedtls_sha256_update(&shaCtx, (const unsigned char*)signedBytes.c_str(), signedBytes.length());
      mbedtls_sha256_finish(&shaCtx, hash);
      mbedtls_sha256_free(&shaCtx);

      uint8_t sigBuf[128];
      size_t sigLen = 0;
      if (mbedtls_base64_decode(sigBuf, sizeof(sigBuf), &sigLen, (const unsigned char*)signatureB64.c_str(), signatureB64.length()) != 0) {
        Serial.println(F("glowline ota: signature is not valid base64"));
        return false;
      }

      mbedtls_pk_context pk;
      mbedtls_pk_init(&pk);
      int ret = mbedtls_pk_parse_public_key(&pk, (const unsigned char*)pem, strlen(pem) + 1);
      if (ret != 0) {
        Serial.print(F("glowline ota: failed to parse embedded public key, keyId="));
        Serial.println(keyId);
        mbedtls_pk_free(&pk);
        return false;
      }

      ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sigBuf, sigLen);
      mbedtls_pk_free(&pk);

      if (ret != 0) {
        Serial.print(F("glowline ota: signature verification FAILED, mbedtls error -0x"));
        Serial.println(-ret, HEX);
        return false;
      }
      Serial.print(F("glowline ota: signature verified OK, keyId="));
      Serial.println(keyId);
      return true;
    }

    // Set once, right before the blocking check/download sequence starts. loop() is about to stop
    // iterating normally -- from under a second (a plain 204) to tens of seconds (an actual
    // download) -- so nothing will call strip.show() again on its own until this returns. This is
    // the only way anything reaches the physical LEDs during that window: a direct, out-of-band
    // render, bypassing the normal per-frame pipeline entirely. Dim solid blue, chosen to be
    // distinct from every other cue this usermod uses (the boot cue's white/green/red, the
    // config-save success signal's green/white) -- so it reads as "doing something on purpose,"
    // not as a crash or a frozen boot cue.
    void showOtaUpdatingCue() {
      bri = OTA_CUE_BRI;
      offMode = false;
      uint16_t total = strip.getLengthTotal();
      for (uint16_t i = 0; i < total; i++) strip.setPixelColor(i, RGBW32(0, 0, 255, 0));
      strip.show();
    }

    // Clears the updating cue after a failed attempt -- a successful one reboots instead, making
    // this moot. Goes through applyJsonState like every other post-boot-cue state change (normal
    // per-frame rendering has resumed by the time this runs), not another direct strip.show().
    void clearOtaUpdatingCue() {
      static const char kOff[] = "{\"on\":false}";
      applyJsonState((const uint8_t*)kOff, strlen(kOff));
    }

    // Records a failure to report once reconnected (the wss link may already be closed at this
    // point -- see performOtaDownload()), clears the updating cue, and re-arms the normal
    // reconnect path. Never sends anything itself; pollHandshake() sends the deferred report the
    // next time CONNECTED fires.
    void recordOtaFailure(const String& reason) {
      Serial.print(F("glowline ota: FAILED: "));
      Serial.println(reason);
      otaFailurePending = true;
      otaFailureReason = sanitizeForJsonString(reason);
      clearOtaUpdatingCue();
      resetAndScheduleImmediateConnect();
    }

    void sendOtaFailedFrame() {
      String msg = String("{\"type\":\"ota_result\",\"status\":\"failed\",\"error\":\"") + otaFailureReason + "\"}";
      sendFrame(0x1, (const uint8_t*)msg.c_str(), msg.length());
      Serial.print(F("glowline ota: sent failed report: "));
      Serial.println(otaFailureReason);
      otaFailurePending = false;
      otaFailureReason = "";
    }

    void sendOtaRolledBackFrame() {
      String msg = String("{\"type\":\"ota_result\",\"status\":\"rolled_back\",\"version\":\"") + otaKnownBadVersion + "\"}";
      sendFrame(0x1, (const uint8_t*)msg.c_str(), msg.length());
      Serial.print(F("glowline ota: sent rolled_back report for version "));
      Serial.println(otaKnownBadVersion);
      otaRollbackReportPending = false;
      Preferences prefs;
      prefs.begin("ota", false);
      prefs.putBool("rbPending", false);
      prefs.end();
    }

    // Downloads and flashes a verified update. By the time this is called, the signature has
    // already passed (checkForOtaUpdate()) -- this function's only remaining job is to fetch the
    // bytes, prove via SHA-256 that they're the bytes the manifest actually described, and commit
    // or abandon accordingly. Blocking throughout; see the class-level OTA comment for why.
    void performOtaDownload(const String& version, uint32_t size, const String& url, const String& sha256Hex) {
      String host, path;
      uint16_t port;
      if (!parseHttpsUrl(url, host, port, path)) {
        recordOtaFailure(F("manifest url not https"));
        return;
      }

      Serial.print(F("glowline ota: verified update available, version "));
      Serial.print(version);
      Serial.print(F(", size "));
      Serial.println(size);

      showOtaUpdatingCue();

      // Close the wss link before downloading: running its TLS session concurrently with the
      // download's own would mean two mbedtls sessions' record buffers live in internal SRAM at
      // once (~40KB each, based on this usermod's own measured wss-handshake heap cost). The
      // existing backoff/reconnect state machine already handles bringing it back afterward.
      client.stop();
      setState(WsState::DISCONNECTED);
      Serial.print(F("glowline ota: free heap after closing wss link: "));
      Serial.println(ESP.getFreeHeap());

      if (!connectHttpsWithRetry(host, port)) {
        recordOtaFailure(F("download: connect/TLS failed"));
        return;
      }
      Serial.print(F("glowline ota: free heap after download TLS connect: "));
      Serial.println(ESP.getFreeHeap());

      client.print(F("GET "));
      client.print(path);
      client.print(F(" HTTP/1.1\r\nHost: "));
      client.print(host);
      client.print(F("\r\nConnection: close\r\n\r\n"));

      String statusLine = "";
      bool headersDone = false;
      int status = 0;
      long contentLength = -1;
      unsigned long headerStartedAt = millis();
      while (!headersDone && millis() - headerStartedAt < 10000) {
        while (client.available()) {
          String line = client.readStringUntil('\n');
          while (line.length() && (line[line.length() - 1] == '\r' || line[line.length() - 1] == '\n')) line.remove(line.length() - 1);
          if (statusLine.length() == 0) {
            statusLine = line;
            int sp1 = line.indexOf(' ');
            int sp2 = line.indexOf(' ', sp1 + 1);
            if (sp1 > 0 && sp2 > sp1) status = line.substring(sp1 + 1, sp2).toInt();
          } else if (line.length() == 0) {
            headersDone = true;
            break;
          } else if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
            contentLength = line.substring(line.indexOf(':') + 1).toInt();
          }
        }
        if (!client.connected() && !client.available()) break;
      }

      // Read the response body for a 400+ error rather than trusting client.lastError() -- that
      // surfaces SSL-library errors, not HTTP error text, so using it here would silently swallow
      // whatever the server actually said.
      if (status != 200) {
        String body;
        unsigned long bodyStart = millis();
        while (millis() - bodyStart < 5000 && (client.available() || client.connected())) {
          while (client.available() && body.length() < 200) body += (char)client.read();
        }
        client.stop();
        recordOtaFailure(String("download: HTTP ") + status + ": " + body);
        return;
      }
      if (contentLength <= 0) {
        client.stop();
        recordOtaFailure(F("download: missing/invalid Content-Length"));
        return;
      }

      if (!Update.begin((size_t)contentLength)) {
        client.stop();
        recordOtaFailure(F("download: Update.begin() failed (not enough free space?)"));
        return;
      }
      Serial.print(F("glowline ota: free heap after Update.begin(): "));
      Serial.println(ESP.getFreeHeap());

      mbedtls_sha256_context shaCtx;
      mbedtls_sha256_init(&shaCtx);
      mbedtls_sha256_starts(&shaCtx, 0);

      uint8_t buf[512];
      long remaining = contentLength;
      unsigned long lastByteAt = millis();
      bool streamError = false;
      while (remaining > 0) {
        int avail = client.available();
        if (avail > 0) {
          int toRead = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
          if (toRead > remaining) toRead = (int)remaining;
          int n = client.read(buf, toRead);
          if (n > 0) {
            mbedtls_sha256_update(&shaCtx, buf, n);
            Update.write(buf, n);
            remaining -= n;
            lastByteAt = millis();
          }
        } else if (!client.connected()) {
          streamError = true;
          break;
        } else if (millis() - lastByteAt > 15000) {
          Serial.println(F("glowline ota: download stalled, aborting"));
          streamError = true;
          break;
        }
      }
      client.stop();

      if (streamError || remaining > 0) {
        mbedtls_sha256_free(&shaCtx);
        Update.abort();
        recordOtaFailure(F("download: connection dropped before completion"));
        return;
      }

      uint8_t hash[32];
      mbedtls_sha256_finish(&shaCtx, hash);
      mbedtls_sha256_free(&shaCtx);

      char hashHex[65];
      for (int i = 0; i < 32; i++) snprintf(&hashHex[i * 2], 3, "%02x", hash[i]);
      hashHex[64] = '\0';

      String expected = sha256Hex;
      expected.toLowerCase();
      if (String(hashHex) != expected) {
        Serial.print(F("glowline ota: SHA-256 MISMATCH, got "));
        Serial.print(hashHex);
        Serial.print(F(", expected "));
        Serial.println(expected);
        Update.abort();
        recordOtaFailure(F("download: sha256 mismatch"));
        return;
      }

      // Only past this point does anything touch the boot partition selector -- see this
      // usermod's confirmation (readme/commit message) that a power loss at any point before here
      // leaves the currently-running, already-working image untouched.
      if (!Update.end(true)) {
        recordOtaFailure(F("download: Update.end() failed"));
        return;
      }

      Serial.println(F("glowline ota: update written and verified, rebooting"));
      ESP.restart();
    }

    // Manifest check: sub-second. Runs once per WS CONNECTED transition (called from
    // pollHandshake()), gated by otaCheckBlockedUntil so a flapping connection can't hammer
    // /ota/check.
    void checkForOtaUpdate() {
      if ((long)(millis() - otaCheckBlockedUntil) < 0) return;
      otaCheckBlockedUntil = millis() + OTA_CHECK_COOLDOWN_MS;

      Serial.print(F("glowline ota: free heap before /ota/check: "));
      Serial.println(ESP.getFreeHeap());

      String path = String("/ota/check?device=") + urlEncode(wsDeviceId) + "&token=" + urlEncode(wsToken) + "&fw=" + urlEncode(String(F(GLOWLINE_FW_VERSION)));

      int status = 0;
      String body;
      int retryAfterSec = -1;
      if (!httpGetSmall(wsHost, wsPort, path, status, body, retryAfterSec)) {
        Serial.println(F("glowline ota: /ota/check request failed (connection-level)"));
        return; // transient -- the next CONNECTED (or reconnect) tries again
      }

      if (status == 204) {
        Serial.println(F("glowline ota: /ota/check -- 204, nothing to do"));
        return;
      }
      if (status == 429) {
        unsigned long retryMs = (retryAfterSec > 0) ? (unsigned long)retryAfterSec * 1000UL : OTA_CHECK_COOLDOWN_MS;
        if (retryMs < OTA_CHECK_COOLDOWN_MS) retryMs = OTA_CHECK_COOLDOWN_MS;
        otaCheckBlockedUntil = millis() + retryMs;
        Serial.print(F("glowline ota: /ota/check -- 429, backing off "));
        Serial.print(retryMs);
        Serial.println(F(" ms"));
        return;
      }
      if (status >= 400) {
        Serial.print(F("glowline ota: /ota/check -- HTTP "));
        Serial.print(status);
        Serial.print(F(": "));
        Serial.println(body);
        return;
      }
      if (status != 200) {
        Serial.print(F("glowline ota: /ota/check -- unexpected status "));
        Serial.println(status);
        return;
      }

      // Own small JSON document rather than WLED's shared pDoc -- pDoc is sized for WLED state
      // JSON, and this runs from inside the WS-connected path, not guaranteed free of a
      // concurrent applyJsonState() use.
      StaticJsonDocument<1024> manifestDoc;
      DeserializationError err = deserializeJson(manifestDoc, body);
      if (err) {
        Serial.print(F("glowline ota: manifest is not valid JSON: "));
        Serial.println(err.c_str());
        return;
      }
      String version = manifestDoc["version"] | "";
      uint32_t size = manifestDoc["size"] | 0;
      String url = manifestDoc["url"] | "";
      String sha256 = manifestDoc["sha256"] | "";
      String keyId = manifestDoc["keyId"] | "";
      String signature = manifestDoc["signature"] | "";
      if (version.length() == 0 || size == 0 || url.length() == 0 || sha256.length() == 0 || keyId.length() == 0 || signature.length() == 0) {
        Serial.println(F("glowline ota: manifest missing required field(s)"));
        return;
      }

      if (version == otaKnownBadVersion) {
        Serial.print(F("glowline ota: refusing known-bad version locally: "));
        Serial.println(version);
        return;
      }

      if (!verifyManifestSignature(version, size, sha256, keyId, signature)) return; // already logged why

      performOtaDownload(version, size, url, sha256);
    }

  public:
    void setup() {
      // Printed unconditionally, on every boot (including the one right after a rollback) --
      // across multiple flashed versions and a rollback in flight, this is the one thing that
      // says at a glance which build is actually running. Set per release via build_flags
      // (-D GLOWLINE_FW_VERSION=\"1.2.0\"); see the readme for the exact command.
      Serial.println(F("================================================================"));
      Serial.print(F("= glowline firmware version: "));
      Serial.println(F(GLOWLINE_FW_VERSION));
      Serial.println(F("================================================================"));

#if OTA_VERIFY_TIMEOUT_MS != (15UL * 60UL * 1000UL)
      // Impossible to miss in serial -- the *_BENCH_..._DO_NOT_SHIP env drops the rollback
      // confirmation window from 15 minutes to something bench-test-sized. If this ever prints on
      // a real unit, that unit was flashed with the wrong environment.
      Serial.println(F("################################################################"));
      Serial.println(F("# glowline: BENCH BUILD -- OTA rollback timeout is NOT 15 minutes #"));
      Serial.print(F("# Actual timeout (ms): "));
      Serial.println((unsigned long)OTA_VERIFY_TIMEOUT_MS);
      Serial.println(F("# DO NOT SHIP THIS BUILD TO A REAL UNIT"));
      Serial.println(F("################################################################"));
#endif

      // "rmt" tag spams a "flush timeout" error on every non-blocking poll of
      // rmt_tx_wait_all_done() -- a cosmetic ESP-IDF logging bug
      // (espressif/esp-idf#17527), not an actual failure. It floods the
      // serial line badly enough to bury real output, so silence it.
      esp_log_level_set("rmt", ESP_LOG_NONE);

      // OTA rollback detection: PENDING_VERIFY only happens right after a fresh OTA flash --
      // never after a USB/factory flash or an already-confirmed reboot. Mark-valid (clearing this)
      // happens only in pollHandshake()'s CONNECTED branch, never here and never on WiFi
      // association alone -- see that comment for why.
      const esp_partition_t* runningPartition = esp_ota_get_running_partition();
      esp_ota_img_states_t otaState;
      if (runningPartition && esp_ota_get_state_partition(runningPartition, &otaState) == ESP_OK && otaState == ESP_OTA_IMG_PENDING_VERIFY) {
        otaPendingVerify = true;
        otaBootTime = millis();
        Serial.println(F("glowline ota: running image is PENDING_VERIFY -- awaiting a successful WS connect to confirm"));
      }

      {
        Preferences prefs;
        prefs.begin("ota", true); // read-only
        otaKnownBadVersion = prefs.getString("failedVer", "");
        otaRollbackReportPending = prefs.getBool("rbPending", false);
        prefs.end();
      }
      if (otaRollbackReportPending && otaKnownBadVersion.length() > 0) {
        Serial.print(F("glowline ota: booted after a rollback from version "));
        Serial.print(otaKnownBadVersion);
        Serial.println(F(" -- will report once connected"));
      }

      // Start the boot cue immediately, before WLED's own first render:
      // beginStrip() (which runs just before usermod setup()) sets bri/color
      // to its own on+orange default, but doesn't call strip.show() with
      // those values -- the first actual render happens once loop() starts,
      // by which point this has already taken over. bri is forced to a
      // fixed 50% for the whole cue; offMode is forced false so the render
      // loop actually runs even if "turn on at boot" is configured off.
      bri = BOOT_CUE_BRI;
      offMode = false;
      bootCuePhaseStartedAt = millis();
    }

    // Called every frame, after effects are processed but before strip.show()
    // -- lets the boot cue paint raw pixels with exact timing, independent of
    // WLED's effect engine and whatever segment/effect state underlies it.
    void handleOverlayDraw() {
      if (bootCuePhase == BootCuePhase::CUE_DONE) return;
      uint16_t total = strip.getLengthTotal();
      uint32_t color;
      uint16_t lit = total;
      switch (bootCuePhase) {
        case BootCuePhase::CUE_FILL: {
          unsigned long elapsed = millis() - bootCuePhaseStartedAt;
          uint32_t filled = ((uint32_t)total * (elapsed < BOOT_CUE_FILL_MS ? elapsed : BOOT_CUE_FILL_MS)) / BOOT_CUE_FILL_MS;
          lit = (uint16_t)filled;
          color = RGBW32(255, 255, 255, 0);
          break;
        }
        case BootCuePhase::CUE_WAIT_WIFI:
          color = RGBW32(255, 255, 255, 0);
          break;
        case BootCuePhase::CUE_GREEN:
          color = RGBW32(0, 255, 0, 0);
          break;
        case BootCuePhase::CUE_RED:
          color = RGBW32(255, 0, 0, 0);
          break;
        default:
          return;
      }
      for (uint16_t i = 0; i < total; i++) {
        strip.setPixelColor(i, i < lit ? color : (uint32_t)0);
      }
    }

    void loop() {
      advanceBootCue();

      // Rollback timeout: unaffected by anything else in loop() being blocked (the OTA
      // check/download sequence runs on the OLD, already-confirmed image, before any reboot --
      // otaPendingVerify is only ever true on a *different* boot, the one right after flashing the
      // new image, when nothing here blocks loop() at all). Checked unconditionally, independent
      // of WiFi/wss state, so a build that can't even associate to WiFi still rolls back instead
      // of bricking silently forever.
      if (otaPendingVerify && millis() - otaBootTime > OTA_VERIFY_TIMEOUT_MS) {
        Serial.println(F("glowline ota: not confirmed within timeout -- persisting failure and rolling back"));
        Preferences prefs;
        prefs.begin("ota", false);
        prefs.putString("failedVer", String(F(GLOWLINE_FW_VERSION)));
        prefs.putBool("rbPending", true);
        prefs.end();
        esp_ota_mark_app_invalid_rollback_and_reboot(); // does not return on success
      }

      if (millis() - lastTime_ >= INTERVAL_MS) {
        lastTime_ = millis();
        Serial.print(F("glowline usermod alive, free heap: "));
        Serial.print(ESP.getFreeHeap());
        Serial.print(F(", ws: "));
        Serial.print(stateName(wsState));
        // Printed unconditionally every 5s (not just on state transitions) so
        // a stalled retry loop is visible in serial even if no transition
        // ever fires -- silence alone becomes evidence of a stall instead of
        // being ambiguous with "nothing to report yet".
        if (wsState == WsState::DISCONNECTED && reconnectDue) {
          Serial.print(F(", next retry in "));
          Serial.print((long)(nextAttemptAt - millis()));
          Serial.print(F(" ms"));
        } else if (wsState == WsState::CONNECTED) {
          Serial.print(F(", last inbound "));
          Serial.print((millis() - lastInboundAt) / 1000);
          Serial.print(F("s ago"));
        }
        Serial.println();
      }

      if (successSignalPlaying && millis() - successSignalStartedAt >= SUCCESS_SIGNAL_GREEN_MS) {
        finishSuccessSignal();
      }

      if (wsHost.length() == 0 || wsPort == 0) return; // not configured yet

      if (!WLED_CONNECTED) {
        if (wsState == WsState::CONNECTED || wsState == WsState::CONNECTING) onDisconnected(F("WiFi lost"));
        return;
      }

      if (wsState == WsState::DISCONNECTED) {
        if (reconnectDue && (long)(millis() - nextAttemptAt) >= 0) {
          reconnectDue = false;
          Serial.print(F("glowline ws: retry attempt firing (waited "));
          Serial.print(lastBackoffMs);
          Serial.println(F(" ms)"));
          beginConnect();
        }
        return;
      }

      if (!client.connected()) {
        onDisconnected(F("tcp closed"));
        return;
      }

      if (wsState == WsState::CONNECTING) {
        pollHandshake();
      } else if (wsState == WsState::CONNECTED) {
        pollFrames();
        if (wsState == WsState::CONNECTED) checkLiveness(); // pollFrames() may have disconnected us (e.g. a close frame)
      }
    }

    void appendConfigData() {
      oappend(F("addInfo('glowline:host',1,'WebSocket server hostname or IP (wss://, TLS but not certificate-verified)');"));
      oappend(F("addInfo('glowline:port',1,'WebSocket server port (443 for a deployed Worker)');"));
      oappend(F("addInfo('glowline:deviceId',1,'Device ID sent as a query param on /ws');"));
      oappend(F("addInfo('glowline:token',1,'Auth token sent as a query param on /ws (encrypted in transit, but the server cert is not verified)');"));
    }

    void addToConfig(JsonObject& root) {
      JsonObject top = root.createNestedObject(F("glowline"));
      top[F("host")] = wsHost;
      top[F("port")] = wsPort;
      top[F("deviceId")] = wsDeviceId;
      top[F("token")] = wsToken;
    }

    bool readFromConfig(JsonObject& root) {
      JsonObject top = root[F("glowline")];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[F("host")], wsHost, String(""));
      configComplete &= getJsonValue(top[F("port")], wsPort, (uint16_t)0);
      configComplete &= getJsonValue(top[F("deviceId")], wsDeviceId, String(""));
      configComplete &= getJsonValue(top[F("token")], wsToken, String(""));

#ifdef GLOWLINE_OTA_TEST_FORCE_BAD_HOST
      // OTA rollback bench-testing only: ignore whatever host is actually saved in Settings ->
      // Usermods (that config lives in cfg.json on the filesystem partition, which an OTA app
      // update never touches -- a real "broken" OTA build would otherwise just keep using the
      // correct, already-working host and never actually fail). example.com resolves and
      // completes a TLS handshake, so beginConnect() gets past DNS/TCP/TLS, but it isn't a
      // WebSocket server, so pollHandshake() never sees a 101 and CONNECTED never fires --
      // mark-valid never runs, and the rollback timeout in loop() eventually fires for real.
      // NEVER define this flag in a build meant to run for real.
      wsHost = "example.com";
      wsPort = 443;
#endif

      // Apply new settings: drop any existing connection and reconnect
      // immediately, whether this is the initial boot load or a change
      // saved from Settings -> Usermods.
      resetAndScheduleImmediateConnect();

      // Only a live save (the second-and-later call) counts as "a config
      // change" for the success signal -- not the initial boot-time load of
      // whatever was already saved.
      if (hasLoadedConfigOnce) successSignalPending = true;
      hasLoadedConfigOnce = true;

      return configComplete;
    }
};

static GlowlineUsermod glowline;
REGISTER_USERMOD(glowline);
