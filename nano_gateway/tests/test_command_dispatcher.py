import os
import sys
import unittest


NANO_GATEWAY_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if NANO_GATEWAY_DIR not in sys.path:
    sys.path.insert(0, NANO_GATEWAY_DIR)

from gateway.command_dispatcher import (  # noqa: E402
    MAX_PENDING_REQUESTS,
    MAX_PENDING_RESPONSES,
    CommandDispatcher,
)
from gateway.state_store import StateStore  # noqa: E402


def _command(session_id, command_id, command, args=None):
    return {
        "v": 1,
        "type": "command",
        "groundStationSessionId": session_id,
        "id": command_id,
        "cmd": command,
        "args": {} if args is None else args,
    }


class CommandDispatcherTests(unittest.TestCase):
    def setUp(self):
        self.state_store = StateStore()
        self.dispatcher = CommandDispatcher(self.state_store)
        self.dispatcher.begin_session(100)

    def test_duplicate_replays_result_without_requeue(self):
        message = _command(100, 1, "arm")
        self.assertTrue(self.dispatcher.submit(message, 100))
        request = self.dispatcher.get_request_nowait()
        self.dispatcher.accepted(request, "CALIBRATING", 51)

        self.assertTrue(self.dispatcher.submit(message, 100))
        first = self.dispatcher.get_response_nowait(100)
        replay = self.dispatcher.get_response_nowait(100)

        self.assertEqual("accepted", first["type"])
        self.assertEqual(first, replay)
        self.assertIsNone(self.dispatcher.get_request_nowait())
        self.assertNotEqual(first["id"], first["carCommandId"])

        self.dispatcher.done(request, "ARMED_READY", 51)
        self.assertEqual(
            "done",
            self.dispatcher.get_response_nowait(100)["type"],
        )
        self.assertTrue(self.dispatcher.submit(message, 100))
        self.assertEqual(
            "done",
            self.dispatcher.get_response_nowait(100)["type"],
        )

    def test_rejects_conflicting_duplicate_and_unsupported_command(self):
        original = _command(100, 1, "arm")
        self.assertTrue(self.dispatcher.submit(original, 100))
        self.assertTrue(
            self.dispatcher.submit(
                _command(100, 1, "arm", {"unexpected": 1}),
                100,
            )
        )
        conflict = self.dispatcher.get_response_nowait(100)
        self.assertEqual("INVALID_ARGUMENT", conflict["error"]["code"])

        self.assertTrue(
            self.dispatcher.submit(_command(100, 2, "manual_drive"), 100)
        )
        unsupported = self.dispatcher.get_response_nowait(100)
        self.assertEqual("UNSUPPORTED_COMMAND", unsupported["error"]["code"])

    def test_malformed_fields_close_boundary_and_session_mismatch_rejects(self):
        malformed = _command(100, 1, "arm")
        malformed["id"] = True
        self.assertFalse(self.dispatcher.submit(malformed, 100))

        self.assertTrue(
            self.dispatcher.submit(_command(99, 1, "arm"), 100)
        )
        mismatch = self.dispatcher.get_response_nowait(100)
        self.assertEqual(99, mismatch["groundStationSessionId"])
        self.assertEqual("SESSION_MISMATCH", mismatch["error"]["code"])

    def test_new_session_invalidates_queued_old_session_command(self):
        self.assertTrue(
            self.dispatcher.submit(_command(100, 1, "arm"), 100)
        )
        self.dispatcher.begin_session(200)
        stale = self.dispatcher.get_request_nowait()
        self.assertFalse(self.dispatcher.is_open(stale))

    def test_estop_clears_normal_queue_and_all_queues_are_bounded(self):
        for command_id in range(1, MAX_PENDING_REQUESTS + 2):
            self.assertTrue(
                self.dispatcher.submit(
                    _command(100, command_id, "arm"),
                    100,
                )
            )
        self.assertLessEqual(
            len(self.dispatcher._requests),
            MAX_PENDING_REQUESTS,
        )

        estop_id = MAX_PENDING_REQUESTS + 2
        self.assertTrue(
            self.dispatcher.submit(
                _command(100, estop_id, "estop"),
                100,
            )
        )
        first = self.dispatcher.get_request_nowait()
        self.assertEqual("estop", first["cmd"])
        for record in self.dispatcher._records.values():
            if record["command"] != "estop":
                self.assertNotEqual("queued", record["state"])
        self.assertLessEqual(
            self.dispatcher._responses.qsize(),
            MAX_PENDING_RESPONSES,
        )
        self.assertFalse(
            self.state_store.get_control_state()["controlEnabled"]
        )

    def test_response_overflow_is_explicit_and_never_blocks(self):
        for command_id in range(1, MAX_PENDING_RESPONSES + 2):
            self.assertTrue(
                self.dispatcher.submit(
                    _command(100, command_id, "unsupported"),
                    100,
                )
            )

        self.assertEqual(
            MAX_PENDING_RESPONSES,
            self.dispatcher._responses.qsize(),
        )
        self.assertTrue(self.dispatcher.consume_response_overflow(100))
        self.assertFalse(self.dispatcher.consume_response_overflow(100))


if __name__ == "__main__":
    unittest.main()
