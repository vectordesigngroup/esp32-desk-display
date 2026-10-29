#pragma once

#include <Arduino.h>
#include "data.h"

void uiBegin();
void uiBoot(const char* a, const char* b, const char* c);
int uiPage();
void uiNext();
void uiPrev();
void uiHold();
void uiSetPaused(bool paused);
void uiOnBoard();
void uiLoop();
