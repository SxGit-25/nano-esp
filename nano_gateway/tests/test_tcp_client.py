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
from gateway.tcp_client import GatewayConfig, NanoTcpClient  # noqa: E402


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
        self.finished = threading.Event()
        self.error = None
        self._thread = threading.Thread(target=self._run)

    def start(self):
        self._thread.start()

    def close(self):
        self._listener.close()
        self._thread.join(3.0)

    def _receive_message(self, peer, parser):
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            data = peer.recv(4096)
            if not data:
                raise AssertionError("client closed before expected frame")
            messages = parser.feed(data)
            if messages:
                return messages[0]
        raise AssertionError("timed out waiting for a client message")

    def _run(self):
        try:
            for connection_index in range(2):
                peer, _ = self._listener.accept()
                peer.settimeout(2.0)
                with peer:
                    parser = FramedJsonParser()
                    hello = self._receive_message(peer, parser)
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

                    heartbeat = self._receive_message(peer, parser)
                    if heartbeat.get("type") != "heartbeat":
                        raise AssertionError("unexpected message: {!r}".format(heartbeat))
                    self.heartbeat_count += 1
            self.finished.set()
        except Exception as error:  # pragma: no cover - asserted by the test
            self.error = error
            self.finished.set()


class NanoTcpClientTests(unittest.TestCase):
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
            self.assertEqual(2, server.heartbeat_count)
            self.assertGreaterEqual(states.count("UP"), 2)
            self.assertEqual(1, client.stats["reconnect_count"])
        finally:
            stop_event.set()
            client_thread.join(3.0)
            server.close()


if __name__ == "__main__":
    unittest.main()
