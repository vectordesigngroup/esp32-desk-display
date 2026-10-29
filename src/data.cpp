#include "data.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "config.h"
#include "secrets.h"

static Status st;
static Weather wx;
static uint32_t generation = 0;
static uint32_t weatherGen = 0;
static uint32_t nextStatus = 0;
static uint32_t nextWeather = 0;
static uint32_t nextReconnect = 0;

static const char* wmo(int code) {
  if (code == 0) return "Clear";
  if (code <= 3) return "Cloudy";
  if (code <= 48) return "Fog";
  if (code <= 57) return "Drizzle";
  if (code <= 67) return "Rain";
  if (code <= 77) return "Snow";
  if (code <= 82) return "Showers";
  if (code <= 86) return "Snow";
  if (code <= 99) return "Storm";
  return "Weather";
}

const char* weatherSummary(int code) { return wmo(code); }

static void copyStr(char* dest, size_t n, const char* src) {
  if (!src) src = "";
  strncpy(dest, src, n - 1);
  dest[n - 1] = 0;
}

static bool tcpOpen(const char* host, uint16_t port) {
  WiFiClient client;
  bool ok = client.connect(host, port, 900);
  client.stop();
  return ok;
}

static bool getUrl(const char* url, String& out, bool secure, bool withKey, uint16_t timeoutMs = 8000) {
  HTTPClient http;
  int code = -1;
  if (secure) {
    WiFiClientSecure client;
    client.setInsecure();
    if (!http.begin(client, url)) return false;
    http.setTimeout(timeoutMs);
    code = http.GET();
    if (code == 200) out = http.getString();
    http.end();
  } else {
    WiFiClient client;
    if (!http.begin(client, url)) return false;
    if (withKey) http.addHeader("X-Desk-Key", BRIDGE_TOKEN);
    http.setTimeout(15000);
    code = http.GET();
    if (code == 200) out = http.getString();
    http.end();
  }
  return code == 200;
}

static void applyScore(JsonVariant value) {
  if (value.isNull()) {
    st.scorePct = -1;
    return;
  }
  float score = value.as<float>();
  if (score < 0) st.scorePct = -1;
  else if (score <= 1.0f) st.scorePct = (int)(score * 100.0f + 0.5f);
  else st.scorePct = (int)(score + 0.5f);
}

static bool applyBridge(const String& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  JsonObject btc = doc["bitcoin"];
  st.btcOk = btc["online"] | false;
  st.btcLocal = btc["local"] | false;
  st.blocks = btc["blocks"] | 0;
  st.headers = btc["headers"] | 0;
  st.tip = btc["tip"] | 0;
  st.mempoolTx = btc["mempool_tx"] | 0;
  st.peers = btc["peers"] | 0;
  st.feeFast = btc["fee_fast"] | 0;
  st.feeHalf = btc["fee_half"] | 0;
  st.feeHour = btc["fee_hour"] | 0;
  st.feeEcon = btc["fee_econ"] | 0;
  st.localFee = btc["local_fee"] | 0;
  st.progress = btc["progress"] | 0.0f;
  st.ibd = btc["ibd"] | false;
  st.pruned = btc["pruned"] | false;

  JsonObject m = doc["market"];
  st.priceOk = m["price_ok"] | false;
  st.priceChgOk = m["chg_ok"] | false;
  st.priceUsd = m["usd"] | 0;
  st.priceChg10 = m["chg10"] | 0;
  st.quoteOk = false;
  st.quoteChgOk = false;
  st.quote = 0;
  st.quoteChg10 = 0;
  st.quotePoints = 0;
  char qcur[8];
  char qspan[8];
  copyStr(qcur, sizeof(qcur), m["qcur"] | "");
  copyStr(qspan, sizeof(qspan), m["qspan"] | "");
  if (qcur[0] && strcmp(qcur, boardCur()) == 0 && strcmp(qspan, boardPriceSpan()) == 0) {
    st.quote = m["quote"] | 0;
    st.quoteOk = (m["quote_ok"] | false) || st.quote > 0;
    st.quoteChgOk = m["qchg_ok"] | false;
    st.quoteChg10 = m["qchg10"] | 0;
    JsonArray quotes = m["quotes"].as<JsonArray>();
    if (!quotes.isNull()) {
      for (JsonVariant point : quotes) {
        if (st.quotePoints >= 30) break;
        int price = point | 0;
        if (price > 0) st.quoteSeries[st.quotePoints++] = price;
      }
    }
  }
  st.memCount = m["count"] | 0;
  st.memVsize = m["vsize"] | 0;
  st.nextTx = m["next_tx"] | 0;
  st.nextMed100 = m["next_med100"] | 0;
  st.hashEh = m["hash_eh"] | 0;
  st.hashPoints = 0;
  JsonArray series = m["series"].as<JsonArray>();
  if (!series.isNull()) {
    for (JsonVariant point : series) {
      if (st.hashPoints >= 30) break;
      int eh = point | 0;
      if (eh > 0) st.hashSeries[st.hashPoints++] = eh;
    }
  }
  st.diffT10 = m["diff_t10"] | 0;
  st.epochOk = m["epoch_ok"] | false;
  st.adj10 = m["adj10"] | 0;
  st.diffLeft = m["left"] | 0;
  st.etaMin = m["eta_min"] | 0;
  st.prev10 = m["prev10"] | 0;
  st.halvingLeft = m["halving"] | 0;
  st.lastTx = m["last_tx"] | 0;
  st.lastAge = m["last_age"] | 0;
  st.lastReward = m["last_reward"] | 0;
  copyStr(st.lastPool, sizeof(st.lastPool), m["pool"] | "");
  st.lnOk = m["ln_ok"] | false;
  st.lnNodes = m["ln_nodes"] | 0;
  st.lnChannelsPub = m["ln_ch"] | 0;
  st.lnBtc = m["ln_btc"] | 0;
  st.lnPpm = m["ln_ppm"] | 0;
  copyStr(st.lnAsOf, sizeof(st.lnAsOf), m["ln_asof"] | "");

  st.postsOk = false;
  st.postCount = 0;
  JsonArray items = doc["posts"]["items"].as<JsonArray>();
  if (!items.isNull()) {
    for (JsonObject item : items) {
      if (st.postCount >= 3) break;
      int i = st.postCount;
      copyStr(st.postUser[i], sizeof(st.postUser[i]), item["user"] | "");
      copyStr(st.postText[i], sizeof(st.postText[i]), item["text"] | "");
      st.postAgo[i] = item["ago"] | 0;
      if (st.postText[i][0]) st.postCount++;
    }
  }
  st.postsOk = st.postCount > 0;

  JsonObject light = doc["lightning"];
  st.startos = light["startos"] | false;
  JsonVariant cln = light["cln"];
  st.clnOk = !cln.isNull();
  st.clnChannels = st.clnOk ? (cln["channels"] | 0) : 0;
  st.clnSat = st.clnOk ? (cln["local_sat"] | 0L) : 0;
  JsonVariant lnd = light["lnd"];
  st.lndOk = !lnd.isNull();
  st.lndChannels = st.lndOk ? (lnd["channels"] | 0) : 0;
  st.lndSat = st.lndOk ? (lnd["local_sat"] | 0L) : 0;

  JsonObject servers = doc["servers"];
  JsonObject pi = servers["pi"];
  st.piUp = pi["up"] | false;
  st.piStats = pi["stats"] | false;
  st.piLoad = pi["load"] | 0.0f;
  st.piMem = pi["mem"] | 0;
  st.piDisk = pi["disk"] | 0;
  st.piUptime = pi["uptime"] | 0;
  st.jellyfin = servers["jellyfin"] | false;
  st.vault = servers["vaultwarden"] | false;
  st.frigateUp = servers["frigate"] | false;
  if (servers["startos"].is<bool>()) st.startos = servers["startos"] | false;

  JsonObject ev = doc["frigate"];
  st.eventOk = ev["ok"] | false;
  st.eventEnded = ev["ended"] | false;
  st.ago = ev["ago"] | 0;
  copyStr(st.camera, sizeof(st.camera), ev["camera"] | "");
  copyStr(st.label, sizeof(st.label), ev["label"] | "");
  applyScore(ev["score"]);

  st.bridge = true;
  generation++;
  return true;
}

static void fetchDirect() {
  if (PI_HOST[0]) {
    st.jellyfin = tcpOpen(PI_HOST, 8096);
    st.vault = tcpOpen(PI_HOST, 8222);
    st.frigateUp = tcpOpen(PI_HOST, 8971);
    st.piUp = st.jellyfin || st.vault || st.frigateUp || tcpOpen(PI_HOST, 22);
  }
  if (STARTOS_HOST[0]) st.startos = tcpOpen(STARTOS_HOST, 443) || tcpOpen(STARTOS_HOST, 22);
  st.bridge = false;
  generation++;
}

static bool fetchMempool() {
  String body;
  if (!getUrl("https://mempool.space/api/blocks/tip/height", body, true, false)) return false;
  int tip = body.toInt();
  if (tip <= 0) return false;
  st.tip = tip;
  if (!st.btcLocal) {
    st.blocks = tip;
    st.headers = tip;
    st.btcOk = true;
    st.progress = 1;
    st.ibd = false;
  }
  String fees;
  if (getUrl("https://mempool.space/api/v1/fees/recommended", fees, true, false)) {
    JsonDocument doc;
    if (!deserializeJson(doc, fees)) {
      st.feeFast = doc["fastestFee"] | 0;
      st.feeHalf = doc["halfHourFee"] | 0;
      st.feeHour = doc["hourFee"] | 0;
      st.feeEcon = doc["economyFee"] | 0;
    }
  }
  String prices;
  if (getUrl("https://mempool.space/api/v1/prices", prices, true, false)) {
    JsonDocument priceDoc;
    if (!deserializeJson(priceDoc, prices)) {
      int usd = priceDoc["USD"] | 0;
      if (usd > 0) {
        st.priceUsd = usd;
        st.priceOk = true;
      }
    }
  }
  st.btcOk = true;
  generation++;
  return true;
}

static bool fetchBridge() {
  char url[192];
  snprintf(url, sizeof(url), "http://%s:%d/status?span=%s&tag=%s&pspan=%s&cur=%s", BRIDGE_HOST, BRIDGE_PORT,
           boardSpan(), boardTag(), boardPriceSpan(), boardCur());
  String body;
  if (!getUrl(url, body, false, true)) return false;
  return applyBridge(body);
}

static bool geocodeZip() {
  char url[64];
  snprintf(url, sizeof(url), "https://api.zippopotam.us/us/%s", boardZip());
  String body;
  if (!getUrl(url, body, true, false, 8000)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonArray places = doc["places"].as<JsonArray>();
  if (places.isNull() || places.size() == 0) return false;
  const char* lat = places[0]["latitude"].as<const char*>();
  const char* lon = places[0]["longitude"].as<const char*>();
  const char* name = places[0]["place name"].as<const char*>();
  if (!lat || !lon || !lat[0] || !lon[0]) return false;
  boardSetPlace(lat, lon, name ? name : boardZip());
  return boardPlaceOk();
}

static int roundToInt(float value) {
  return (int)(value + (value >= 0 ? 0.5f : -0.5f));
}

// 0 = Sunday. 1970-01-01 was a Thursday.
static int ymdWeekday(int year, int month, int day) {
  year -= month <= 2;
  int era = (year >= 0 ? year : year - 399) / 400;
  unsigned yoe = (unsigned)(year - era * 400);
  unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int z = era * 146097 + (int)doe - 719468;
  int weekday = (z + 4) % 7;
  if (weekday < 0) weekday += 7;
  return weekday;
}

static bool applyDaily(JsonDocument& doc) {
  static const char* kDow[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  JsonArray times = doc["daily"]["time"].as<JsonArray>();
  JsonArray codes = doc["daily"]["weather_code"].as<JsonArray>();
  JsonArray highs = doc["daily"]["temperature_2m_max"].as<JsonArray>();
  JsonArray lows = doc["daily"]["temperature_2m_min"].as<JsonArray>();
  JsonArray rain = doc["daily"]["precipitation_sum"].as<JsonArray>();
  JsonArray chance = doc["daily"]["precipitation_probability_max"].as<JsonArray>();
  if (times.isNull() || highs.isNull() || lows.isNull()) return false;

  struct tm now;
  bool hasTime = getLocalTime(&now, 0);
  int count = (int)times.size();
  if (count > 4) count = 4;
  if (count <= 0) return false;

  for (int i = 0; i < 4; i++) {
    ForecastDay& day = wx.days[i];
    if (i >= count) {
      day.code = -1;
      day.hi = 0;
      day.lo = 0;
      day.rainPct = -1;
      day.rain100 = 0;
      day.label[0] = 0;
      continue;
    }
    int year = 0;
    int month = 0;
    int dom = 0;
    const char* date = times[i] | "";
    sscanf(date, "%d-%d-%d", &year, &month, &dom);
    bool today = hasTime && year == now.tm_year + 1900 && month == now.tm_mon + 1 && dom == now.tm_mday;
    if (today || (!hasTime && i == 0)) copyStr(day.label, sizeof(day.label), "Today");
    else if (month >= 1 && month <= 12 && dom >= 1) {
      copyStr(day.label, sizeof(day.label), kDow[ymdWeekday(year, month, dom)]);
    } else {
      snprintf(day.label, sizeof(day.label), "D+%d", i);
    }
    day.code = codes.isNull() ? -1 : (codes[i] | -1);
    day.hi = roundToInt(highs[i] | 0.0f);
    day.lo = roundToInt(lows[i] | 0.0f);
    day.rainPct = chance.isNull() ? -1 : (chance[i] | 0);
    float inches = rain.isNull() ? 0.0f : (rain[i] | 0.0f);
    day.rain100 = roundToInt(inches * 100.0f);
    if (day.rain100 < 0) day.rain100 = 0;
  }
  return true;
}

static bool fetchWeather() {
  if (!boardPlaceOk() && !geocodeZip()) {
    Serial.println("zip fail");
    return false;
  }
  char url[400];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
           "&current=temperature_2m,relative_humidity_2m,weather_code"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,"
           "precipitation_probability_max&temperature_unit=fahrenheit"
           "&precipitation_unit=inch&timezone=%s&forecast_days=4",
           boardLat(), boardLon(), boardTzQuery());
  String body;
  if (!getUrl(url, body, true, false, 12000)) {
    Serial.println("weather fail");
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.println("weather fail");
    return false;
  }

  bool any = false;
  JsonObject cur = doc["current"];
  if (!cur.isNull()) {
    wx.tempF = cur["temperature_2m"] | 0.0f;
    wx.humidity = cur["relative_humidity_2m"] | 0;
    wx.code = cur["weather_code"] | -1;
    copyStr(wx.summary, sizeof(wx.summary), wmo(wx.code));
    wx.ok = true;
    any = true;
  }
  if (applyDaily(doc)) {
    wx.forecast = true;
    any = true;
  }
  if (!any) {
    Serial.println("weather fail");
    return false;
  }
  weatherGen++;
  Serial.printf("weather ok %d hi %d lo %d rain %d %s\n", (int)(wx.tempF + (wx.tempF >= 0 ? 0.5f : -0.5f)),
                wx.forecast ? wx.days[0].hi : 0, wx.forecast ? wx.days[0].lo : 0,
                wx.forecast ? wx.days[0].rainPct : -1, boardPlace());
  return true;
}

void dataRefreshWeather() { nextWeather = 0; }

void dataInvalidateQuote() {
  st.quoteOk = false;
  st.quoteChgOk = false;
  st.quote = 0;
  st.quoteChg10 = 0;
  st.quotePoints = 0;
}

void dataKickStatus() { nextStatus = 0; }

void dataBegin() {
  memset(&st, 0, sizeof(st));
  memset(&wx, 0, sizeof(wx));
  st.scorePct = -1;
  wx.code = -1;
}

bool dataLoop() {
  if (WiFi.status() != WL_CONNECTED) {
    uint32_t now = millis();
    if ((int32_t)(now - nextReconnect) >= 0) {
      nextReconnect = now + 10000;
      WiFi.reconnect();
    }
    return false;
  }

  bool fetched = false;
  uint32_t now = millis();
  if (nextWeather == 0 || (int32_t)(now - nextWeather) >= 0) {
    bool ok = fetchWeather();
    nextWeather = now + (ok ? WEATHER_EVERY_MS : 20000);
    fetched = true;
  }
  if (nextStatus == 0 || (int32_t)(now - nextStatus) >= 0) {
    nextStatus = now + STATUS_EVERY_MS;
    if (!fetchBridge()) {
      fetchDirect();
      if (!st.btcLocal) fetchMempool();
    }
    boardProbeServers();
    fetched = true;
  }
  return fetched;
}

uint32_t dataGeneration() { return generation; }
uint32_t weatherGeneration() { return weatherGen; }
const Status& dataStatus() { return st; }
const Weather& dataWeather() { return wx; }
