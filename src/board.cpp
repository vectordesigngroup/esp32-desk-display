#include "board.h"

#include <Preferences.h>
#include "config.h"
#include <WiFi.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct TzRow {
  const char* id;
  const char* label;
  const char* posix;
  const char* query;
};

static const TzRow kTz[] = {
    {"new_york", "Eastern", "EST5EDT,M3.2.0,M11.1.0", "America%2FNew_York"},
    {"chicago", "Central", "CST6CDT,M3.2.0,M11.1.0", "America%2FChicago"},
    {"denver", "Mountain", "MST7MDT,M3.2.0,M11.1.0", "America%2FDenver"},
    {"phoenix", "Arizona", "MST7", "America%2FPhoenix"},
    {"los_angeles", "Pacific", "PST8PDT,M3.2.0,M11.1.0", "America%2FLos_Angeles"},
    {"anchorage", "Alaska", "AKST9AKDT,M3.2.0,M11.1.0", "America%2FAnchorage"},
    {"honolulu", "Hawaii", "HST10", "Pacific%2FHonolulu"},
    {"utc", "UTC", "UTC0", "UTC"},
    {"london", "London", "GMT0BST,M3.5.0/1,M10.5.0", "Europe%2FLondon"},
    {"paris", "Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3", "Europe%2FParis"},
};

struct SpanRow {
  const char* id;
  const char* label;
};

static const SpanRow kSpan[] = {
    {"3d", "3 days"},
    {"1w", "7 days"},
    {"1m", "30 days"},
    {"3m", "3 months"},
    {"1y", "1 year"},
};

static const SpanRow kPriceSpan[] = {
    {"1d", "24 hours"},
    {"1w", "7 days"},
    {"1m", "30 days"},
    {"3m", "3 months"},
    {"1y", "1 year"},
};

static const SpanRow kCur[] = {
    {"USD", "US dollar"},
    {"EUR", "Euro"},
    {"GBP", "British pound"},
    {"CAD", "Canadian dollar"},
    {"CHF", "Swiss franc"},
    {"AUD", "Australian dollar"},
    {"JPY", "Yen"},
};

struct BoardServer {
  char name[13];
  char host[40];
  uint16_t port;
  bool up;
};

static const char* kIds[] = {
    "clock", "weather", "bitcoin", "network", "hashrate", "lightning", "servers", "posts", "note", "price"};
static const char* kLabels[] = {
    "Clock", "Weather", "Bitcoin", "Network", "Hashrate", "Lightning", "Servers", "Posts", "Note", "Price"};
static const uint8_t kKinds[] = {
    KIND_CLOCK, KIND_WEATHER, KIND_BITCOIN, KIND_NETWORK, KIND_HASH,
    KIND_LIGHTNING, KIND_SERVERS, KIND_POST, KIND_NOTE, KIND_PRICE};

static const char* kDefaultOrder = "clock,weather,bitcoin,network,hashrate,lightning,servers,posts,price";

static const int kMaxPages = 12;
static char gOrder[96] = "";
static char gTitle[24] = "Note";
static char gBody[161] = "";
static uint16_t gSeconds = 12;
static int gTz = 0;
static int gSpan = 2;
static int gPriceSpan = 0;
static int gCur = 0;
static char gZip[8] = DEFAULT_ZIP;
static char gLat[16] = DEFAULT_LAT;
static char gLon[16] = DEFAULT_LON;
static char gPlace[20] = DEFAULT_PLACE;
static bool gPlaceOk = true;
static char gTag[25] = "bitcoin";
static BoardServer gSrv[4];
static int gPosts = 0;
static int gLoggedSlots = -1;
static BoardSlot gSlots[12];
static int gSlotCount = 0;

static int pageIndex(const char* id) {
  for (int i = 0; i < (int)(sizeof(kIds) / sizeof(kIds[0])); i++) {
    if (strcmp(id, kIds[i]) == 0) return i;
  }
  return -1;
}

static void copyClean(char* dest, size_t n, const char* src, bool keepNewlines) {
  size_t j = 0;
  bool blank = false;
  if (!src) src = "";
  for (const char* p = src; *p && j + 1 < n; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '\r') continue;
    if (c == '\n') {
      if (!keepNewlines) continue;
      if (j == 0) continue;
      dest[j++] = '\n';
      blank = false;
      continue;
    }
    if (c < 32 || c > 126) c = ' ';
    if (c == ' ') {
      if (blank || j == 0) continue;
      blank = true;
    } else {
      blank = false;
    }
    dest[j++] = (char)c;
  }
  while (j > 0 && (dest[j - 1] == ' ' || dest[j - 1] == '\n')) j--;
  dest[j] = 0;
}

static void rebuild() {
  bool seen[kMaxPages];
  memset(seen, 0, sizeof(seen));
  gSlotCount = 0;
  char tmp[sizeof(gOrder)];
  memcpy(tmp, gOrder, sizeof(tmp));
  char* cursor = tmp;
  while (*cursor && gSlotCount < (int)(sizeof(gSlots) / sizeof(gSlots[0]))) {
    while (*cursor == ' ' || *cursor == ',') cursor++;
    if (!*cursor) break;
    char* end = cursor;
    while (*end && *end != ',') end++;
    char saved = *end;
    *end = 0;
    int id = pageIndex(cursor);
    if (id >= 0 && id < kMaxPages && !seen[id]) {
      seen[id] = true;
      if (kKinds[id] == KIND_POST) {
        int count = gPosts > 0 ? gPosts : 1;
        if (count > 3) count = 3;
        for (int p = 0; p < count && gSlotCount < (int)(sizeof(gSlots) / sizeof(gSlots[0])); p++) {
          gSlots[gSlotCount].kind = KIND_POST;
          gSlots[gSlotCount].index = gPosts > 0 ? (uint8_t)p : 255;
          gSlotCount++;
        }
      } else {
        gSlots[gSlotCount].kind = kKinds[id];
        gSlots[gSlotCount].index = 0;
        gSlotCount++;
      }
    }
    *end = saved;
    cursor = end;
  }
  if (gSlotCount == 0) {
    strncpy(gOrder, "clock", sizeof(gOrder) - 1);
    gOrder[sizeof(gOrder) - 1] = 0;
    gSlots[0].kind = KIND_CLOCK;
    gSlots[0].index = 0;
    gSlotCount = 1;
  }
  if (gSlotCount != gLoggedSlots) {
    gLoggedSlots = gSlotCount;
    Serial.printf("board slots %d\n", gSlotCount);
  }
}

static void applyOrder(const char* order) {
  bool seen[kMaxPages];
  memset(seen, 0, sizeof(seen));
  gOrder[0] = 0;
  char tmp[160];
  strncpy(tmp, order ? order : "", sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = 0;
  char* cursor = tmp;
  while (*cursor) {
    while (*cursor == ' ' || *cursor == ',') cursor++;
    if (!*cursor) break;
    char* end = cursor;
    while (*end && *end != ',' && *end != ' ') end++;
    char saved = *end;
    *end = 0;
    int id = pageIndex(cursor);
    if (id >= 0 && id < kMaxPages && !seen[id]) {
      seen[id] = true;
      if (gOrder[0]) strncat(gOrder, ",", sizeof(gOrder) - strlen(gOrder) - 1);
      strncat(gOrder, kIds[id], sizeof(gOrder) - strlen(gOrder) - 1);
    }
    *end = saved;
    cursor = end;
  }
  if (!gOrder[0]) {
    strncpy(gOrder, "clock", sizeof(gOrder) - 1);
    gOrder[sizeof(gOrder) - 1] = 0;
  }
}

static void applyText(const char* title, const char* body) {
  copyClean(gTitle, sizeof(gTitle), title, false);
  if (!gTitle[0]) strncpy(gTitle, "Note", sizeof(gTitle) - 1);
  copyClean(gBody, sizeof(gBody), body, true);
}

static void clampSeconds(int seconds) {
  if (seconds < 4) seconds = 4;
  if (seconds > 120) seconds = 120;
  gSeconds = (uint16_t)seconds;
}

static int tzIndex(const char* id) {
  for (int i = 0; i < (int)(sizeof(kTz) / sizeof(kTz[0])); i++) {
    if (id && strcmp(id, kTz[i].id) == 0) return i;
  }
  return 0;
}

static int spanIndex(const char* id) {
  for (int i = 0; i < (int)(sizeof(kSpan) / sizeof(kSpan[0])); i++) {
    if (id && strcmp(id, kSpan[i].id) == 0) return i;
  }
  return 2;
}

static int priceSpanIndex(const char* id) {
  for (int i = 0; i < (int)(sizeof(kPriceSpan) / sizeof(kPriceSpan[0])); i++) {
    if (id && strcmp(id, kPriceSpan[i].id) == 0) return i;
  }
  return 0;
}

static int curIndex(const char* id) {
  for (int i = 0; i < (int)(sizeof(kCur) / sizeof(kCur[0])); i++) {
    if (id && strcmp(id, kCur[i].id) == 0) return i;
  }
  return 0;
}

static bool digits5(const char* text) {
  if (!text) return false;
  for (int i = 0; i < 5; i++) {
    if (text[i] < '0' || text[i] > '9') return false;
  }
  return text[5] == 0;
}

static bool copyCoord(char* dest, size_t n, const char* src) {
  if (!src || !src[0] || n < 2) return false;
  size_t j = 0;
  int dots = 0;
  for (const char* p = src; *p; p++) {
    char c = *p;
    if (c == '-' && j == 0) {
      dest[j++] = c;
      continue;
    }
    if (c == '.' && dots == 0) {
      dots++;
      if (j + 1 >= n) return false;
      dest[j++] = c;
      continue;
    }
    if (c < '0' || c > '9') return false;
    if (j + 1 >= n) return false;
    dest[j++] = c;
  }
  if (j == 0 || dest[0] == '.') return false;
  dest[j] = 0;
  return true;
}

static void setDefaultPlace() {
  strncpy(gLat, DEFAULT_LAT, sizeof(gLat) - 1);
  strncpy(gLon, DEFAULT_LON, sizeof(gLon) - 1);
  strncpy(gPlace, DEFAULT_PLACE, sizeof(gPlace) - 1);
  gLat[sizeof(gLat) - 1] = 0;
  gLon[sizeof(gLon) - 1] = 0;
  gPlace[sizeof(gPlace) - 1] = 0;
  gPlaceOk = true;
}

static void applyZip(const char* zip) {
  char next[8];
  strncpy(next, zip ? zip : "", sizeof(next) - 1);
  next[sizeof(next) - 1] = 0;
  if (!digits5(next)) strncpy(next, DEFAULT_ZIP, sizeof(next) - 1);
  if (strcmp(next, gZip) != 0) {
    gLat[0] = 0;
    gLon[0] = 0;
    gPlace[0] = 0;
    gPlaceOk = false;
  }
  strncpy(gZip, next, sizeof(gZip) - 1);
  gZip[sizeof(gZip) - 1] = 0;
}

static void applyTag(const char* tag) {
  size_t j = 0;
  if (tag && tag[0] == '#') tag++;
  if (!tag) tag = "";
  for (const char* p = tag; *p && j + 1 < sizeof(gTag); p++) {
    char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
      gTag[j++] = c;
    }
  }
  gTag[j] = 0;
  if (!gTag[0]) strncpy(gTag, "bitcoin", sizeof(gTag) - 1);
}

static bool hostOk(const char* host) {
  int n = 0;
  if (!host || !host[0]) return false;
  for (const char* p = host; *p; p++) {
    char c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    if (!ok) return false;
    if (++n > 39) return false;
  }
  return true;
}

static void loadServers(const char* text) {
  memset(gSrv, 0, sizeof(gSrv));
  if (!text || !text[0]) return;
  int index = 0;
  const char* cursor = text;
  while (*cursor && index < 4) {
    char row[80];
    size_t n = 0;
    while (*cursor && *cursor != ';' && n + 1 < sizeof(row)) row[n++] = *cursor++;
    row[n] = 0;
    if (*cursor == ';') cursor++;
    char* bar1 = strchr(row, '|');
    char* bar2 = bar1 ? strchr(bar1 + 1, '|') : nullptr;
    if (!bar1 || !bar2) continue;
    *bar1 = 0;
    *bar2 = 0;
    int port = atoi(bar2 + 1);
    boardSetServer(index, row, bar1 + 1, port);
    if (gSrv[index].name[0]) index++;
  }
}

static void saveServers(Preferences& prefs) {
  char buf[280];
  buf[0] = 0;
  for (int i = 0; i < 4; i++) {
    if (!gSrv[i].name[0]) continue;
    char row[72];
    snprintf(row, sizeof(row), "%s|%s|%u", gSrv[i].name, gSrv[i].host, (unsigned)gSrv[i].port);
    if (buf[0] && strlen(buf) + 1 < sizeof(buf)) strncat(buf, ";", sizeof(buf) - strlen(buf) - 1);
    if (strlen(buf) + strlen(row) < sizeof(buf)) strncat(buf, row, sizeof(buf) - strlen(buf) - 1);
  }
  prefs.putString("srv", buf);
}

void boardBegin() {
  Preferences prefs;
  prefs.begin("desk", true);
  String order = prefs.getString("order", kDefaultOrder);
  gSeconds = prefs.getUShort("sec", 12);
  String title = prefs.getString("title", "Note");
  String body = prefs.getString("body", "");
  String tz = prefs.getString("tz", "new_york");
  String zip = prefs.getString("zip", DEFAULT_ZIP);
  String span = prefs.getString("span", "1m");
  String tag = prefs.getString("tag", "bitcoin");
  String pspan = prefs.getString("pspan", "1d");
  String cur = prefs.getString("cur", "USD");
  bool priceReady = prefs.getBool("pinit", false);
  String lat = prefs.getString("lat", "");
  String lon = prefs.getString("lon", "");
  String place = prefs.getString("place", "");
  String servers = prefs.getString("srv", "");
  prefs.end();
  clampSeconds(gSeconds);
  applyOrder(order.c_str());
  applyText(title.c_str(), body.c_str());
  gTz = tzIndex(tz.c_str());
  gSpan = spanIndex(span.c_str());
  gPriceSpan = priceSpanIndex(pspan.c_str());
  gCur = curIndex(cur.c_str());
  applyTag(tag.c_str());
  applyZip(zip.c_str());
  if (copyCoord(gLat, sizeof(gLat), lat.c_str()) && copyCoord(gLon, sizeof(gLon), lon.c_str())) {
    copyClean(gPlace, sizeof(gPlace), place.c_str(), false);
    gPlaceOk = true;
  } else if (strcmp(gZip, DEFAULT_ZIP) == 0) {
    setDefaultPlace();
  } else {
    gPlaceOk = false;
  }
  loadServers(servers.c_str());
  if (!priceReady && !strstr(gOrder, "price")) {
    if (gOrder[0] && strlen(gOrder) + 1 < sizeof(gOrder)) strncat(gOrder, ",", sizeof(gOrder) - strlen(gOrder) - 1);
    strncat(gOrder, "price", sizeof(gOrder) - strlen(gOrder) - 1);
  }
  if (!priceReady) {
    Preferences writePrefs;
    writePrefs.begin("desk", false);
    writePrefs.putString("order", gOrder);
    writePrefs.putBool("pinit", true);
    writePrefs.end();
  }
  rebuild();
  Serial.printf("board %us %s %s %s #%s %s %s\n", (unsigned)gSeconds, gOrder, kTz[gTz].id, gZip, gTag,
                kPriceSpan[gPriceSpan].id, kCur[gCur].id);
}

bool boardSyncPosts(int count) {
  if (count < 0) count = 0;
  if (count > 3) count = 3;
  if (count == gPosts) return false;
  gPosts = count;
  rebuild();
  return true;
}

int boardSlotCount() { return gSlotCount < 1 ? 1 : gSlotCount; }

BoardSlot boardSlot(int index) {
  if (gSlotCount < 1 || index < 0 || index >= gSlotCount) {
    BoardSlot slot;
    slot.kind = KIND_CLOCK;
    slot.index = 0;
    return slot;
  }
  return gSlots[index];
}

uint32_t boardRotateMs() { return (uint32_t)gSeconds * 1000UL; }
int boardSeconds() { return gSeconds; }
const char* boardOrder() { return gOrder; }
const char* boardNoteTitle() { return gTitle; }
const char* boardNoteBody() { return gBody; }
int boardCatalogCount() { return (int)(sizeof(kIds) / sizeof(kIds[0])); }
const char* boardPageId(int index) { return kIds[index]; }
const char* boardPageLabel(int index) { return kLabels[index]; }

bool boardSave(int seconds, const String& order, const String& noteTitle, const String& noteBody,
               const String& tz, const String& zip, const String& span, const String& tag,
               const String& priceSpan, const String& currency) {
  clampSeconds(seconds);
  applyOrder(order.c_str());
  String title = noteTitle;
  String body = noteBody;
  if (title.length() > 40) title.remove(40);
  if (body.length() > 400) body.remove(400);
  applyText(title.c_str(), body.c_str());
  gTz = tzIndex(tz.c_str());
  gSpan = spanIndex(span.c_str());
  gPriceSpan = priceSpanIndex(priceSpan.c_str());
  gCur = curIndex(currency.c_str());
  applyZip(zip.c_str());
  applyTag(tag.c_str());
  if (!gPlaceOk && strcmp(gZip, DEFAULT_ZIP) == 0) setDefaultPlace();
  rebuild();
  Preferences prefs;
  prefs.begin("desk", false);
  prefs.putString("order", gOrder);
  prefs.putUShort("sec", gSeconds);
  prefs.putString("title", gTitle);
  prefs.putString("body", gBody);
  prefs.putString("tz", kTz[gTz].id);
  prefs.putString("zip", gZip);
  prefs.putString("span", kSpan[gSpan].id);
  prefs.putString("tag", gTag);
  prefs.putString("pspan", kPriceSpan[gPriceSpan].id);
  prefs.putString("cur", kCur[gCur].id);
  prefs.putBool("pinit", true);
  if (!gPlaceOk) {
    prefs.putString("lat", "");
    prefs.putString("lon", "");
    prefs.putString("place", "");
  }
  saveServers(prefs);
  prefs.end();
  configTzTime(kTz[gTz].posix, "pool.ntp.org", "time.nist.gov");
  Serial.printf("board saved %us %s %s %s #%s srv %d %s %s\n", (unsigned)gSeconds, kTz[gTz].id, gZip,
                kSpan[gSpan].id, gTag, boardServerCount(), kPriceSpan[gPriceSpan].id, kCur[gCur].id);
  return true;
}

void boardSetServer(int index, const char* name, const char* host, int port) {
  if (index < 0 || index >= 4) return;
  BoardServer& slot = gSrv[index];
  bool was = slot.up;
  memset(&slot, 0, sizeof(slot));
  if (!name || !hostOk(host) || port < 1 || port > 65535) return;
  copyClean(slot.name, sizeof(slot.name), name, false);
  for (char* p = slot.name; *p; p++) {
    if (*p == '|' || *p == ';') *p = ' ';
  }
  if (!slot.name[0]) return;
  strncpy(slot.host, host, sizeof(slot.host) - 1);
  slot.host[sizeof(slot.host) - 1] = 0;
  slot.port = (uint16_t)port;
  slot.up = was;
}

bool boardServerAt(int index, const char** name, const char** host, int* port, bool* up) {
  if (index < 0 || index >= 4 || !gSrv[index].name[0]) return false;
  if (name) *name = gSrv[index].name;
  if (host) *host = gSrv[index].host;
  if (port) *port = gSrv[index].port;
  if (up) *up = gSrv[index].up;
  return true;
}

int boardServerCount() {
  int count = 0;
  for (int i = 0; i < 4; i++) {
    if (gSrv[i].name[0]) count++;
  }
  return count;
}

void boardProbeServers() {
  int up = 0;
  int count = 0;
  for (int i = 0; i < 4; i++) {
    if (!gSrv[i].name[0]) {
      gSrv[i].up = false;
      continue;
    }
    count++;
    WiFiClient client;
    gSrv[i].up = client.connect(gSrv[i].host, gSrv[i].port, 500);
    client.stop();
    if (gSrv[i].up) up++;
  }
  if (count > 0) Serial.printf("servers %d/%d\n", up, count);
}

const char* boardTz() { return kTz[gTz].id; }
const char* boardTzLabel() { return kTz[gTz].label; }
const char* boardTzPosix() { return kTz[gTz].posix; }
const char* boardTzQuery() { return kTz[gTz].query; }
int boardTzCount() { return (int)(sizeof(kTz) / sizeof(kTz[0])); }
const char* boardTzId(int index) { return kTz[index].id; }
const char* boardTzName(int index) { return kTz[index].label; }

const char* boardZip() { return gZip; }
const char* boardLat() { return gLat; }
const char* boardLon() { return gLon; }
const char* boardPlace() { return gPlace; }
bool boardPlaceOk() { return gPlaceOk && gLat[0] && gLon[0]; }

void boardSetPlace(const char* lat, const char* lon, const char* place) {
  if (!copyCoord(gLat, sizeof(gLat), lat) || !copyCoord(gLon, sizeof(gLon), lon)) return;
  copyClean(gPlace, sizeof(gPlace), place, false);
  gPlaceOk = true;
  Preferences prefs;
  prefs.begin("desk", false);
  prefs.putString("lat", gLat);
  prefs.putString("lon", gLon);
  prefs.putString("place", gPlace);
  prefs.end();
  Serial.printf("place %s\n", gPlace);
}

const char* boardPriceSpan() { return kPriceSpan[gPriceSpan].id; }
const char* boardPriceSpanLabel() { return kPriceSpan[gPriceSpan].label; }
int boardPriceSpanCount() { return (int)(sizeof(kPriceSpan) / sizeof(kPriceSpan[0])); }
const char* boardPriceSpanId(int index) { return kPriceSpan[index].id; }
const char* boardPriceSpanName(int index) { return kPriceSpan[index].label; }
const char* boardCur() { return kCur[gCur].id; }
const char* boardCurLabel() { return kCur[gCur].label; }
int boardCurCount() { return (int)(sizeof(kCur) / sizeof(kCur[0])); }
const char* boardCurId(int index) { return kCur[index].id; }
const char* boardCurName(int index) { return kCur[index].label; }

const char* boardSpan() { return kSpan[gSpan].id; }
const char* boardSpanLabel() { return kSpan[gSpan].label; }
int boardSpanCount() { return (int)(sizeof(kSpan) / sizeof(kSpan[0])); }
const char* boardSpanId(int index) { return kSpan[index].id; }
const char* boardSpanName(int index) { return kSpan[index].label; }
const char* boardTag() { return gTag; }
