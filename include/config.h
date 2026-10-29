#pragma once

// Optional machine-specific overrides. include/local.h is not part of the repo.
#if defined(__has_include)
#  if __has_include("local.h")
#    include "local.h"
#  endif
#endif

#ifndef PI_HOST
#define PI_HOST ""
#endif
#ifndef STARTOS_HOST
#define STARTOS_HOST ""
#endif

// Used only before a ZIP is saved on the board. Change these in include/local.h.
#ifndef DEFAULT_ZIP
#define DEFAULT_ZIP "10001"
#endif
#ifndef DEFAULT_LAT
#define DEFAULT_LAT "40.7484"
#endif
#ifndef DEFAULT_LON
#define DEFAULT_LON "-73.9967"
#endif
#ifndef DEFAULT_PLACE
#define DEFAULT_PLACE "New York"
#endif

#define PAGE_HOLD_MS 20000
#define STATUS_EVERY_MS 15000
#define WEATHER_EVERY_MS 600000

#define PIN_POWER_ON 15
#define PIN_BUTTON_PREV 0
#define PIN_BUTTON_NEXT 14
