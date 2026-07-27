#include "tjc_display.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"

namespace {

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
        left.has_control_enabled == right.has_control_enabled &&
        left.control_enabled == right.control_enabled &&
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

bool tokenEqual(
    const uint8_t *frame,
    size_t frame_length,
    const char *token
) {
    const size_t token_length = strlen(token);
    return frame_length == token_length &&
        memcmp(frame, token, token_length) == 0;
}

}  // namespace

void TjcDisplay::begin() {
    event_reader_.reset();
    Serial1.begin(
        kGroundStationBaud,
        SERIAL_8N1,
        kTjcRxPin,
        kTjcTxPin
    );

    last_refresh_ms_ = millis();
}

void TjcDisplay::poll() {
    const uint32_t now = millis();
    if (now - last_refresh_ms_ >= kTjcRefreshIntervalMs) {
        refresh();
        last_refresh_ms_ = now;
    }
}

bool TjcDisplay::pollEvent(TjcInputEvent &event) {
    size_t processed = 0;
    while (
        Serial1.available() > 0 &&
        processed < kTjcMaxInputBytesPerPoll
    ) {
        const int next = Serial1.read();
        if (next < 0) {
            break;
        }
        ++processed;
        const TjcEventReader::Result result =
            event_reader_.append(static_cast<uint8_t>(next));
        if (result == TjcEventReader::Result::FRAME) {
            if (decodeEvent(
                event_reader_.frame(),
                event_reader_.frameLength(),
                event
            )) {
                return true;
            }
        }
    }
    return false;
}

void TjcDisplay::forceRefresh() {
    refresh();
    last_refresh_ms_ = millis();
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

void TjcDisplay::showVehicleStatus(const NanoVehicleStatus &status) {
    if (vehicle_known_ && vehicleStatusEqual(vehicle_status_, status)) {
        return;
    }
    vehicle_known_ = true;
    vehicle_status_ = status;
    writeVehicleStatus(status);
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

void TjcDisplay::sendCommand(const char *command) {
    Serial1.print(command);
    Serial1.write(0xFF);
    Serial1.write(0xFF);
    Serial1.write(0xFF);
}

void TjcDisplay::writeVehicleStatus(const NanoVehicleStatus &status) {
    /*
     * The current safety page exposes only tFault. Do not emit fields which
     * are absent from the HMI: a full status refresh can overflow its UART RX
     * buffer and produces TJC error 0x24.
     */
    if (!status.valid) {
        setText(kTjcFaultTextComponent, "--");
        return;
    }

    setText(kTjcFaultTextComponent, status.fault_code);
}

void TjcDisplay::showCommandResult(const NanoCommandResult &result) {
    /* Command-result controls are not present in the current HMI revision. */
    (void)result;
}

void TjcDisplay::showInputReceipt(const TjcInputEvent &event) {
    const char *text = "RX:SYNC";
    if (event.type == TjcInputEventType::COMMAND) {
        switch (event.command) {
            case GroundStationCommand::ARM:
                text = "RX:ARM";
                break;
            case GroundStationCommand::DRIVE_FORWARD_500:
                text = "RX:FWD_500";
                break;
            case GroundStationCommand::DRIVE_BACKWARD_500:
                text = "RX:BACK_500";
                break;
            case GroundStationCommand::TURN_LEFT_90:
                text = "RX:LEFT_90";
                break;
            case GroundStationCommand::TURN_RIGHT_90:
                text = "RX:RIGHT_90";
                break;
            case GroundStationCommand::STOP:
                text = "RX:STOP";
                break;
            default:
                return;
        }
    }
    setText(kTjcRxTextComponent, text);
}

bool TjcDisplay::decodeEvent(
    const uint8_t *frame,
    size_t length,
    TjcInputEvent &event
) const {
    event.type = TjcInputEventType::COMMAND;
    if (tokenEqual(frame, length, kTjcArmEventToken)) {
        event.command = GroundStationCommand::ARM;
    } else if (tokenEqual(frame, length, kTjcDisarmEventToken)) {
        event.command = GroundStationCommand::DISARM;
    } else if (tokenEqual(frame, length, kTjcStopEventToken)) {
        event.command = GroundStationCommand::STOP;
    } else if (tokenEqual(frame, length, kTjcEstopEventToken)) {
        event.command = GroundStationCommand::ESTOP;
    } else if (tokenEqual(frame, length, kTjcClearFaultEventToken)) {
        event.command = GroundStationCommand::CLEAR_FAULT;
    } else if (tokenEqual(frame, length, kTjcGetStatusEventToken)) {
        event.command = GroundStationCommand::GET_STATUS;
    } else if (tokenEqual(frame, length, kTjcForward500EventToken)) {
        event.command = GroundStationCommand::DRIVE_FORWARD_500;
    } else if (tokenEqual(frame, length, kTjcBackward500EventToken)) {
        event.command = GroundStationCommand::DRIVE_BACKWARD_500;
    } else if (tokenEqual(frame, length, kTjcLeft90EventToken)) {
        event.command = GroundStationCommand::TURN_LEFT_90;
    } else if (tokenEqual(frame, length, kTjcRight90EventToken)) {
        event.command = GroundStationCommand::TURN_RIGHT_90;
    } else if (tokenEqual(frame, length, kTjcSyncEventToken)) {
        event.type = TjcInputEventType::SYNC;
    } else {
        return false;
    }
    return true;
}

void TjcDisplay::setText(const char *component, const char *text) {
    char sanitized[64];
    size_t sanitized_length = 0;
    while (
        text[sanitized_length] != '\0' &&
        sanitized_length < sizeof(sanitized) - 1
    ) {
        const uint8_t byte =
            static_cast<uint8_t>(text[sanitized_length]);
        sanitized[sanitized_length] =
            byte >= 0x20 && byte <= 0x7E &&
                byte != '"' && byte != '\\'
            ? static_cast<char>(byte)
            : '?';
        ++sanitized_length;
    }
    sanitized[sanitized_length] = '\0';

    char command[112];
    const int command_length = snprintf(
        command,
        sizeof(command),
        "%s.txt=\"%s\"",
        component,
        sanitized
    );
    if (command_length <= 0 || command_length >= static_cast<int>(sizeof(command))) {
        return;
    }
    sendCommand(command);
}
