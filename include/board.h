#pragma once

#include <Arduino.h>

enum PageKind : uint8_t {
  KIND_CLOCK = 1,
  KIND_WEATHER = 2,
  KIND_BITCOIN = 3,
  KIND_NETWORK = 4,
  KIND_HASH = 5,
  KIND_LIGHTNING = 6,
  KIND_SERVERS = 7,
  KIND_POST = 8,
  KIND_NOTE = 9,
  KIND_PRICE = 10
};

struct BoardSlot {
  uint8_t kind;
  uint8_t index;
};

void boardBegin();
bool boardSyncPosts(int count);
int boardSlotCount();
BoardSlot boardSlot(int index);
uint32_t boardRotateMs();
int boardSeconds();
const char* boardOrder();
const char* boardNoteTitle();
const char* boardNoteBody();
int boardCatalogCount();
const char* boardPageId(int index);
const char* boardPageLabel(int index);
bool boardSave(int seconds, const String& order, const String& noteTitle, const String& noteBody,
               const String& tz, const String& zip, const String& span, const String& tag,
               const String& priceSpan, const String& currency);
void boardSetServer(int index, const char* name, const char* host, int port);
bool boardServerAt(int index, const char** name, const char** host, int* port, bool* up);
int boardServerCount();
void boardProbeServers();

const char* boardTz();
const char* boardTzLabel();
const char* boardTzPosix();
const char* boardTzQuery();
int boardTzCount();
const char* boardTzId(int index);
const char* boardTzName(int index);

const char* boardZip();
const char* boardLat();
const char* boardLon();
const char* boardPlace();
bool boardPlaceOk();
void boardSetPlace(const char* lat, const char* lon, const char* place);

const char* boardSpan();
const char* boardSpanLabel();
int boardSpanCount();
const char* boardSpanId(int index);
const char* boardSpanName(int index);
const char* boardTag();

const char* boardPriceSpan();
const char* boardPriceSpanLabel();
int boardPriceSpanCount();
const char* boardPriceSpanId(int index);
const char* boardPriceSpanName(int index);
const char* boardCur();
const char* boardCurLabel();
int boardCurCount();
const char* boardCurId(int index);
const char* boardCurName(int index);
