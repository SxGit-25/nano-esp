"""Thread-safe source of Nano gateway snapshot and status messages."""

import threading
import time


SYSTEM_STATE_NAMES = {
    0: "IDLE",
    1: "CALIBRATING",
    2: "ARMED",
    3: "EXECUTING",
    4: "EXECUTING",
    5: "SAFE_STOP",
    6: "FAULT",
}

TRANSACTION_STATE_NAMES = {
    0: "NONE",
    1: "ACCEPTED",
    2: "RUNNING",
    3: "CANCELLING",
    4: "DONE",
    5: "FAILED",
}

FAULT_CODE_NAMES = {
    0: "NONE",
    1: "COMMAND_TIMEOUT",
    2: "CANCEL_STOP_TIMEOUT",
    3: "LINK_LOST",
    4: "LINE_DATA_INVALID",
    5: "LINE_LOST",
    6: "IMU_INVALID",
    7: "IMU_CALIBRATION_FAILED",
    8: "IMU_DISCONTINUITY",
    9: "SENSOR_NOT_READY",
    10: "ENCODER_LEFT_NO_MOTION",
    11: "ENCODER_RIGHT_NO_MOTION",
    12: "ENCODER_WRONG_DIRECTION",
    13: "CONTROL_DEADLINE_MISSED",
    14: "RX_OVERFLOW",
    15: "EMERGENCY_STOPPED",
}


class StateStore(object):
    """Own all state used to build outward-facing JSON messages."""

    def __init__(self, clock=None):
        self._clock = clock or time.monotonic
        self._started_at = self._clock()
        self._lock = threading.Lock()
        self._revision = 1
        self._esp_nano_link = "DOWN"
        self._reset_ti_state()

    def set_esp_nano_link(self, state):
        if state not in ("UP", "DEGRADED", "DOWN"):
            raise ValueError("invalid ESP--Nano link state")
        with self._lock:
            if self._esp_nano_link != state:
                self._esp_nano_link = state
                self._revision += 1

    def mark_ti_disconnected(self):
        with self._lock:
            if self._nano_ti_link == "DOWN" and self._system_state == "DISCONNECTED":
                return
            self._reset_ti_state()
            self._revision += 1

    def update_from_mspm0(self, status, hello):
        """Replace the vehicle state from one decoded MSPM0 STATUS frame."""

        system_state = status["system_state"]
        transaction_state = status["transaction_state"]
        fault_code = status["latched_fault_code"]
        with self._lock:
            self._nano_ti_link = "UP"
            self._system_state = SYSTEM_STATE_NAMES.get(
                system_state,
                "UNKNOWN_{}".format(system_state),
            )
            self._armed = system_state in (2, 3, 4)
            self._active_command_id = status["reported_command_id"] or None
            self._fault_code = FAULT_CODE_NAMES.get(
                fault_code,
                "UNKNOWN_{}".format(fault_code),
            )
            self._transaction_state = TRANSACTION_STATE_NAMES.get(
                transaction_state,
                "UNKNOWN_{}".format(transaction_state),
            )
            self._motion_type = status["motion_type"]
            self._progress_percent = status["progress_percent"]
            self._line_error_x100 = status["line_error_x100"]
            self._distance_mm = status["distance_mm"]
            self._heading_mdeg = status["heading_mdeg"]
            self._elapsed_ms = status["elapsed_ms"]
            self._last_protocol_error = status["last_protocol_error"]
            self._lap_count = status["lap_count"]
            self._segment_index = status["segment_index"]
            self._ti_boot_id = hello.get("boot_id")
            self._ti_capability_flags = hello.get("capability_flags")
            self._revision += 1

    @property
    def ti_online(self):
        with self._lock:
            return self._nano_ti_link == "UP"

    def build_message(self, message_type):
        if message_type not in ("snapshot", "status"):
            raise ValueError("message_type must be snapshot or status")
        with self._lock:
            revision = self._revision
            message = {
                "v": 1,
                "type": message_type,
                "timestampMs": self._uptime_ms(),
                "systemState": self._system_state,
                "armed": self._armed,
                "activeCommandId": self._active_command_id,
                "activeTaskId": None,
                "faultCode": self._fault_code,
                "transactionState": self._transaction_state,
                "motionType": self._motion_type,
                "progressPercent": self._progress_percent,
                "lineErrorX100": self._line_error_x100,
                "distanceMm": self._distance_mm,
                "headingMdeg": self._heading_mdeg,
                "leftWheelSpeedMmPerSec": None,
                "rightWheelSpeedMmPerSec": None,
                "elapsedMs": self._elapsed_ms,
                "lastProtocolError": self._last_protocol_error,
                "lapCount": self._lap_count,
                "segmentIndex": self._segment_index,
                "tiBootId": self._ti_boot_id,
                "tiCapabilityFlags": self._ti_capability_flags,
                "links": {
                    "espNano": self._esp_nano_link,
                    "nanoTi": self._nano_ti_link,
                },
            }
        return revision, message

    def _reset_ti_state(self):
        self._nano_ti_link = "DOWN"
        self._system_state = "DISCONNECTED"
        self._armed = False
        self._active_command_id = None
        self._fault_code = "NONE"
        self._transaction_state = "NONE"
        self._motion_type = None
        self._progress_percent = None
        self._line_error_x100 = None
        self._distance_mm = None
        self._heading_mdeg = None
        self._elapsed_ms = None
        self._last_protocol_error = None
        self._lap_count = None
        self._segment_index = None
        self._ti_boot_id = None
        self._ti_capability_flags = None

    def _uptime_ms(self):
        return int((self._clock() - self._started_at) * 1000.0) & 0xFFFFFFFF
