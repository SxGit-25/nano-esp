#pragma once

#include <Arduino.h>

#include "../network/tcp_server.h"

class TjcDisplay {
public:
    void begin();
    void showAp(bool ready);
    void showNano(NanoLinkState state);

private:
    void setText(const char *component, const char *text);

    bool ap_known_ = false;
    bool ap_ready_ = false;
    bool nano_known_ = false;
    NanoLinkState nano_state_ = NanoLinkState::DOWN;
};
