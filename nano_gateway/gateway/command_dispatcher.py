"""Thread-safe command boundary between TCP and the MSPM0 serial worker."""

import json
import heapq
import queue
import threading
from collections import OrderedDict


PROTOCOL_VERSION = 1
SUPPORTED_COMMANDS = frozenset(
    (
        "arm",
        "disarm",
        "stop",
        "estop",
        "clear_fault",
        "drive_distance",
        "turn_relative",
    )
)

_PRIORITY_ESTOP = 0
_PRIORITY_STOP = 2
_PRIORITY_NORMAL = 3
MAX_PENDING_REQUESTS = 32
MAX_PENDING_RESPONSES = 64
MAX_SESSION_RECORDS = 256


def _is_uint32_nonzero(value):
    return (
        isinstance(value, int)
        and not isinstance(value, bool)
        and 0 < value <= 0xFFFFFFFF
    )


class CommandDispatcher(object):
    """Validate, deduplicate and queue commands without touching the UART."""

    def __init__(self, state_store):
        self.state_store = state_store
        self._lock = threading.Lock()
        self._requests = []
        self._responses = queue.Queue(maxsize=MAX_PENDING_RESPONSES)
        self._sequence = 0
        self._current_session_id = None
        self._highest_command_id = 0
        self._records = OrderedDict()
        self._pending_disconnect_session_id = None
        self._response_overflow_session_id = None

    def begin_session(self, session_id):
        if not _is_uint32_nonzero(session_id):
            raise ValueError("session_id must be a non-zero uint32")
        with self._lock:
            if session_id != self._current_session_id:
                self._current_session_id = session_id
                self._highest_command_id = 0
                self._records = OrderedDict(
                    (key, record)
                    for key, record in self._records.items()
                    if (
                        record["state"] == "queued"
                        and record["survive_reconnect"]
                    )
                )

    def end_session(self, session_id):
        """Disable admission and request cancellation after a TCP disconnect."""

        self.state_store.set_control_enabled(False)
        if not _is_uint32_nonzero(session_id):
            return
        with self._lock:
            for key, record in self._records.items():
                if (
                    key[0] == session_id
                    and record["state"] == "queued"
                    and record["command"] in ("stop", "disarm", "estop")
                ):
                    record["survive_reconnect"] = True
            self._fail_queued_locked(
                session_id,
                reason="LINK_LOST",
                exclude_commands=("stop", "disarm", "estop"),
            )
            if self._pending_disconnect_session_id is None:
                self._pending_disconnect_session_id = session_id
            elif self._pending_disconnect_session_id != session_id:
                self._pending_disconnect_session_id = 0

    def submit(self, message, session_id):
        """Validate one command and enqueue it once.

        Return ``False`` only for malformed mandatory fields, for which the
        transport must close the TCP connection. Business-level failures are
        returned as a cached ``rejected`` response.
        """

        if (
            message.get("v") != PROTOCOL_VERSION
            or message.get("type") != "command"
            or not _is_uint32_nonzero(
                message.get("groundStationSessionId")
            )
            or not _is_uint32_nonzero(message.get("id"))
            or not isinstance(message.get("cmd"), str)
            or not isinstance(message.get("args"), dict)
        ):
            return False

        ground_session_id = message["groundStationSessionId"]
        command_id = message["id"]
        command = message["cmd"]
        args = message["args"]
        fingerprint = self._fingerprint(command, args)
        key = (ground_session_id, command_id)

        with self._lock:
            if (
                ground_session_id != session_id
                or ground_session_id != self._current_session_id
            ):
                self._queue_response_locked(
                    self._error_response(
                        ground_session_id,
                        command_id,
                        "SESSION_MISMATCH",
                        "command does not belong to the active ground station session",
                        "groundStationSessionId",
                    ),
                    delivery_session_id=session_id,
                )
                return True

            existing = self._records.get(key)
            if existing is not None:
                if existing["fingerprint"] != fingerprint:
                    self._queue_response_locked(
                        self._error_response(
                            ground_session_id,
                            command_id,
                            "INVALID_ARGUMENT",
                            "command id was reused with different content",
                            "id",
                        )
                    )
                elif existing["last_response"] is not None:
                    self._queue_response_locked(existing["last_response"])
                return True

            if command_id <= self._highest_command_id:
                response = self._error_response(
                    ground_session_id,
                    command_id,
                    "INVALID_ARGUMENT",
                    "new command id must increase monotonically",
                    "id",
                )
                self._queue_response_locked(response)
                return True

            if (
                len(self._records) >= MAX_SESSION_RECORDS
                and not self._evict_terminal_record_locked()
            ):
                self._queue_response_locked(
                    self._error_response(
                        ground_session_id,
                        command_id,
                        "BUSY",
                        "command history is full",
                    )
                )
                return True

            self._highest_command_id = command_id
            record = {
                "fingerprint": fingerprint,
                "command": command,
                "state": "queued",
                "last_response": None,
                "survive_reconnect": False,
            }
            self._records[key] = record

            request = {
                "kind": "command",
                "groundStationSessionId": ground_session_id,
                "id": command_id,
                "cmd": command,
                "args": dict(args),
            }
            if command not in SUPPORTED_COMMANDS:
                response = self._error_response(
                    ground_session_id,
                    command_id,
                    "UNSUPPORTED_COMMAND",
                    "command is not implemented in this gateway phase",
                    "cmd",
                )
                self._set_response_locked(record, response, terminal=True)
                return True
            if not self._valid_args(command, args):
                response = self._error_response(
                    ground_session_id,
                    command_id,
                    "INVALID_ARGUMENT",
                    "invalid arguments for {}".format(command),
                    "args",
                )
                self._set_response_locked(record, response, terminal=True)
                return True

            if command in ("disarm", "estop"):
                self.state_store.set_control_enabled(False)
            if command == "estop":
                self._fail_queued_locked(
                    ground_session_id,
                    reason="EMERGENCY_STOPPED",
                    exclude_commands=("stop", "disarm", "estop"),
                )

            if not self._put_request_locked(
                request,
                self._priority(command),
            ):
                response = self._error_response(
                    ground_session_id,
                    command_id,
                    "BUSY",
                    "Nano command queue is full",
                )
                self._set_response_locked(record, response, terminal=True)
        return True

    def get_request_nowait(self):
        with self._lock:
            if self._pending_disconnect_session_id is not None:
                session_id = self._pending_disconnect_session_id
                self._pending_disconnect_session_id = None
                return {
                    "kind": "disconnect",
                    "groundStationSessionId": session_id,
                }
            if not self._requests:
                return None
            _, _, request = heapq.heappop(self._requests)
            return request

    def is_open(self, request):
        if request.get("kind") != "command":
            return True
        key = self._request_key(request)
        with self._lock:
            record = self._records.get(key)
            return record is not None and record["state"] != "terminal"

    def accepted(self, request, state, car_command_id):
        response = self._base_response(request, "accepted")
        response["state"] = state
        response["carCommandId"] = car_command_id
        self._record_response(request, response, terminal=False)

    def rejected(self, request, code, message, field=None):
        response = self._error_response(
            request["groundStationSessionId"],
            request["id"],
            code,
            message,
            field,
        )
        self._record_response(request, response, terminal=True)

    def done(self, request, reason, car_command_id=None):
        response = self._base_response(request, "done")
        if car_command_id is not None:
            response["carCommandId"] = car_command_id
        response["reason"] = reason
        self._record_response(request, response, terminal=True)

    def failed(
        self,
        request,
        reason,
        car_command_id=None,
        fault_code=None,
    ):
        response = self._base_response(request, "failed")
        if car_command_id is not None:
            response["carCommandId"] = car_command_id
        response["reason"] = reason
        if fault_code is not None:
            response["faultCode"] = fault_code
        self._record_response(request, response, terminal=True)

    def get_response_nowait(self, session_id):
        while True:
            try:
                delivery_session_id, response = self._responses.get_nowait()
            except queue.Empty:
                return None
            if delivery_session_id == session_id:
                return response

    def consume_response_overflow(self, session_id):
        with self._lock:
            if self._response_overflow_session_id != session_id:
                return False
            self._response_overflow_session_id = None
            return True

    def _record_response(self, request, response, terminal):
        key = self._request_key(request)
        with self._lock:
            record = self._records.get(key)
            if record is None or record["state"] == "terminal":
                return
            self._set_response_locked(record, response, terminal)

    def _set_response_locked(self, record, response, terminal):
        record["last_response"] = dict(response)
        record["state"] = "terminal" if terminal else "accepted"
        self._queue_response_locked(response)

    def _fail_queued_locked(self, session_id, reason, exclude_commands):
        for key, record in self._records.items():
            if (
                key[0] != session_id
                or record["state"] != "queued"
                or record["command"] in exclude_commands
            ):
                continue
            response = {
                "v": PROTOCOL_VERSION,
                "type": "failed",
                "groundStationSessionId": key[0],
                "id": key[1],
                "reason": reason,
            }
            self._set_response_locked(record, response, terminal=True)

    def _put_request_locked(self, request, priority):
        self._prune_closed_requests_locked()
        if len(self._requests) >= MAX_PENDING_REQUESTS:
            if priority != _PRIORITY_ESTOP:
                return False
            worst_index = max(
                range(len(self._requests)),
                key=lambda index: self._requests[index][:2],
            )
            if self._requests[worst_index][0] == _PRIORITY_ESTOP:
                return False
            _, _, displaced = self._requests.pop(worst_index)
            heapq.heapify(self._requests)
            record = self._records.get(self._request_key(displaced))
            if record is not None and record["state"] != "terminal":
                response = {
                    "v": PROTOCOL_VERSION,
                    "type": "failed",
                    "groundStationSessionId": displaced[
                        "groundStationSessionId"
                    ],
                    "id": displaced["id"],
                    "reason": "EMERGENCY_STOPPED",
                }
                self._set_response_locked(record, response, terminal=True)
        self._sequence += 1
        heapq.heappush(
            self._requests,
            (priority, self._sequence, request),
        )
        return True

    def _queue_response_locked(self, response, delivery_session_id=None):
        destination = (
            response.get("groundStationSessionId")
            if delivery_session_id is None
            else delivery_session_id
        )
        item = (destination, dict(response))
        try:
            self._responses.put_nowait(item)
        except queue.Full:
            self._response_overflow_session_id = destination

    def _prune_closed_requests_locked(self):
        retained = []
        for item in self._requests:
            request = item[2]
            if request.get("kind") != "command":
                retained.append(item)
                continue
            record = self._records.get(self._request_key(request))
            if record is not None and record["state"] != "terminal":
                retained.append(item)
        if len(retained) != len(self._requests):
            self._requests = retained
            heapq.heapify(self._requests)

    def _evict_terminal_record_locked(self):
        for key, record in list(self._records.items()):
            if record["state"] == "terminal":
                del self._records[key]
                return True
        return False

    @staticmethod
    def _priority(command):
        if command == "estop":
            return _PRIORITY_ESTOP
        if command in ("stop", "disarm"):
            return _PRIORITY_STOP
        return _PRIORITY_NORMAL

    @staticmethod
    def _fingerprint(command, args):
        return (
            command,
            json.dumps(args, ensure_ascii=False, sort_keys=True),
        )

    @staticmethod
    def _valid_args(command, args):
        if command in ("arm", "disarm", "stop", "estop", "clear_fault"):
            return not args
        if command == "drive_distance":
            expected = {
                "distanceMm",
                "speedMmPerSec",
                "headingMode",
                "endBehavior",
                "timeoutMs",
            }
            return (
                set(args) == expected and
                isinstance(args["distanceMm"], int) and
                not isinstance(args["distanceMm"], bool) and
                -5000 <= args["distanceMm"] <= 5000 and
                abs(args["distanceMm"]) >= 10 and
                isinstance(args["speedMmPerSec"], int) and
                not isinstance(args["speedMmPerSec"], bool) and
                50 <= args["speedMmPerSec"] <= 1200 and
                isinstance(args["headingMode"], int) and
                not isinstance(args["headingMode"], bool) and
                args["headingMode"] in (0, 1) and
                isinstance(args["endBehavior"], int) and
                not isinstance(args["endBehavior"], bool) and
                args["endBehavior"] == 0 and
                isinstance(args["timeoutMs"], int) and
                not isinstance(args["timeoutMs"], bool) and
                100 <= args["timeoutMs"] <= 60000
            )
        if command == "turn_relative":
            expected = {
                "angleMdeg",
                "maxWheelSpeedMmPerSec",
                "turnMode",
                "timeoutMs",
            }
            return (
                set(args) == expected and
                isinstance(args["angleMdeg"], int) and
                not isinstance(args["angleMdeg"], bool) and
                -360000 <= args["angleMdeg"] <= 360000 and
                args["angleMdeg"] != 0 and
                isinstance(args["maxWheelSpeedMmPerSec"], int) and
                not isinstance(args["maxWheelSpeedMmPerSec"], bool) and
                50 <= args["maxWheelSpeedMmPerSec"] <= 1200 and
                isinstance(args["turnMode"], int) and
                not isinstance(args["turnMode"], bool) and
                args["turnMode"] in (0, 1) and
                isinstance(args["timeoutMs"], int) and
                not isinstance(args["timeoutMs"], bool) and
                100 <= args["timeoutMs"] <= 60000
            )
        return False

    @staticmethod
    def _request_key(request):
        return (request["groundStationSessionId"], request["id"])

    @staticmethod
    def _base_response(request, message_type):
        return {
            "v": PROTOCOL_VERSION,
            "type": message_type,
            "groundStationSessionId": request["groundStationSessionId"],
            "id": request["id"],
        }

    @staticmethod
    def _error_response(session_id, command_id, code, message, field=None):
        error = {
            "code": code,
            "message": message,
        }
        if field is not None:
            error["field"] = field
        return {
            "v": PROTOCOL_VERSION,
            "type": "rejected",
            "groundStationSessionId": session_id,
            "id": command_id,
            "error": error,
        }
