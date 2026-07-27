#pragma once

#include <Arduino.h>

#include "../network/tcp_server.h"
#include "tjc_event_reader.h"

enum class TjcInputEventType : uint8_t {
    COMMAND,
    SYNC,
};

struct TjcInputEvent {
    TjcInputEventType type = TjcInputEventType::SYNC;
    GroundStationCommand command = GroundStationCommand::GET_STATUS;
};

class TjcDisplay {
public:
    void begin();
    void poll();
    bool pollEvent(TjcInputEvent &event);
    void forceRefresh();
    void showAp(bool ready);
    void showNano(NanoLinkState state);
    void showTi(bool online);
    void showVehicleStatus(const NanoVehicleStatus &status);
    void showCommandResult(const NanoCommandResult &result);
    void showInputReceipt(const TjcInputEvent &event);

private:
    void refresh();
    void sendCommand(const char *command);
    void writeVehicleStatus(const NanoVehicleStatus &status);
    void setText(const char *component, const char *text);
    bool decodeEvent(
        const uint8_t *frame,
        size_t length,
        TjcInputEvent &event
    ) const;

    TjcEventReader event_reader_;
    bool ap_known_ = false;
    bool ap_ready_ = false;
    bool nano_known_ = false;
    NanoLinkState nano_state_ = NanoLinkState::DOWN;
    bool ti_known_ = false;
    bool ti_online_ = false;
    bool vehicle_known_ = false;
    NanoVehicleStatus vehicle_status_;
    uint32_t last_refresh_ms_ = 0;
};
