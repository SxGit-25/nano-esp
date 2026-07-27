#pragma once

#include <stddef.h>
#include <stdint.h>

class TjcEventReader {
public:
    static const size_t kMaxFrameLength = 48;

    enum class Result {
        NONE,
        FRAME,
        DISCARDED,
    };

    Result append(uint8_t byte);
    const uint8_t *frame() const;
    size_t frameLength() const;
    void reset();

private:
    bool appendDataByte(uint8_t byte);

    uint8_t frame_[kMaxFrameLength];
    size_t frame_length_ = 0;
    uint8_t terminator_length_ = 0;
    bool discarding_ = false;
    bool frame_ready_ = false;
};
