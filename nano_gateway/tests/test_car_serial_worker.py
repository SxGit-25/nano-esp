import os
import sys
import threading
import unittest


NANO_GATEWAY_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if NANO_GATEWAY_DIR not in sys.path:
    sys.path.insert(0, NANO_GATEWAY_DIR)

from gateway.car_serial_worker import CarSerialWorker  # noqa: E402


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


class CarSerialWorkerTests(unittest.TestCase):
    def test_single_thread_publishes_read_only_status(self):
        link = _FakeLink()
        snapshots = []
        status_seen = threading.Event()
        stop_event = threading.Event()

        def publish(snapshot):
            snapshots.append(snapshot)
            if snapshot["links"]["nanoTi"] == "UP":
                status_seen.set()

        worker = CarSerialWorker(
            port="/dev/fake",
            link_factory=lambda **kwargs: link,
            status_callback=publish,
            heartbeat_interval_s=0.01,
            status_interval_s=0.02,
            reconnect_delay_s=0.01,
        )
        thread = threading.Thread(target=worker.run, args=(stop_event,))
        thread.start()
        try:
            self.assertTrue(status_seen.wait(1.0))
            self.assertTrue(link.heartbeat_seen.wait(1.0))
        finally:
            stop_event.set()
            thread.join(1.0)

        online = next(
            snapshot
            for snapshot in snapshots
            if snapshot["links"]["nanoTi"] == "UP"
        )
        self.assertEqual("EXECUTING", online["systemState"])
        self.assertTrue(online["armed"])
        self.assertEqual(42, online["activeCommandId"])
        self.assertEqual("NONE", online["faultCode"])
        self.assertEqual(-8, online["lineErrorX100"])
        self.assertEqual(123, online["tiBootId"])
        self.assertIn("heartbeat", link.calls)
        self.assertIn("get_status", link.calls)
        self.assertEqual(1, len(link.thread_ids))
        self.assertNotIn("arm", link.calls)
        self.assertNotIn("drive_distance", link.calls)


if __name__ == "__main__":
    unittest.main()
