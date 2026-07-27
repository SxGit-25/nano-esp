#pragma once

#include <stddef.h>
#include <stdint.h>

enum class GroundStationCommand : uint8_t {
    ARM,
    DISARM,
    STOP,
    ESTOP,
    CLEAR_FAULT,
    GET_STATUS,
};

inline const char *groundStationCommandName(GroundStationCommand command) {
    switch (command) {
        case GroundStationCommand::ARM:
            return "arm";
        case GroundStationCommand::DISARM:
            return "disarm";
        case GroundStationCommand::STOP:
            return "stop";
        case GroundStationCommand::ESTOP:
            return "estop";
        case GroundStationCommand::CLEAR_FAULT:
            return "clear_fault";
        case GroundStationCommand::GET_STATUS:
            return "get_status";
    }
    return "unknown";
}

inline const char *groundStationCommandDisplayName(
    GroundStationCommand command
) {
    switch (command) {
        case GroundStationCommand::ARM:
            return "ARM";
        case GroundStationCommand::DISARM:
            return "DISARM";
        case GroundStationCommand::STOP:
            return "STOP";
        case GroundStationCommand::ESTOP:
            return "ESTOP";
        case GroundStationCommand::CLEAR_FAULT:
            return "CLEAR_FAULT";
        case GroundStationCommand::GET_STATUS:
            return "GET_STATUS";
    }
    return "UNKNOWN";
}
