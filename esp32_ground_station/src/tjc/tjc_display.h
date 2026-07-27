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

private:
    void processRxByte(uint8_t byte);
    bool processRxFrame();
    void logRxFrame(uint8_t length) const;
    bool isRecentTxEcho(uint8_t length) const;
    void rememberTxCommand(const char *command);
    void refresh();
    void synchronizeDisplay();
    void probeDisplay();
    void enableCommandFeedback();
    void showMainPage();
    void sendCommand(const char *command);
    void writeVehicleStatus(const NanoVehicleStatus &status);
    void writeCommandResult(const NanoCommandResult &result);
    void setText(const char *component, const char *text);
    bool decodeEvent(
        const uint8_t *frame,
        size_t length,
        TjcInputEvent &event
    ) const;

    static const uint8_t kRxFrameCapacity = 96;
    static const uint8_t kTxHistoryCapacity = 20;
    static const uint8_t kTxCommandCapacity = 80;

    TjcEventReader event_reader_;
    bool ap_known_ = false;
    bool ap_ready_ = false;
    bool nano_known_ = false;
    NanoLinkState nano_state_ = NanoLinkState::DOWN;
    bool ti_known_ = false;
    bool ti_online_ = false;
    bool vehicle_known_ = false;
    NanoVehicleStatus vehicle_status_;
    bool command_result_known_ = false;
    NanoCommandResult command_result_;
    uint8_t rx_frame_[kRxFrameCapacity] = {};
    uint8_t rx_data_length_ = 0;
    uint8_t rx_ff_count_ = 0;
    bool rx_overflow_ = false;
    char tx_history_[kTxHistoryCapacity][kTxCommandCapacity] = {};
    uint8_t tx_history_length_[kTxHistoryCapacity] = {};
    uint8_t tx_history_next_ = 0;
    uint32_t last_refresh_ms_ = 0;
    uint32_t last_probe_ms_ = 0;
    uint32_t last_valid_response_ms_ = 0;
    uint32_t last_debug_log_ms_ = 0;
    uint32_t transmit_count_ = 0;
    uint32_t success_count_ = 0;
    uint32_t error_count_ = 0;
    uint32_t echo_count_ = 0;
    uint8_t last_error_code_ = 0;
    bool power_on_frame_seen_ = false;
    bool display_online_ = false;
    bool readback_verified_ = false;
    bool refresh_pending_ = false;
    bool startup_page_wait_active_ = false;
    bool tx_visual_test_active_ = false;
    uint32_t startup_page_wait_started_ms_ = 0;
};
