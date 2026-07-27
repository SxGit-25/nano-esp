#pragma once

#include <stddef.h>
#include <stdint.h>

enum class TransmitPriority : uint8_t {
    HANDSHAKE = 0,
    ESTOP = 1,
    SAFETY = 2,
    NORMAL_COMMAND = 3,
    HEARTBEAT = 4,
};

enum class TransmitFrameKind : uint8_t {
    HANDSHAKE,
    COMMAND,
    GET_STATUS,
    HEARTBEAT,
};

class TransmitQueue {
public:
    static const size_t kFrameLength = 512;
    static const size_t kCapacity = 8;

    struct Frame {
        uint8_t bytes[kFrameLength];
        uint16_t length = 0;
        uint16_t offset = 0;
        uint32_t connection_generation = 0;
        uint32_t command_id = 0;
        TransmitPriority priority = TransmitPriority::HEARTBEAT;
        TransmitFrameKind kind = TransmitFrameKind::HEARTBEAT;
    };

    struct EvictedFrame {
        bool valid = false;
        uint32_t connection_generation = 0;
        uint32_t command_id = 0;
        TransmitFrameKind kind = TransmitFrameKind::HEARTBEAT;
    };

    bool enqueue(
        const uint8_t *bytes,
        size_t length,
        TransmitPriority priority,
        TransmitFrameKind kind,
        uint32_t connection_generation,
        uint32_t command_id,
        EvictedFrame *evicted
    );
    Frame *front();
    const Frame *front() const;
    void advanceFront(size_t length);
    void popFront();
    void clear();
    size_t count() const;
    bool contains(TransmitFrameKind kind) const;

private:
    size_t firstMovableIndex() const;
    size_t insertionIndex(TransmitPriority priority) const;
    bool makeRoom(
        TransmitPriority incoming_priority,
        TransmitFrameKind incoming_kind,
        EvictedFrame *evicted
    );
    void removeAt(size_t index);

    Frame frames_[kCapacity];
    size_t count_ = 0;
};
