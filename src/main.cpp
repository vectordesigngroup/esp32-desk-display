#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>

#include "board.h"
#include "config.h"
#include "data.h"
#include "portal.h"
#include "secrets.h"
#include "ui.h"

static void connectWifi() {
  uiBoot("DESK DISPLAY", "Joining Wi-Fi", WIFI_SSID);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_STA);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      Serial.printf("wifi reason %u\n", (unsigned)info.wifi_sta_disconnected.reason);
    }
  });

  int networks = WiFi.scanNetworks();
  bool found = false;
  int rssi = 0;
  int channel = 0;
  for (int i = 0; i < networks; i++) {
    if (WiFi.SSID(i) == WIFI_SSID) {
      found = true;
      rssi = WiFi.RSSI(i);
      channel = WiFi.channel(i);
    }
  }
  WiFi.scanDelete();
  if (found) Serial.printf("ap visible rssi %d ch %d\n", rssi, channel);
  else Serial.printf("ap not visible, others %d\n", networks);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 25000) {
    delay(500);
    Serial.printf("st %d\n", (int)WiFi.status());
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("wifi %s\n", WiFi.localIP().toString().c_str());
    return;
  }
  Serial.printf("join failed st %d\n", (int)WiFi.status());
  WiFi.disconnect(true);
  delay(300);

  uiBoot("SETUP WI-FI", "Join DeskDisplay", "Then open 192.168.4.1");
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(20);
  if (!wm.autoConnect("DeskDisplay")) {
    uiBoot("NO WI-FI", "Restarting", "Check the 2.4 GHz network");
    delay(2500);
    ESP.restart();
  }
  Serial.printf("wifi %s\n", WiFi.localIP().toString().c_str());
}

static bool fell(uint8_t pin, bool& held) {
  bool down = digitalRead(pin) == LOW;
  bool edge = down && !held;
  held = down;
  return edge;
}

void setup() {
  Serial.begin(115200);
  uint32_t wait = millis();
  while (!Serial && millis() - wait < 800) delay(10);
  Serial.println("desk display boot");

  uiBegin();
  dataBegin();
  boardBegin();
  connectWifi();
  configTzTime(boardTzPosix(), "pool.ntp.org", "time.nist.gov");
  if (WiFi.status() == WL_CONNECTED) portalBegin();
  uiBoot("DESK DISPLAY", "Loading", WiFi.localIP().toString().c_str());
  dataLoop();
}

void loop() {
  static bool prevHeld = false;
  static bool nextHeld = false;
  static uint32_t lastEdge = 0;

  bool fetched = dataLoop();
  if (fetched) {
    const Status& s = dataStatus();
    const Weather& w = dataWeather();
    Serial.printf(
        "bridge %s blocks %d price %d posts %d pi %s disk %d startos %s weather %d/%d rain %d hashpts %d\n",
        s.bridge ? "ok" : "down", s.blocks, s.priceUsd, s.postCount, s.piUp ? "up" : "down", s.piDisk,
        s.startos ? "up" : "down", w.forecast ? w.days[0].hi : 0, w.forecast ? w.days[0].lo : 0,
        w.forecast ? w.days[0].rainPct : -1, s.hashPoints);
  }

  portalLoop();

  uint32_t now = millis();
  if (now - lastEdge > 200) {
    if (fell(PIN_BUTTON_PREV, prevHeld)) {
      uiPrev();
      lastEdge = now;
    }
    if (fell(PIN_BUTTON_NEXT, nextHeld)) {
      uiNext();
      lastEdge = now;
    }
  } else {
    prevHeld = digitalRead(PIN_BUTTON_PREV) == LOW;
    nextHeld = digitalRead(PIN_BUTTON_NEXT) == LOW;
  }

  uiLoop();
  delay(20);
}
