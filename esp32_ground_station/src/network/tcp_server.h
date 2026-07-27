#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "../commands/ground_station_command.h"
#include "framed_json.h"
#include "transmit_queue.h"

enum class NanoLinkState {
    DOWN,
    CONNECTING,
    UP,
    DEGRADED,
};

struct NanoVehicleStatus {
    bool valid = false;
    bool armed = false;
    bool has_control_enabled = false;
    bool control_enabled = false;
    bool has_active_command_id = false;
    bool has_distance_mm = false;
    bool has_heading_mdeg = false;
    bool has_line_error_x100 = false;
    bool has_progress_percent = false;
    bool has_lap_count = false;
    bool has_segment_index = false;
    uint32_t timestamp_ms = 0;
    uint32_t active_command_id = 0;
    int32_t distance_mm = 0;
    int32_t heading_mdeg = 0;
    int16_t line_error_x100 = 0;
    uint8_t progress_percent = 0;
    uint8_t lap_count = 0;
    uint8_t segment_index = 0;
    char system_state[20] = {};
    char fault_code[32] = {};
};

enum class NanoCommandResultState : uint8_t {
    NONE,
    QUEUED,
    ACCEPTED,
    REJECTED,
    DONE,
    FAILED,
    LOCAL_REJECTED,
    SESSION_ENDED,
};

inline const char *nanoCommandResultStateName(NanoCommandResultState state) {
    switch (state) {
        case NanoCommandResultState::QUEUED:
            return "SENT";
        case NanoCommandResultState::ACCEPTED:
            return "ACCEPTED";
        case NanoCommandResultState::REJECTED:
            return "REJECTED";
        case NanoCommandResultState::DONE:
            return "DONE";
        case NanoCommandResultState::FAILED:
            return "FAILED";
        case NanoCommandResultState::LOCAL_REJECTED:
            return "REJECTED";
        case NanoCommandResultState::SESSION_ENDED:
            return "FAILED";
        case NanoCommandResultState::NONE:
            break;
    }
    return "--";
}

struct NanoCommandResult {
    bool valid = false;
    bool has_car_command_id = false;
    uint32_t ground_station_session_id = 0;
    uint32_t id = 0;
    uint32_t car_command_id = 0;
    GroundStationCommand command = GroundStationCommand::GET_STATUS;
    NanoCommandResultState state = NanoCommandResultState::NONE;
    char detail[64] = {};
};

class GroundStationTcpServer {
public:
    explicit GroundStationTcpServer(uint16_t port);

    void begin();
    void poll();

    NanoLinkState linkState() const;
    bool tiOnline() const;
    const NanoVehicleStatus &vehicleStatus() const;
    const NanoCommandResult &commandResult() const;
    uint32_t groundStationSessionId() const;
    bool requestCommand(GroundStationCommand command);

private:
    static const size_t kReceiveChunkLength = 256;
    static const size_t kCommandHistoryLength = 16;

    struct IssuedCommand {
        bool valid = false;
        bool terminal = false;
        uint32_t connection_generation = 0;
        uint32_t id = 0;
        GroundStationCommand command = GroundStationCommand::ARM;
    };

    void acceptClient();
    void readClient();
    void flushTransmitQueue();
    void handleMessage(const String &json);
    bool handleStatus(JsonObject object);
    bool handleCommandResult(JsonObject object, const char *type);
    void queueHelloAck();
    void queueHeartbeat();
    bool queueGetStatus();
    bool queueJson(
        const JsonDocument &document,
        TransmitPriority priority,
        TransmitFrameKind kind,
        uint32_t command_id = 0
    );
    bool hasCommandHistorySlot() const;
    bool addIssuedCommand(uint32_t id, GroundStationCommand command);
    IssuedCommand *findIssuedCommand(uint32_t id);
    void markEvictedCommand(const TransmitQueue::EvictedFrame &evicted);
    void setQueuedResult(uint32_t id, GroundStationCommand command);
    void setLocalRejectedResult(
        uint32_t id,
        GroundStationCommand command,
        const char *reason
    );
    void endCommandSession();
    void closeClient(const char *reason);
    void updateLinkTimeout();
    void setLinkState(NanoLinkState state);
    void setTiOnline(bool online);

    WiFiServer server_;
    WiFiClient client_;
    FramedJsonReader reader_;
    TransmitQueue transmit_queue_;
    IssuedCommand command_history_[kCommandHistoryLength];
    size_t next_command_history_index_ = 0;
    bool hello_complete_ = false;
    bool pending_status_request_ = false;
    uint32_t ground_station_session_id_ = 0;
    uint32_t next_command_id_ = 1;
    uint32_t connection_generation_ = 0;
    uint32_t pending_status_request_generation_ = 0;
    uint32_t connected_at_ms_ = 0;
    uint32_t last_valid_message_ms_ = 0;
    uint32_t next_heartbeat_ms_ = 0;
    uint32_t last_transmit_progress_ms_ = 0;
    NanoLinkState link_state_ = NanoLinkState::DOWN;
    bool ti_online_ = false;
    NanoVehicleStatus vehicle_status_;
    NanoCommandResult command_result_;
};
