#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/commands/command_result_policy.h"
#include "../src/commands/ground_station_command.h"
#include "../src/network/transmit_queue.h"
#include "../src/tjc/tjc_event_reader.h"

namespace {

void testCommandNames() {
    assert(
        strcmp(
            groundStationCommandName(GroundStationCommand::CLEAR_FAULT),
            "clear_fault"
        ) == 0
    );
    assert(
        strcmp(
            groundStationCommandName(GroundStationCommand::GET_STATUS),
            "get_status"
        ) == 0
    );
    assert(
        strcmp(
            groundStationCommandDisplayName(
                GroundStationCommand::CLEAR_FAULT
            ),
            "CLEAR_FAULT"
        ) == 0
    );
}

void testCommandResultPolicy() {
    assert(shouldApplyCommandResultToDisplay(
        true,
        100,
        9,
        100,
        9
    ));
    assert(!shouldApplyCommandResultToDisplay(
        true,
        100,
        10,
        100,
        9
    ));
    assert(!shouldApplyCommandResultToDisplay(
        true,
        101,
        9,
        100,
        9
    ));
    assert(shouldAcceptCommandResultTransition(false));
    assert(!shouldAcceptCommandResultTransition(true));
}

void testTjcFrames() {
    TjcEventReader reader;
    const uint8_t arm[] = {
        'G', 'S', ':', 'A', 'R', 'M', 0xFF, 0xFF, 0xFF,
    };
    TjcEventReader::Result result = TjcEventReader::Result::NONE;
    for (size_t index = 0; index < sizeof(arm); ++index) {
        result = reader.append(arm[index]);
    }
    assert(result == TjcEventReader::Result::FRAME);
    assert(reader.frameLength() == 6);
    assert(memcmp(reader.frame(), "GS:ARM", 6) == 0);

    const uint8_t sync[] = {
        'G', 'S', ':', 'S', 'Y', 'N', 'C', 0xFF, 0xFF, 0xFF,
    };
    for (size_t index = 0; index < sizeof(sync); ++index) {
        result = reader.append(sync[index]);
    }
    assert(result == TjcEventReader::Result::FRAME);
    assert(reader.frameLength() == 7);
    assert(memcmp(reader.frame(), "GS:SYNC", 7) == 0);

    for (size_t index = 0; index <= TjcEventReader::kMaxFrameLength; ++index) {
        result = reader.append('X');
    }
    assert(result == TjcEventReader::Result::NONE);
    result = reader.append(0xFF);
    assert(result == TjcEventReader::Result::NONE);
    result = reader.append(0xFF);
    assert(result == TjcEventReader::Result::NONE);
    result = reader.append(0xFF);
    assert(result == TjcEventReader::Result::DISCARDED);
}

void enqueueByte(
    TransmitQueue &queue,
    uint8_t value,
    TransmitPriority priority,
    TransmitFrameKind kind,
    uint32_t command_id = 0
) {
    TransmitQueue::EvictedFrame evicted;
    assert(queue.enqueue(
        &value,
        1,
        priority,
        kind,
        7,
        command_id,
        &evicted
    ));
}

void testTransmitPriority() {
    TransmitQueue queue;
    enqueueByte(
        queue,
        'N',
        TransmitPriority::NORMAL_COMMAND,
        TransmitFrameKind::COMMAND,
        1
    );
    enqueueByte(
        queue,
        'H',
        TransmitPriority::HEARTBEAT,
        TransmitFrameKind::HEARTBEAT
    );
    enqueueByte(
        queue,
        'S',
        TransmitPriority::SAFETY,
        TransmitFrameKind::COMMAND,
        2
    );
    enqueueByte(
        queue,
        'E',
        TransmitPriority::ESTOP,
        TransmitFrameKind::COMMAND,
        3
    );
    enqueueByte(
        queue,
        'A',
        TransmitPriority::HANDSHAKE,
        TransmitFrameKind::HANDSHAKE
    );

    const char expected[] = {'A', 'E', 'S', 'N', 'H'};
    for (size_t index = 0; index < sizeof(expected); ++index) {
        assert(queue.front() != nullptr);
        assert(queue.front()->bytes[0] == expected[index]);
        queue.popFront();
    }
    assert(queue.front() == nullptr);
}

void testPartialFrameIsNeverPreempted() {
    TransmitQueue queue;
    const uint8_t normal[] = {'N', 'N'};
    TransmitQueue::EvictedFrame evicted;
    assert(queue.enqueue(
        normal,
        sizeof(normal),
        TransmitPriority::NORMAL_COMMAND,
        TransmitFrameKind::COMMAND,
        1,
        1,
        &evicted
    ));
    queue.advanceFront(1);
    enqueueByte(
        queue,
        'E',
        TransmitPriority::ESTOP,
        TransmitFrameKind::COMMAND,
        2
    );
    assert(queue.front()->bytes[0] == 'N');
    assert(queue.front()->offset == 1);
    queue.advanceFront(1);
    queue.popFront();
    assert(queue.front()->bytes[0] == 'E');
}

void testEstopEvictsOnlyUnsentLowerPriorityFrame() {
    TransmitQueue queue;
    for (uint32_t id = 1; id <= TransmitQueue::kCapacity; ++id) {
        enqueueByte(
            queue,
            static_cast<uint8_t>(id),
            TransmitPriority::NORMAL_COMMAND,
            TransmitFrameKind::COMMAND,
            id
        );
    }

    const uint8_t estop = 'E';
    TransmitQueue::EvictedFrame evicted;
    assert(queue.enqueue(
        &estop,
        1,
        TransmitPriority::ESTOP,
        TransmitFrameKind::COMMAND,
        7,
        99,
        &evicted
    ));
    assert(evicted.valid);
    assert(evicted.kind == TransmitFrameKind::COMMAND);
    assert(evicted.command_id == TransmitQueue::kCapacity);
    assert(queue.count() == TransmitQueue::kCapacity);
    assert(queue.front()->bytes[0] == 'E');
}

}  // namespace

int main() {
    testCommandNames();
    testCommandResultPolicy();
    testTjcFrames();
    testTransmitPriority();
    testPartialFrameIsNeverPreempted();
    testEstopEvictsOnlyUnsentLowerPriorityFrame();
    return 0;
}
