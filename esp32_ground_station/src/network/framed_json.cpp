#include "framed_json.h"

#include <string.h>

bool FramedJsonReader::append(const uint8_t *data, size_t length) {
    if (length > sizeof(buffer_) - buffered_length_) {
        return false;
    }
    memcpy(buffer_ + buffered_length_, data, length);
    buffered_length_ += length;
    return true;
}

FramedJsonReader::Result FramedJsonReader::next(String &json) {
    if (buffered_length_ < 4) {
        return Result::NONE;
    }

    const uint32_t payload_length =
        static_cast<uint32_t>(buffer_[0]) |
        (static_cast<uint32_t>(buffer_[1]) << 8) |
        (static_cast<uint32_t>(buffer_[2]) << 16) |
        (static_cast<uint32_t>(buffer_[3]) << 24);
    if (payload_length == 0 || payload_length > kMaxJsonLength) {
        return Result::INVALID_LENGTH;
    }

    const size_t frame_length = 4 + static_cast<size_t>(payload_length);
    if (buffered_length_ < frame_length) {
        return Result::NONE;
    }
    if (!isValidUtf8(buffer_ + 4, payload_length)) {
        return Result::INVALID_UTF8;
    }

    json = "";
    if (!json.reserve(payload_length)) {
        return Result::INVALID_LENGTH;
    }
    for (size_t index = 0; index < payload_length; ++index) {
        json += static_cast<char>(buffer_[4 + index]);
    }

    const size_t remaining_length = buffered_length_ - frame_length;
    if (remaining_length > 0) {
        memmove(buffer_, buffer_ + frame_length, remaining_length);
    }
    buffered_length_ = remaining_length;
    return Result::MESSAGE;
}

void FramedJsonReader::reset() {
    buffered_length_ = 0;
}

bool FramedJsonReader::isValidUtf8(const uint8_t *data, size_t length) {
    size_t index = 0;
    while (index < length) {
        const uint8_t first = data[index++];
        if (first <= 0x7F) {
            continue;
        }

        uint8_t continuation_count = 0;
        uint8_t second_minimum = 0x80;
        uint8_t second_maximum = 0xBF;
        if (first >= 0xC2 && first <= 0xDF) {
            continuation_count = 1;
        } else if (first == 0xE0) {
            continuation_count = 2;
            second_minimum = 0xA0;
        } else if (first >= 0xE1 && first <= 0xEC) {
            continuation_count = 2;
        } else if (first == 0xED) {
            continuation_count = 2;
            second_maximum = 0x9F;
        } else if (first >= 0xEE && first <= 0xEF) {
            continuation_count = 2;
        } else if (first == 0xF0) {
            continuation_count = 3;
            second_minimum = 0x90;
        } else if (first >= 0xF1 && first <= 0xF3) {
            continuation_count = 3;
        } else if (first == 0xF4) {
            continuation_count = 3;
            second_maximum = 0x8F;
        } else {
            return false;
        }

        if (index + continuation_count > length) {
            return false;
        }
        const uint8_t second = data[index++];
        if (second < second_minimum || second > second_maximum) {
            return false;
        }
        for (uint8_t count = 1; count < continuation_count; ++count) {
            const uint8_t continuation = data[index++];
            if (continuation < 0x80 || continuation > 0xBF) {
                return false;
            }
        }
    }
    return true;
}
