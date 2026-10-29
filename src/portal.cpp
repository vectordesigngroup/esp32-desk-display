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

static const char PAGE_STYLE[] PROGMEM = R"PORTALCSS(
:root{--bg:#090b10;--line:#3a3124;--amber:#f7931a;--ink:#f6f1e8;--muted:#a39886;--ok:#3ddc97}
*{box-sizing:border-box}
body{margin:0;overflow-x:hidden;color:var(--ink);font:16px/1.45 "Segoe UI",system-ui,sans-serif;background:
radial-gradient(70rem 26rem at 50% -8rem,rgba(247,147,26,.30),transparent 58%),
radial-gradient(24rem 16rem at 100% 0,rgba(61,220,151,.12),transparent 55%),var(--bg)}
body:before{content:"";position:fixed;inset:0;pointer-events:none;
background-image:linear-gradient(rgba(247,147,26,.055) 1px,transparent 1px),linear-gradient(90deg,rgba(247,147,26,.055) 1px,transparent 1px);
background-size:28px 28px;mask-image:radial-gradient(circle at 50% 0,#000,transparent 72%)}
main{max-width:42rem;margin:0 auto;padding:1.1rem 1rem 2rem;position:relative}
a{color:var(--amber);text-decoration:none}a:hover{text-decoration:underline}
h1{font-size:1.85rem;line-height:1.05;margin:.15rem 0 .35rem;letter-spacing:-.03em}
h2{font-size:1rem;margin:0 0 .45rem}
.kicker{margin:0;color:var(--amber);font-size:.72rem;letter-spacing:.18em;text-transform:uppercase}
.hint{color:var(--muted);margin:.15rem 0 .7rem}
.hero{display:grid;grid-template-columns:minmax(0,1.35fr) minmax(12rem,.8fr);gap:.8rem 1.1rem;align-items:center;margin-bottom:.35rem}
.hero>*{min-width:0}
.bezel{display:block;width:100%;max-width:100%;height:auto;overflow:visible;filter:drop-shadow(0 16px 28px rgba(0,0,0,.45))}
.trace{stroke-dasharray:7 5;animation:flow 1.4s linear infinite}
@keyframes flow{to{stroke-dashoffset:-24}}
.meta{display:flex;flex-wrap:wrap;gap:.4rem;margin:.55rem 0 0}
.pill{display:inline-flex;align-items:center;gap:.4rem;border:1px solid var(--line);background:#10141b;border-radius:999px;padding:.18rem .65rem;color:var(--muted);font-size:.82rem}
.pill b{color:var(--ink);font-weight:600}
.dot{width:.5rem;height:.5rem;border-radius:50%;background:var(--ok);box-shadow:0 0 .5rem var(--ok)}
.tiles{display:grid;grid-template-columns:repeat(3,1fr);gap:.5rem;margin:.35rem 0 .2rem}
.tiles article{background:#10141b;border:1px solid #2a3140;border-radius:14px;padding:.6rem .7rem}
.tiles b{display:block;margin-top:.25rem}
.tiles span{color:var(--muted);font-size:.75rem}
.saved{display:flex;gap:.55rem;align-items:center;background:rgba(61,220,151,.1);border:1px solid rgba(61,220,151,.45);color:#c9ffe8;border-radius:14px;padding:.65rem .8rem;margin:.7rem 0}
section{background:rgba(16,20,27,.92);border:1px solid var(--line);border-radius:18px;padding:.85rem .9rem .95rem;margin:.75rem 0}
ol.pages{list-style:none;margin:.2rem 0 0;padding:0;display:flex;flex-direction:column;gap:.4rem}
.pages li{display:flex;gap:.35rem;align-items:center}
.nudge{width:2.05rem;height:2.05rem;padding:0;border-radius:11px;background:#1b212c;color:var(--muted);border:1px solid #31384a;display:grid;place-items:center;cursor:pointer}
.nudge:hover{color:var(--ink);border-color:var(--amber)}
.nudge svg{width:1rem;height:1rem;fill:none;stroke:currentColor;stroke-width:2.4;stroke-linecap:round;stroke-linejoin:round}
.page{position:relative;flex:1;display:flex;align-items:center;gap:.6rem;min-height:2.7rem;background:#0e131b;border:1px solid #2a3140;border-radius:14px;padding:.3rem .55rem .3rem .4rem;cursor:pointer}
.page:has(input:checked){border-color:rgba(247,147,26,.75);background:#1b140e;box-shadow:inset 0 0 0 1px rgba(247,147,26,.18)}
.ord{width:1.15rem;text-align:center;color:transparent;font-variant-numeric:tabular-nums;font-size:.78rem}
.page:has(input:checked) .ord{color:var(--amber)}
.ico{width:1.3rem;height:1.3rem;flex:none;color:#6f675c}
.tiles .ico,.page:has(input:checked) .ico{color:var(--amber)}
.ico path,.ico circle,.ico rect,.ico line,.ico polyline{fill:none;stroke:currentColor;stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round}
.name{font-weight:600}
.page input{position:absolute;opacity:0;pointer-events:none}
.sw{margin-left:auto;width:2.25rem;height:1.22rem;border-radius:999px;background:#2c3340;position:relative;flex:none}
.sw:after{content:"";position:absolute;top:.12rem;left:.12rem;width:.98rem;height:.98rem;border-radius:50%;background:#8d95a3;transition:transform .15s ease}
.page input:checked+.sw{background:var(--amber)}
.page input:checked+.sw:after{transform:translateX(1.02rem);background:#1a1006}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:.65rem}
label.field,label.wide{display:flex;flex-direction:column;gap:.28rem;font-size:.8rem;color:var(--muted)}
label.wide{margin-top:.65rem}
input,select,textarea{font:inherit;color:var(--ink);background:#0c1016;border:1px solid #2e3544;border-radius:11px;padding:.55rem .65rem;width:100%}
textarea{resize:vertical;min-height:5.5rem}
input:focus,select:focus,textarea:focus{outline:2px solid rgba(247,147,26,.4);border-color:var(--amber)}
.srv{display:grid;grid-template-columns:6.2rem 1fr 5.2rem;gap:.4rem;margin:.45rem 0;padding-top:.15rem}
.srv .tag{grid-column:1/-1;color:var(--muted);font-size:.75rem;letter-spacing:.04em}
.bar{position:sticky;bottom:0;display:flex;justify-content:space-between;align-items:center;gap:1rem;margin-top:.4rem;padding:.85rem 0 1rem;background:linear-gradient(transparent,#090b10 32%)}
button.save,button.go{border:0;border-radius:999px;background:linear-gradient(#ffd27a,#f7931a);color:#1a1006;font-weight:700;padding:.75rem 1.35rem;cursor:pointer}
button.go{margin-top:.8rem}
.mark{width:1.1rem;height:1.1rem;vertical-align:-.15rem;margin-right:.25rem}
@media(max-width:680px){.hero,.grid,.tiles,.srv{grid-template-columns:minmax(0,1fr)}}
)PORTALCSS";

static const char PANEL_ART[] PROGMEM = R"PORTALSVG(
<svg class="bezel" viewBox="0 0 360 156" width="360" height="156" role="img" aria-label="Desk display">
<defs>
<linearGradient id="rim" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#ffe3ad"/><stop offset="1" stop-color="#f7931a"/></linearGradient>
<linearGradient id="glow" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f7931a" stop-opacity=".45"/><stop offset="1" stop-color="#f7931a" stop-opacity="0"/></linearGradient>
</defs>
<rect x="4" y="18" width="236" height="124" rx="20" fill="#17130f" stroke="url(#rim)" stroke-width="2.5"/>
<rect x="16" y="30" width="212" height="100" rx="8" fill="#07090e"/>
<rect x="16" y="30" width="212" height="10" fill="#f7931a" opacity=".13"><animate attributeName="y" values="30;120;30" dur="5s" repeatCount="indefinite"/></rect>
<text x="28" y="74" fill="#f6f1e8" font-size="32" font-family="ui-monospace,Consolas,monospace">09:41</text>
<text x="30" y="94" fill="#a39886" font-size="12" font-family="Segoe UI,sans-serif">DESK</text>
<polygon points="28,122 58,112 86,116 118,96 146,102 176,78 214,84 214,122" fill="url(#glow)"/>
<polyline class="trace" points="28,122 58,112 86,116 118,96 146,102 176,78 214,84" fill="none" stroke="#f7931a" stroke-width="2.4" stroke-linejoin="round" stroke-linecap="round"/>
<circle cx="104" cy="122" r="2.3" fill="#f7931a"/><circle cx="116" cy="122" r="2.3" fill="#3d382f"/><circle cx="128" cy="122" r="2.3" fill="#3d382f"/>
<g transform="translate(258,36)" fill="none" stroke="#f7931a" stroke-linecap="round" stroke-linejoin="round">
<circle cx="40" cy="42" r="36" fill="#1c140c" stroke="url(#rim)" stroke-width="2.5"/>
<circle cx="40" cy="42" r="28" stroke-width="1.1" opacity=".55"/>
<path stroke-width="2.3" d="M34 22 v40 M43 22 v40"/>
<path stroke-width="2.6" d="M27 32 h14 a6 6 0 0 1 0 12 H27"/>
<path stroke-width="2.6" d="M27 44 h16 a7 7 0 0 1 0 14 H27"/>
</g>
</svg>
)PORTALSVG";

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

static void appendIcon(String& html, const char* id) {
  html += F("<svg class=\"ico\" viewBox=\"0 0 24 24\" aria-hidden=\"true\">");
  if (strcmp(id, "clock") == 0) html += F("<circle cx=\"12\" cy=\"12\" r=\"8\"/><path d=\"M12 8v4.5L15 15\"/>");
  else if (strcmp(id, "weather") == 0)
    html += F("<circle cx=\"12\" cy=\"12\" r=\"3.1\"/><path d=\"M12 5v1.8M12 17.2V19M5 12h1.8M17.2 12H19M7.1 7.1l1.3 1.3M15.6 15.6l1.3 1.3M16.9 7.1l-1.3 1.3M8.4 15.6 7.1 16.9\"/>");
  else if (strcmp(id, "bitcoin") == 0)
    html += F("<circle cx=\"12\" cy=\"12\" r=\"8\"/><path d=\"M9.2 8.2h3.4a1.7 1.7 0 0 1 0 3.4H9.2zm0 3.4h3.8a1.7 1.7 0 0 1 0 3.4H9.2M10.6 6.6v10.8M13.2 6.6v10.8\"/>");
  else if (strcmp(id, "network") == 0)
    html += F("<path d=\"M5 15.5a9 9 0 0 1 14 0M7.6 12.8a5.5 5.5 0 0 1 8.8 0\"/><circle cx=\"12\" cy=\"17.2\" r=\"1.15\"/>");
  else if (strcmp(id, "hashrate") == 0) html += F("<path d=\"M6 17V10M10 17V7M14 17v-6M18 17V9\"/>");
  else if (strcmp(id, "lightning") == 0) html += F("<path d=\"M13 3 6.5 13H12l-1 8 6.5-10H12z\"/>");
  else if (strcmp(id, "servers") == 0)
    html += F("<rect x=\"4\" y=\"4.5\" width=\"16\" height=\"5\" rx=\"1.2\"/><rect x=\"4\" y=\"14.5\" width=\"16\" height=\"5\" rx=\"1.2\"/><path d=\"M7 7h.1M7 17h.1\"/>");
  else if (strcmp(id, "posts") == 0) html += F("<path d=\"M5 5.5h14v9.2H9.2L5 18z\"/>");
  else if (strcmp(id, "note") == 0) html += F("<path d=\"M7 3.8h7.2L18 7.6V20H7zM14.2 3.8V7.6H18\"/>");
  else if (strcmp(id, "price") == 0) html += F("<path d=\"M4 16.5 8.5 11l3.2 3.1L19 7\"/><path d=\"M14 7h5v5\"/>");
  else html += F("<circle cx=\"12\" cy=\"12\" r=\"8\"/>");
  html += F("</svg>");
}

static void appendHead(String& html, const char* title) {
  html += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
  html += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  html += F("<title>");
  appendEscaped(html, title);
  html += F("</title><style>");
  html += FPSTR(PAGE_STYLE);
  html += F("</style></head><body><main>");
}

static void appendRow(String& html, const char* id, const char* label, bool on) {
  html += F("<li><button type=\"button\" class=\"nudge\" onclick=\"move(this,-1)\" aria-label=\"Move up\">");
  html += F("<svg viewBox=\"0 0 24 24\"><path d=\"M6 14l6-6 6 6\"/></svg></button>");
  html += F("<button type=\"button\" class=\"nudge\" onclick=\"move(this,1)\" aria-label=\"Move down\">");
  html += F("<svg viewBox=\"0 0 24 24\"><path d=\"M6 10l6 6 6-6\"/></svg></button><label class=\"page\"><span class=\"ord\"></span>");
  appendIcon(html, id);
  html += F("<span class=\"name\">");
  appendEscaped(html, label);
  html += F("</span><input type=\"checkbox\" name=\"page\" value=\"");
  html += id;
  html += on ? F("\" checked>" ) : F("\">");
  html += F("<i class=\"sw\"></i></label></li>");
}

static void handleRoot() {
  if (!allow()) return;
  String html;
  html.reserve(22000);
  appendHead(html, "Desk display");
  html += F("<div class=\"hero\">");
  html += FPSTR(PANEL_ART);
  html += F("<div><p class=\"kicker\">LilyGO T-Display-S3</p><h1>Desk display</h1>");
  html += F("<p class=\"hint\">Turn pages on, then set the order. Each post still gets its own screen. Save applies immediately.</p>");
  html += F("<div class=\"meta\"><span class=\"pill\"><span class=\"dot\"></span>");
  html += WiFi.localIP().toString();
  html += F("</span><span class=\"pill\">");
  html += WiFi.RSSI();
  html += F(" dBm</span>");
  if (boardPlace()[0]) {
    html += F("<span class=\"pill\">");
    appendEscaped(html, boardPlace());
    html += F(" ");
    appendEscaped(html, boardZip());
    html += F("</span>");
  }
  html += F("</div></div></div>");
  if (server.hasArg("saved")) {
    html += F("<p class=\"saved\"><svg class=\"ico\" viewBox=\"0 0 24 24\" aria-hidden=\"true\"><path d=\"M5 12.5 10 17l9-10\"/></svg>");
    html += F("Saved. The screen is using this now.</p>");
  }
  html += F("<div class=\"tiles\"><article>");
  appendIcon(html, "clock");
  html += F("<b>");
  html += boardSeconds();
  html += F("s</b><span>on each page</span></article><article>");
  appendIcon(html, "hashrate");
  html += F("<b>");
  appendEscaped(html, boardSpanLabel());
  html += F("</b><span>hashrate</span></article><article>");
  appendIcon(html, "price");
  html += F("<b>");
  appendEscaped(html, boardCur());
  html += F("</b><span>");
  appendEscaped(html, boardPriceSpanLabel());
  html += F("</span></article></div>");
  html += F("<form method=\"POST\" action=\"/board\">");
  html += F("<section><h2>Pages</h2><ol class=\"pages\">");

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

  html += F("</ol></section><section><h2>Place</h2><div class=\"grid\"><label class=\"field\">Seconds on each page<input type=\"number\" name=\"seconds\" min=\"4\" max=\"120\" value=\"");
  html += boardSeconds();
  html += F("\"></label><label class=\"field\">Time zone<select name=\"tz\">");
  for (int i = 0; i < boardTzCount(); i++) {
    html += F("<option value=\"");
    html += boardTzId(i);
    html += strcmp(boardTzId(i), boardTz()) == 0 ? F("\" selected>") : F("\">");
    appendEscaped(html, boardTzName(i));
    html += F("</option>");
  }
  html += F("</select></label></div><label class=\"wide\">Weather ZIP code<input type=\"text\" name=\"zip\" maxlength=\"5\" inputmode=\"numeric\" value=\"");
  appendEscaped(html, boardZip());
  html += F("\"></label><p class=\"hint\">US ZIP code. The clock and the forecast use the time zone above.</p></section>");
  html += F("<section><h2>Charts</h2><div class=\"grid\"><label class=\"field\">Hashrate<select name=\"span\">");
  for (int i = 0; i < boardSpanCount(); i++) {
    html += F("<option value=\"");
    html += boardSpanId(i);
    html += strcmp(boardSpanId(i), boardSpan()) == 0 ? F("\" selected>") : F("\">");
    appendEscaped(html, boardSpanName(i));
    html += F("</option>");
  }
  html += F("</select></label><label class=\"field\">Price chart<select name=\"pspan\">");
  for (int i = 0; i < boardPriceSpanCount(); i++) {
    html += F("<option value=\"");
    html += boardPriceSpanId(i);
    html += strcmp(boardPriceSpanId(i), boardPriceSpan()) == 0 ? F("\" selected>") : F("\">");
    appendEscaped(html, boardPriceSpanName(i));
    html += F("</option>");
  }
  html += F("</select></label></div><label class=\"wide\">Currency<select name=\"cur\">");
  for (int i = 0; i < boardCurCount(); i++) {
    html += F("<option value=\"");
    html += boardCurId(i);
    html += strcmp(boardCurId(i), boardCur()) == 0 ? F("\" selected>") : F("\">");
    appendEscaped(html, boardCurName(i));
    html += F("</option>");
  }
  html += F("</select></label><p class=\"hint\">The price page uses this window and currency. The Bitcoin page keeps the US dollar price.</p></section>");
  html += F("<section><h2>Posts</h2><label class=\"field\">Search word<input type=\"text\" name=\"tag\" maxlength=\"24\" value=\"");
  appendEscaped(html, boardTag());
  html += F("\"></label><p class=\"hint\">A word or hashtag, without the #. Each matching post still gets its own screen.</p></section>");
  html += F("<section><h2>More servers</h2><p class=\"hint\">Optional. Name, address, and port. Empty rows are skipped. Jellyfin, Vault, Frigate, and StartOS stay on the page.</p>");
  for (int i = 0; i < 4; i++) {
    const char* name = "";
    const char* host = "";
    int port = 0;
    boardServerAt(i, &name, &host, &port, nullptr);
    html += F("<div class=\"srv\"><span class=\"tag\">Server ");
    html += i + 1;
    html += F("</span><input type=\"text\" name=\"sname");
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
    html += F("\"></div>");
  }
  html += F("</section><section><h2>Note</h2><label class=\"field\">Title<input type=\"text\" name=\"noteTitle\" maxlength=\"22\" value=\"");
  appendEscaped(html, boardNoteTitle());
  html += F("\"></label><label class=\"wide\">Text<textarea name=\"noteBody\" maxlength=\"160\" rows=\"4\">");
  appendEscaped(html, boardNoteBody());
  html += F("</textarea></label><p class=\"hint\">Turn on Note to show that text. Leave it off and the words stay saved here.</p></section>");
  html += F("<div class=\"bar\"><a href=\"/firmware\">Firmware update</a><button class=\"save\" type=\"submit\">Save</button></div></form>");
  html += F("<script>function paint(){var items=document.querySelectorAll('.pages li'),n=0;");
  html += F("for(var i=0;i<items.length;i++){var box=items[i].querySelector('input');var ord=items[i].querySelector('.ord');");
  html += F("if(box.checked){n++;ord.textContent=n;}else ord.textContent='';}}");
  html += F("function move(btn,dir){var li=btn.parentNode,list=li.parentNode;");
  html += F("if(dir<0&&li.previousElementSibling)list.insertBefore(li,li.previousElementSibling);");
  html += F("else if(dir>0&&li.nextElementSibling)list.insertBefore(li.nextElementSibling,li);paint();}");
  html += F("document.querySelector('.pages').addEventListener('change',paint);paint();</script>");
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
  String html;
  html.reserve(8000);
  appendHead(html, "Desk display firmware");
  html += F("<div class=\"hero\"><svg class=\"bezel\" viewBox=\"0 0 220 150\" role=\"img\" aria-label=\"Firmware chip\">");
  html += F("<rect x=\"48\" y=\"28\" width=\"124\" height=\"94\" rx=\"12\" fill=\"#17130f\" stroke=\"#f7931a\" stroke-width=\"2.5\"/>");
  html += F("<rect x=\"68\" y=\"46\" width=\"84\" height=\"48\" rx=\"6\" fill=\"#07090e\"/>");
  html += F("<path d=\"M110 58v24M98 70h24\" stroke=\"#f7931a\" stroke-width=\"3\" stroke-linecap=\"round\"/>");
  html += F("<g stroke=\"#f7931a\" stroke-width=\"3\" stroke-linecap=\"round\">");
  html += F("<path d=\"M68 48H40M68 70H40M68 92H40M152 48h28M152 70h28M152 92h28M86 28V8M110 28V8M134 28V8M86 122v20M110 122v20M134 122v20\"/>");
  html += F("</g></svg><div><p class=\"kicker\">Update</p><h1>Firmware</h1>");
  html += F("<p class=\"hint\">Choose a firmware .bin. The screen reboots when the update is written. Leave the USB serial port closed.</p>");
  html += F("<form method=\"POST\" action=\"/update\" enctype=\"multipart/form-data\">");
  html += F("<input type=\"file\" name=\"firmware\" accept=\".bin,application/octet-stream\" required>");
  html += F("<button class=\"go\" type=\"submit\">Update</button></form>");
  html += F("<p><a href=\"/\">Back to the board</a></p></div></div></main></body></html>");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", html);
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
