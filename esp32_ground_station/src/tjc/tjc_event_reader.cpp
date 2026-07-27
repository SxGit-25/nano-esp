#include "tjc_event_reader.h"

TjcEventReader::Result TjcEventReader::append(uint8_t byte) {
    if (frame_ready_) {
        frame_length_ = 0;
        frame_ready_ = false;
    }
    if (byte == 0xFF) {
        ++terminator_length_;
        if (terminator_length_ < 3) {
            return Result::NONE;
        }

        const Result result = discarding_ ? Result::DISCARDED : Result::FRAME;
        terminator_length_ = 0;
        discarding_ = false;
        frame_ready_ = true;
        return result;
    }

    while (terminator_length_ > 0) {
        if (!appendDataByte(0xFF)) {
            discarding_ = true;
        }
        --terminator_length_;
    }
    if (!appendDataByte(byte)) {
        discarding_ = true;
    }
    return Result::NONE;
}

const uint8_t *TjcEventReader::frame() const {
    return frame_;
}

size_t TjcEventReader::frameLength() const {
    return frame_length_;
}

void TjcEventReader::reset() {
    frame_length_ = 0;
    terminator_length_ = 0;
    discarding_ = false;
    frame_ready_ = false;
}

bool TjcEventReader::appendDataByte(uint8_t byte) {
    if (discarding_) {
        return false;
    }
    if (frame_length_ == kMaxFrameLength) {
        return false;
    }
    frame_[frame_length_++] = byte;
    return true;
}
