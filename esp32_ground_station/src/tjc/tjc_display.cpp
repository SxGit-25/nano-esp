#include "tjc_display.h"

#include <stdio.h>

#include "app_config.h"

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
        Serial.println("TJC visual test: main.tAp=\"UART OK\"");
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
    delay(100);

    last_refresh_ms_ = millis();
    last_debug_log_ms_ = last_refresh_ms_;
}

void TjcDisplay::poll() {
    if (tx_visual_test_active_) {
        return;
    }

    bool display_ready = false;
    while (Serial1.available() > 0) {
        processRxByte(static_cast<uint8_t>(Serial1.read()));
        if (processRxFrame()) {
            display_ready = true;
        }
    }

    const uint32_t now = millis();
    if (display_ready) {
        startup_page_wait_active_ = true;
        startup_page_wait_started_ms_ = now;
    } else if (
        startup_page_wait_active_
        && now - startup_page_wait_started_ms_ >= kTjcStartupPageDurationMs
    ) {
        startup_page_wait_active_ = false;
        synchronizeDisplay();
        refresh();
        last_refresh_ms_ = now;
    } else if (
        !startup_page_wait_active_
        && now - last_refresh_ms_ >= kTjcRefreshIntervalMs
    ) {
        refresh();
        last_refresh_ms_ = now;
    }
    if (now - last_debug_log_ms_ >= 1000) {
        Serial.printf(
            "TJC debug: ready=%s tx=%lu ok=%lu error=%lu lastError=0x%02X\n",
            display_ready_seen_ ? "yes" : "no",
            static_cast<unsigned long>(transmit_count_),
            static_cast<unsigned long>(success_count_),
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

    if (length == 1) {
        if (type == 0x01) {
            success_count_++;
            return false;
        }
        if (type == 0x88) {
            display_ready_seen_ = true;
            return true;
        }
        if (type == 0x00 && sync_response_pending_) {
            sync_response_pending_ = false;
            return false;
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

void TjcDisplay::showAp(bool ready) {
    if (tx_visual_test_active_) {
        return;
    }

    if (ap_known_ && ap_ready_ == ready) {
        return;
    }
    ap_known_ = true;
    ap_ready_ = ready;
    setText(kTjcApTextComponent, ready ? "READY" : "DOWN");
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
    setText(kTjcNanoTextComponent, text);
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
    setText(kTjcTiTextComponent, online ? "UP" : "DOWN");
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
}

void TjcDisplay::synchronizeDisplay() {
    sync_response_pending_ = true;
    Serial1.write(0x00);
    Serial1.write(0xFF);
    Serial1.write(0xFF);
    Serial1.write(0xFF);
    Serial1.flush();

    enableCommandFeedback();
    showMainPage();
    Serial1.flush();
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
    Serial1.print(command);
    Serial1.write(0xFF);
    Serial1.write(0xFF);
    Serial1.write(0xFF);
    transmit_count_++;
}

void TjcDisplay::setText(const char *component, const char *text) {
    char command[80];
    const int command_length = snprintf(
        command,
        sizeof(command),
        "%s.%s.txt=\"%s\"",
        kTjcMainPage,
        component,
        text
    );
    if (command_length <= 0 || command_length >= static_cast<int>(sizeof(command))) {
        return;
    }
    sendCommand(command);
}
