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

void TjcDisplay::showVehicleStatus(const NanoVehicleStatus &status) {
    if (vehicle_known_ && vehicleStatusEqual(vehicle_status_, status)) {
        return;
    }
    vehicle_known_ = true;
    vehicle_status_ = status;

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
    Serial1.write(reinterpret_cast<const uint8_t *>(command), command_length);
    Serial1.write(0xFF);
    Serial1.write(0xFF);
    Serial1.write(0xFF);
}
