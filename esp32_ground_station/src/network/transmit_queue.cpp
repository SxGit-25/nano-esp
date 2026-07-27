#include "transmit_queue.h"

#include <string.h>

bool TransmitQueue::enqueue(
    const uint8_t *bytes,
    size_t length,
    TransmitPriority priority,
    TransmitFrameKind kind,
    uint32_t connection_generation,
    uint32_t command_id,
    EvictedFrame *evicted
) {
    if (evicted != nullptr) {
        *evicted = EvictedFrame();
    }
    if (bytes == nullptr || length == 0 || length > kFrameLength) {
        return false;
    }
    if (count_ == kCapacity &&
        !makeRoom(priority, kind, evicted)) {
        return false;
    }

    const size_t insert_at = insertionIndex(priority);
    for (size_t index = count_; index > insert_at; --index) {
        frames_[index] = frames_[index - 1];
    }

    Frame &frame = frames_[insert_at];
    memcpy(frame.bytes, bytes, length);
    frame.length = static_cast<uint16_t>(length);
    frame.offset = 0;
    frame.connection_generation = connection_generation;
    frame.command_id = command_id;
    frame.priority = priority;
    frame.kind = kind;
    ++count_;
    return true;
}

TransmitQueue::Frame *TransmitQueue::front() {
    return count_ == 0 ? nullptr : &frames_[0];
}

const TransmitQueue::Frame *TransmitQueue::front() const {
    return count_ == 0 ? nullptr : &frames_[0];
}

void TransmitQueue::advanceFront(size_t length) {
    if (count_ == 0) {
        return;
    }
    const size_t remaining = frames_[0].length - frames_[0].offset;
    frames_[0].offset += static_cast<uint16_t>(
        length > remaining ? remaining : length
    );
}

void TransmitQueue::popFront() {
    if (count_ == 0) {
        return;
    }
    removeAt(0);
}

void TransmitQueue::clear() {
    count_ = 0;
}

size_t TransmitQueue::count() const {
    return count_;
}

bool TransmitQueue::contains(TransmitFrameKind kind) const {
    for (size_t index = 0; index < count_; ++index) {
        if (frames_[index].kind == kind) {
            return true;
        }
    }
    return false;
}

size_t TransmitQueue::firstMovableIndex() const {
    /*
     * A partially transmitted length-prefixed frame must remain first. Moving
     * another frame ahead of it would corrupt the byte stream at the peer.
     */
    return count_ > 0 && frames_[0].offset > 0 ? 1 : 0;
}

size_t TransmitQueue::insertionIndex(TransmitPriority priority) const {
    size_t index = firstMovableIndex();
    while (index < count_ &&
           static_cast<uint8_t>(frames_[index].priority) <=
               static_cast<uint8_t>(priority)) {
        ++index;
    }
    return index;
}

bool TransmitQueue::makeRoom(
    TransmitPriority incoming_priority,
    TransmitFrameKind incoming_kind,
    EvictedFrame *evicted
) {
    const size_t first_movable = firstMovableIndex();
    size_t candidate = count_;
    for (size_t index = count_; index > first_movable; --index) {
        const Frame &frame = frames_[index - 1];
        if (frame.kind == TransmitFrameKind::HANDSHAKE) {
            continue;
        }
        const bool lower_priority =
            static_cast<uint8_t>(frame.priority) >
            static_cast<uint8_t>(incoming_priority);
        const bool replace_estop =
            incoming_kind == TransmitFrameKind::COMMAND &&
            incoming_priority == TransmitPriority::ESTOP &&
            frame.priority == TransmitPriority::ESTOP;
        if (lower_priority || replace_estop) {
            candidate = index - 1;
            break;
        }
    }
    if (candidate == count_) {
        return false;
    }

    if (evicted != nullptr) {
        evicted->valid = true;
        evicted->connection_generation =
            frames_[candidate].connection_generation;
        evicted->command_id = frames_[candidate].command_id;
        evicted->kind = frames_[candidate].kind;
    }
    removeAt(candidate);
    return true;
}

void TransmitQueue::removeAt(size_t index) {
    if (index >= count_) {
        return;
    }
    for (size_t next = index + 1; next < count_; ++next) {
        frames_[next - 1] = frames_[next];
    }
    --count_;
}
