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
static const uint32_t kTjcStartupPageDurationMs = 2200;
static const bool kRunTjcFullDebug = false;
static const bool kRunTjcTxVisualTest = false;
static const size_t kTjcMaxInputBytesPerPoll = 64;

/* Change these only to match the component names in the TJC HMI project. */
static const char kTjcMainPage[] = "main";
static const char kTjcApTextComponent[] = "tAp";
static const char kTjcNanoTextComponent[] = "tNano";
static const char kTjcTiTextComponent[] = "tTi";
static const char kTjcSystemTextComponent[] = "tSystem";
static const char kTjcArmedTextComponent[] = "tArmed";
static const char kTjcFaultTextComponent[] = "tFault";
static const char kTjcDistanceTextComponent[] = "tDistance";
static const char kTjcHeadingTextComponent[] = "tHeading";
static const char kTjcLineErrorTextComponent[] = "tLineError";
static const char kTjcLeftSpeedTextComponent[] = "tLeftSpeed";
static const char kTjcRightSpeedTextComponent[] = "tRightSpeed";
static const char kTjcProgressTextComponent[] = "tProgress";
static const char kTjcLapCountTextComponent[] = "tLapCount";
static const char kTjcSegmentTextComponent[] = "tSegment";
static const char kTjcControlTextComponent[] = "tControl";
static const char kTjcCommandIdTextComponent[] = "tCmdId";
static const char kTjcCommandNameTextComponent[] = "tCmdName";
static const char kTjcCommandStateTextComponent[] = "tCmdState";
static const char kTjcCommandDetailTextComponent[] = "tCmdDetail";

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
static const char kTjcSyncEventToken[] = "GS:SYNC";
