#pragma once

#include <Arduino.h>

#include "../network/tcp_server.h"

class TjcDisplay {
public:
    void begin();
    void poll();
    void showAp(bool ready);
    void showNano(NanoLinkState state);
    void showTi(bool online);
    void showVehicleStatus(const NanoVehicleStatus &status);

private:
    void processRxByte(uint8_t byte);
    bool processRxFrame();
    void logRxFrame(uint8_t length) const;
    void refresh();
    void synchronizeDisplay();
    void enableCommandFeedback();
    void showMainPage();
    void sendCommand(const char *command);
    void writeVehicleStatus(const NanoVehicleStatus &status);
    void setText(const char *component, const char *text);

    static const uint8_t kRxFrameCapacity = 96;

    bool ap_known_ = false;
    bool ap_ready_ = false;
    bool nano_known_ = false;
    NanoLinkState nano_state_ = NanoLinkState::DOWN;
    bool ti_known_ = false;
    bool ti_online_ = false;
    bool vehicle_known_ = false;
    NanoVehicleStatus vehicle_status_;
    uint8_t rx_frame_[kRxFrameCapacity] = {};
    uint8_t rx_data_length_ = 0;
    uint8_t rx_ff_count_ = 0;
    bool rx_overflow_ = false;
    uint32_t last_refresh_ms_ = 0;
    uint32_t last_debug_log_ms_ = 0;
    uint32_t transmit_count_ = 0;
    uint32_t success_count_ = 0;
    uint32_t error_count_ = 0;
    uint8_t last_error_code_ = 0;
    bool display_ready_seen_ = false;
    bool sync_response_pending_ = false;
    bool startup_page_wait_active_ = false;
    bool tx_visual_test_active_ = false;
    uint32_t startup_page_wait_started_ms_ = 0;
};
