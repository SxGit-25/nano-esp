import os
import socket
import sys
import threading
import time
import unittest


NANO_GATEWAY_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if NANO_GATEWAY_DIR not in sys.path:
    sys.path.insert(0, NANO_GATEWAY_DIR)

from gateway.framed_json import FramedJsonParser, encode_message  # noqa: E402
from gateway.command_dispatcher import (  # noqa: E402
    MAX_PENDING_RESPONSES,
    CommandDispatcher,
)
from gateway.state_store import StateStore  # noqa: E402
from gateway.tcp_client import GatewayConfig, NanoTcpClient  # noqa: E402


def _status(system_state, timestamp_ms):
    state_number = 2 if system_state == "ARMED" else 0
    return {
        "session_id": 7,
        "reported_command_id": 0,
        "system_state": state_number,
        "transaction_state": 0,
        "motion_type": 0,
        "progress_percent": 0,
        "distance_mm": timestamp_ms,
        "heading_mdeg": 0,
        "elapsed_ms": 0,
        "latched_fault_code": 0,
        "last_protocol_error": 0,
        "lap_count": 0,
        "segment_index": 0,
        "line_error_x100": 0,
    }


class _ReconnectServer(object):
    def __init__(self):
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(2)
        self._listener.settimeout(3.0)
        self.port = self._listener.getsockname()[1]
        self.hello_count = 0
        self.heartbeat_count = 0
        self.snapshot_count = 0
        self.finished = threading.Event()
        self.error = None
        self._thread = threading.Thread(target=self._run)

    def start(self):
        self._thread.start()

    def close(self):
        self._listener.close()
        self._thread.join(3.0)

    def _receive_message(self, peer, parser, pending):
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            if pending:
                return pending.pop(0)
            data = peer.recv(4096)
            if not data:
                raise AssertionError("client closed before expected frame")
            messages = parser.feed(data)
            if messages:
                pending.extend(messages)
        raise AssertionError("timed out waiting for a client message")

    def _run(self):
        try:
            for connection_index in range(2):
                peer, _ = self._listener.accept()
                peer.settimeout(2.0)
                with peer:
                    parser = FramedJsonParser()
                    pending = []
                    hello = self._receive_message(peer, parser, pending)
                    if hello.get("type") != "hello" or hello.get("role") != "nano":
                        raise AssertionError("unexpected hello: {!r}".format(hello))
                    self.hello_count += 1

                    hello_ack = encode_message(
                        {
                            "v": 1,
                            "type": "hello_ack",
                            "role": "esp32",
                            "version": "2.1",
                            "groundStationSessionId": 100,
                        }
                    )
                    peer.sendall(hello_ack[:3])
                    peer.sendall(hello_ack[3:])

                    snapshot = self._receive_message(peer, parser, pending)
                    if snapshot.get("type") != "snapshot":
                        raise AssertionError("unexpected message: {!r}".format(snapshot))
                    self.snapshot_count += 1

                    heartbeat = self._receive_message(peer, parser, pending)
                    if heartbeat.get("type") != "heartbeat":
                        raise AssertionError("unexpected message: {!r}".format(heartbeat))
                    self.heartbeat_count += 1
            self.finished.set()
        except Exception as error:  # pragma: no cover - asserted by the test
            self.error = error
            self.finished.set()


class NanoTcpClientTests(unittest.TestCase):
    @staticmethod
    def _complete_handshake(client, session_id=100):
        client._started_at = time.monotonic()
        client._connected_at = time.monotonic()
        client._handle_message(
            {
                "v": 1,
                "type": "hello_ack",
                "role": "esp32",
                "version": "2.1",
                "groundStationSessionId": session_id,
            }
        )
        client._outbound.clear()

    def test_hello_heartbeat_and_reconnect(self):
        server = _ReconnectServer()
        server.start()
        stop_event = threading.Event()
        states = []
        client = NanoTcpClient(
            GatewayConfig(
                host="127.0.0.1",
                port=server.port,
                reconnect_initial_s=0.02,
                reconnect_max_s=0.05,
                reconnect_reset_s=1.0,
                heartbeat_interval_s=0.02,
                hello_timeout_s=0.5,
                link_down_timeout_s=0.5,
                link_degraded_timeout_s=0.25,
            )
        )
        client.on_state_change = states.append
        client_thread = threading.Thread(target=client.run, args=(stop_event,))
        client_thread.start()

        try:
            self.assertTrue(server.finished.wait(3.0))
            self.assertIsNone(server.error)
            self.assertEqual(2, server.hello_count)
            self.assertEqual(2, server.snapshot_count)
            self.assertEqual(2, server.heartbeat_count)
            self.assertGreaterEqual(states.count("UP"), 2)
            self.assertEqual(1, client.stats["reconnect_count"])
        finally:
            stop_event.set()
            client_thread.join(3.0)
            server.close()

    def test_only_latest_serial_status_is_queued(self):
        state_store = StateStore()
        client = NanoTcpClient(state_store=state_store)
        client._started_at = time.monotonic()
        state_store.set_esp_nano_link("UP")
        state_store.update_from_mspm0(
            _status("IDLE", 1),
            {"boot_id": 10, "capability_flags": 0x55},
        )
        state_store.update_from_mspm0(
            _status("ARMED", 2),
            {"boot_id": 10, "capability_flags": 0x55},
        )

        client._queue_latest_status()
        messages = FramedJsonParser().feed(bytes(client._outbound))

        self.assertEqual(1, len(messages))
        self.assertEqual(2, messages[0]["distanceMm"])
        self.assertEqual("ARMED", messages[0]["systemState"])
        self.assertEqual("UP", messages[0]["links"]["nanoTi"])
        self.assertEqual("UP", messages[0]["links"]["espNano"])

        client._outbound.clear()
        client._queue_heartbeat(time.monotonic())
        heartbeat = FramedJsonParser().feed(bytes(client._outbound))[0]
        self.assertTrue(heartbeat["tiOnline"])

    def test_status_waits_behind_unsent_network_data(self):
        state_store = StateStore()
        client = NanoTcpClient(state_store=state_store)
        client._outbound.extend(b"pending")
        state_store.update_from_mspm0(
            _status("IDLE", 3),
            {"boot_id": 10, "capability_flags": 0x55},
        )

        client._queue_latest_status()
        self.assertEqual(b"pending", bytes(client._outbound))

        client._outbound.clear()
        client._queue_latest_status()
        messages = FramedJsonParser().feed(bytes(client._outbound))
        self.assertEqual(3, messages[0]["distanceMm"])

    def test_heartbeat_has_priority_over_status(self):
        state_store = StateStore()
        client = NanoTcpClient(state_store=state_store)
        client._started_at = time.monotonic()
        state_store.update_from_mspm0(
            _status("IDLE", 4),
            {"boot_id": 10, "capability_flags": 0x55},
        )

        client._queue_heartbeat(time.monotonic())
        client._queue_latest_status()
        messages = FramedJsonParser().feed(bytes(client._outbound))

        self.assertEqual(1, len(messages))
        self.assertEqual("heartbeat", messages[0]["type"])

    def test_command_is_queued_and_result_is_framed_without_uart_access(self):
        state_store = StateStore()
        dispatcher = CommandDispatcher(state_store)
        client = NanoTcpClient(
            state_store=state_store,
            command_dispatcher=dispatcher,
        )
        self._complete_handshake(client)

        self.assertTrue(
            client._handle_message(
                {
                    "v": 1,
                    "type": "command",
                    "groundStationSessionId": 100,
                    "id": 7,
                    "cmd": "arm",
                    "args": {},
                }
            )
        )
        request = dispatcher.get_request_nowait()
        dispatcher.accepted(request, "CALIBRATING", 55)
        self.assertTrue(client._queue_command_response())
        response = FramedJsonParser().feed(bytes(client._outbound))[0]

        self.assertEqual("accepted", response["type"])
        self.assertEqual(100, response["groundStationSessionId"])
        self.assertEqual(7, response["id"])
        self.assertEqual(55, response["carCommandId"])

    def test_get_status_returns_session_bound_snapshot(self):
        client = NanoTcpClient()
        self._complete_handshake(client)

        self.assertTrue(
            client._handle_message(
                {
                    "v": 1,
                    "type": "get_status",
                    "groundStationSessionId": 100,
                }
            )
        )
        snapshot = FramedJsonParser().feed(bytes(client._outbound))[0]
        self.assertEqual("snapshot", snapshot["type"])
        self.assertEqual(100, snapshot["groundStationSessionId"])

    def test_response_overflow_closes_session_and_disables_control(self):
        state_store = StateStore()
        dispatcher = CommandDispatcher(state_store)
        client = NanoTcpClient(
            state_store=state_store,
            command_dispatcher=dispatcher,
        )
        self._complete_handshake(client)
        state_store.set_control_enabled(True)
        for command_id in range(1, MAX_PENDING_RESPONSES + 2):
            dispatcher.submit(
                {
                    "v": 1,
                    "type": "command",
                    "groundStationSessionId": 100,
                    "id": command_id,
                    "cmd": "unsupported",
                    "args": {},
                },
                100,
            )

        self.assertFalse(client._queue_command_response())
        self.assertEqual("DOWN", client.connection_state)
        self.assertFalse(
            state_store.get_control_state()["controlEnabled"]
        )


if __name__ == "__main__":
    unittest.main()
