#pragma once

#include <Arduino.h>

struct Status {
  bool bridge;
  bool btcOk;
  bool btcLocal;
  int blocks;
  int headers;
  int tip;
  int mempoolTx;
  int peers;
  int feeFast;
  int feeHalf;
  int feeHour;
  int feeEcon;
  int localFee;
  float progress;
  bool ibd;
  bool pruned;

  bool priceOk;
  bool priceChgOk;
  int priceUsd;
  int priceChg10;
  bool quoteOk;
  bool quoteChgOk;
  int quote;
  int quoteChg10;
  int quotePoints;
  int quoteSeries[30];
  int memCount;
  int memVsize;
  int nextTx;
  int nextMed100;
  int hashEh;
  int hashPoints;
  int hashSeries[30];
  int diffT10;
  bool epochOk;
  int adj10;
  int diffLeft;
  int etaMin;
  int prev10;
  int halvingLeft;
  int lastTx;
  int lastAge;
  int lastReward;
  char lastPool[20];

  bool lnOk;
  int lnNodes;
  int lnChannelsPub;
  int lnBtc;
  int lnPpm;
  char lnAsOf[12];

  bool postsOk;
  int postCount;
  int postAgo[3];
  char postUser[3][20];
  char postText[3][160];

  bool startos;
  bool clnOk;
  bool lndOk;
  int clnChannels;
  int lndChannels;
  long clnSat;
  long lndSat;

  bool piUp;
  bool jellyfin;
  bool vault;
  bool frigateUp;
  bool piStats;
  float piLoad;
  int piMem;
  int piDisk;
  uint32_t piUptime;

  bool eventOk;
  bool eventEnded;
  int ago;
  int scorePct;
  char camera[32];
  char label[24];
};

struct ForecastDay {
  int code;
  int hi;
  int lo;
  int rainPct;
  int rain100;
  char label[8];
};

struct Weather {
  bool ok;
  bool forecast;
  float tempF;
  int humidity;
  int code;
  char summary[16];
  ForecastDay days[4];
};

void dataBegin();
void dataRefreshWeather();
void dataKickStatus();
void dataInvalidateQuote();
// Returns true when a fetch finished on this call.
bool dataLoop();
uint32_t dataGeneration();
uint32_t weatherGeneration();
const Status& dataStatus();
const Weather& dataWeather();
const char* weatherSummary(int code);
