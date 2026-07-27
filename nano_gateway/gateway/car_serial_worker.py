"""Single-owner worker for the Nano--MSPM0 V1.1 serial link."""

import importlib
import logging
import os
import struct
import sys
import time

from gateway.command_dispatcher import CommandDispatcher
from gateway.state_store import FAULT_CODE_NAMES


TYPE_ARM = 0x02
TYPE_CANCEL = 0x03
TYPE_EMERGENCY_STOP = 0x04
TYPE_CLEAR_FAULT = 0x05
TYPE_DRIVE_DISTANCE = 0x10
TYPE_TURN_RELATIVE = 0x12

COMPLETION_REASONS = {
    1: "TARGET_REACHED",
    3: "ARMED_READY",
    4: "FAULT_CLEARED",
    5: "EMERGENCY_STOP_APPLIED",
    6: "CANCEL_COMPLETED",
}

FAILURE_REASONS = {
    0: "UNSPECIFIED",
    1: "DEVICE_TIMEOUT",
    2: "USER_CANCELLED",
    3: "CALIBRATION_FAILED",
    4: "SAFETY_FAULT",
    5: "LINK_LOST",
    6: "CANCEL_TIMEOUT",
    7: "INVALID_SENSOR",
}

TERMINAL_DEADLINES_S = {
    TYPE_ARM: 4.5,
    TYPE_CANCEL: 2.5,
    TYPE_EMERGENCY_STOP: 1.5,
    TYPE_CLEAR_FAULT: 1.5,
    TYPE_DRIVE_DISTANCE: 8.0,
    TYPE_TURN_RELATIVE: 7.0,
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


class _AsyncLinkAdapter(object):
    """Expose V1.1 control transactions without blocking on their terminal."""

    def __init__(self, link):
        self.link = link

    def begin_arm(self):
        return self.link.begin_arm()

    def begin_cancel(self, target_command_id):
        if hasattr(self.link, "begin_cancel"):
            return self.link.begin_cancel(target_command_id)
        command_id = self.link._next_command_id()
        payload = struct.pack(
            "<IIII",
            self.link.session_id,
            command_id,
            target_command_id,
            1000,
        )
        ack, elapsed = self.link._submit_command(
            TYPE_CANCEL,
            command_id,
            payload,
        )
        return command_id, ack, elapsed

    def begin_emergency_stop(self):
        if hasattr(self.link, "begin_emergency_stop"):
            return self.link.begin_emergency_stop()
        command_id = self.link._next_command_id()
        payload = struct.pack(
            "<II",
            self.link.session_id,
            command_id,
        )
        ack, elapsed = self.link._submit_command(
            TYPE_EMERGENCY_STOP,
            command_id,
            payload,
        )
        return command_id, ack, elapsed

    def begin_clear_fault(self):
        if hasattr(self.link, "begin_clear_fault"):
            return self.link.begin_clear_fault()
        command_id = self.link._next_command_id()
        payload = struct.pack(
            "<II",
            self.link.session_id,
            command_id,
        )
        ack, elapsed = self.link._submit_command(
            TYPE_CLEAR_FAULT,
            command_id,
            payload,
        )
        return command_id, ack, elapsed

    def begin_drive_distance(
        self,
        distance_mm,
        speed_mm_per_sec,
        heading_mode,
        end_behavior,
        device_timeout_ms,
    ):
        return self.link.begin_drive_distance(
            distance_mm,
            speed_mm_per_sec,
            heading_mode,
            end_behavior,
            device_timeout_ms,
        )

    def begin_turn_relative(
        self,
        angle_mdeg,
        max_wheel_speed_mm_per_sec,
        turn_mode,
        device_timeout_ms,
    ):
        return self.link.begin_turn_relative(
            angle_mdeg,
            max_wheel_speed_mm_per_sec,
            turn_mode,
            device_timeout_ms,
        )

    def take_terminal(self, command_id, request_type):
        if hasattr(self.link, "take_terminal"):
            return self.link.take_terminal(command_id, request_type)
        return self.link._take_terminal(command_id, request_type)

    def pump(self, timeout_s):
        """Read at most one terminal frame while retaining UART ownership."""

        if hasattr(self.link, "pump"):
            self.link.pump(timeout_s)
            return
        if not hasattr(self.link, "_receive_frame"):
            return
        try:
            frame = self.link._receive_frame(time.monotonic() + timeout_s)
        except Exception as error:
            if error.__class__.__name__ == "LinkTimeout":
                return
            raise
        if frame.flags & 0x01:
            self.link._unsolicited_frames.append(frame)


class CarSerialWorker(object):
    """The only thread allowed to instantiate or call CarSerialLink."""

    def __init__(
        self,
        port,
        link_factory,
        state_store,
        command_dispatcher=None,
        heartbeat_interval_s=0.1,
        status_interval_s=1.0,
        reconnect_delay_s=1.0,
        response_timeout_s=0.3,
        command_poll_interval_s=0.02,
        logger=None,
    ):
        if not port:
            raise ValueError("port must be provided")
        if heartbeat_interval_s <= 0 or status_interval_s <= 0:
            raise ValueError("serial intervals must be positive")
        if reconnect_delay_s <= 0 or response_timeout_s <= 0:
            raise ValueError("serial timeouts must be positive")
        if command_poll_interval_s <= 0:
            raise ValueError("command_poll_interval_s must be positive")

        self.port = port
        self.link_factory = link_factory
        self.state_store = state_store
        self.command_dispatcher = (
            command_dispatcher or CommandDispatcher(state_store)
        )
        self.heartbeat_interval_s = heartbeat_interval_s
        self.status_interval_s = status_interval_s
        self.reconnect_delay_s = reconnect_delay_s
        self.response_timeout_s = response_timeout_s
        self.command_poll_interval_s = command_poll_interval_s
        self.logger = logger or logging.getLogger(__name__)

        self._started_at = None
        self._link_online = False
        self._hello = None
        self._pending = {}
        self._arm_enable_key = None

    def run(self, stop_event):
        """Maintain the serial session and execute queued commands."""

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
                self.state_store.update_from_mspm0(status, hello)
                self._service_link(_AsyncLinkAdapter(link), stop_event)
            except Exception as error:
                if not stop_event.is_set():
                    self.logger.warning("MSPM0 serial link closed: %s", error)
            finally:
                self._fail_pending_for_link_loss()
                if link is not None:
                    try:
                        link.close()
                    except Exception:
                        pass
                self._hello = None
                self._set_link_online(False)

            if not stop_event.is_set():
                self._service_offline_queue(stop_event)

    def _service_link(self, adapter, stop_event):
        now = time.monotonic()
        next_heartbeat = now
        next_status = now + self.status_interval_s

        while not stop_event.is_set():
            self._expire_pending()
            self._process_one_request(adapter)
            terminal_seen = self._collect_terminals(adapter)

            now = time.monotonic()
            if now >= next_heartbeat:
                adapter.link.heartbeat(self._uptime_ms(now))
                next_heartbeat = now + self.heartbeat_interval_s
                terminal_seen = self._collect_terminals(adapter) or terminal_seen

            now = time.monotonic()
            if terminal_seen:
                next_status = now
            if now >= next_status:
                status, _ = adapter.link.get_status()
                self.state_store.update_from_mspm0(status, self._hello)
                next_status = now + self.status_interval_s
                self._collect_terminals(adapter)

            delay = min(
                self.command_poll_interval_s,
                max(
                    0.0,
                    min(next_heartbeat, next_status) - time.monotonic(),
                ),
            )
            if self._pending and delay > 0:
                adapter.pump(delay)
                self._collect_terminals(adapter)
            elif delay > 0:
                stop_event.wait(delay)

    def _process_one_request(self, adapter):
        request = self.command_dispatcher.get_request_nowait()
        if request is None:
            return
        if request.get("kind") == "disconnect":
            self._handle_disconnect(adapter, request)
            return
        if not self.command_dispatcher.is_open(request):
            return

        command = request["cmd"]
        if command == "arm":
            self._start_arm(adapter, request)
        elif command == "clear_fault":
            self._start_clear_fault(adapter, request)
        elif command in ("stop", "disarm"):
            self._start_cancel(adapter, request)
        elif command == "estop":
            self._start_estop(adapter, request)
        elif command == "drive_distance":
            self._start_drive_distance(adapter, request)
        elif command == "turn_relative":
            self._start_turn_relative(adapter, request)

    def _start_arm(self, adapter, request):
        state = self.state_store.get_control_state()
        if not state["tiOnline"]:
            self._reject_offline(request)
            return
        if state["systemState"] in ("SAFE_STOP", "FAULT") or (
            state["faultCode"] != "NONE"
        ):
            self.command_dispatcher.rejected(
                request,
                "FAULT_ACTIVE",
                "clear the MSPM0 fault before arming",
            )
            return
        if self._has_main_transaction():
            self.command_dispatcher.rejected(
                request,
                "BUSY",
                "another MSPM0 main transaction is active",
            )
            return
        self.state_store.set_control_enabled(False)
        if self._begin_request(
            adapter,
            request,
            "arm",
            TYPE_ARM,
            adapter.begin_arm,
            "CALIBRATING",
            role="main",
        ):
            self._arm_enable_key = self._request_key(request)
        else:
            self._arm_enable_key = None

    def _start_clear_fault(self, adapter, request):
        if self._has_main_transaction():
            self.command_dispatcher.rejected(
                request,
                "BUSY",
                "another MSPM0 main transaction is active",
            )
            return
        self.state_store.set_control_enabled(False)
        self._arm_enable_key = None
        self._begin_request(
            adapter,
            request,
            "clear_fault",
            TYPE_CLEAR_FAULT,
            adapter.begin_clear_fault,
            "RECOVERING",
            role="main",
        )

    def _start_drive_distance(self, adapter, request):
        state = self.state_store.get_control_state()
        if not state["tiOnline"]:
            self._reject_offline(request)
            return
        if not state["controlEnabled"]:
            self.command_dispatcher.rejected(
                request,
                "NOT_ARMED",
                "arm the MSPM0 before driving",
            )
            return
        if self._has_main_transaction():
            self.command_dispatcher.rejected(
                request,
                "BUSY",
                "another MSPM0 main transaction is active",
            )
            return
        args = request["args"]
        self._begin_request(
            adapter,
            request,
            "drive_distance",
            TYPE_DRIVE_DISTANCE,
            lambda: adapter.begin_drive_distance(
                args["distanceMm"],
                args["speedMmPerSec"],
                args["headingMode"],
                args["endBehavior"],
                args["timeoutMs"],
            ),
            "EXECUTING",
            role="main",
        )

    def _start_turn_relative(self, adapter, request):
        state = self.state_store.get_control_state()
        if not state["tiOnline"]:
            self._reject_offline(request)
            return
        if not state["controlEnabled"]:
            self.command_dispatcher.rejected(
                request,
                "NOT_ARMED",
                "arm the MSPM0 before turning",
            )
            return
        if self._has_main_transaction():
            self.command_dispatcher.rejected(
                request,
                "BUSY",
                "another MSPM0 main transaction is active",
            )
            return
        args = request["args"]
        self._begin_request(
            adapter,
            request,
            "turn_relative",
            TYPE_TURN_RELATIVE,
            lambda: adapter.begin_turn_relative(
                args["angleMdeg"],
                args["maxWheelSpeedMmPerSec"],
                args["turnMode"],
                args["timeoutMs"],
            ),
            "EXECUTING",
            role="main",
        )

    def _start_cancel(self, adapter, request):
        if self._has_cancel_transaction():
            self.command_dispatcher.rejected(
                request,
                "BUSY",
                "a cancellation is already in progress",
            )
            return
        target_command_id = self._active_target_command_id()
        if target_command_id is None:
            reason = "DISARMED" if request["cmd"] == "disarm" else "STOPPED"
            self.command_dispatcher.done(request, reason)
            return
        self._arm_enable_key = None
        self._begin_request(
            adapter,
            request,
            request["cmd"],
            TYPE_CANCEL,
            lambda: adapter.begin_cancel(target_command_id),
            "CANCELLING",
            role="cancel",
            target_command_id=target_command_id,
        )

    def _start_estop(self, adapter, request):
        self.state_store.set_control_enabled(False)
        self._arm_enable_key = None
        self._begin_request(
            adapter,
            request,
            "estop",
            TYPE_EMERGENCY_STOP,
            adapter.begin_emergency_stop,
            "SAFE_STOP",
            role="estop",
        )

    def _begin_request(
        self,
        adapter,
        request,
        operation,
        request_type,
        begin,
        accepted_state,
        role,
        target_command_id=None,
    ):
        try:
            car_command_id, _, _ = begin()
        except Exception as error:
            self._reject_serial_error(request, error)
            return False

        self._pending[car_command_id] = {
            "request": request,
            "operation": operation,
            "request_type": request_type,
            "role": role,
            "target_command_id": target_command_id,
            "deadline": (
                time.monotonic() + TERMINAL_DEADLINES_S[request_type]
            ),
        }
        self.state_store.map_active_command(request["id"], car_command_id)
        self.command_dispatcher.accepted(
            request,
            accepted_state,
            car_command_id,
        )
        self.logger.info(
            "MSPM0 accepted command: session=%s id=%s cmd=%s carCommandId=%s",
            request["groundStationSessionId"],
            request["id"],
            request["cmd"],
            car_command_id,
        )
        return True

    def _handle_disconnect(self, adapter, event):
        session_id = event["groundStationSessionId"]
        self.state_store.set_control_enabled(False)
        self._arm_enable_key = None
        target_command_id = None
        for car_command_id, context in list(self._pending.items()):
            request = context["request"]
            if (
                request is None
                or (
                    session_id != 0
                    and request["groundStationSessionId"] != session_id
                )
                or context["role"] == "estop"
            ):
                continue
            self.command_dispatcher.failed(
                request,
                "LINK_LOST",
                car_command_id=car_command_id,
            )
            self.state_store.clear_active_command(car_command_id)
            if context["role"] == "main":
                target_command_id = car_command_id

        if target_command_id is None or self._has_cancel_transaction():
            return
        try:
            car_command_id, _, _ = adapter.begin_cancel(target_command_id)
        except Exception as error:
            self.logger.warning(
                "cannot cancel command %s after TCP disconnect: %s",
                target_command_id,
                error,
            )
            return
        self._pending[car_command_id] = {
            "request": None,
            "operation": "disconnect_cancel",
            "request_type": TYPE_CANCEL,
            "role": "cancel",
            "target_command_id": target_command_id,
            "deadline": (
                time.monotonic() + TERMINAL_DEADLINES_S[TYPE_CANCEL]
            ),
        }

    def _expire_pending(self):
        now = time.monotonic()
        for car_command_id, context in list(self._pending.items()):
            if now < context["deadline"]:
                continue
            del self._pending[car_command_id]
            self.state_store.clear_active_command(car_command_id)
            self.state_store.set_control_enabled(False)
            if context["operation"] == "arm":
                self._arm_enable_key = None
            request = context["request"]
            if request is None:
                self.logger.warning(
                    "internal %s transaction %s timed out",
                    context["operation"],
                    car_command_id,
                )
            elif self.command_dispatcher.is_open(request):
                self.command_dispatcher.failed(
                    request,
                    "TIMEOUT",
                    car_command_id=car_command_id,
                )

    def _collect_terminals(self, adapter):
        terminal_seen = False
        for car_command_id, context in list(self._pending.items()):
            terminal = adapter.take_terminal(
                car_command_id,
                context["request_type"],
            )
            if terminal is None:
                continue
            terminal_seen = True
            del self._pending[car_command_id]
            self.state_store.clear_active_command(car_command_id)
            self._finish_context(car_command_id, context, terminal)
        return terminal_seen

    def _finish_context(self, car_command_id, context, terminal):
        request = context["request"]
        operation = context["operation"]
        failed = "failure_reason" in terminal
        request_open = (
            request is not None and
            self.command_dispatcher.is_open(request)
        )

        if operation in ("arm", "estop") or failed:
            self.state_store.set_control_enabled(False)
        if operation == "arm":
            if (
                not failed and
                request_open and
                self._arm_enable_key == self._request_key(request)
            ):
                self.state_store.set_control_enabled(True)
            self._arm_enable_key = None

        if not request_open:
            return
        if failed:
            reason = FAILURE_REASONS.get(
                terminal["failure_reason"],
                "FAILURE_{}".format(terminal["failure_reason"]),
            )
            fault_code = FAULT_CODE_NAMES.get(
                terminal.get("latched_fault_code", 0),
                "UNKNOWN_{}".format(terminal.get("latched_fault_code", 0)),
            )
            self.command_dispatcher.failed(
                request,
                reason,
                car_command_id=car_command_id,
                fault_code=fault_code,
            )
            self.logger.warning(
                "MSPM0 command failed: id=%s cmd=%s carCommandId=%s reason=%s",
                request["id"],
                request["cmd"],
                car_command_id,
                reason,
            )
            return

        reason = COMPLETION_REASONS.get(
            terminal.get("completion_reason"),
            "COMPLETED_{}".format(terminal.get("completion_reason")),
        )
        if operation == "disarm":
            reason = "DISARMED"
        self.command_dispatcher.done(
            request,
            reason,
            car_command_id=car_command_id,
        )
        self.logger.info(
            "MSPM0 command done: id=%s cmd=%s carCommandId=%s reason=%s",
            request["id"],
            request["cmd"],
            car_command_id,
            reason,
        )

    def _active_target_command_id(self):
        for car_command_id, context in self._pending.items():
            if context["role"] == "main":
                return car_command_id
        state = self.state_store.get_control_state()
        if state["transactionState"] in ("ACCEPTED", "RUNNING", "CANCELLING"):
            return state["activeCarCommandId"]
        return None

    def _has_main_transaction(self):
        return any(
            context["role"] == "main"
            for context in self._pending.values()
        )

    def _has_cancel_transaction(self):
        return any(
            context["role"] == "cancel"
            for context in self._pending.values()
        )

    def _reject_offline(self, request):
        self.command_dispatcher.rejected(
            request,
            "TI_OFFLINE",
            "Nano--MSPM0 serial link is offline",
        )

    def _reject_serial_error(self, request, error):
        error_name = error.__class__.__name__
        if error_name == "LinkTimeout":
            code = "TIMEOUT"
        elif error_name == "ProtocolError":
            code = "PROTOCOL_ERROR"
        elif error_name in ("UnexpectedResponse", "CommandFailed"):
            code = "TI_REJECTED"
        else:
            code = "INTERNAL_ERROR"
        self.command_dispatcher.rejected(
            request,
            code,
            str(error) or error_name,
        )

    def _fail_pending_for_link_loss(self):
        for car_command_id, context in list(self._pending.items()):
            request = context["request"]
            if request is not None:
                self.command_dispatcher.failed(
                    request,
                    "TI_OFFLINE",
                    car_command_id=car_command_id,
                )
        self._pending = {}
        self._arm_enable_key = None
        self.state_store.clear_active_command()
        self.state_store.set_control_enabled(False)

    def _service_offline_queue(self, stop_event):
        deadline = time.monotonic() + self.reconnect_delay_s
        while not stop_event.is_set() and time.monotonic() < deadline:
            request = self.command_dispatcher.get_request_nowait()
            if request is None:
                stop_event.wait(
                    min(
                        self.command_poll_interval_s,
                        max(0.0, deadline - time.monotonic()),
                    )
                )
                continue
            if request.get("kind") == "command":
                if self.command_dispatcher.is_open(request):
                    self._reject_offline(request)

    def _set_link_online(self, online):
        changed = self._link_online != online
        self._link_online = online
        if changed and not online:
            self._publish_disconnected()

    def _publish_disconnected(self):
        self.state_store.mark_ti_disconnected()

    def _uptime_ms(self, now=None):
        if self._started_at is None:
            return 0
        current = now if now is not None else time.monotonic()
        return int((current - self._started_at) * 1000.0) & 0xFFFFFFFF

    @staticmethod
    def _request_key(request):
        return (
            request["groundStationSessionId"],
            request["id"],
        )
