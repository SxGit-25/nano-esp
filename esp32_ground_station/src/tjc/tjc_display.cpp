#include "tjc_display.h"

#include <stdio.h>

#include "app_config.h"

void TjcDisplay::begin() {
    Serial1.begin(
        kGroundStationBaud,
        SERIAL_8N1,
        kTjcRxPin,
        kTjcTxPin
    );
}

void TjcDisplay::showAp(bool ready) {
    if (ap_known_ && ap_ready_ == ready) {
        return;
    }
    ap_known_ = true;
    ap_ready_ = ready;
    setText(kTjcApTextComponent, ready ? "READY" : "DOWN");
}

void TjcDisplay::showNano(NanoLinkState state) {
    if (nano_known_ && nano_state_ == state) {
        return;
    }
    nano_known_ = true;
    nano_state_ = state;

    const char *text = "DOWN";
    if (state == NanoLinkState::CONNECTING) {
        text = "CONNECTING";
    } else if (state == NanoLinkState::UP) {
        text = "UP";
    } else if (state == NanoLinkState::DEGRADED) {
        text = "DEGRADED";
    }
    setText(kTjcNanoTextComponent, text);
}

void TjcDisplay::showTi(bool online) {
    if (ti_known_ && ti_online_ == online) {
        return;
    }
    ti_known_ = true;
    ti_online_ = online;
    setText(kTjcTiTextComponent, online ? "UP" : "DOWN");
}

void TjcDisplay::setText(const char *component, const char *text) {
    char command[80];
    const int command_length = snprintf(
        command,
        sizeof(command),
        "%s.txt=\"%s\"",
        component,
        text
    );
    if (command_length <= 0 || command_length >= static_cast<int>(sizeof(command))) {
        return;
    }
    Serial1.write(reinterpret_cast<const uint8_t *>(command), command_length);
    Serial1.write(0xFF);
    Serial1.write(0xFF);
    Serial1.write(0xFF);
}
