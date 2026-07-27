import os
import sys
import unittest


NANO_GATEWAY_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if NANO_GATEWAY_DIR not in sys.path:
    sys.path.insert(0, NANO_GATEWAY_DIR)

from gateway.state_store import StateStore  # noqa: E402


class _Clock(object):
    def __init__(self):
        self.now = 10.0

    def __call__(self):
        return self.now


def _running_status():
    return {
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
    }


class StateStoreTests(unittest.TestCase):
    def test_snapshot_starts_disconnected(self):
        clock = _Clock()
        store = StateStore(clock=clock)
        clock.now = 10.125

        revision, snapshot = store.build_message("snapshot")

        self.assertEqual(1, revision)
        self.assertEqual("snapshot", snapshot["type"])
        self.assertEqual(125, snapshot["timestampMs"])
        self.assertEqual("DISCONNECTED", snapshot["systemState"])
        self.assertFalse(snapshot["controlEnabled"])
        self.assertIsNone(snapshot["activeCommandId"])
        self.assertIsNone(snapshot["activeCarCommandId"])
        self.assertEqual("DOWN", snapshot["links"]["espNano"])
        self.assertEqual("DOWN", snapshot["links"]["nanoTi"])
        self.assertIsNone(snapshot["leftWheelSpeedMmPerSec"])

    def test_status_update_and_disconnect_are_coherent(self):
        store = StateStore()
        store.set_esp_nano_link("UP")
        store.update_from_mspm0(
            _running_status(),
            {"boot_id": 123, "capability_flags": 0x55},
        )

        online_revision, online = store.build_message("status")
        self.assertEqual("EXECUTING", online["systemState"])
        self.assertTrue(online["armed"])
        self.assertFalse(online["controlEnabled"])
        self.assertIsNone(online["activeCommandId"])
        self.assertEqual(42, online["activeCarCommandId"])
        self.assertEqual(-8, online["lineErrorX100"])
        self.assertEqual("UP", online["links"]["nanoTi"])

        store.mark_ti_disconnected()
        disconnected_revision, disconnected = store.build_message("status")
        self.assertGreater(disconnected_revision, online_revision)
        self.assertEqual("DISCONNECTED", disconnected["systemState"])
        self.assertFalse(disconnected["armed"])
        self.assertIsNone(disconnected["distanceMm"])
        self.assertEqual("DOWN", disconnected["links"]["nanoTi"])

    def test_control_admission_is_distinct_from_physical_armed_state(self):
        store = StateStore()
        store.update_from_mspm0(
            _running_status(),
            {"boot_id": 123, "capability_flags": 0x55},
        )
        store.set_control_enabled(True)
        store.map_active_command(9, 42)

        _, status = store.build_message("status")
        self.assertTrue(status["armed"])
        self.assertTrue(status["controlEnabled"])
        self.assertEqual(9, status["activeCommandId"])
        self.assertEqual(42, status["activeCarCommandId"])

        store.set_control_enabled(False)
        _, disarmed = store.build_message("status")
        self.assertTrue(disarmed["armed"])
        self.assertFalse(disarmed["controlEnabled"])


if __name__ == "__main__":
    unittest.main()
