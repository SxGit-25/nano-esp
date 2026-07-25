#pragma once

#include <Arduino.h>

class FramedJsonReader {
public:
    static const size_t kMaxJsonLength = 4096;

    enum class Result {
        NONE,
        MESSAGE,
        INVALID_LENGTH,
        INVALID_UTF8,
    };

    bool append(const uint8_t *data, size_t length);
    Result next(String &json);
    void reset();

private:
    static bool isValidUtf8(const uint8_t *data, size_t length);

    uint8_t buffer_[kMaxJsonLength + 4];
    size_t buffered_length_ = 0;
};
