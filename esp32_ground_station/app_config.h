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
static const int kTjcRxPin = 6;
static const int kTjcTxPin = 4;

static const uint32_t kNetworkHeartbeatIntervalMs = 1000;
static const uint32_t kNanoLinkDegradedTimeoutMs = 3000;
static const uint32_t kNanoLinkDownTimeoutMs = 5000;
static const uint32_t kHelloTimeoutMs = 5000;
static const uint32_t kTcpSendStallTimeoutMs = 2000;
static const uint32_t kTjcRefreshIntervalMs = 1000;
static const size_t kTjcMaxInputBytesPerPoll = 64;
static const bool kRunTjcTxVisualTest = false;

/* Change these only to match the component names in the TJC HMI project. */
/* These names match the global text components in the HMI project. */
static const char kTjcApTextComponent[] = "tAp";
static const char kTjcNanoTextComponent[] = "tNano";
static const char kTjcTiTextComponent[] = "tTi";
static const char kTjcFaultTextComponent[] = "tFault";
static const char kTjcRxTextComponent[] = "tRx";

/*
 * A TJC button sends one strict ASCII token followed by FF FF FF. This avoids
 * coupling firmware to unknown page/component IDs. STOP and ESTOP should be
 * emitted by the button Press Event; other commands by the Release Event.
 */
static const char kTjcArmEventToken[] = "GS:ARM";
static const char kTjcDisarmEventToken[] = "GS:DISARM";
static const char kTjcStopEventToken[] = "GS:STOP";
static const char kTjcEstopEventToken[] = "GS:ESTOP";
static const char kTjcClearFaultEventToken[] = "GS:CLEAR_FAULT";
static const char kTjcGetStatusEventToken[] = "GS:GET_STATUS";
static const char kTjcForward500EventToken[] = "GS:FWD_500";
static const char kTjcBackward500EventToken[] = "GS:BACK_500";
static const char kTjcLeft90EventToken[] = "GS:LEFT_90";
static const char kTjcRight90EventToken[] = "GS:RIGHT_90";
static const char kTjcSyncEventToken[] = "GS:SYNC";
