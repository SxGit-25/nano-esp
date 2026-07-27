#include "tcp_server.h"

#include <errno.h>
#include <esp_system.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "../commands/command_result_policy.h"

GroundStationTcpServer::GroundStationTcpServer(uint16_t port) : server_(port) {
}

void GroundStationTcpServer::begin() {
    ground_station_session_id_ = esp_random();
    if (ground_station_session_id_ == 0) {
        ground_station_session_id_ = 1;
    }
    next_command_id_ = 1;
    connection_generation_ = 0;
    transmit_queue_.clear();
    server_.begin();
    Serial.printf(
        "TCP server listening on port %u, groundStationSessionId=%lu\n",
        kGroundStationTcpPort,
        static_cast<unsigned long>(ground_station_session_id_)
    );
}

void GroundStationTcpServer::poll() {
    acceptClient();
    if (!client_) {
        return;
    }
    if (!client_.connected()) {
        closeClient("peer disconnected");
        return;
    }

    readClient();
    if (!client_) {
        return;
    }
    updateLinkTimeout();
    if (!client_) {
        return;
    }
    flushTransmitQueue();
}

NanoLinkState GroundStationTcpServer::linkState() const {
    return link_state_;
}

bool GroundStationTcpServer::tiOnline() const {
    return ti_online_;
}

const NanoVehicleStatus &GroundStationTcpServer::vehicleStatus() const {
    return vehicle_status_;
}

const NanoCommandResult &GroundStationTcpServer::commandResult() const {
    return command_result_;
}

uint32_t GroundStationTcpServer::groundStationSessionId() const {
    return ground_station_session_id_;
}

bool GroundStationTcpServer::requestCommand(
    GroundStationCommand command
) {
    if (command == GroundStationCommand::GET_STATUS) {
        if (!client_ || !client_.connected() || !hello_complete_) {
            setLocalRejectedResult(
                0,
                GroundStationCommand::GET_STATUS,
                "NANO_LINK_DOWN"
            );
            return false;
        }
        if (!queueGetStatus()) {
            setLocalRejectedResult(
                0,
                GroundStationCommand::GET_STATUS,
                "TX_QUEUE_FULL"
            );
            return false;
        }
        setQueuedResult(0, GroundStationCommand::GET_STATUS);
        return true;
    }
    if (!client_ || !client_.connected() || !hello_complete_) {
        setLocalRejectedResult(0, command, "NANO_LINK_DOWN");
        return false;
    }
    if (next_command_id_ == 0) {
        setLocalRejectedResult(0, command, "COMMAND_ID_EXHAUSTED");
        return false;
    }
    if (!hasCommandHistorySlot()) {
        setLocalRejectedResult(0, command, "COMMAND_HISTORY_FULL");
        return false;
    }

    const uint32_t command_id = next_command_id_;
    next_command_id_ =
        command_id == UINT32_MAX ? 0 : command_id + 1;

    StaticJsonDocument<256> message;
    message["v"] = 1;
    message["type"] = "command";
    message["groundStationSessionId"] = ground_station_session_id_;
    message["id"] = command_id;
    message["cmd"] = groundStationCommandName(command);
    JsonObject args = message.createNestedObject("args");
    if (command == GroundStationCommand::DRIVE_FORWARD_500 ||
        command == GroundStationCommand::DRIVE_BACKWARD_500) {
        args["distanceMm"] = command ==
            GroundStationCommand::DRIVE_FORWARD_500 ? 500 : -500;
        args["speedMmPerSec"] = 200;
        args["headingMode"] = 1;
        args["endBehavior"] = 0;
        args["timeoutMs"] = 6000;
    } else if (command == GroundStationCommand::TURN_LEFT_90 ||
               command == GroundStationCommand::TURN_RIGHT_90) {
        args["angleMdeg"] = command == GroundStationCommand::TURN_LEFT_90 ?
            -90000 : 90000;
        args["maxWheelSpeedMmPerSec"] = 150;
        args["turnMode"] = 0;
        args["timeoutMs"] = 5000;
    }

    TransmitPriority priority = TransmitPriority::NORMAL_COMMAND;
    if (command == GroundStationCommand::ESTOP) {
        priority = TransmitPriority::ESTOP;
    } else if (
        command == GroundStationCommand::STOP ||
        command == GroundStationCommand::DISARM ||
        command == GroundStationCommand::CLEAR_FAULT
    ) {
        priority = TransmitPriority::SAFETY;
    }

    if (!addIssuedCommand(command_id, command)) {
        setLocalRejectedResult(command_id, command, "COMMAND_HISTORY_FULL");
        return false;
    }
    if (!queueJson(
            message,
            priority,
            TransmitFrameKind::COMMAND,
            command_id
        )) {
        IssuedCommand *issued = findIssuedCommand(command_id);
        if (issued != nullptr) {
            issued->valid = false;
        }
        setLocalRejectedResult(command_id, command, "TX_QUEUE_FULL");
        return false;
    }
    setQueuedResult(command_id, command);
    Serial.printf(
        "Queued command id=%lu cmd=%s\n",
        static_cast<unsigned long>(command_id),
        groundStationCommandName(command)
    );
    return true;
}

void GroundStationTcpServer::acceptClient() {
    WiFiClient incoming = server_.available();
    if (!incoming) {
        return;
    }
    if (client_ && client_.connected()) {
        Serial.println("Rejecting additional Nano TCP client");
        incoming.stop();
        return;
    }

    client_ = incoming;
    client_.setNoDelay(true);
    reader_.reset();
    transmit_queue_.clear();
    for (size_t index = 0; index < kCommandHistoryLength; ++index) {
        command_history_[index] = IssuedCommand();
    }
    next_command_history_index_ = 0;
    pending_status_request_ = false;
    ++connection_generation_;
    if (connection_generation_ == 0) {
        ++connection_generation_;
    }
    hello_complete_ = false;
    connected_at_ms_ = millis();
    last_valid_message_ms_ = connected_at_ms_;
    next_heartbeat_ms_ = 0;
    last_transmit_progress_ms_ = connected_at_ms_;
    vehicle_status_ = NanoVehicleStatus();
    setTiOnline(false);
    setLinkState(NanoLinkState::CONNECTING);
    Serial.printf(
        "Nano TCP client connected from %s:%u\n",
        client_.remoteIP().toString().c_str(),
        client_.remotePort()
    );
}

void GroundStationTcpServer::readClient() {
    uint8_t chunk[kReceiveChunkLength];
    while (client_.available() > 0) {
        const int read_length = client_.read(
            chunk,
            min(static_cast<int>(sizeof(chunk)), client_.available())
        );
        if (read_length <= 0) {
            return;
        }
        if (!reader_.append(chunk, static_cast<size_t>(read_length))) {
            closeClient("receive buffer overflow");
            return;
        }

        while (true) {
            String json;
            const FramedJsonReader::Result result = reader_.next(json);
            if (result == FramedJsonReader::Result::NONE) {
                break;
            }
            if (result != FramedJsonReader::Result::MESSAGE) {
                closeClient("invalid length-prefixed JSON frame");
                return;
            }
            handleMessage(json);
            if (!client_) {
                return;
            }
        }
    }
}

void GroundStationTcpServer::flushTransmitQueue() {
    TransmitQueue::Frame *frame = transmit_queue_.front();
    while (
        frame != nullptr &&
        frame->connection_generation != connection_generation_
    ) {
        transmit_queue_.popFront();
        frame = transmit_queue_.front();
    }
    if (frame == nullptr) {
        return;
    }

    const size_t remaining = frame->length - frame->offset;
    const int socket_fd = client_.fd();
    if (socket_fd < 0) {
        closeClient("invalid TCP socket");
        return;
    }

    const ssize_t sent = send(
        socket_fd,
        frame->bytes + frame->offset,
        remaining,
        MSG_DONTWAIT
    );
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (millis() - last_transmit_progress_ms_ >=
                kTcpSendStallTimeoutMs) {
                closeClient("TCP transmit stalled");
            }
            return;
        }
        closeClient("TCP send failed");
        return;
    }
    if (sent == 0) {
        closeClient("TCP socket closed during send");
        return;
    }
    last_transmit_progress_ms_ = millis();
    transmit_queue_.advanceFront(static_cast<size_t>(sent));
    frame = transmit_queue_.front();
    if (frame != nullptr && frame->offset == frame->length) {
        transmit_queue_.popFront();
    }
}

void GroundStationTcpServer::handleMessage(const String &json) {
    DynamicJsonDocument document(json.length() + 1024);
    const DeserializationError error = deserializeJson(document, json);
    if (error || !document.is<JsonObject>()) {
        closeClient("invalid JSON object");
        return;
    }

    JsonObject object = document.as<JsonObject>();
    const char *type = object["type"];
    if (!object["v"].is<uint8_t>() || object["v"].as<uint8_t>() != 1 ||
        type == nullptr) {
        closeClient("invalid JSON base fields");
        return;
    }

    if (!hello_complete_) {
        const char *role = object["role"];
        const char *version = object["version"];
        if (strcmp(type, "hello") != 0 || role == nullptr ||
            strcmp(role, "nano") != 0 || version == nullptr) {
            closeClient("hello required before normal messages");
            return;
        }

        hello_complete_ = true;
        last_valid_message_ms_ = millis();
        next_heartbeat_ms_ = last_valid_message_ms_;
        setLinkState(NanoLinkState::UP);
        queueHelloAck();
        Serial.printf(
            "Nano hello accepted, groundStationSessionId=%lu\n",
            static_cast<unsigned long>(ground_station_session_id_)
        );
        return;
    }

    if (strcmp(type, "heartbeat") == 0) {
        if (!object["source"].is<const char *>() ||
            strcmp(object["source"].as<const char *>(), "nano") != 0 ||
            !object["uptimeMs"].is<uint32_t>() ||
            !object["tiOnline"].is<bool>()) {
            closeClient("invalid Nano heartbeat");
            return;
        }
        setTiOnline(object["tiOnline"].as<bool>());
        last_valid_message_ms_ = millis();
        setLinkState(NanoLinkState::UP);
        return;
    }

    if (strcmp(type, "status") == 0 || strcmp(type, "snapshot") == 0) {
        if (!handleStatus(object)) {
            closeClient("invalid Nano status");
            return;
        }
        if (
            strcmp(type, "snapshot") == 0 &&
            pending_status_request_ &&
            pending_status_request_generation_ == connection_generation_
        ) {
            pending_status_request_ = false;
            if (
                command_result_.valid &&
                command_result_.ground_station_session_id ==
                    ground_station_session_id_ &&
                command_result_.id == 0 &&
                command_result_.command ==
                    GroundStationCommand::GET_STATUS &&
                command_result_.state == NanoCommandResultState::QUEUED
            ) {
                command_result_.state = NanoCommandResultState::DONE;
                snprintf(
                    command_result_.detail,
                    sizeof(command_result_.detail),
                    "%s",
                    "SNAPSHOT_RECEIVED"
                );
            }
        }
        last_valid_message_ms_ = millis();
        setLinkState(NanoLinkState::UP);
        return;
    }

    if (
        strcmp(type, "accepted") == 0 ||
        strcmp(type, "rejected") == 0 ||
        strcmp(type, "done") == 0 ||
        strcmp(type, "failed") == 0
    ) {
        if (!handleCommandResult(object, type)) {
            closeClient("invalid Nano command result");
            return;
        }
        last_valid_message_ms_ = millis();
        setLinkState(NanoLinkState::UP);
        return;
    }

    closeClient("unexpected stage-3 message");
}

bool GroundStationTcpServer::handleStatus(JsonObject object) {
    const char *system_state = object["systemState"];
    JsonObject links = object["links"].as<JsonObject>();
    const char *nano_ti = links["nanoTi"];
    if (!object["timestampMs"].is<uint32_t>() ||
        system_state == nullptr ||
        !object["armed"].is<bool>() ||
        (object.containsKey("controlEnabled") &&
         !object["controlEnabled"].is<bool>()) ||
        links.isNull() ||
        nano_ti == nullptr ||
        (strcmp(nano_ti, "UP") != 0 && strcmp(nano_ti, "DOWN") != 0)) {
        return false;
    }

    const char *fault_code = object["faultCode"];
    if (strlen(system_state) >= sizeof(vehicle_status_.system_state) ||
        (fault_code != nullptr &&
         strlen(fault_code) >= sizeof(vehicle_status_.fault_code))) {
        return false;
    }
    const bool status_changed =
        !vehicle_status_.valid ||
        vehicle_status_.armed != object["armed"].as<bool>() ||
        vehicle_status_.has_control_enabled !=
            object["controlEnabled"].is<bool>() ||
        (
            vehicle_status_.has_control_enabled &&
            vehicle_status_.control_enabled !=
                object["controlEnabled"].as<bool>()
        ) ||
        strcmp(vehicle_status_.system_state, system_state) != 0 ||
        strcmp(
            vehicle_status_.fault_code,
            fault_code == nullptr ? "UNKNOWN" : fault_code
        ) != 0;

    NanoVehicleStatus next;
    next.valid = true;
    next.armed = object["armed"].as<bool>();
    if (object["controlEnabled"].is<bool>()) {
        next.has_control_enabled = true;
        next.control_enabled = object["controlEnabled"].as<bool>();
    }
    next.timestamp_ms = object["timestampMs"].as<uint32_t>();
    if (object["activeCommandId"].is<uint32_t>()) {
        next.has_active_command_id = true;
        next.active_command_id = object["activeCommandId"].as<uint32_t>();
    }
    if (object["distanceMm"].is<int32_t>()) {
        next.has_distance_mm = true;
        next.distance_mm = object["distanceMm"].as<int32_t>();
    }
    if (object["headingMdeg"].is<int32_t>()) {
        next.has_heading_mdeg = true;
        next.heading_mdeg = object["headingMdeg"].as<int32_t>();
    }
    if (object["lineErrorX100"].is<int16_t>()) {
        next.has_line_error_x100 = true;
        next.line_error_x100 = object["lineErrorX100"].as<int16_t>();
    }
    if (object["progressPercent"].is<uint8_t>()) {
        next.has_progress_percent = true;
        next.progress_percent = object["progressPercent"].as<uint8_t>();
    }
    if (object["lapCount"].is<uint8_t>()) {
        next.has_lap_count = true;
        next.lap_count = object["lapCount"].as<uint8_t>();
    }
    if (object["segmentIndex"].is<uint8_t>()) {
        next.has_segment_index = true;
        next.segment_index = object["segmentIndex"].as<uint8_t>();
    }
    snprintf(next.system_state, sizeof(next.system_state), "%s", system_state);
    snprintf(
        next.fault_code,
        sizeof(next.fault_code),
        "%s",
        fault_code == nullptr ? "UNKNOWN" : fault_code
    );
    vehicle_status_ = next;
    setTiOnline(strcmp(nano_ti, "UP") == 0);

    if (status_changed) {
        Serial.printf(
            "MSPM0 status: system=%s armed=%s fault=%s\n",
            vehicle_status_.system_state,
            vehicle_status_.armed ? "true" : "false",
            vehicle_status_.fault_code
        );
    }
    return true;
}

bool GroundStationTcpServer::handleCommandResult(
    JsonObject object,
    const char *type
) {
    if (
        !object["groundStationSessionId"].is<uint32_t>() ||
        object["groundStationSessionId"].as<uint32_t>() !=
            ground_station_session_id_ ||
        !object["id"].is<uint32_t>() ||
        object["id"].as<uint32_t>() == 0
    ) {
        return false;
    }

    const uint32_t command_id = object["id"].as<uint32_t>();
    IssuedCommand *issued = findIssuedCommand(command_id);
    if (
        issued == nullptr ||
        issued->connection_generation != connection_generation_
    ) {
        Serial.printf(
            "Ignoring result for unknown command id=%lu\n",
            static_cast<unsigned long>(command_id)
        );
        return true;
    }

    if (
        object.containsKey("carCommandId") &&
        !object["carCommandId"].is<uint32_t>()
    ) {
        return false;
    }

    const bool was_terminal = issued->terminal;
    NanoCommandResult next;
    next.valid = true;
    next.ground_station_session_id = ground_station_session_id_;
    next.id = command_id;
    next.command = issued->command;
    if (object["carCommandId"].is<uint32_t>()) {
        next.has_car_command_id = true;
        next.car_command_id = object["carCommandId"].as<uint32_t>();
    }

    const char *detail = nullptr;
    if (strcmp(type, "accepted") == 0) {
        detail = object["state"];
        if (detail == nullptr) {
            return false;
        }
        next.state = NanoCommandResultState::ACCEPTED;
    } else if (strcmp(type, "rejected") == 0) {
        JsonObject error_object = object["error"].as<JsonObject>();
        detail = error_object["code"];
        if (error_object.isNull() || detail == nullptr) {
            return false;
        }
        if (
            error_object.containsKey("message") &&
            !error_object["message"].is<const char *>()
        ) {
            return false;
        }
        next.state = NanoCommandResultState::REJECTED;
        issued->terminal = true;
    } else if (strcmp(type, "done") == 0) {
        detail = object["reason"];
        if (detail == nullptr) {
            return false;
        }
        next.state = NanoCommandResultState::DONE;
        issued->terminal = true;
    } else {
        detail = object["reason"];
        if (detail == nullptr) {
            return false;
        }
        if (
            object.containsKey("faultCode") &&
            !object["faultCode"].is<const char *>()
        ) {
            return false;
        }
        next.state = NanoCommandResultState::FAILED;
        issued->terminal = true;
    }

    if (strlen(detail) >= sizeof(next.detail)) {
        return false;
    }
    snprintf(next.detail, sizeof(next.detail), "%s", detail);

    if (!shouldAcceptCommandResultTransition(was_terminal)) {
        Serial.printf(
            "Ignoring late result for terminal command id=%lu\n",
            static_cast<unsigned long>(command_id)
        );
        return true;
    }
    if (!shouldApplyCommandResultToDisplay(
            command_result_.valid,
            command_result_.ground_station_session_id,
            command_result_.id,
            ground_station_session_id_,
            command_id
        )) {
        return true;
    }
    command_result_ = next;
    Serial.printf(
        "Command result id=%lu cmd=%s state=%s detail=%s\n",
        static_cast<unsigned long>(command_id),
        groundStationCommandName(issued->command),
        nanoCommandResultStateName(command_result_.state),
        command_result_.detail
    );
    return true;
}

void GroundStationTcpServer::queueHelloAck() {
    StaticJsonDocument<192> response;
    response["v"] = 1;
    response["type"] = "hello_ack";
    response["role"] = "esp32";
    response["version"] = "2.1";
    response["groundStationSessionId"] = ground_station_session_id_;
    if (!queueJson(
            response,
            TransmitPriority::HANDSHAKE,
            TransmitFrameKind::HANDSHAKE
        )) {
        closeClient("hello_ack queue full");
    }
}

void GroundStationTcpServer::queueHeartbeat() {
    StaticJsonDocument<128> message;
    message["v"] = 1;
    message["type"] = "heartbeat";
    message["source"] = "esp32";
    message["uptimeMs"] = millis();
    if (transmit_queue_.contains(TransmitFrameKind::HEARTBEAT)) {
        return;
    }
    if (!queueJson(
            message,
            TransmitPriority::HEARTBEAT,
            TransmitFrameKind::HEARTBEAT
        )) {
        Serial.println("Dropping ESP32 heartbeat because transmit queue is full");
    }
}

bool GroundStationTcpServer::queueGetStatus() {
    if (transmit_queue_.contains(TransmitFrameKind::GET_STATUS)) {
        pending_status_request_ = true;
        pending_status_request_generation_ = connection_generation_;
        return true;
    }
    StaticJsonDocument<128> message;
    message["v"] = 1;
    message["type"] = "get_status";
    message["groundStationSessionId"] = ground_station_session_id_;
    if (!queueJson(
            message,
            TransmitPriority::NORMAL_COMMAND,
            TransmitFrameKind::GET_STATUS
        )) {
        Serial.println("Dropping get_status because transmit queue is full");
        return false;
    }
    pending_status_request_ = true;
    pending_status_request_generation_ = connection_generation_;
    Serial.println("Queued get_status request");
    return true;
}

bool GroundStationTcpServer::queueJson(
    const JsonDocument &document,
    TransmitPriority priority,
    TransmitFrameKind kind,
    uint32_t command_id
) {
    uint8_t frame[TransmitQueue::kFrameLength];
    const size_t payload_length = serializeJson(
        document,
        frame + 4,
        sizeof(frame) - 4
    );
    if (payload_length == 0 || payload_length > sizeof(frame) - 4) {
        return false;
    }
    frame[0] = static_cast<uint8_t>(payload_length & 0xFF);
    frame[1] = static_cast<uint8_t>((payload_length >> 8) & 0xFF);
    frame[2] = static_cast<uint8_t>((payload_length >> 16) & 0xFF);
    frame[3] = static_cast<uint8_t>((payload_length >> 24) & 0xFF);

    const bool queue_was_empty = transmit_queue_.count() == 0;
    TransmitQueue::EvictedFrame evicted;
    if (!transmit_queue_.enqueue(
            frame,
            payload_length + 4,
            priority,
            kind,
            connection_generation_,
            command_id,
            &evicted
        )) {
        return false;
    }
    markEvictedCommand(evicted);
    if (queue_was_empty) {
        last_transmit_progress_ms_ = millis();
    }
    return true;
}

bool GroundStationTcpServer::hasCommandHistorySlot() const {
    for (size_t index = 0; index < kCommandHistoryLength; ++index) {
        if (
            !command_history_[index].valid ||
            command_history_[index].terminal
        ) {
            return true;
        }
    }
    return false;
}

bool GroundStationTcpServer::addIssuedCommand(
    uint32_t id,
    GroundStationCommand command
) {
    for (size_t offset = 0; offset < kCommandHistoryLength; ++offset) {
        const size_t index =
            (next_command_history_index_ + offset) %
            kCommandHistoryLength;
        IssuedCommand &issued = command_history_[index];
        if (issued.valid && !issued.terminal) {
            continue;
        }
        issued.valid = true;
        issued.terminal = false;
        issued.connection_generation = connection_generation_;
        issued.id = id;
        issued.command = command;
        next_command_history_index_ =
            (index + 1) % kCommandHistoryLength;
        return true;
    }
    return false;
}

GroundStationTcpServer::IssuedCommand *
GroundStationTcpServer::findIssuedCommand(uint32_t id) {
    for (size_t index = 0; index < kCommandHistoryLength; ++index) {
        IssuedCommand &issued = command_history_[index];
        if (
            issued.valid &&
            issued.id == id &&
            issued.connection_generation == connection_generation_
        ) {
            return &issued;
        }
    }
    return nullptr;
}

void GroundStationTcpServer::markEvictedCommand(
    const TransmitQueue::EvictedFrame &evicted
) {
    if (
        !evicted.valid ||
        evicted.connection_generation != connection_generation_
    ) {
        return;
    }
    if (evicted.kind == TransmitFrameKind::GET_STATUS) {
        pending_status_request_ = false;
        Serial.println("Dropped queued get_status for higher-priority traffic");
        return;
    }
    if (
        evicted.kind != TransmitFrameKind::COMMAND ||
        evicted.command_id == 0
    ) {
        return;
    }

    IssuedCommand *issued = findIssuedCommand(evicted.command_id);
    if (issued != nullptr) {
        issued->valid = false;
    }
    Serial.printf(
        "Dropped unsent command id=%lu for higher-priority traffic\n",
        static_cast<unsigned long>(evicted.command_id)
    );
}

void GroundStationTcpServer::setQueuedResult(
    uint32_t id,
    GroundStationCommand command
) {
    command_result_ = NanoCommandResult();
    command_result_.valid = true;
    command_result_.ground_station_session_id =
        ground_station_session_id_;
    command_result_.id = id;
    command_result_.command = command;
    command_result_.state = NanoCommandResultState::QUEUED;
    snprintf(
        command_result_.detail,
        sizeof(command_result_.detail),
        "%s",
        "WAITING_FOR_NANO"
    );
}

void GroundStationTcpServer::setLocalRejectedResult(
    uint32_t id,
    GroundStationCommand command,
    const char *reason
) {
    command_result_ = NanoCommandResult();
    command_result_.valid = true;
    command_result_.ground_station_session_id =
        ground_station_session_id_;
    command_result_.id = id;
    command_result_.command = command;
    command_result_.state = NanoCommandResultState::LOCAL_REJECTED;
    snprintf(
        command_result_.detail,
        sizeof(command_result_.detail),
        "%s",
        reason
    );
    Serial.printf(
        "Rejected local command cmd=%s reason=%s\n",
        groundStationCommandName(command),
        reason
    );
}

void GroundStationTcpServer::endCommandSession() {
    if (
        command_result_.valid &&
        (
            command_result_.state == NanoCommandResultState::QUEUED ||
            command_result_.state == NanoCommandResultState::ACCEPTED
        )
    ) {
        command_result_.state = NanoCommandResultState::SESSION_ENDED;
        snprintf(
            command_result_.detail,
            sizeof(command_result_.detail),
            "%s",
            "TCP_LINK_ENDED"
        );
    }
    for (size_t index = 0; index < kCommandHistoryLength; ++index) {
        command_history_[index] = IssuedCommand();
    }
    next_command_history_index_ = 0;
    pending_status_request_ = false;
}

void GroundStationTcpServer::closeClient(const char *reason) {
    Serial.printf("Closing Nano TCP client: %s\n", reason);
    client_.stop();
    reader_.reset();
    transmit_queue_.clear();
    endCommandSession();
    hello_complete_ = false;
    vehicle_status_ = NanoVehicleStatus();
    setTiOnline(false);
    setLinkState(NanoLinkState::DOWN);
}

void GroundStationTcpServer::updateLinkTimeout() {
    const uint32_t now = millis();
    if (!hello_complete_) {
        if (now - connected_at_ms_ >= kHelloTimeoutMs) {
            closeClient("hello timeout");
        }
        return;
    }

    const uint32_t age_ms = now - last_valid_message_ms_;
    if (age_ms >= kNanoLinkDownTimeoutMs) {
        closeClient("Nano heartbeat timeout");
        return;
    }
    if (age_ms >= kNanoLinkDegradedTimeoutMs) {
        setLinkState(NanoLinkState::DEGRADED);
    }
    if (now >= next_heartbeat_ms_) {
        queueHeartbeat();
        next_heartbeat_ms_ = now + kNetworkHeartbeatIntervalMs;
    }
}

void GroundStationTcpServer::setLinkState(NanoLinkState state) {
    if (link_state_ == state) {
        return;
    }
    link_state_ = state;
    const char *name = "DOWN";
    if (state == NanoLinkState::CONNECTING) {
        name = "CONNECTING";
    } else if (state == NanoLinkState::UP) {
        name = "UP";
    } else if (state == NanoLinkState::DEGRADED) {
        name = "DEGRADED";
    }
    Serial.printf("Nano link state: %s\n", name);
}

void GroundStationTcpServer::setTiOnline(bool online) {
    if (ti_online_ == online) {
        return;
    }
    ti_online_ = online;
    Serial.printf("TI link state: %s\n", online ? "UP" : "DOWN");
}
