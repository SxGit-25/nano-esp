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
    DRIVE_FORWARD_500,
    DRIVE_BACKWARD_500,
    TURN_LEFT_90,
    TURN_RIGHT_90,
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
        case GroundStationCommand::DRIVE_FORWARD_500:
            return "drive_distance";
        case GroundStationCommand::DRIVE_BACKWARD_500:
            return "drive_distance";
        case GroundStationCommand::TURN_LEFT_90:
        case GroundStationCommand::TURN_RIGHT_90:
            return "turn_relative";
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
        case GroundStationCommand::DRIVE_FORWARD_500:
            return "FORWARD_500";
        case GroundStationCommand::DRIVE_BACKWARD_500:
            return "BACKWARD_500";
        case GroundStationCommand::TURN_LEFT_90:
            return "LEFT_90";
        case GroundStationCommand::TURN_RIGHT_90:
            return "RIGHT_90";
    }
    return "UNKNOWN";
}
