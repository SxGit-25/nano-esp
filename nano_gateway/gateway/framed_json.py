"""Length-prefixed JSON framing for the ESP32--Nano TCP connection.

The transport is a TCP byte stream. This module accepts fragmented and
coalesced reads and produces only UTF-8 JSON objects.
"""

import json
import struct


MIN_JSON_LENGTH = 1
MAX_JSON_LENGTH = 4096
LENGTH_PREFIX_SIZE = 4


class FramedJsonError(ValueError):
    """Raised when a peer sends an invalid framed JSON message."""


def encode_message(message):
    """Return one little-endian length-prefixed JSON object frame."""

    if not isinstance(message, dict):
        raise FramedJsonError("JSON root must be an object")

    try:
        encoded = json.dumps(
            message,
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        raise FramedJsonError("message cannot be JSON encoded: {}".format(error))

    if not MIN_JSON_LENGTH <= len(encoded) <= MAX_JSON_LENGTH:
        raise FramedJsonError("JSON length must be within 1..4096 bytes")
    return struct.pack("<I", len(encoded)) + encoded


class FramedJsonParser(object):
    """Incrementally parse little-endian length-prefixed JSON objects."""

    def __init__(self):
        self._buffer = bytearray()

    def feed(self, data):
        """Consume bytes and return every complete JSON object received."""

        if data:
            self._buffer.extend(bytearray(data))

        messages = []
        while len(self._buffer) >= LENGTH_PREFIX_SIZE:
            payload_length = struct.unpack_from("<I", self._buffer, 0)[0]
            if not MIN_JSON_LENGTH <= payload_length <= MAX_JSON_LENGTH:
                raise FramedJsonError("JSON length must be within 1..4096 bytes")

            frame_length = LENGTH_PREFIX_SIZE + payload_length
            if len(self._buffer) < frame_length:
                break

            payload = bytes(self._buffer[LENGTH_PREFIX_SIZE:frame_length])
            del self._buffer[:frame_length]

            try:
                decoded = payload.decode("utf-8")
            except UnicodeDecodeError as error:
                raise FramedJsonError("JSON payload is not valid UTF-8") from error

            try:
                message = json.loads(decoded)
            except ValueError as error:
                raise FramedJsonError("JSON payload cannot be parsed") from error
            if not isinstance(message, dict):
                raise FramedJsonError("JSON root must be an object")
            messages.append(message)

        return messages
