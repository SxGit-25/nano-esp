#pragma once

#include <Arduino.h>

#include "secrets.h"

#ifndef CAR_GS_WIFI_SSID
#error "Create secrets.h from secrets.h.example and configure CAR_GS_WIFI_SSID"
#endif

#ifndef CAR_GS_WIFI_PASSWORD
#error "Create secrets.h from secrets.h.example and configure CAR_GS_WIFI_PASSWORD"
#endif

static const uint16_t kGroundStationTcpPort = 8765;
static const uint32_t kGroundStationBaud = 115200;
static const int kTjcRxPin = 5;
static const int kTjcTxPin = 4;

static const uint32_t kNetworkHeartbeatIntervalMs = 1000;
static const uint32_t kNanoLinkDegradedTimeoutMs = 3000;
static const uint32_t kNanoLinkDownTimeoutMs = 5000;
static const uint32_t kHelloTimeoutMs = 5000;
static const uint32_t kTcpSendStallTimeoutMs = 2000;

/* Change these only to match the component names in the TJC HMI project. */
static const char kTjcApTextComponent[] = "tAp";
static const char kTjcNanoTextComponent[] = "tNano";
static const char kTjcTiTextComponent[] = "tTi";
