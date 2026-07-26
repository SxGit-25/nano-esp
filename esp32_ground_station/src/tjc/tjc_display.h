#pragma once

#include <Arduino.h>

#include "../network/tcp_server.h"

class TjcDisplay {
public:
    void begin();
    void showAp(bool ready);
    void showNano(NanoLinkState state);
    void showTi(bool online);
    void showVehicleStatus(const NanoVehicleStatus &status);

private:
    void setText(const char *component, const char *text);

    bool ap_known_ = false;
    bool ap_ready_ = false;
    bool nano_known_ = false;
    NanoLinkState nano_state_ = NanoLinkState::DOWN;
    bool ti_known_ = false;
    bool ti_online_ = false;
    bool vehicle_known_ = false;
    NanoVehicleStatus vehicle_status_;
};
