"""Single-owner read-only worker for the Nano--MSPM0 serial link."""

import importlib
import logging
import os
import sys
import time


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
        state_store,
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
        self.state_store = state_store
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
                self.state_store.update_from_mspm0(status, hello)
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
                self.state_store.update_from_mspm0(status, self._hello)
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
        self.state_store.mark_ti_disconnected()

    def _uptime_ms(self, now=None):
        if self._started_at is None:
            return 0
        current = now if now is not None else time.monotonic()
        return int((current - self._started_at) * 1000.0) & 0xFFFFFFFF
