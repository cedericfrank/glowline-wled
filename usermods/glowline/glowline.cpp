#include "wled.h"
#include <WiFi.h>
#include <base64.h>

/*
 * Minimal diagnostic usermod. Does not affect LED output or WLED state.
 *
 * - Prints a heartbeat and the current free heap to Serial every 5 seconds.
 * - Maintains a plaintext (ws://) WebSocket connection to
 *   ws://host:port/ws?device=<deviceId>&token=<token>, where host, port,
 *   deviceId and token are all configurable in Settings -> Usermods (nothing
 *   hardcoded). Sends a hello message on connect, logs every message
 *   received, and reconnects with exponential backoff (1s doubling up to a
 *   60s cap) on disconnect. Every state transition is logged, and free heap
 *   is logged on connect.
 *
 * The WebSocket client (handshake + RFC 6455 framing) is hand-rolled on top
 * of plain WiFiClient rather than using a third-party library: this
 * particular Arduino-ESP32 core build ships no TLS-capable client class at
 * all (no WiFiClientSecure/NetworkClientSecure header), and every WebSocket
 * client library available for it unconditionally includes one even when
 * only plaintext ws:// is needed. Revisit this once the core exposes a
 * usable TLS client and wss:// support is wanted.
 */
class GlowlineUsermod : public Usermod {
  private:
    // Heartbeat
    unsigned long lastTime_ = 0;
    static const unsigned long INTERVAL_MS = 5000;

    // WebSocket config (Settings -> Usermods)
    String wsHost = "";
    uint16_t wsPort = 0;
    String wsDeviceId = "";
    String wsToken = "";

    // TCP connection
    WiFiClient client;

    enum class WsState : uint8_t { DISCONNECTED, CONNECTING, CONNECTED };
    WsState wsState = WsState::DISCONNECTED;

    static const unsigned long BACKOFF_MIN_MS = 1000;
    static const unsigned long BACKOFF_MAX_MS = 60000;
    unsigned long backoffMs = BACKOFF_MIN_MS;
    bool reconnectDue = false;
    unsigned long nextAttemptAt = 0;

    // Handshake
    static const unsigned long HANDSHAKE_TIMEOUT_MS = 5000;
    unsigned long connectStartedAt = 0;
    String handshakeStatusLine = "";

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

      if (!client.connect(wsHost.c_str(), wsPort)) {
        onDisconnected(F("tcp connect failed"));
        return;
      }

      client.print(F("GET /ws?device="));
      client.print(urlEncode(wsDeviceId));
      client.print(F("&token="));
      client.print(urlEncode(wsToken));
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
          Serial.print(F("glowline ws: connected, free heap: "));
          Serial.println(ESP.getFreeHeap());
          sendFrame(0x1, (const uint8_t*)"hello from glowline", 20);
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

    void finishFrame() {
      size_t len = (size_t)((payloadIdx < MAX_FRAME_PAYLOAD) ? payloadIdx : MAX_FRAME_PAYLOAD);
      switch (frameOpcode) {
        case 0x1: // text
        case 0x0: // continuation (treated as text here)
          Serial.print(F("glowline ws: received: "));
          Serial.write(frameBuf, len);
          if (payloadIdx > MAX_FRAME_PAYLOAD) Serial.print(F(" ...[truncated]"));
          Serial.println();
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

  public:
    void setup() {}

    void loop() {
      if (millis() - lastTime_ >= INTERVAL_MS) {
        lastTime_ = millis();
        Serial.print(F("glowline usermod alive, free heap: "));
        Serial.println(ESP.getFreeHeap());
      }

      if (wsHost.length() == 0 || wsPort == 0) return; // not configured yet

      if (!WLED_CONNECTED) {
        if (wsState == WsState::CONNECTED || wsState == WsState::CONNECTING) onDisconnected(F("WiFi lost"));
        return;
      }

      if (wsState == WsState::DISCONNECTED) {
        if (reconnectDue && (long)(millis() - nextAttemptAt) >= 0) {
          reconnectDue = false;
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
      }
    }

    void appendConfigData() {
      oappend(F("addInfo('glowline:host',1,'WebSocket server hostname or IP (ws://, no TLS)');"));
      oappend(F("addInfo('glowline:port',1,'WebSocket server port');"));
      oappend(F("addInfo('glowline:deviceId',1,'Device ID sent as a query param on /ws');"));
      oappend(F("addInfo('glowline:token',1,'Auth token sent as a query param on /ws (sent in plaintext over ws://)');"));
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

      // Apply new settings: drop any existing connection and reconnect
      // immediately, whether this is the initial boot load or a change
      // saved from Settings -> Usermods.
      resetAndScheduleImmediateConnect();

      return configComplete;
    }
};

static GlowlineUsermod glowline;
REGISTER_USERMOD(glowline);
