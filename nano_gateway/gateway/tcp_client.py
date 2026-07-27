"""Non-blocking TCP client for the ESP32 ground station.

This module deliberately does not access the Nano--MSPM0 UART. It only owns
the Wi-Fi session and passes validated commands through a thread-safe queue.
"""

import errno
import logging
import select
import socket
import time

from gateway.framed_json import FramedJsonError, FramedJsonParser, encode_message
from gateway.command_dispatcher import CommandDispatcher
from gateway.state_store import StateStore


PROTOCOL_VERSION = 1
DEFAULT_PORT = 8765


class GatewayConfig(object):
    """Connection timing and endpoint configuration for one Nano."""

    def __init__(
        self,
        host="192.168.4.1",
        port=DEFAULT_PORT,
        reconnect_initial_s=0.5,
        reconnect_max_s=5.0,
        reconnect_reset_s=30.0,
        heartbeat_interval_s=1.0,
        hello_timeout_s=5.0,
        link_down_timeout_s=5.0,
        link_degraded_timeout_s=3.0,
    ):
        if not isinstance(host, str) or not host:
            raise ValueError("host must be a non-empty string")
        if not 1 <= port <= 65535:
            raise ValueError("port must be within 1..65535")
        if reconnect_initial_s <= 0 or reconnect_max_s < reconnect_initial_s:
            raise ValueError("invalid reconnect delay")
        if heartbeat_interval_s <= 0:
            raise ValueError("heartbeat_interval_s must be positive")
        if hello_timeout_s <= 0 or link_down_timeout_s <= 0:
            raise ValueError("link timeouts must be positive")
        if not 0 < link_degraded_timeout_s < link_down_timeout_s:
            raise ValueError("link_degraded_timeout_s must be before DOWN")

        self.host = host
        self.port = port
        self.reconnect_initial_s = reconnect_initial_s
        self.reconnect_max_s = reconnect_max_s
        self.reconnect_reset_s = reconnect_reset_s
        self.heartbeat_interval_s = heartbeat_interval_s
        self.hello_timeout_s = hello_timeout_s
        self.link_down_timeout_s = link_down_timeout_s
        self.link_degraded_timeout_s = link_degraded_timeout_s


class NanoTcpClient(object):
    """TCP client with hello, 1 s heartbeat and bounded reconnect backoff."""

    def __init__(
        self,
        config=None,
        logger=None,
        version="2.1",
        state_store=None,
        command_dispatcher=None,
    ):
        self.config = config or GatewayConfig()
        self.logger = logger or logging.getLogger(__name__)
        self.version = version

        self._socket = None
        self._connecting = False
        self._parser = FramedJsonParser()
        self._outbound = bytearray()
        self._handshake_ready = False
        self._connected_at = None
        self._last_rx_at = None
        self._next_heartbeat_at = None
        self._next_connect_at = 0.0
        self._reconnect_delay_s = self.config.reconnect_initial_s
        self._started_at = None
        self._had_handshake = False
        self._last_state_revision = 0
        self.state_store = state_store or StateStore()
        self.command_dispatcher = (
            command_dispatcher or CommandDispatcher(self.state_store)
        )

        self.ground_station_session_id = None
        self.connection_state = "DOWN"
        self.on_state_change = None
        self.on_message = None
        self.stats = {
            "connect_attempts": 0,
            "reconnect_count": 0,
            "last_hello_rtt_ms": None,
        }

    @property
    def connected(self):
        return self._socket is not None and self._handshake_ready

    def run(self, stop_event):
        """Run until ``stop_event`` is set; this method owns the TCP socket."""

        self._started_at = time.monotonic()
        self._next_connect_at = self._started_at
        try:
            while not stop_event.is_set():
                self.poll()
        finally:
            self.close("client stopped", reconnect=False)

    def poll(self):
        """Run one bounded event-loop iteration for embedding or tests."""

        now = time.monotonic()
        if self._socket is None and now >= self._next_connect_at:
            self._begin_connect(now)

        timeout = self._poll_timeout(now)
        if self._socket is None:
            time.sleep(timeout)
            return

        read_list = [self._socket]
        write_list = [self._socket] if self._connecting or self._outbound else []
        try:
            readable, writable, exceptional = select.select(
                read_list,
                write_list,
                read_list,
                timeout,
            )
        except (OSError, ValueError) as error:
            self.close("select failed: {}".format(error))
            return

        if exceptional:
            self.close("socket exception")
            return
        if self._connecting and writable:
            self._finish_connect()
        if self._socket is None:
            return
        if readable:
            self._read_socket()
        if self._socket is None:
            return
        if writable and self._outbound:
            self._write_socket()
        if self._socket is None:
            return

        now = time.monotonic()
        if not self._handshake_ready:
            if now - self._connected_at >= self.config.hello_timeout_s:
                self.close("hello_ack timeout")
            return

        age = now - self._last_rx_at
        if age >= self.config.link_down_timeout_s:
            self.close("peer heartbeat timeout")
            return
        if age >= self.config.link_degraded_timeout_s:
            self._set_connection_state("DEGRADED")
        if not self._queue_command_response():
            return
        if now >= self._next_heartbeat_at:
            self._queue_heartbeat(now)
        self._queue_latest_status()

    def close(self, reason, reconnect=True):
        """Close the active socket and, unless stopped, schedule a reconnect."""

        now = time.monotonic()
        was_active = self._socket is not None or self.connection_state != "DOWN"
        stable_connection = (
            self._connected_at is not None
            and now - self._connected_at >= self.config.reconnect_reset_s
        )
        session_id = self.ground_station_session_id
        had_handshake = self._handshake_ready
        if self._socket is not None:
            try:
                self._socket.close()
            except OSError:
                pass

        self._socket = None
        self._connecting = False
        self._parser = FramedJsonParser()
        self._outbound.clear()
        self._handshake_ready = False
        self._connected_at = None
        self._last_rx_at = None
        self._next_heartbeat_at = None
        self._last_state_revision = 0
        self.ground_station_session_id = None
        if had_handshake:
            self.command_dispatcher.end_session(session_id)

        if reconnect:
            if stable_connection:
                self._reconnect_delay_s = self.config.reconnect_initial_s
            self._next_connect_at = now + self._reconnect_delay_s
            self._reconnect_delay_s = min(
                self.config.reconnect_max_s,
                self._reconnect_delay_s * 2.0,
            )
        if was_active:
            self.logger.warning("ESP32 TCP connection closed: %s", reason)
            self._set_connection_state("DOWN")

    def _begin_connect(self, now):
        self.stats["connect_attempts"] += 1
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setblocking(False)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        result = sock.connect_ex((self.config.host, self.config.port))
        if result in (0, errno.EISCONN):
            self._socket = sock
            self._connecting = False
            self._on_connected(now)
        elif result in (
            errno.EINPROGRESS,
            errno.EWOULDBLOCK,
            errno.EALREADY,
        ):
            self._socket = sock
            self._connecting = True
            self._connected_at = now
            self._set_connection_state("CONNECTING")
        else:
            sock.close()
            self._schedule_connect_retry(now, result)

    def _finish_connect(self):
        error_number = self._socket.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
        if error_number:
            self.close("connect failed: {}".format(error_number))
            return
        self._connecting = False
        self._on_connected(time.monotonic())

    def _on_connected(self, now):
        self._connected_at = now
        self._last_rx_at = now
        self._queue_message(
            {
                "v": PROTOCOL_VERSION,
                "type": "hello",
                "role": "nano",
                "version": self.version,
            }
        )
        self._set_connection_state("CONNECTING")
        self.logger.info(
            "connected to ESP32 TCP server at %s:%s",
            self.config.host,
            self.config.port,
        )

    def _schedule_connect_retry(self, now, error_number):
        self._next_connect_at = now + self._reconnect_delay_s
        self.logger.warning(
            "ESP32 TCP connect failed (%s); retry in %.1f s",
            error_number,
            self._reconnect_delay_s,
        )
        self._reconnect_delay_s = min(
            self.config.reconnect_max_s,
            self._reconnect_delay_s * 2.0,
        )

    def _read_socket(self):
        try:
            data = self._socket.recv(4096)
        except BlockingIOError:
            return
        except OSError as error:
            self.close("read failed: {}".format(error))
            return

        if not data:
            self.close("peer closed connection")
            return

        try:
            messages = self._parser.feed(data)
        except FramedJsonError as error:
            self.close("invalid peer frame: {}".format(error))
            return

        for message in messages:
            if not self._handle_message(message):
                return

    def _write_socket(self):
        try:
            sent = self._socket.send(self._outbound)
        except BlockingIOError:
            return
        except OSError as error:
            self.close("write failed: {}".format(error))
            return
        if sent:
            del self._outbound[:sent]

    def _handle_message(self, message):
        if not self._handshake_ready:
            if not self._validate_hello_ack(message):
                self.close("invalid hello_ack")
                return False
            if self._had_handshake:
                self.stats["reconnect_count"] += 1
            self._had_handshake = True
            self.ground_station_session_id = message["groundStationSessionId"]
            self.command_dispatcher.begin_session(
                self.ground_station_session_id
            )
            self._handshake_ready = True
            now = time.monotonic()
            self._last_rx_at = now
            self._next_heartbeat_at = now
            self.stats["last_hello_rtt_ms"] = int(
                (now - self._connected_at) * 1000.0
            )
            self._set_connection_state("UP")
            self._queue_snapshot(self.ground_station_session_id)
            self.logger.info(
                "ESP32 hello_ack received; groundStationSessionId=%s, helloRttMs=%s",
                self.ground_station_session_id,
                self.stats["last_hello_rtt_ms"],
            )
        else:
            if not self._validate_peer_message(message):
                self.close("invalid post-handshake message")
                return False
            self._last_rx_at = time.monotonic()
            self._set_connection_state("UP")
            if message["type"] == "command":
                if not self.command_dispatcher.submit(
                    message,
                    self.ground_station_session_id,
                ):
                    self.close("invalid command fields")
                    return False
                self.logger.info(
                    "ground-station command received: session=%s id=%s cmd=%s",
                    message.get("groundStationSessionId"),
                    message.get("id"),
                    message.get("cmd"),
                )
            elif message["type"] == "get_status":
                self._queue_snapshot(self.ground_station_session_id)

        if self.on_message is not None:
            self.on_message(message)
        return True

    def _validate_hello_ack(self, message):
        return (
            message.get("v") == PROTOCOL_VERSION
            and message.get("type") == "hello_ack"
            and message.get("role") == "esp32"
            and isinstance(message.get("version"), str)
            and _is_uint32_nonzero(message.get("groundStationSessionId"))
        )

    def _validate_peer_message(self, message):
        if message.get("v") != PROTOCOL_VERSION:
            return False
        message_type = message.get("type")
        if not isinstance(message_type, str):
            return False
        if message_type == "heartbeat":
            return (
                message.get("source") == "esp32"
                and _is_uint32(message.get("uptimeMs"))
            )
        if message_type == "command":
            return True
        if message_type == "get_status":
            return (
                message.get("groundStationSessionId")
                == self.ground_station_session_id
            )
        return False

    def _queue_command_response(self):
        if self.command_dispatcher.consume_response_overflow(
            self.ground_station_session_id
        ):
            self.close("command response queue overflow")
            return False
        if self._outbound:
            return True
        response = self.command_dispatcher.get_response_nowait(
            self.ground_station_session_id
        )
        if response is not None:
            self._queue_message(response)
        return True

    def _queue_heartbeat(self, now):
        uptime_ms = int((now - self._started_at) * 1000.0)
        if not self._outbound:
            self._queue_message(
                {
                    "v": PROTOCOL_VERSION,
                    "type": "heartbeat",
                    "source": "nano",
                    "uptimeMs": uptime_ms,
                    "tiOnline": self.state_store.ti_online,
                }
            )
        self._next_heartbeat_at = now + self.config.heartbeat_interval_s

    def _queue_latest_status(self):
        if self._outbound:
            return
        revision, message = self.state_store.build_message("status")
        if revision == self._last_state_revision:
            return
        self._queue_message(message)
        self._last_state_revision = revision

    def _queue_snapshot(self, session_id=None):
        revision, message = self.state_store.build_message("snapshot")
        if session_id is not None:
            message["groundStationSessionId"] = session_id
        self._queue_message(message)
        self._last_state_revision = revision

    def _queue_message(self, message):
        self._outbound.extend(encode_message(message))

    def _poll_timeout(self, now):
        deadlines = [now + 0.1]
        if self._socket is None:
            deadlines.append(self._next_connect_at)
        elif self._connecting:
            deadlines.append(self._connected_at + self.config.hello_timeout_s)
        elif self._handshake_ready:
            deadlines.extend(
                [
                    self._next_heartbeat_at,
                    self._last_rx_at + self.config.link_down_timeout_s,
                ]
            )
        else:
            deadlines.append(self._connected_at + self.config.hello_timeout_s)
        return max(0.0, min(deadlines) - now)

    def _set_connection_state(self, state):
        if self.connection_state == state:
            return
        self.connection_state = state
        link_state = state if state in ("UP", "DEGRADED") else "DOWN"
        self.state_store.set_esp_nano_link(link_state)
        if self.on_state_change is not None:
            self.on_state_change(state)


def _is_uint32(value):
    return (
        isinstance(value, int)
        and not isinstance(value, bool)
        and 0 <= value <= 0xFFFFFFFF
    )


def _is_uint32_nonzero(value):
    return _is_uint32(value) and value != 0
