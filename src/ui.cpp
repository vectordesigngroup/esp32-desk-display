#include "ui.h"

#include <TFT_eSPI.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "config.h"

static TFT_eSPI tft;
static TFT_eSprite pageSprite(&tft);
static TFT_eSPI* gfx = &tft;
static int page = 0;
static int shown = -1;
static bool dirty = true;
static uint32_t holdUntil = 0;
static uint32_t nextRotate = 0;
static bool rotateArmed = false;
static bool uiPause = false;
static int lastSec = -1;
static uint32_t seenGen = 0;
static uint32_t seenWeather = 0;

static uint16_t BG, PANEL, ORANGE, DIM, GREEN, RED, AMBER, WHITE, BLUE, CLOUD;

static void fmtComma(char* out, size_t n, long value) {
  bool neg = value < 0;
  unsigned long x = neg ? (unsigned long)(-value) : (unsigned long)value;
  char rev[24];
  int i = 0;
  int digits = 0;
  if (x == 0) rev[i++] = '0';
  while (x > 0 && i < (int)sizeof(rev)) {
    if (digits > 0 && digits % 3 == 0) rev[i++] = ',';
    rev[i++] = (char)('0' + (x % 10));
    x /= 10;
    digits++;
  }
  if (neg && i < (int)sizeof(rev)) rev[i++] = '-';
  int p = 0;
  while (i > 0 && p + 1 < (int)n) out[p++] = rev[--i];
  out[p] = 0;
}

static void fmtUp(char* out, size_t n, uint32_t seconds) {
  uint32_t days = seconds / 86400UL;
  uint32_t hours = (seconds % 86400UL) / 3600UL;
  uint32_t mins = (seconds % 3600UL) / 60UL;
  if (days > 0) snprintf(out, n, "%lu d %lu h", (unsigned long)days, (unsigned long)hours);
  else if (hours > 0) snprintf(out, n, "%lu h %lu m", (unsigned long)hours, (unsigned long)mins);
  else snprintf(out, n, "%lu m", (unsigned long)mins);
}

static void fmtTenths(char* out, size_t n, int tenths) {
  int sign = tenths < 0 ? -1 : 1;
  int v = tenths < 0 ? -tenths : tenths;
  snprintf(out, n, "%s%d.%d%%", sign < 0 ? "-" : "+", v / 10, v % 10);
}

static void fmtMinutes(char* out, size_t n, int minutes) {
  if (minutes <= 0) {
    snprintf(out, n, "--");
    return;
  }
  int days = minutes / 1440;
  int hours = (minutes % 1440) / 60;
  int mins = minutes % 60;
  if (days > 0) snprintf(out, n, "%d d %d h", days, hours);
  else if (hours > 0) snprintf(out, n, "%d h %d m", hours, mins);
  else snprintf(out, n, "%d m", mins);
}

static void fmtAgeShort(char* out, size_t n, int seconds) {
  if (seconds < 0) seconds = 0;
  if (seconds < 90) snprintf(out, n, "now");
  else if (seconds < 3600) snprintf(out, n, "%dm", seconds / 60);
  else if (seconds < 86400) snprintf(out, n, "%dh", seconds / 3600);
  else snprintf(out, n, "%dd", seconds / 86400);
}

static void clipText(char* out, size_t n, const char* src, int maxPx) {
  if (!src) src = "";
  if (n == 0) return;
  gfx->setTextFont(2);
  char buf[120];
  size_t len = strlen(src);
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, src, len);
  buf[len] = 0;
  if (gfx->textWidth(buf) <= maxPx) {
    if (len >= n) len = n - 1;
    memcpy(out, buf, len);
    out[len] = 0;
    return;
  }
  while (len > 0) {
    len--;
    buf[len] = 0;
    char trial[124];
    snprintf(trial, sizeof(trial), "%s...", buf);
    if (gfx->textWidth(trial) <= maxPx) {
      strncpy(out, trial, n - 1);
      out[n - 1] = 0;
      return;
    }
  }
  out[0] = 0;
}

static void row(int y, const char* key, const char* value, uint16_t color) {
  gfx->fillRect(0, y, 320, 18, BG);
  gfx->setTextFont(2);
  gfx->setTextColor(DIM, BG);
  gfx->setCursor(8, y);
  gfx->print(key);
  char shown[64];
  clipText(shown, sizeof(shown), value, 210);
  gfx->setTextColor(color, BG);
  int w = gfx->textWidth(shown);
  gfx->setCursor(312 - w, y);
  gfx->print(shown);
}

static void meter(int y, const char* key, int pct, uint16_t color) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  gfx->fillRect(0, y, 320, 18, BG);
  gfx->setTextFont(2);
  gfx->setTextColor(DIM, BG);
  gfx->setCursor(8, y);
  gfx->print(key);
  gfx->drawRect(70, y + 4, 160, 10, DIM);
  int fill = 158 * pct / 100;
  gfx->fillRect(71, y + 5, fill, 8, color);
  if (fill < 158) gfx->fillRect(71 + fill, y + 5, 158 - fill, 8, BG);
  char value[8];
  snprintf(value, sizeof(value), "%d%%", pct);
  gfx->setTextColor(color, BG);
  int w = gfx->textWidth(value);
  gfx->setCursor(312 - w, y);
  gfx->print(value);
}

static void service(int x, int y, const char* name, bool up) {
  gfx->fillCircle(x + 4, y + 7, 4, up ? GREEN : RED);
  gfx->setTextFont(2);
  gfx->setTextColor(WHITE, BG);
  gfx->setCursor(x + 14, y);
  gfx->print(name);
}

static void drawDots() {
  int count = boardSlotCount();
  if (count < 1) count = 1;
  int step = count > 18 ? 10 : 14;
  gfx->fillRect(0, 154, 320, 16, BG);
  int x0 = (320 - count * step) / 2;
  for (int i = 0; i < count; i++) {
    gfx->fillCircle(x0 + i * step + step / 2, 162, 3, i == page ? ORANGE : DIM);
  }
}

static void drawHeader(const char* title, const char* side) {
  gfx->fillRect(0, 0, 320, 24, PANEL);
  gfx->setTextFont(2);
  int sideW = (side && side[0]) ? gfx->textWidth(side) : 0;
  int titleMax = 270 - (sideW > 0 ? sideW + 12 : 0);
  if (titleMax < 48) titleMax = 48;
  char shown[40];
  clipText(shown, sizeof(shown), title, titleMax);
  gfx->setTextColor(ORANGE, PANEL);
  gfx->setCursor(8, 4);
  gfx->print(shown);
  if (sideW > 0) {
    gfx->setTextColor(DIM, PANEL);
    gfx->setCursor(292 - sideW, 4);
    gfx->print(side);
  }
  gfx->fillCircle(308, 12, 4, WiFi.status() == WL_CONNECTED ? GREEN : RED);
}

static void drawClock(bool timeOnly) {
  struct tm now;
  bool hasTime = getLocalTime(&now, 0);
  char hm[12] = "--:--:--";
  char date[28] = "Waiting for time";
  if (hasTime) {
    strftime(hm, sizeof(hm), "%H:%M:%S", &now);
    strftime(date, sizeof(date), "%a %b %d", &now);
  }

  gfx->fillRect(0, 36, 320, 64, BG);
  gfx->setTextFont(7);
  gfx->setTextColor(WHITE, BG);
  int tw = gfx->textWidth(hm);
  gfx->setCursor(160 - tw / 2, 44);
  gfx->print(hm);
  if (timeOnly) return;

  gfx->fillRect(0, 108, 320, 40, BG);
  gfx->setTextFont(4);
  gfx->setTextColor(WHITE, BG);
  int dw = gfx->textWidth(date);
  gfx->setCursor(160 - dw / 2, 112);
  gfx->print(date);
}

static void fmtMvB(char* out, size_t n, int vsize) {
  if (vsize <= 0) {
    snprintf(out, n, "--");
    return;
  }
  int tenths = (int)(((long long)vsize + 50000LL) / 100000LL);
  snprintf(out, n, "%d.%d MvB", tenths / 10, tenths % 10);
}

static void fmtMed(char* out, size_t n, int med100) {
  if (med100 <= 0) {
    snprintf(out, n, "--");
    return;
  }
  int whole = med100 / 100;
  int frac = med100 % 100;
  if (frac == 0) snprintf(out, n, "%d sat", whole);
  else if (frac % 10 == 0) snprintf(out, n, "%d.%d sat", whole, frac / 10);
  else snprintf(out, n, "%d.%02d sat", whole, frac);
}

static void drawBitcoin() {
  const Status& s = dataStatus();
  char value[48];
  char num[24];

  if (s.priceOk && s.priceUsd > 0) {
    fmtComma(num, sizeof(num), s.priceUsd);
    if (s.priceChgOk) {
      char pct[16];
      fmtTenths(pct, sizeof(pct), s.priceChg10);
      snprintf(value, sizeof(value), "$%s  %s", num, pct);
    } else {
      snprintf(value, sizeof(value), "$%s", num);
    }
    uint16_t color = WHITE;
    if (s.priceChgOk && s.priceChg10 > 0) color = GREEN;
    else if (s.priceChgOk && s.priceChg10 < 0) color = RED;
    row(28, "Price", value, color);
  } else {
    row(28, "Price", "--", DIM);
  }

  if (s.btcOk && s.blocks > 0) {
    fmtComma(value, sizeof(value), s.blocks);
    if (s.pruned && strlen(value) < 28) strcat(value, " pruned");
    row(46, "Block", value, WHITE);
    int pct = (int)(s.progress * 100.0f + 0.5f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (s.ibd) snprintf(value, sizeof(value), "syncing %d%%", pct);
    else if (s.tip > s.blocks) snprintf(value, sizeof(value), "%d behind", s.tip - s.blocks);
    else snprintf(value, sizeof(value), "synced");
    row(64, "Chain", value, (s.ibd || (s.tip > s.blocks)) ? AMBER : GREEN);
  } else {
    row(46, "Block", "offline", RED);
    row(64, "Chain", "--", DIM);
  }

  if (s.feeFast > 0 || s.feeHalf > 0 || s.feeHour > 0) {
    if (s.feeEcon > 0 && s.feeEcon != s.feeHour) {
      snprintf(value, sizeof(value), "%d / %d / %d  e%d", s.feeFast, s.feeHalf, s.feeHour, s.feeEcon);
    } else {
      snprintf(value, sizeof(value), "%d / %d / %d", s.feeFast, s.feeHalf, s.feeHour);
    }
    row(82, "Fees", value, ORANGE);
  } else {
    row(82, "Fees", "--", DIM);
  }

  int tx = s.memCount > 0 ? s.memCount : s.mempoolTx;
  if (tx > 0 || s.memVsize > 0) {
    char mvb[16];
    fmtComma(num, sizeof(num), tx);
    fmtMvB(mvb, sizeof(mvb), s.memVsize);
    if (s.memVsize > 0) snprintf(value, sizeof(value), "%s tx  %s", num, mvb);
    else snprintf(value, sizeof(value), "%s tx", num);
    row(100, "Mempool", value, WHITE);
  } else {
    row(100, "Mempool", "--", DIM);
  }

  if (s.nextTx > 0 || s.nextMed100 > 0) {
    char med[16];
    fmtComma(num, sizeof(num), s.nextTx);
    fmtMed(med, sizeof(med), s.nextMed100);
    if (s.nextTx > 0 && s.nextMed100 > 0) snprintf(value, sizeof(value), "%s tx  %s", num, med);
    else if (s.nextTx > 0) snprintf(value, sizeof(value), "%s tx", num);
    else snprintf(value, sizeof(value), "%s", med);
    row(118, "Next block", value, WHITE);
  } else {
    row(118, "Next block", "--", DIM);
  }

  if (s.btcLocal) {
    snprintf(value, sizeof(value), "%d", s.peers);
    row(136, "Peers", value, WHITE);
  } else if (s.feeEcon > 0) {
    snprintf(value, sizeof(value), "%d sat/vB", s.feeEcon);
    row(136, "Economy", value, DIM);
  } else {
    row(136, "Peers", "--", DIM);
  }
}

static void drawNetwork() {
  const Status& s = dataStatus();
  char value[48];
  char num[24];

  if (s.hashEh > 0) {
    fmtComma(num, sizeof(num), s.hashEh);
    snprintf(value, sizeof(value), "%s EH/s", num);
    row(28, "Hashrate", value, WHITE);
  } else {
    row(28, "Hashrate", "--", DIM);
  }

  if (s.diffT10 > 0) {
    snprintf(value, sizeof(value), "%d.%d T", s.diffT10 / 10, s.diffT10 % 10);
    row(46, "Difficulty", value, WHITE);
  } else {
    row(46, "Difficulty", "--", DIM);
  }

  if (s.epochOk) {
    char pct[16];
    fmtTenths(pct, sizeof(pct), s.adj10);
    if (s.diffLeft > 0) snprintf(value, sizeof(value), "%s  %d blk", pct, s.diffLeft);
    else snprintf(value, sizeof(value), "%s", pct);
    row(64, "Retarget", value, s.adj10 >= 0 ? GREEN : RED);
    fmtMinutes(value, sizeof(value), s.etaMin);
    row(82, "ETA", value, WHITE);
    fmtTenths(value, sizeof(value), s.prev10);
    row(100, "Previous", value, WHITE);
  } else {
    row(64, "Retarget", "--", DIM);
    row(82, "ETA", "--", DIM);
    row(100, "Previous", "--", DIM);
  }

  if (s.halvingLeft > 0) {
    fmtComma(num, sizeof(num), s.halvingLeft);
    int days = (int)((long long)s.halvingLeft * 10LL / 1440LL);
    int tenthsYear = days > 0 ? days * 10 / 365 : 0;
    if (tenthsYear > 0) snprintf(value, sizeof(value), "%s  ~%d.%d y", num, tenthsYear / 10, tenthsYear % 10);
    else snprintf(value, sizeof(value), "%s blk", num);
    row(118, "Halving", value, ORANGE);
  } else {
    row(118, "Halving", "--", DIM);
  }

  if (s.lastPool[0] || s.lastAge > 0 || s.lastReward > 0) {
    char age[12];
    char reward[16];
    fmtAgeShort(age, sizeof(age), s.lastAge);
    reward[0] = 0;
    if (s.lastReward > 0) {
      int cents = (int)(((long long)s.lastReward + 500000LL) / 1000000LL);
      snprintf(reward, sizeof(reward), "%d.%02d", cents / 100, cents % 100);
    }
    if (s.lastPool[0] && reward[0]) snprintf(value, sizeof(value), "%s  %s BTC  %s", s.lastPool, reward, age);
    else if (s.lastPool[0]) snprintf(value, sizeof(value), "%s  %s", s.lastPool, age);
    else if (reward[0]) snprintf(value, sizeof(value), "%s BTC  %s", reward, age);
    else snprintf(value, sizeof(value), "%s", age);
    row(136, "Last block", value, WHITE);
  } else {
    row(136, "Last block", "--", DIM);
  }
}

static void drawLightning() {
  const Status& s = dataStatus();
  char value[40];
  char num[24];
  row(28, "StartOS", s.startos ? "online" : "offline", s.startos ? GREEN : RED);

  if (s.clnOk) {
    char sats[24];
    fmtComma(sats, sizeof(sats), s.clnSat);
    snprintf(value, sizeof(value), "%d ch  %s", s.clnChannels, sats);
    row(46, "CLN", value, WHITE);
  } else {
    row(46, "CLN", s.startos ? "no data" : "offline", s.startos ? AMBER : DIM);
  }

  if (s.lndOk) {
    char sats[24];
    fmtComma(sats, sizeof(sats), s.lndSat);
    snprintf(value, sizeof(value), "%d ch  %s", s.lndChannels, sats);
    row(64, "LND", value, WHITE);
  } else {
    row(64, "LND", s.startos ? "no data" : "offline", s.startos ? AMBER : DIM);
  }

  if (!s.lnOk) {
    row(82, "Nodes", "--", DIM);
    row(100, "Channels", "--", DIM);
    row(118, "Capacity", "--", DIM);
    row(136, "Avg fee", "--", DIM);
    return;
  }

  fmtComma(num, sizeof(num), s.lnNodes);
  row(82, "Nodes", num, WHITE);
  fmtComma(num, sizeof(num), s.lnChannelsPub);
  row(100, "Channels", num, WHITE);
  fmtComma(num, sizeof(num), s.lnBtc);
  snprintf(value, sizeof(value), "%s BTC", num);
  row(118, "Capacity", value, ORANGE);
  snprintf(value, sizeof(value), "%d ppm", s.lnPpm);
  row(136, "Avg fee", value, WHITE);
}

static uint16_t diskColor(int pct) {
  if (pct >= 95) return RED;
  if (pct >= 85) return AMBER;
  return GREEN;
}

static void drawServiceGrid(int y0) {
  const Status& s = dataStatus();
  const char* names[8];
  bool ups[8];
  int count = 0;
  names[count] = "Jellyfin";
  ups[count++] = s.jellyfin;
  names[count] = "Vault";
  ups[count++] = s.vault;
  names[count] = "Frigate";
  ups[count++] = s.frigateUp;
  names[count] = "StartOS";
  ups[count++] = s.startos;
  for (int i = 0; i < 4 && count < 8; i++) {
    const char* name = nullptr;
    bool up = false;
    if (!boardServerAt(i, &name, nullptr, nullptr, &up) || !name || !name[0]) continue;
    names[count] = name;
    ups[count++] = up;
  }
  for (int i = 0; i < count; i++) {
    service(8 + (i % 2) * 156, y0 + (i / 2) * 18, names[i], ups[i]);
  }
}

static void drawServers() {
  const Status& s = dataStatus();
  char value[40];
  if (s.piUp && s.piStats) fmtUp(value, sizeof(value), s.piUptime);
  else snprintf(value, sizeof(value), s.piUp ? "up" : "down");
  row(28, "Pi", value, s.piUp ? GREEN : RED);

  if (boardServerCount() > 0) {
    if (s.piStats) {
      int tenths = (int)(s.piLoad * 10.0f + 0.5f);
      snprintf(value, sizeof(value), "%d.%d   RAM %d%%   Disk %d%%", tenths / 10, tenths % 10, s.piMem, s.piDisk);
      row(46, "Load", value, diskColor(s.piDisk));
    } else if (!s.bridge && !s.piUp) {
      row(46, "Stats", "bridge offline", AMBER);
    } else {
      row(46, "Stats", s.piUp ? "checking" : "down", DIM);
    }
    gfx->fillRect(0, 64, 320, 88, BG);
    drawServiceGrid(66);
    return;
  }

  if (s.piStats) {
    int tenths = (int)(s.piLoad * 10.0f + 0.5f);
    snprintf(value, sizeof(value), "%d.%d", tenths / 10, tenths % 10);
    row(46, "Load", value, s.piLoad >= 4.0f ? AMBER : WHITE);
    meter(66, "RAM", s.piMem, s.piMem >= 90 ? RED : GREEN);
    meter(86, "Disk", s.piDisk, diskColor(s.piDisk));
  } else if (!s.bridge && !s.piUp) {
    row(46, "Stats", "bridge offline", AMBER);
  } else {
    row(46, "Stats", s.piUp ? "checking" : "down", DIM);
  }

  gfx->fillRect(0, 112, 320, 40, BG);
  drawServiceGrid(114);
}

static void drawSun(int cx, int cy, int r) {
  gfx->fillCircle(cx, cy, r, ORANGE);
  const int dir[8][2] = {{2, 0}, {2, 2}, {0, 2}, {-2, 2}, {-2, 0}, {-2, -2}, {0, -2}, {2, -2}};
  for (int i = 0; i < 8; i++) {
    int dx = dir[i][0];
    int dy = dir[i][1];
    gfx->drawLine(cx + dx * (r + 2) / 2, cy + dy * (r + 2) / 2, cx + dx * (r + 7) / 2, cy + dy * (r + 7) / 2, ORANGE);
  }
}

static void drawCloud(int cx, int cy, int r) {
  gfx->fillCircle(cx - r / 2, cy - r / 4, (r * 2) / 3, CLOUD);
  gfx->fillCircle(cx + r / 3, cy - r / 5, r / 2, CLOUD);
  gfx->fillRoundRect(cx - r, cy, r * 2, (r * 2) / 3, r / 3, CLOUD);
}

static void drawDrops(int cx, int cy, int r, int count) {
  if (count < 1) count = 1;
  if (count > 3) count = 3;
  for (int i = 0; i < count; i++) {
    int x = cx + (i - count / 2) * (r / 2 + 2);
    int y = cy + r / 2 + 2;
    gfx->drawLine(x, y, x - 2, y + r / 2, BLUE);
  }
}

static void drawSky(int code, int cx, int cy, int r) {
  if (code < 0) code = 1;
  if (code == 0) {
    drawSun(cx, cy, r);
  } else if (code == 1) {
    drawSun(cx - r / 3, cy - r / 3, (r * 2) / 3);
    drawCloud(cx + 2, cy + 3, (r * 3) / 4);
  } else if (code <= 3) {
    drawCloud(cx, cy, r);
  } else if (code <= 48) {
    gfx->drawLine(cx - r, cy - 4, cx + r, cy - 4, CLOUD);
    gfx->drawLine(cx - r + 4, cy + 2, cx + r - 2, cy + 2, CLOUD);
    gfx->drawLine(cx - r, cy + 8, cx + r, cy + 8, CLOUD);
  } else if (code <= 67 || (code >= 80 && code <= 82)) {
    drawCloud(cx, cy - 3, r);
    drawDrops(cx, cy, r, 3);
  } else if (code <= 77 || (code >= 85 && code <= 86)) {
    drawCloud(cx, cy - 4, r);
    gfx->fillCircle(cx - r / 2, cy + r / 2, 2, WHITE);
    gfx->fillCircle(cx, cy + r / 2 + 3, 2, WHITE);
    gfx->fillCircle(cx + r / 2, cy + r / 2, 2, WHITE);
  } else {
    drawCloud(cx, cy - 4, r);
    gfx->drawLine(cx + 2, cy + 2, cx - 4, cy + r / 2, AMBER);
    gfx->drawLine(cx - 4, cy + r / 2, cx + 3, cy + r / 2, AMBER);
    gfx->drawLine(cx + 3, cy + r / 2, cx - 3, cy + r, AMBER);
  }
}

static int dropCount(int pct) {
  if (pct >= 50) return 3;
  if (pct >= 20) return 2;
  if (pct > 0) return 1;
  return 0;
}

static void drawWeather() {
  const Weather& weather = dataWeather();
  if (!weather.ok && !weather.forecast) {
    gfx->setTextFont(4);
    gfx->setTextColor(WHITE, BG);
    gfx->setCursor(16, 58);
    gfx->print(WiFi.status() == WL_CONNECTED ? "Loading" : "Offline");
    return;
  }

  int code = weather.ok ? weather.code : weather.days[0].code;
  drawSky(code, 40, 58, 16);

  if (weather.ok) {
    int whole = (int)(weather.tempF + (weather.tempF >= 0 ? 0.5f : -0.5f));
    char temp[8];
    snprintf(temp, sizeof(temp), "%d", whole);
    gfx->setTextFont(6);
    gfx->setTextColor(WHITE, BG);
    gfx->setCursor(78, 28);
    gfx->print(temp);
    int gap = gfx->textWidth(temp);
    gfx->setTextFont(4);
    gfx->setCursor(82 + gap, 32);
    gfx->print("F");
  }

  if (weather.forecast && weather.days[0].label[0]) {
    const ForecastDay& today = weather.days[0];
    char hi[8];
    char lo[8];
    snprintf(hi, sizeof(hi), "%d", today.hi);
    snprintf(lo, sizeof(lo), "%d", today.lo);
    gfx->setTextFont(4);
    gfx->setTextColor(ORANGE, BG);
    gfx->setCursor(78, 80);
    gfx->print(hi);
    int hiWidth = gfx->textWidth(hi);
    gfx->setTextColor(DIM, BG);
    gfx->setCursor(78 + hiWidth + 8, 80);
    gfx->print(lo);

    int drops = dropCount(today.rainPct);
    if (drops > 0) drawDrops(236, 40, 14, drops);
    if (today.rainPct >= 0) {
      char pct[8];
      snprintf(pct, sizeof(pct), "%d%%", today.rainPct);
      gfx->setTextFont(4);
      gfx->setTextColor(today.rainPct >= 40 ? BLUE : DIM, BG);
      gfx->setCursor(258, 36);
      gfx->print(pct);
    }

    int shown = 0;
    for (int i = 1; i < 4; i++) {
      const ForecastDay& day = weather.days[i];
      if (!day.label[0]) continue;
      int x = 8 + shown * 104;
      gfx->setTextFont(2);
      gfx->setTextColor(DIM, BG);
      gfx->setCursor(x, 116);
      gfx->print(day.label);
      drawSky(day.code, x + 46, 124, 8);
      char high[8];
      snprintf(high, sizeof(high), "%d", day.hi);
      gfx->setTextFont(4);
      gfx->setTextColor(WHITE, BG);
      gfx->setCursor(x + 60, 112);
      gfx->print(high);
      shown++;
    }
  }
}

static void fmtAxis(char* out, size_t n, int value) {
  if (value >= 1000000) {
    int whole = value / 1000000;
    int tenth = (value % 1000000) / 100000;
    if (tenth == 0) snprintf(out, n, "%dM", whole);
    else snprintf(out, n, "%d.%dM", whole, tenth);
  } else if (value >= 10000) {
    snprintf(out, n, "%dk", value / 1000);
  } else {
    snprintf(out, n, "%d", value);
  }
}

static void drawChart(const int* values, int count, uint16_t color, int top, int bottom) {
  int mn = values[0];
  int mx = mn;
  for (int i = 1; i < count; i++) {
    if (values[i] < mn) mn = values[i];
    if (values[i] > mx) mx = values[i];
  }
  if (mx - mn < 4) {
    int mid = (mn + mx) / 2;
    mn = mid - 2;
    mx = mid + 2;
  }
  int span = mx - mn;
  if (span < 1) span = 1;

  const int left = 48;
  const int right = 308;
  char label[8];
  gfx->setTextFont(2);
  gfx->setTextColor(DIM, BG);
  fmtAxis(label, sizeof(label), mx);
  gfx->setCursor(4, top);
  gfx->print(label);
  fmtAxis(label, sizeof(label), mn);
  gfx->setCursor(4, bottom - 14);
  gfx->print(label);
  gfx->drawLine(left, top, left, bottom, DIM);
  gfx->drawLine(left, bottom, right, bottom, DIM);
  gfx->drawLine(left, (top + bottom) / 2, right, (top + bottom) / 2, PANEL);

  int prevX = 0;
  int prevY = 0;
  for (int i = 0; i < count; i++) {
    int x = left + (right - left) * i / (count - 1);
    int y = bottom - (int)(((long long)(bottom - top) * (values[i] - mn)) / span);
    if (y < top) y = top;
    if (y > bottom) y = bottom;
    if (i > 0) {
      gfx->drawLine(prevX, prevY, x, y, color);
      if (prevY + 1 <= bottom && y + 1 <= bottom) gfx->drawLine(prevX, prevY + 1, x, y + 1, color);
    }
    prevX = x;
    prevY = y;
  }
}

static void drawHashrate() {
  const Status& s = dataStatus();
  if (s.hashPoints < 2) {
    gfx->setTextFont(4);
    gfx->setTextColor(WHITE, BG);
    gfx->setCursor(16, 58);
    gfx->print(s.bridge ? "No history" : "Needs bridge");
    return;
  }
  drawChart(s.hashSeries, s.hashPoints, ORANGE, 36, 132);
}

static void drawPrice() {
  const Status& s = dataStatus();
  bool have = s.quoteOk && s.quote > 0;
  if (have) {
    char num[24];
    char line[40];
    fmtComma(num, sizeof(num), s.quote);
    snprintf(line, sizeof(line), "%s %s", boardCur(), num);
    uint16_t color = WHITE;
    if (s.quoteChgOk && s.quoteChg10 > 0) color = GREEN;
    else if (s.quoteChgOk && s.quoteChg10 < 0) color = RED;
    gfx->setTextFont(4);
    gfx->setTextColor(color, BG);
    int priceW = gfx->textWidth(line);
    gfx->setCursor(8, 28);
    gfx->print(line);
    if (s.quoteChgOk) {
      char pct[16];
      fmtTenths(pct, sizeof(pct), s.quoteChg10);
      gfx->setTextFont(2);
      int changeW = gfx->textWidth(pct);
      if (16 + priceW + changeW < 308) {
        gfx->setTextColor(color, BG);
        gfx->setCursor(312 - changeW, 36);
        gfx->print(pct);
      }
    }
  }
  if (s.quotePoints >= 2) {
    uint16_t color = ORANGE;
    if (s.quoteChgOk && s.quoteChg10 > 0) color = GREEN;
    else if (s.quoteChgOk && s.quoteChg10 < 0) color = RED;
    drawChart(s.quoteSeries, s.quotePoints, color, 64, 142);
  } else if (!have) {
    gfx->setTextFont(4);
    gfx->setTextColor(WHITE, BG);
    gfx->setCursor(16, 70);
    gfx->print(s.bridge ? "No price" : "Needs bridge");
  } else {
    gfx->setTextFont(4);
    gfx->setTextColor(WHITE, BG);
    gfx->setCursor(16, 78);
    gfx->print("No history");
  }
}

static void drawWrapped(const char* text, int x, int y, int maxWidth, int lineStep, int maxLines) {
  gfx->setTextFont(4);
  gfx->setTextColor(WHITE, BG);
  const char* p = text ? text : "";
  char buf[160];
  for (int line = 0; line < maxLines && *p;) {
    while (*p == ' ') p++;
    if (*p == '\n') {
      p++;
      line++;
      continue;
    }
    if (!*p) break;

    const char* breakAt = nullptr;
    const char* q = p;
    while (*q && *q != '\n') {
      const char* wordEnd = q;
      while (*wordEnd && *wordEnd != ' ' && *wordEnd != '\n') wordEnd++;
      size_t take = (size_t)(wordEnd - p);
      if (take >= sizeof(buf)) take = sizeof(buf) - 1;
      memcpy(buf, p, take);
      buf[take] = 0;
      if (gfx->textWidth(buf) > maxWidth) break;
      breakAt = wordEnd;
      if (*wordEnd != ' ') break;
      q = wordEnd + 1;
    }

    size_t n = 0;
    bool cutWord = false;
    if (breakAt && breakAt != p) {
      n = (size_t)(breakAt - p);
      while (n > 0 && p[n - 1] == ' ') n--;
    } else {
      cutWord = true;
      size_t limit = 0;
      while (p[limit] && p[limit] != ' ' && p[limit] != '\n' && limit + 1 < sizeof(buf)) limit++;
      while (n < limit) {
        memcpy(buf, p, n + 1);
        buf[n + 1] = 0;
        if (gfx->textWidth(buf) > maxWidth) break;
        n++;
      }
      if (n == 0) n = 1;
    }
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, p, n);
    buf[n] = 0;

    const char* rest = p + n;
    while (*rest == ' ') rest++;
    bool more = line + 1 >= maxLines && *rest && *rest != '\n';
    if (more) {
      char with[164];
      snprintf(with, sizeof(with), "%s...", buf);
      while (n > 0 && gfx->textWidth(with) > maxWidth) {
        n--;
        buf[n] = 0;
        snprintf(with, sizeof(with), "%s...", buf);
      }
      gfx->setCursor(x, y + line * lineStep);
      gfx->print(with);
      break;
    }
    gfx->setCursor(x, y + line * lineStep);
    gfx->print(buf);
    p += n;
    if (!cutWord) {
      while (*p == ' ') p++;
      if (*p == '\n') p++;
    }
    line++;
  }
}

static void drawPost(int index) {
  const Status& s = dataStatus();
  char heading[56];
  snprintf(heading, sizeof(heading), "#%s", boardTag());
  if (index < 0 || index >= s.postCount || !s.postText[index][0]) {
    drawHeader(heading, "");
    gfx->setTextFont(4);
    gfx->setTextColor(WHITE, BG);
    gfx->setCursor(16, 58);
    gfx->print(s.bridge ? "No posts" : "Needs bridge");
    return;
  }
  char who[24];
  snprintf(who, sizeof(who), "@%s", s.postUser[index][0] ? s.postUser[index] : "post");
  char age[12];
  fmtAgeShort(age, sizeof(age), s.postAgo[index]);
  snprintf(heading, sizeof(heading), "#%s %s", boardTag(), who);
  drawHeader(heading, age);
  drawWrapped(s.postText[index], 8, 32, 304, 28, 4);
}

static void drawNote() {
  const char* title = boardNoteTitle();
  drawHeader(title && title[0] ? title : "NOTE", "");
  const char* body = boardNoteBody();
  if (!body || !body[0]) {
    gfx->setTextFont(4);
    gfx->setTextColor(WHITE, BG);
    gfx->setCursor(16, 58);
    gfx->print("Add a note");
    return;
  }
  drawWrapped(body, 8, 32, 304, 28, 4);
}

static void render() {
  gfx->fillRect(0, 24, 320, 146, BG);
  const Status& s = dataStatus();
  BoardSlot slot = boardSlot(page);
  if (slot.kind == KIND_CLOCK) {
    drawHeader("CLOCK", boardTzLabel());
    drawClock(false);
  } else if (slot.kind == KIND_WEATHER) {
    drawHeader("WEATHER", boardPlace()[0] ? boardPlace() : boardZip());
    drawWeather();
  } else if (slot.kind == KIND_BITCOIN) {
    drawHeader("BITCOIN", s.btcLocal ? "this PC" : "network");
    drawBitcoin();
  } else if (slot.kind == KIND_NETWORK) {
    drawHeader("NETWORK", "");
    drawNetwork();
  } else if (slot.kind == KIND_HASH) {
    drawHeader("HASHRATE", boardSpanLabel());
    drawHashrate();
  } else if (slot.kind == KIND_PRICE) {
    drawHeader("PRICE", boardPriceSpanLabel());
    drawPrice();
  } else if (slot.kind == KIND_LIGHTNING) {
    drawHeader("LIGHTNING", s.lnAsOf);
    drawLightning();
  } else if (slot.kind == KIND_SERVERS) {
    drawHeader("SERVERS", s.bridge ? "live" : "ports");
    drawServers();
  } else if (slot.kind == KIND_NOTE) {
    drawNote();
  } else {
    drawPost(slot.index == 255 ? -1 : (int)slot.index);
  }
  drawDots();
  shown = page;
  dirty = false;
  seenGen = dataGeneration();
  seenWeather = weatherGeneration();
  lastSec = -1;
}

static bool beginPageSprite() {
  if (pageSprite.created()) return true;
  pageSprite.setColorDepth(16);
  return pageSprite.createSprite(320, 170) != nullptr;
}

static void matrixTransition() {
  if (!beginPageSprite()) {
    render();
    return;
  }

  pageSprite.setViewport(0, 0, 320, 170);
  pageSprite.setTextWrap(false);
  gfx = &pageSprite;
  render();
  gfx = &tft;

  static const char kGlyphs[] = "00112233445566778899ABCDEF#$%&*+<>/=";
  const int glyphCount = (int)sizeof(kGlyphs) - 1;
  const int cols = 320 / 6;
  const int rows = 170 / 8;
  int8_t stagger[54];
  int8_t trail[54];
  char dropped[54][22];
  int frames = rows;
  for (int c = 0; c < cols; c++) {
    stagger[c] = (int8_t)random(0, 10);
    trail[c] = (int8_t)(5 + random(0, 5));
    int travel = stagger[c] + rows + trail[c];
    if (travel > frames) frames = travel;
    for (int r = 0; r < rows; r++) dropped[c][r] = kGlyphs[random(glyphCount)];
  }

  uint16_t headC = tft.color565(190, 255, 190);
  uint16_t midC = tft.color565(0, 230, 70);
  uint16_t dimC = tft.color565(0, 110, 34);
  tft.setTextFont(1);
  tft.setTextWrap(false);

  for (int frame = 0; frame < frames; frame++) {
    for (int c = 0; c < cols; c++) {
      int head = frame - stagger[c];
      if (head < 0) continue;
      int reveal = head - trail[c];
      if (reveal >= 0 && reveal < rows) {
        int y = reveal * 8;
        int h = (y + 8 > 170) ? 170 - y : 8;
        pageSprite.pushSprite(c * 6, y, c * 6, y, 6, h);
      }
      if (head < rows) {
        dropped[c][head] = kGlyphs[random(glyphCount)];
        tft.setTextColor(headC, TFT_BLACK);
        tft.setCursor(c * 6, head * 8);
        tft.write(dropped[c][head]);
      }
      int mid = head - 1;
      if (mid > reveal && mid >= 0 && mid < rows) {
        tft.setTextColor(midC, TFT_BLACK);
        tft.setCursor(c * 6, mid * 8);
        tft.write(dropped[c][mid]);
      }
      int dim = head - 3;
      if (dim > reveal && dim >= 0 && dim < rows) {
        tft.setTextColor(dimC, TFT_BLACK);
        tft.setCursor(c * 6, dim * 8);
        tft.write(dropped[c][dim]);
      }
    }
    delay(1);
  }
  pageSprite.pushSprite(0, 0);
}

void uiBegin() {
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);
  delay(20);
  pinMode(PIN_BUTTON_PREV, INPUT_PULLUP);
  pinMode(PIN_BUTTON_NEXT, INPUT_PULLUP);

  BG = gfx->color565(6, 8, 12);
  PANEL = gfx->color565(18, 24, 34);
  ORANGE = gfx->color565(247, 147, 26);
  DIM = gfx->color565(150, 158, 170);
  GREEN = gfx->color565(70, 200, 110);
  RED = gfx->color565(230, 74, 68);
  AMBER = gfx->color565(240, 176, 46);
  WHITE = TFT_WHITE;
  BLUE = gfx->color565(90, 170, 230);
  CLOUD = gfx->color565(176, 186, 200);

  gfx->init();
  gfx->setRotation(1);
  randomSeed(esp_random());
  gfx->fillScreen(BG);
  gfx->setTextWrap(false);
}

void uiBoot(const char* a, const char* b, const char* c) {
  gfx->fillScreen(BG);
  gfx->fillRect(0, 0, 320, 6, ORANGE);
  gfx->setTextFont(4);
  gfx->setTextColor(WHITE, BG);
  gfx->setCursor(16, 40);
  gfx->print(a);
  gfx->setTextFont(2);
  gfx->setTextColor(ORANGE, BG);
  gfx->setCursor(16, 78);
  gfx->print(b);
  gfx->setTextColor(DIM, BG);
  gfx->setCursor(16, 104);
  gfx->print(c);
  shown = -1;
  dirty = true;
}

int uiPage() { return page; }

static int pageCount() {
  int n = boardSlotCount();
  return n < 1 ? 1 : n;
}

void uiNext() {
  page = (page + 1) % pageCount();
  holdUntil = millis() + PAGE_HOLD_MS;
  nextRotate = millis() + boardRotateMs();
  dirty = true;
}

void uiPrev() {
  page = (page + pageCount() - 1) % pageCount();
  holdUntil = millis() + PAGE_HOLD_MS;
  nextRotate = millis() + boardRotateMs();
  dirty = true;
}

void uiHold() { holdUntil = millis() + PAGE_HOLD_MS; }

void uiSetPaused(bool paused) {
  uiPause = paused;
  if (!paused) dirty = true;
}

void uiOnBoard() {
  if (page >= pageCount()) page = 0;
  nextRotate = millis() + boardRotateMs();
  dirty = true;
}

void uiLoop() {
  if (uiPause) return;
  uint32_t now = millis();
  if (boardSyncPosts(dataStatus().postCount)) dirty = true;
  if (page >= pageCount()) page = 0;
  if (!rotateArmed) {
    rotateArmed = true;
    nextRotate = now + boardRotateMs();
  }
  if ((int32_t)(now - holdUntil) >= 0 && (int32_t)(now - nextRotate) >= 0) {
    page = (page + 1) % pageCount();
    nextRotate = now + boardRotateMs();
    dirty = true;
  }
  BoardSlot slot = boardSlot(page);
  if (dirty || page != shown) {
    if (shown >= 0 && page != shown) matrixTransition();
    else render();
    return;
  }
  if (slot.kind != KIND_CLOCK && dataGeneration() != seenGen) {
    render();
    return;
  }
  if (slot.kind == KIND_CLOCK) {
    struct tm tmNow;
    if (getLocalTime(&tmNow, 0) && tmNow.tm_sec != lastSec) {
      drawClock(true);
      lastSec = tmNow.tm_sec;
    }
  } else if (slot.kind == KIND_WEATHER && weatherGeneration() != seenWeather) {
    render();
  }
}
