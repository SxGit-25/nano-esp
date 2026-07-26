"""Single-owner read-only worker for the Nano--MSPM0 serial link."""

import importlib
import logging
import os
import sys
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


def load_car_serial_link(mspm0_link_directory):
    """Load CarSerialLink from the existing sibling mspm0_link project."""

    directory = os.path.abspath(mspm0_link_directory)
    module_path = os.path.join(directory, "serial_link.py")
    if not os.path.isfile(module_path):
        raise RuntimeError(
            "serial_link.py was not found in {}".format(directory)
        )
    if directory not in sys.path:
        sys.path.insert(0, directory)
    module = importlib.import_module("serial_link")
    return module.CarSerialLink


class CarSerialWorker(object):
    """The only thread allowed to instantiate or call CarSerialLink."""

    def __init__(
        self,
        port,
        link_factory,
        status_callback,
        heartbeat_interval_s=0.1,
        status_interval_s=1.0,
        reconnect_delay_s=1.0,
        response_timeout_s=0.3,
        logger=None,
    ):
        if not port:
            raise ValueError("port must be provided")
        if heartbeat_interval_s <= 0 or status_interval_s <= 0:
            raise ValueError("serial intervals must be positive")
        if reconnect_delay_s <= 0 or response_timeout_s <= 0:
            raise ValueError("serial timeouts must be positive")

        self.port = port
        self.link_factory = link_factory
        self.status_callback = status_callback
        self.heartbeat_interval_s = heartbeat_interval_s
        self.status_interval_s = status_interval_s
        self.reconnect_delay_s = reconnect_delay_s
        self.response_timeout_s = response_timeout_s
        self.logger = logger or logging.getLogger(__name__)

        self._started_at = None
        self._link_online = False
        self._hello = None

    def run(self, stop_event):
        """Maintain the read-only serial session until stop_event is set."""

        self._started_at = time.monotonic()
        self._publish_disconnected()

        while not stop_event.is_set():
            link = None
            try:
                link = self.link_factory(
                    port=self.port,
                    response_timeout=self.response_timeout_s,
                )
                link.open()
                hello, hello_elapsed = link.hello()
                status, _ = link.get_status()
                self._hello = hello
                self._set_link_online(True)
                self.logger.info(
                    "MSPM0 link ready at %s; bootId=%s, helloRttMs=%s",
                    self.port,
                    hello["boot_id"],
                    int(hello_elapsed * 1000.0),
                )
                self._publish_status(status)
                self._service_link(link, stop_event)
            except Exception as error:
                if not stop_event.is_set():
                    self.logger.warning("MSPM0 serial link closed: %s", error)
            finally:
                if link is not None:
                    try:
                        link.close()
                    except Exception:
                        pass
                self._hello = None
                self._set_link_online(False)

            if not stop_event.is_set():
                stop_event.wait(self.reconnect_delay_s)

    def _service_link(self, link, stop_event):
        now = time.monotonic()
        next_heartbeat = now
        next_status = now + self.status_interval_s

        while not stop_event.is_set():
            now = time.monotonic()
            if now >= next_heartbeat:
                link.heartbeat(self._uptime_ms(now))
                next_heartbeat = now + self.heartbeat_interval_s

            now = time.monotonic()
            if now >= next_status:
                status, _ = link.get_status()
                self._publish_status(status)
                next_status = now + self.status_interval_s

            deadline = min(next_heartbeat, next_status)
            delay = deadline - time.monotonic()
            if delay > 0:
                stop_event.wait(min(delay, 0.02))

    def _set_link_online(self, online):
        changed = self._link_online != online
        self._link_online = online
        if changed and not online:
            self._publish_disconnected()

    def _publish_disconnected(self):
        self.status_callback(
            {
                "v": 1,
                "type": "status",
                "timestampMs": self._uptime_ms(),
                "systemState": "DISCONNECTED",
                "armed": False,
                "activeCommandId": None,
                "activeTaskId": None,
                "links": {
                    "nanoTi": "DOWN",
                },
            }
        )

    def _publish_status(self, status):
        system_state = status["system_state"]
        transaction_state = status["transaction_state"]
        fault_code = status["latched_fault_code"]
        active_command_id = status["reported_command_id"] or None
        hello = self._hello or {}

        self.status_callback(
            {
                "v": 1,
                "type": "status",
                "timestampMs": self._uptime_ms(),
                "systemState": SYSTEM_STATE_NAMES.get(
                    system_state,
                    "UNKNOWN_{}".format(system_state),
                ),
                "armed": system_state in (2, 3, 4),
                "activeCommandId": active_command_id,
                "activeTaskId": None,
                "faultCode": FAULT_CODE_NAMES.get(
                    fault_code,
                    "UNKNOWN_{}".format(fault_code),
                ),
                "transactionState": TRANSACTION_STATE_NAMES.get(
                    transaction_state,
                    "UNKNOWN_{}".format(transaction_state),
                ),
                "motionType": status["motion_type"],
                "progressPercent": status["progress_percent"],
                "lineErrorX100": status["line_error_x100"],
                "distanceMm": status["distance_mm"],
                "headingMdeg": status["heading_mdeg"],
                "elapsedMs": status["elapsed_ms"],
                "lastProtocolError": status["last_protocol_error"],
                "lapCount": status["lap_count"],
                "segmentIndex": status["segment_index"],
                "tiBootId": hello.get("boot_id"),
                "tiCapabilityFlags": hello.get("capability_flags"),
                "links": {
                    "nanoTi": "UP",
                },
            }
        )

    def _uptime_ms(self, now=None):
        if self._started_at is None:
            return 0
        current = now if now is not None else time.monotonic()
        return int((current - self._started_at) * 1000.0) & 0xFFFFFFFF
