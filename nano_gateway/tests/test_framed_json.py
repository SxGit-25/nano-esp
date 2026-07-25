import json
import os
import struct
import sys
import unittest


NANO_GATEWAY_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if NANO_GATEWAY_DIR not in sys.path:
    sys.path.insert(0, NANO_GATEWAY_DIR)

from gateway.framed_json import (  # noqa: E402
    FramedJsonError,
    FramedJsonParser,
    encode_message,
)


class FramedJsonParserTests(unittest.TestCase):
    def test_half_length_field_is_buffered(self):
        parser = FramedJsonParser()
        frame = encode_message({"type": "heartbeat"})

        self.assertEqual([], parser.feed(frame[:2]))
        self.assertEqual([{"type": "heartbeat"}], parser.feed(frame[2:]))

    def test_length_and_json_can_arrive_separately(self):
        parser = FramedJsonParser()
        frame = encode_message({"v": 1})

        self.assertEqual([], parser.feed(frame[:4]))
        self.assertEqual([{"v": 1}], parser.feed(frame[4:]))

    def test_json_can_arrive_in_multiple_reads(self):
        parser = FramedJsonParser()
        frame = encode_message({"text": "split message"})

        self.assertEqual([], parser.feed(frame[:7]))
        self.assertEqual([], parser.feed(frame[7:12]))
        self.assertEqual([{"text": "split message"}], parser.feed(frame[12:]))

    def test_multiple_complete_messages_are_returned_together(self):
        parser = FramedJsonParser()
        joined = encode_message({"id": 1}) + encode_message({"id": 2})

        self.assertEqual([{"id": 1}, {"id": 2}], parser.feed(joined))

    def test_zero_length_is_rejected(self):
        with self.assertRaises(FramedJsonError):
            FramedJsonParser().feed(struct.pack("<I", 0))

    def test_oversize_length_is_rejected(self):
        with self.assertRaises(FramedJsonError):
            FramedJsonParser().feed(struct.pack("<I", 4097))

    def test_invalid_utf8_is_rejected(self):
        payload = b"\xff"
        with self.assertRaises(FramedJsonError):
            FramedJsonParser().feed(struct.pack("<I", len(payload)) + payload)

    def test_invalid_json_is_rejected(self):
        payload = b"{not-json}"
        with self.assertRaises(FramedJsonError):
            FramedJsonParser().feed(struct.pack("<I", len(payload)) + payload)

    def test_non_object_json_root_is_rejected(self):
        payload = json.dumps(["not", "an", "object"]).encode("utf-8")
        with self.assertRaises(FramedJsonError):
            FramedJsonParser().feed(struct.pack("<I", len(payload)) + payload)


if __name__ == "__main__":
    unittest.main()
