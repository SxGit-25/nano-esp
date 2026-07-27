#pragma once

#include <stdint.h>

inline bool shouldApplyCommandResultToDisplay(
    bool display_valid,
    uint32_t display_session_id,
    uint32_t display_command_id,
    uint32_t incoming_session_id,
    uint32_t incoming_command_id
) {
    return display_valid &&
        display_session_id == incoming_session_id &&
        display_command_id == incoming_command_id;
}

inline bool shouldAcceptCommandResultTransition(bool command_is_terminal) {
    return !command_is_terminal;
}
