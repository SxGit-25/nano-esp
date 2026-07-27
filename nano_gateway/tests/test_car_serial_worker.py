import os
import sys
import threading
import time
import unittest


NANO_GATEWAY_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if NANO_GATEWAY_DIR not in sys.path:
    sys.path.insert(0, NANO_GATEWAY_DIR)

from gateway.car_serial_worker import (  # noqa: E402
    TYPE_ARM,
    CarSerialWorker,
)
from gateway.command_dispatcher import CommandDispatcher  # noqa: E402
from gateway.state_store import StateStore  # noqa: E402


class _FakeLink(object):
    def __init__(self):
        self.calls = []
        self.thread_ids = set()
        self.heartbeat_seen = threading.Event()

    def _record(self, name):
        self.calls.append(name)
        self.thread_ids.add(threading.current_thread().ident)

    def open(self):
        self._record("open")

    def close(self):
        self._record("close")

    def hello(self):
        self._record("hello")
        return (
            {
                "boot_id": 123,
                "capability_flags": 0x55,
            },
            0.004,
        )

    def heartbeat(self, uptime_ms):
        del uptime_ms
        self._record("heartbeat")
        self.heartbeat_seen.set()
        return {}, 0.002

    def get_status(self):
        self._record("get_status")
        return (
            {
                "session_id": 7,
                "reported_command_id": 42,
                "system_state": 3,
                "transaction_state": 2,
                "motion_type": 0x11,
                "progress_percent": 25,
                "distance_mm": 320,
                "heading_mdeg": -1200,
                "elapsed_ms": 900,
                "latched_fault_code": 0,
                "last_protocol_error": 0,
                "lap_count": 1,
                "segment_index": 2,
                "line_error_x100": -8,
            },
            0.003,
        )


class _CommandFakeLink(object):
    def __init__(self, auto_complete_arm=True):
        self.calls = []
        self.thread_ids = set()
        self.ready = threading.Event()
        self.cancel_seen = threading.Event()
        self.auto_complete_arm = auto_complete_arm
        self._next_command_id = 100
        self._terminals = {}
        self._request_types = {}
        self.system_state = 0
        self.transaction_state = 0
        self.reported_command_id = 0
        self.fault_code = 0

    def _record(self, name):
        self.calls.append(name)
        self.thread_ids.add(threading.current_thread().ident)

    def _new_command(self, request_type):
        command_id = self._next_command_id
        self._next_command_id += 1
        self._request_types[command_id] = request_type
        self.reported_command_id = command_id
        self.transaction_state = 2
        return command_id

    def _queue_done(self, command_id, request_type, completion_reason):
        self._terminals[(command_id, request_type)] = {
            "command_id": command_id,
            "request_type": request_type,
            "completion_reason": completion_reason,
        }

    def open(self):
        self._record("open")

    def close(self):
        self._record("close")

    def hello(self):
        self._record("hello")
        return {"boot_id": 123, "capability_flags": 0x55}, 0.001

    def heartbeat(self, uptime_ms):
        del uptime_ms
        self._record("heartbeat")
        self.ready.set()
        return {}, 0.001

    def get_status(self):
        self._record("get_status")
        return (
            {
                "session_id": 7,
                "reported_command_id": self.reported_command_id,
                "system_state": self.system_state,
                "transaction_state": self.transaction_state,
                "motion_type": 0,
                "progress_percent": 0,
                "distance_mm": 0,
                "heading_mdeg": 0,
                "elapsed_ms": 0,
                "latched_fault_code": self.fault_code,
                "last_protocol_error": 0,
                "lap_count": 0,
                "segment_index": 0,
                "line_error_x100": 0,
            },
            0.001,
        )

    def begin_arm(self):
        self._record("begin_arm")
        command_id = self._new_command(0x02)
        self.system_state = 1
        if self.auto_complete_arm:
            self._queue_done(command_id, 0x02, 3)
        return command_id, {}, 0.001

    def begin_cancel(self, target_command_id):
        self._record("begin_cancel")
        self.calls.append(("cancel_target", target_command_id))
        self.cancel_seen.set()
        target_type = self._request_types.get(target_command_id)
        if target_type is not None:
            self._terminals[(target_command_id, target_type)] = {
                "command_id": target_command_id,
                "request_type": target_type,
                "failure_reason": 2,
                "latched_fault_code": 0,
            }
        command_id = self._new_command(0x03)
        self._queue_done(command_id, 0x03, 6)
        return command_id, {}, 0.001

    def begin_emergency_stop(self):
        self._record("begin_emergency_stop")
        command_id = self._new_command(0x04)
        self.system_state = 5
        self.fault_code = 15
        self._queue_done(command_id, 0x04, 5)
        return command_id, {}, 0.001

    def begin_clear_fault(self):
        self._record("begin_clear_fault")
        command_id = self._new_command(0x05)
        self.system_state = 0
        self.fault_code = 0
        self._queue_done(command_id, 0x05, 4)
        return command_id, {}, 0.001

    def take_terminal(self, command_id, request_type):
        self._record("take_terminal")
        terminal = self._terminals.pop((command_id, request_type), None)
        if terminal is not None:
            self.reported_command_id = command_id
            self.transaction_state = 4
            if request_type == 0x02 and "failure_reason" not in terminal:
                self.system_state = 2
            elif request_type == 0x03:
                self.system_state = 2
        return terminal

    def pump(self, timeout_s):
        self._record("pump")
        time.sleep(min(timeout_s, 0.001))


def _command(session_id, command_id, command):
    return {
        "v": 1,
        "type": "command",
        "groundStationSessionId": session_id,
        "id": command_id,
        "cmd": command,
        "args": {},
    }


def _wait_response(dispatcher, session_id, command_id, message_type):
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        response = dispatcher.get_response_nowait(session_id)
        if (
            response is not None
            and response["id"] == command_id
            and response["type"] == message_type
        ):
            return response
        time.sleep(0.002)
    raise AssertionError(
        "timed out waiting for {} for id {}".format(
            message_type,
            command_id,
        )
    )


class CarSerialWorkerTests(unittest.TestCase):
    def test_single_thread_publishes_read_only_status(self):
        link = _FakeLink()
        stop_event = threading.Event()
        state_store = StateStore()

        worker = CarSerialWorker(
            port="/dev/fake",
            link_factory=lambda **kwargs: link,
            state_store=state_store,
            heartbeat_interval_s=0.01,
            status_interval_s=0.02,
            reconnect_delay_s=0.01,
        )
        thread = threading.Thread(target=worker.run, args=(stop_event,))
        thread.start()
        try:
            self.assertTrue(link.heartbeat_seen.wait(1.0))
            _, online = state_store.build_message("status")
        finally:
            stop_event.set()
            thread.join(1.0)

        _, disconnected = state_store.build_message("status")
        self.assertEqual("DOWN", disconnected["links"]["nanoTi"])
        self.assertEqual("EXECUTING", online["systemState"])
        self.assertTrue(online["armed"])
        self.assertIsNone(online["activeCommandId"])
        self.assertEqual(42, online["activeCarCommandId"])
        self.assertEqual("NONE", online["faultCode"])
        self.assertEqual(-8, online["lineErrorX100"])
        self.assertEqual(123, online["tiBootId"])
        self.assertIn("heartbeat", link.calls)
        self.assertIn("get_status", link.calls)
        self.assertEqual(1, len(link.thread_ids))
        self.assertNotIn("arm", link.calls)
        self.assertNotIn("drive_distance", link.calls)

    def test_control_lifecycle_and_upper_disarm_lock(self):
        link = _CommandFakeLink()
        stop_event = threading.Event()
        state_store = StateStore()
        dispatcher = CommandDispatcher(state_store)
        dispatcher.begin_session(100)
        worker = CarSerialWorker(
            port="/dev/fake",
            link_factory=lambda **kwargs: link,
            state_store=state_store,
            command_dispatcher=dispatcher,
            heartbeat_interval_s=0.01,
            status_interval_s=0.02,
            reconnect_delay_s=0.01,
            command_poll_interval_s=0.002,
        )
        thread = threading.Thread(target=worker.run, args=(stop_event,))
        thread.start()
        try:
            self.assertTrue(link.ready.wait(1.0))
            self.assertTrue(dispatcher.submit(_command(100, 1, "arm"), 100))
            accepted = _wait_response(dispatcher, 100, 1, "accepted")
            done = _wait_response(dispatcher, 100, 1, "done")
            self.assertEqual(100, accepted["carCommandId"])
            self.assertEqual("ARMED_READY", done["reason"])
            self.assertTrue(
                state_store.get_control_state()["controlEnabled"]
            )

            self.assertTrue(
                dispatcher.submit(_command(100, 2, "disarm"), 100)
            )
            disarmed = _wait_response(dispatcher, 100, 2, "done")
            self.assertEqual("DISARMED", disarmed["reason"])
            control_state = state_store.get_control_state()
            self.assertTrue(control_state["armed"])
            self.assertFalse(control_state["controlEnabled"])

            self.assertTrue(
                dispatcher.submit(_command(100, 3, "clear_fault"), 100)
            )
            self.assertEqual(
                "FAULT_CLEARED",
                _wait_response(dispatcher, 100, 3, "done")["reason"],
            )
            self.assertTrue(
                dispatcher.submit(_command(100, 4, "estop"), 100)
            )
            self.assertEqual(
                "EMERGENCY_STOP_APPLIED",
                _wait_response(dispatcher, 100, 4, "done")["reason"],
            )
            self.assertFalse(
                state_store.get_control_state()["controlEnabled"]
            )
        finally:
            stop_event.set()
            thread.join(1.0)

        self.assertEqual(1, len(link.thread_ids))
        self.assertIn("begin_arm", link.calls)
        self.assertIn("begin_clear_fault", link.calls)
        self.assertIn("begin_emergency_stop", link.calls)

    def test_stop_cancels_active_car_transaction(self):
        link = _CommandFakeLink(auto_complete_arm=False)
        stop_event = threading.Event()
        state_store = StateStore()
        dispatcher = CommandDispatcher(state_store)
        dispatcher.begin_session(100)
        worker = CarSerialWorker(
            port="/dev/fake",
            link_factory=lambda **kwargs: link,
            state_store=state_store,
            command_dispatcher=dispatcher,
            heartbeat_interval_s=0.01,
            status_interval_s=0.02,
            reconnect_delay_s=0.01,
            command_poll_interval_s=0.002,
        )
        thread = threading.Thread(target=worker.run, args=(stop_event,))
        thread.start()
        try:
            self.assertTrue(link.ready.wait(1.0))
            self.assertTrue(dispatcher.submit(_command(100, 1, "arm"), 100))
            arm_accepted = _wait_response(
                dispatcher,
                100,
                1,
                "accepted",
            )
            self.assertTrue(
                dispatcher.submit(_command(100, 2, "stop"), 100)
            )
            stop_accepted = _wait_response(
                dispatcher,
                100,
                2,
                "accepted",
            )
            arm_failed = _wait_response(dispatcher, 100, 1, "failed")
            stop_done = _wait_response(dispatcher, 100, 2, "done")

            self.assertEqual(
                arm_accepted["carCommandId"],
                link.calls[link.calls.index("begin_cancel") + 1][1],
            )
            self.assertNotEqual(2, stop_accepted["carCommandId"])
            self.assertEqual("USER_CANCELLED", arm_failed["reason"])
            self.assertEqual("CANCEL_COMPLETED", stop_done["reason"])
        finally:
            stop_event.set()
            thread.join(1.0)

    def test_terminal_deadline_fails_open_command(self):
        state_store = StateStore()
        state_store.set_control_enabled(True)
        dispatcher = CommandDispatcher(state_store)
        dispatcher.begin_session(100)
        self.assertTrue(dispatcher.submit(_command(100, 1, "arm"), 100))
        request = dispatcher.get_request_nowait()
        worker = CarSerialWorker(
            port="/dev/fake",
            link_factory=lambda **kwargs: None,
            state_store=state_store,
            command_dispatcher=dispatcher,
        )
        dispatcher.accepted(request, "CALIBRATING", 55)
        dispatcher.get_response_nowait(100)
        worker._pending[55] = {
            "request": request,
            "operation": "arm",
            "request_type": TYPE_ARM,
            "role": "main",
            "target_command_id": None,
            "deadline": time.monotonic() - 0.001,
        }

        worker._expire_pending()

        failed = dispatcher.get_response_nowait(100)
        self.assertEqual("failed", failed["type"])
        self.assertEqual("TIMEOUT", failed["reason"])
        self.assertFalse(
            state_store.get_control_state()["controlEnabled"]
        )

    def test_late_arm_done_cannot_reenable_invalidated_admission(self):
        state_store = StateStore()
        dispatcher = CommandDispatcher(state_store)
        dispatcher.begin_session(100)
        worker = CarSerialWorker(
            port="/dev/fake",
            link_factory=lambda **kwargs: None,
            state_store=state_store,
            command_dispatcher=dispatcher,
        )

        self.assertTrue(dispatcher.submit(_command(100, 1, "arm"), 100))
        arm_request = dispatcher.get_request_nowait()
        dispatcher.accepted(arm_request, "CALIBRATING", 55)
        dispatcher.get_response_nowait(100)
        arm_context = {
            "request": arm_request,
            "operation": "arm",
            "request_type": TYPE_ARM,
            "role": "main",
            "target_command_id": None,
            "deadline": time.monotonic() + 1.0,
        }
        worker._pending[55] = arm_context
        worker._arm_enable_key = (100, 1)

        dispatcher.end_session(100)
        worker._handle_disconnect(
            _CommandFakeLink(auto_complete_arm=False),
            {
                "kind": "disconnect",
                "groundStationSessionId": 100,
            },
        )
        worker._finish_context(
            55,
            arm_context,
            {
                "command_id": 55,
                "request_type": TYPE_ARM,
                "completion_reason": 3,
            },
        )

        self.assertFalse(
            state_store.get_control_state()["controlEnabled"]
        )

    def test_estop_invalidates_late_arm_enable(self):
        state_store = StateStore()
        dispatcher = CommandDispatcher(state_store)
        dispatcher.begin_session(100)
        worker = CarSerialWorker(
            port="/dev/fake",
            link_factory=lambda **kwargs: None,
            state_store=state_store,
            command_dispatcher=dispatcher,
        )

        self.assertTrue(dispatcher.submit(_command(100, 1, "arm"), 100))
        arm_request = dispatcher.get_request_nowait()
        dispatcher.accepted(arm_request, "CALIBRATING", 55)
        dispatcher.get_response_nowait(100)
        worker._arm_enable_key = (100, 1)

        self.assertTrue(dispatcher.submit(_command(100, 2, "estop"), 100))
        estop_request = dispatcher.get_request_nowait()
        worker._start_estop(
            _CommandFakeLink(auto_complete_arm=False),
            estop_request,
        )
        worker._finish_context(
            55,
            {
                "request": arm_request,
                "operation": "arm",
                "request_type": TYPE_ARM,
                "role": "main",
                "target_command_id": None,
                "deadline": time.monotonic() + 1.0,
            },
            {
                "command_id": 55,
                "request_type": TYPE_ARM,
                "completion_reason": 3,
            },
        )

        self.assertFalse(
            state_store.get_control_state()["controlEnabled"]
        )

    def test_tcp_disconnect_cancels_session_transaction(self):
        link = _CommandFakeLink(auto_complete_arm=False)
        stop_event = threading.Event()
        state_store = StateStore()
        dispatcher = CommandDispatcher(state_store)
        dispatcher.begin_session(100)
        worker = CarSerialWorker(
            port="/dev/fake",
            link_factory=lambda **kwargs: link,
            state_store=state_store,
            command_dispatcher=dispatcher,
            heartbeat_interval_s=0.01,
            status_interval_s=0.02,
            reconnect_delay_s=0.01,
            command_poll_interval_s=0.002,
        )
        thread = threading.Thread(target=worker.run, args=(stop_event,))
        thread.start()
        try:
            self.assertTrue(link.ready.wait(1.0))
            dispatcher.submit(_command(100, 1, "arm"), 100)
            accepted = _wait_response(dispatcher, 100, 1, "accepted")

            dispatcher.end_session(100)

            self.assertTrue(link.cancel_seen.wait(1.0))
            failed = _wait_response(dispatcher, 100, 1, "failed")
            self.assertEqual("LINK_LOST", failed["reason"])
            self.assertIn(
                ("cancel_target", accepted["carCommandId"]),
                link.calls,
            )
            self.assertFalse(
                state_store.get_control_state()["controlEnabled"]
            )
        finally:
            stop_event.set()
            thread.join(1.0)


if __name__ == "__main__":
    unittest.main()
