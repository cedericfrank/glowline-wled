#include "wled.h"

/*
 * Minimal usermod: prints a heartbeat and the current free heap to Serial
 * every 5 seconds. Does not affect LED output or WLED state.
 */
class GlowlineUsermod : public Usermod {
  private:
    unsigned long lastTime_ = 0;
    static const unsigned long INTERVAL_MS = 5000;

  public:
    void setup() {}

    void loop() {
      if (millis() - lastTime_ < INTERVAL_MS) return;
      lastTime_ = millis();

      Serial.print(F("glowline usermod alive, free heap: "));
      Serial.println(ESP.getFreeHeap());
    }
};

static GlowlineUsermod glowline;
REGISTER_USERMOD(glowline);
