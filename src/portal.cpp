#include "portal.h"

#include <ESPmDNS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include <string.h>

#include "board.h"
#include "data.h"
#include "secrets.h"
#include "ui.h"

static WebServer server(80);
static bool uploadAllowed = false;

static const char FIRMWARE_PAGE[] PROGMEM = R"html(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Desk display firmware</title>
<style>
body{font-family:sans-serif;background:#10141c;color:#eee;margin:0}
main{max-width:32rem;margin:2rem auto;padding:0 1rem}
h1{color:#f7931a;font-size:1.5rem}
button{background:#f7931a;color:#111;border:0;padding:.75rem 1rem;font-weight:700}
a{color:#f7931a}
input{display:block;margin:1rem 0}
</style>
</head>
<body>
<main>
<h1>Firmware</h1>
<p>Choose a firmware .bin. The screen reboots when the update is written.</p>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="firmware" accept=".bin,application/octet-stream" required>
<button type="submit">Update</button>
</form>
<p><a href="/">Back to the board</a></p>
</main>
</body>
</html>
)html";

static bool allow() {
  if (server.authenticate(OTA_USER, OTA_PASS)) return true;
  server.requestAuthentication();
  return false;
}

static void appendEscaped(String& out, const char* text) {
  if (!text) return;
  for (const char* p = text; *p; p++) {
    char c = *p;
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else out += c;
  }
}

static void appendRow(String& html, const char* id, const char* label, bool on) {
  html += F("<li><button type=\"button\" onclick=\"move(this,-1)\">Up</button>");
  html += F("<button type=\"button\" onclick=\"move(this,1)\">Down</button><label><input type=\"checkbox\" name=\"page\" value=\"");
  html += id;
  html += on ? F("\" checked> ") : F("\"> ");
  appendEscaped(html, label);
  html += F("</label></li>");
}

static void handleRoot() {
  if (!allow()) return;
  String html;
  html.reserve(9000);
  html += F("<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  html += F("<title>Desk display</title><style>");
  html += F("body{font-family:sans-serif;background:#10141c;color:#eee;margin:0}");
  html += F("main{max-width:32rem;margin:1.4rem auto;padding:0 1rem 2rem}");
  html += F("h1,h2{color:#f7931a}h1{font-size:1.6rem}h2{font-size:1.15rem}a{color:#f7931a}");
  html += F("button,input,textarea{font:inherit}button{background:#f7931a;color:#111;border:0;padding:.4rem .7rem;font-weight:700}");
  html += F("ol{list-style:none;padding:0}li{display:flex;gap:.45rem;align-items:center;margin:.35rem 0}");
  html += F("label.row{display:flex;gap:.45rem;align-items:center}");
  html += F("input[type=number],input[type=text],textarea,select{width:100%;box-sizing:border-box;background:#1c2430;color:#eee;border:1px solid #334;padding:.5rem}");
  html += F(".saved{color:#46c86e}.hint{color:#9aa}</style></head><body><main>");
  html += F("<h1>Desk display</h1>");
  if (server.hasArg("saved")) html += F("<p class=\"saved\">Saved. The screen is using this now.</p>");
  html += F("<p class=\"hint\">Check the pages you want, then move them up or down. Each post gets its own screen. Save applies immediately.</p>");
  html += F("<form method=\"POST\" action=\"/board\">");
  html += F("<ol>");

  bool used[12];
  memset(used, 0, sizeof(used));
  char orderCopy[96];
  strncpy(orderCopy, boardOrder(), sizeof(orderCopy) - 1);
  orderCopy[sizeof(orderCopy) - 1] = 0;
  char* cursor = orderCopy;
  int catalog = boardCatalogCount();
  while (*cursor) {
    while (*cursor == ',') cursor++;
    if (!*cursor) break;
    char* end = cursor;
    while (*end && *end != ',') end++;
    char saved = *end;
    *end = 0;
    for (int i = 0; i < catalog && i < (int)(sizeof(used) / sizeof(used[0])); i++) {
      if (!used[i] && strcmp(cursor, boardPageId(i)) == 0) {
        used[i] = true;
        appendRow(html, boardPageId(i), boardPageLabel(i), true);
      }
    }
    *end = saved;
    cursor = end;
  }
  for (int i = 0; i < catalog && i < (int)(sizeof(used) / sizeof(used[0])); i++) {
    if (!used[i]) appendRow(html, boardPageId(i), boardPageLabel(i), false);
  }

  html += F("</ol><p><label>Seconds on each page<br><input type=\"number\" name=\"seconds\" min=\"4\" max=\"120\" value=\"");
  html += boardSeconds();
  html += F("\"></label></p>");
  html += F("<p><label>Time zone<br><select name=\"tz\">");
  for (int i = 0; i < boardTzCount(); i++) {
    html += F("<option value=\"");
    html += boardTzId(i);
    html += strcmp(boardTzId(i), boardTz()) == 0 ? F("\" selected>") : F("\">");
    appendEscaped(html, boardTzName(i));
    html += F("</option>");
  }
  html += F("</select></label></p>");
  html += F("<p><label>Weather ZIP code<br><input type=\"text\" name=\"zip\" maxlength=\"5\" inputmode=\"numeric\" value=\"");
  appendEscaped(html, boardZip());
  html += F("\"></label></p>");
  html += F("<p class=\"hint\">US ZIP code. The clock and the forecast use the time zone above.</p>");
  html += F("<p><label>Hashrate<br><select name=\"span\">");
  for (int i = 0; i < boardSpanCount(); i++) {
    html += F("<option value=\"");
    html += boardSpanId(i);
    html += strcmp(boardSpanId(i), boardSpan()) == 0 ? F("\" selected>") : F("\">");
    appendEscaped(html, boardSpanName(i));
    html += F("</option>");
  }
  html += F("</select></label></p>");
  html += F("<p><label>Price chart<br><select name=\"pspan\">");
  for (int i = 0; i < boardPriceSpanCount(); i++) {
    html += F("<option value=\"");
    html += boardPriceSpanId(i);
    html += strcmp(boardPriceSpanId(i), boardPriceSpan()) == 0 ? F("\" selected>") : F("\">");
    appendEscaped(html, boardPriceSpanName(i));
    html += F("</option>");
  }
  html += F("</select></label></p>");
  html += F("<p><label>Currency<br><select name=\"cur\">");
  for (int i = 0; i < boardCurCount(); i++) {
    html += F("<option value=\"");
    html += boardCurId(i);
    html += strcmp(boardCurId(i), boardCur()) == 0 ? F("\" selected>") : F("\">");
    appendEscaped(html, boardCurName(i));
    html += F("</option>");
  }
  html += F("</select></label></p>");
  html += F("<p class=\"hint\">The price page uses this window and currency. The Bitcoin page keeps the US dollar price.</p>");
  html += F("<p><label>Post search<br><input type=\"text\" name=\"tag\" maxlength=\"24\" value=\"");
  appendEscaped(html, boardTag());
  html += F("\"></label></p>");
  html += F("<p class=\"hint\">A word or hashtag, without the #. Each matching post still gets its own screen.</p>");
  html += F("<h2>More servers</h2><p class=\"hint\">Optional. Name, address, and port. Empty rows are skipped. Jellyfin, Vault, Frigate, and StartOS stay on the page.</p>");
  for (int i = 0; i < 4; i++) {
    const char* name = "";
    const char* host = "";
    int port = 0;
    boardServerAt(i, &name, &host, &port, nullptr);
    html += F("<p>Server ");
    html += i + 1;
    html += F("<br><input type=\"text\" name=\"sname");
    html += i;
    html += F("\" maxlength=\"12\" placeholder=\"Name\" value=\"");
    appendEscaped(html, name);
    html += F("\"><input type=\"text\" name=\"shost");
    html += i;
    html += F("\" maxlength=\"39\" placeholder=\"Address\" value=\"");
    appendEscaped(html, host);
    html += F("\"><input type=\"number\" name=\"sport");
    html += i;
    html += F("\" min=\"1\" max=\"65535\" placeholder=\"Port\" value=\"");
    if (port > 0) html += port;
    html += F("\"></p>");
  }
  html += F("<p><label>Note title<br><input type=\"text\" name=\"noteTitle\" maxlength=\"22\" value=\"");
  appendEscaped(html, boardNoteTitle());
  html += F("\"></label></p>");
  html += F("<p><label>Note<br><textarea name=\"noteBody\" maxlength=\"160\" rows=\"4\">");
  appendEscaped(html, boardNoteBody());
  html += F("</textarea></label></p>");
  html += F("<p class=\"hint\">Turn on Note to show that text. Leave it off and the words stay saved here.</p>");
  html += F("<button type=\"submit\">Save</button></form>");
  html += F("<p><a href=\"/firmware\">Firmware update</a></p>");
  html += F("<script>function move(btn,dir){var li=btn.parentNode;var list=li.parentNode;");
  html += F("if(dir<0&&li.previousElementSibling)list.insertBefore(li,li.previousElementSibling);");
  html += F("else if(dir>0&&li.nextElementSibling)list.insertBefore(li.nextElementSibling,li);}</script>");
  html += F("</main></body></html>");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", html);
}

static void handleBoard() {
  if (!allow()) return;
  int seconds = server.arg("seconds").toInt();
  String order;
  int count = server.args();
  for (int i = 0; i < count; i++) {
    if (server.argName(i) != "page") continue;
    String id = server.arg(i);
    if (id.length() == 0 || id.length() > 16) continue;
    if (order.length()) order += ',';
    order += id;
  }
  String title = server.arg("noteTitle");
  String body = server.arg("noteBody");
  if (title.length() > 40) title.remove(40);
  if (body.length() > 400) body.remove(400);
  for (int i = 0; i < 4; i++) {
    String nameKey = String("sname") + i;
    String hostKey = String("shost") + i;
    String portKey = String("sport") + i;
    boardSetServer(i, server.arg(nameKey).c_str(), server.arg(hostKey).c_str(), server.arg(portKey).toInt());
  }
  boardSave(seconds, order, title, body, server.arg("tz"), server.arg("zip"), server.arg("span"), server.arg("tag"),
            server.arg("pspan"), server.arg("cur"));
  dataRefreshWeather();
  dataInvalidateQuote();
  dataKickStatus();
  uiOnBoard();
  server.sendHeader("Location", "/?saved=1");
  server.send(303, "text/plain", "Saved");
}

static void handleFirmware() {
  if (!allow()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", FIRMWARE_PAGE);
}

static void handleUpdateDone() {
  if (!uploadAllowed || !server.authenticate(OTA_USER, OTA_PASS)) {
    uploadAllowed = false;
    server.requestAuthentication();
    uiSetPaused(false);
    return;
  }
  bool ok = !Update.hasError();
  server.send(200, "text/plain", ok ? "Updated. Restarting." : "Update failed.");
  if (ok) {
    delay(400);
    ESP.restart();
  }
  uiSetPaused(false);
}

static void handleUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadAllowed = server.authenticate(OTA_USER, OTA_PASS);
    if (!uploadAllowed) return;
    uiSetPaused(true);
    uiBoot("UPDATE", "Do not unplug", WiFi.localIP().toString().c_str());
    Serial.printf("ota start %u\n", (unsigned)up.totalSize);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!uploadAllowed) return;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (!uploadAllowed) return;
    if (Update.end(true)) Serial.printf("ota wrote %u\n", (unsigned)up.totalSize);
    else Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    uploadAllowed = false;
    uiSetPaused(false);
  }
}

void portalBegin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/board", HTTP_POST, handleBoard);
  server.on("/firmware", HTTP_GET, handleFirmware);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpload);
  server.begin();
  if (MDNS.begin("desk-display")) MDNS.addService("http", "tcp", 80);
  Serial.print("portal http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
}

void portalLoop() {
  if (WiFi.status() == WL_CONNECTED) server.handleClient();
}
