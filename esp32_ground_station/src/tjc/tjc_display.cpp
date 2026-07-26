#include "tjc_display.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"

namespace {

constexpr char kTjcProbeText[] = "UART OK";
constexpr uint32_t kTjcProbeIntervalMs = 1000;
constexpr uint32_t kTjcOnlineTimeoutMs = 3000;

bool vehicleStatusEqual(
    const NanoVehicleStatus &left,
    const NanoVehicleStatus &right
) {
    if (left.valid != right.valid) {
        return false;
    }
    if (!left.valid) {
        return true;
    }
    return left.armed == right.armed &&
        left.has_active_command_id == right.has_active_command_id &&
        left.has_distance_mm == right.has_distance_mm &&
        left.has_heading_mdeg == right.has_heading_mdeg &&
        left.has_line_error_x100 == right.has_line_error_x100 &&
        left.has_progress_percent == right.has_progress_percent &&
        left.has_lap_count == right.has_lap_count &&
        left.has_segment_index == right.has_segment_index &&
        left.active_command_id == right.active_command_id &&
        left.distance_mm == right.distance_mm &&
        left.heading_mdeg == right.heading_mdeg &&
        left.line_error_x100 == right.line_error_x100 &&
        left.progress_percent == right.progress_percent &&
        left.lap_count == right.lap_count &&
        left.segment_index == right.segment_index &&
        strcmp(left.system_state, right.system_state) == 0 &&
        strcmp(left.fault_code, right.fault_code) == 0;
}

void formatInt32(char *buffer, size_t length, bool valid, int32_t value) {
    if (!valid) {
        snprintf(buffer, length, "--");
        return;
    }
    snprintf(buffer, length, "%ld", static_cast<long>(value));
}

void formatUint8(char *buffer, size_t length, bool valid, uint8_t value) {
    if (!valid) {
        snprintf(buffer, length, "--");
        return;
    }
    snprintf(buffer, length, "%u", static_cast<unsigned int>(value));
}

}  // namespace

void TjcDisplay::begin() {
    tx_visual_test_active_ = kRunTjcTxVisualTest;
    if (tx_visual_test_active_) {
        Serial1.begin(
            kGroundStationBaud,
            SERIAL_8N1,
            -1,
            kTjcTxPin
        );

        Serial.println("TJC TX-only visual test started");
        delay(kTjcStartupPageDurationMs);

        Serial1.write(0x00);
        Serial1.write(0xFF);
        Serial1.write(0xFF);
        Serial1.write(0xFF);
        Serial1.flush();
        delay(100);

        showMainPage();
        Serial1.flush();
        delay(500);

        sendCommand("dim=30");
        Serial1.flush();
        Serial.println("TJC visual test: dim=30");
        delay(1000);

        setText(kTjcApTextComponent, "UART OK");
        Serial1.flush();
        Serial.println("TJC visual test: tAp=\"UART OK\"");
        delay(1000);

        sendCommand("dim=100");
        Serial1.flush();
        Serial.println("TJC visual test: dim=100; test complete");
        return;
    }

    Serial1.begin(
        kGroundStationBaud,
        SERIAL_8N1,
        kTjcRxPin,
        kTjcTxPin
    );

    delay(kTjcStartupPageDurationMs);
    while (Serial1.available() > 0) {
        processRxByte(static_cast<uint8_t>(Serial1.read()));
        processRxFrame();
    }

    synchronizeDisplay();

    last_refresh_ms_ = millis();
    last_debug_log_ms_ = last_refresh_ms_;
}

void TjcDisplay::poll() {
    if (tx_visual_test_active_) {
        return;
    }

    bool power_on_frame = false;
    while (Serial1.available() > 0) {
        processRxByte(static_cast<uint8_t>(Serial1.read()));
        if (processRxFrame()) {
            power_on_frame = true;
        }
    }

    const uint32_t now = millis();
    if (
        display_online_
        && now - last_valid_response_ms_ >= kTjcOnlineTimeoutMs
    ) {
        display_online_ = false;
        readback_verified_ = false;
        refresh_pending_ = false;
        Serial.println("TJC transport: RESPONSE_TIMEOUT");
    }

    if (power_on_frame) {
        display_online_ = false;
        readback_verified_ = false;
        refresh_pending_ = false;
        startup_page_wait_active_ = true;
        startup_page_wait_started_ms_ = now;
    } else if (
        startup_page_wait_active_
        && now - startup_page_wait_started_ms_ >= kTjcStartupPageDurationMs
    ) {
        startup_page_wait_active_ = false;
        synchronizeDisplay();
    } else if (
        !startup_page_wait_active_
        && !readback_verified_
        && now - last_probe_ms_ >= kTjcProbeIntervalMs
    ) {
        synchronizeDisplay();
    } else if (
        !startup_page_wait_active_
        && readback_verified_
        && refresh_pending_
    ) {
        refresh_pending_ = false;
        refresh();
        last_refresh_ms_ = now;
    } else if (
        !startup_page_wait_active_
        && readback_verified_
        && now - last_refresh_ms_ >= kTjcRefreshIntervalMs
    ) {
        refresh();
        last_refresh_ms_ = now;
    }
    if (now - last_debug_log_ms_ >= 1000) {
        Serial.printf(
            "TJC debug: online=%s verified=%s powerOn=%s "
            "tx=%lu ok=%lu echo=%lu error=%lu lastError=0x%02X\n",
            display_online_ ? "yes" : "no",
            readback_verified_ ? "yes" : "no",
            power_on_frame_seen_ ? "yes" : "no",
            static_cast<unsigned long>(transmit_count_),
            static_cast<unsigned long>(success_count_),
            static_cast<unsigned long>(echo_count_),
            static_cast<unsigned long>(error_count_),
            last_error_code_
        );
        last_debug_log_ms_ = now;
    }
}

void TjcDisplay::processRxByte(uint8_t byte) {
    if (byte == 0xFF) {
        if (rx_ff_count_ < 3) {
            rx_ff_count_++;
        }
        return;
    }

    while (rx_ff_count_ > 0) {
        if (rx_data_length_ < kRxFrameCapacity) {
            rx_frame_[rx_data_length_++] = 0xFF;
        } else {
            rx_overflow_ = true;
        }
        rx_ff_count_--;
    }

    if (rx_data_length_ < kRxFrameCapacity) {
        rx_frame_[rx_data_length_++] = byte;
    } else {
        rx_overflow_ = true;
    }
}

bool TjcDisplay::processRxFrame() {
    if (rx_ff_count_ != 3) {
        return false;
    }

    const bool overflow = rx_overflow_;
    rx_ff_count_ = 0;
    rx_overflow_ = false;

    if (overflow || rx_data_length_ == 0) {
        rx_data_length_ = 0;
        error_count_++;
        return false;
    }

    const uint8_t type = rx_frame_[0];
    const uint8_t length = rx_data_length_;
    rx_data_length_ = 0;

    if (isRecentTxEcho(length)) {
        echo_count_++;
        if (echo_count_ <= 3 || echo_count_ % 10 == 0) {
            Serial.printf(
                "TJC transport: TX_ECHO count=%lu command=%.*s\n",
                static_cast<unsigned long>(echo_count_),
                static_cast<int>(length),
                reinterpret_cast<const char *>(rx_frame_)
            );
        }
        return false;
    }

    if (length == 1) {
        if (type == 0x01) {
            success_count_++;
            last_valid_response_ms_ = millis();
            return false;
        }
        if (type == 0x88) {
            power_on_frame_seen_ = true;
            Serial.println("TJC transport: POWER_ON_FRAME");
            return true;
        }

        error_count_++;
        last_error_code_ = type;
        return false;
    }

    if (type == 0x55 && length == 4) {
        Serial.printf(
            "TJC custom: type=0x%02X id=%u value=%u\n",
            rx_frame_[1],
            rx_frame_[2],
            rx_frame_[3]
        );
    } else if (type == 0x65 && length == 4) {
        Serial.printf(
            "TJC touch: page=%u component=%u event=%s\n",
            rx_frame_[1],
            rx_frame_[2],
            rx_frame_[3] == 0x01 ? "PRESS" : "RELEASE"
        );
    } else if (type == 0x66 && length == 2) {
        Serial.printf("TJC page: %u\n", rx_frame_[1]);
    } else if (type == 0x70) {
        Serial.printf(
            "TJC string: %.*s\n",
            static_cast<int>(length - 1),
            reinterpret_cast<const char *>(&rx_frame_[1])
        );
        if (
            length == sizeof(kTjcProbeText)
            && memcmp(
                &rx_frame_[1],
                kTjcProbeText,
                sizeof(kTjcProbeText) - 1
            ) == 0
        ) {
            const bool first_verification = !readback_verified_;
            display_online_ = true;
            readback_verified_ = true;
            refresh_pending_ = true;
            last_valid_response_ms_ = millis();
            if (first_verification) {
                Serial.println(
                    "TJC transport: VERIFIED by get tAp.txt"
                );
            }
        }
    } else if (type == 0x71 && length == 5) {
        const uint32_t value =
            static_cast<uint32_t>(rx_frame_[1])
            | (static_cast<uint32_t>(rx_frame_[2]) << 8)
            | (static_cast<uint32_t>(rx_frame_[3]) << 16)
            | (static_cast<uint32_t>(rx_frame_[4]) << 24);
        Serial.printf(
            "TJC number: %lu\n",
            static_cast<unsigned long>(value)
        );
    } else {
        logRxFrame(length);
    }
    return false;
}

void TjcDisplay::logRxFrame(uint8_t length) const {
    Serial.print("TJC RX:");
    for (uint8_t i = 0; i < length; i++) {
        Serial.printf(" %02X", rx_frame_[i]);
    }
    Serial.println();
}

bool TjcDisplay::isRecentTxEcho(uint8_t length) const {
    for (uint8_t i = 0; i < kTxHistoryCapacity; i++) {
        if (
            tx_history_length_[i] == length
            && memcmp(tx_history_[i], rx_frame_, length) == 0
        ) {
            return true;
        }
    }
    return false;
}

void TjcDisplay::rememberTxCommand(const char *command) {
    const size_t length = strlen(command);
    if (length == 0 || length >= kTxCommandCapacity) {
        return;
    }

    memcpy(tx_history_[tx_history_next_], command, length);
    tx_history_[tx_history_next_][length] = '\0';
    tx_history_length_[tx_history_next_] = static_cast<uint8_t>(length);
    tx_history_next_ =
        static_cast<uint8_t>((tx_history_next_ + 1) % kTxHistoryCapacity);
}

void TjcDisplay::showAp(bool ready) {
    if (tx_visual_test_active_) {
        return;
    }

    if (ap_known_ && ap_ready_ == ready) {
        return;
    }
    ap_known_ = true;
    ap_ready_ = ready;
    if (readback_verified_) {
        setText(kTjcApTextComponent, ready ? "READY" : "DOWN");
    }
}

void TjcDisplay::showNano(NanoLinkState state) {
    if (tx_visual_test_active_) {
        return;
    }

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
    if (readback_verified_) {
        setText(kTjcNanoTextComponent, text);
    }
}

void TjcDisplay::showTi(bool online) {
    if (tx_visual_test_active_) {
        return;
    }

    if (ti_known_ && ti_online_ == online) {
        return;
    }
    ti_known_ = true;
    ti_online_ = online;
    if (readback_verified_) {
        setText(kTjcTiTextComponent, online ? "UP" : "DOWN");
    }
}

void TjcDisplay::showVehicleStatus(const NanoVehicleStatus &status) {
    if (tx_visual_test_active_) {
        return;
    }

    if (vehicle_known_ && vehicleStatusEqual(vehicle_status_, status)) {
        return;
    }
    vehicle_known_ = true;
    vehicle_status_ = status;
    if (readback_verified_) {
        writeVehicleStatus(status);
    }
}

void TjcDisplay::refresh() {
    if (ap_known_) {
        setText(kTjcApTextComponent, ap_ready_ ? "READY" : "DOWN");
    }
    if (nano_known_) {
        const char *text = "DOWN";
        if (nano_state_ == NanoLinkState::CONNECTING) {
            text = "CONNECTING";
        } else if (nano_state_ == NanoLinkState::UP) {
            text = "UP";
        } else if (nano_state_ == NanoLinkState::DEGRADED) {
            text = "DEGRADED";
        }
        setText(kTjcNanoTextComponent, text);
    }
    if (ti_known_) {
        setText(kTjcTiTextComponent, ti_online_ ? "UP" : "DOWN");
    }
    if (vehicle_known_) {
        writeVehicleStatus(vehicle_status_);
    }
}

void TjcDisplay::synchronizeDisplay() {
    enableCommandFeedback();
    showMainPage();
    Serial1.flush();
    delay(100);
    probeDisplay();
}

void TjcDisplay::probeDisplay() {
    setText(kTjcApTextComponent, kTjcProbeText);

    char command[48];
    const int command_length = snprintf(
        command,
        sizeof(command),
        "get %s.txt",
        kTjcApTextComponent
    );
    if (
        command_length > 0
        && command_length < static_cast<int>(sizeof(command))
    ) {
        sendCommand(command);
    }
    Serial1.flush();
    last_probe_ms_ = millis();
}

void TjcDisplay::enableCommandFeedback() {
    static const char command[] = "bkcmd=3";
    sendCommand(command);
}

void TjcDisplay::showMainPage() {
    char command[32];
    const int command_length = snprintf(
        command,
        sizeof(command),
        "page %s",
        kTjcMainPage
    );
    if (
        command_length <= 0
        || command_length >= static_cast<int>(sizeof(command))
    ) {
        return;
    }
    sendCommand(command);
}

void TjcDisplay::sendCommand(const char *command) {
    rememberTxCommand(command);
    Serial1.print(command);
    Serial1.write(0xFF);
    Serial1.write(0xFF);
    Serial1.write(0xFF);
    transmit_count_++;
}

void TjcDisplay::writeVehicleStatus(const NanoVehicleStatus &status) {
    if (!status.valid) {
        setText(kTjcSystemTextComponent, "--");
        setText(kTjcArmedTextComponent, "--");
        setText(kTjcFaultTextComponent, "--");
        setText(kTjcDistanceTextComponent, "--");
        setText(kTjcHeadingTextComponent, "--");
        setText(kTjcLineErrorTextComponent, "--");
        setText(kTjcLeftSpeedTextComponent, "--");
        setText(kTjcRightSpeedTextComponent, "--");
        setText(kTjcProgressTextComponent, "--");
        setText(kTjcLapCountTextComponent, "--");
        setText(kTjcSegmentTextComponent, "--");
        return;
    }

    char value[24];
    setText(kTjcSystemTextComponent, status.system_state);
    setText(kTjcArmedTextComponent, status.armed ? "YES" : "NO");
    setText(kTjcFaultTextComponent, status.fault_code);

    formatInt32(value, sizeof(value), status.has_distance_mm, status.distance_mm);
    setText(kTjcDistanceTextComponent, value);
    formatInt32(value, sizeof(value), status.has_heading_mdeg, status.heading_mdeg);
    setText(kTjcHeadingTextComponent, value);
    formatInt32(
        value,
        sizeof(value),
        status.has_line_error_x100,
        status.line_error_x100
    );
    setText(kTjcLineErrorTextComponent, value);

    /* MSPM0 V1.1 STATUS does not contain wheel speed fields. */
    setText(kTjcLeftSpeedTextComponent, "--");
    setText(kTjcRightSpeedTextComponent, "--");

    formatUint8(
        value,
        sizeof(value),
        status.has_progress_percent,
        status.progress_percent
    );
    setText(kTjcProgressTextComponent, value);
    formatUint8(value, sizeof(value), status.has_lap_count, status.lap_count);
    setText(kTjcLapCountTextComponent, value);
    formatUint8(value, sizeof(value), status.has_segment_index, status.segment_index);
    setText(kTjcSegmentTextComponent, value);
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
    sendCommand(command);
}
