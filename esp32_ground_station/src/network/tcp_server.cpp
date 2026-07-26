#include "tcp_server.h"

#include <errno.h>
#include <esp_system.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"

GroundStationTcpServer::GroundStationTcpServer(uint16_t port) : server_(port) {
}

void GroundStationTcpServer::begin() {
    ground_station_session_id_ = esp_random();
    if (ground_station_session_id_ == 0) {
        ground_station_session_id_ = 1;
    }
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

uint32_t GroundStationTcpServer::groundStationSessionId() const {
    return ground_station_session_id_;
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
    transmit_head_ = 0;
    transmit_count_ = 0;
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
    if (transmit_count_ == 0) {
        return;
    }

    PendingFrame &frame = transmit_queue_[transmit_head_];
    const size_t remaining = frame.length - frame.offset;
    const int socket_fd = client_.fd();
    if (socket_fd < 0) {
        closeClient("invalid TCP socket");
        return;
    }

    const ssize_t sent = send(
        socket_fd,
        frame.bytes + frame.offset,
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
    frame.offset += static_cast<size_t>(sent);
    if (frame.offset == frame.length) {
        transmit_head_ = (transmit_head_ + 1) % kTransmitQueueLength;
        --transmit_count_;
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

    if (strcmp(type, "status") == 0) {
        if (!handleStatus(object)) {
            closeClient("invalid Nano status");
            return;
        }
        last_valid_message_ms_ = millis();
        setLinkState(NanoLinkState::UP);
        return;
    }

    closeClient("unexpected stage-2 message");
}

bool GroundStationTcpServer::handleStatus(JsonObject object) {
    const char *system_state = object["systemState"];
    JsonObject links = object["links"].as<JsonObject>();
    const char *nano_ti = links["nanoTi"];
    if (!object["timestampMs"].is<uint32_t>() ||
        system_state == nullptr ||
        !object["armed"].is<bool>() ||
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
        strcmp(vehicle_status_.system_state, system_state) != 0 ||
        strcmp(
            vehicle_status_.fault_code,
            fault_code == nullptr ? "UNKNOWN" : fault_code
        ) != 0;

    NanoVehicleStatus next;
    next.valid = true;
    next.armed = object["armed"].as<bool>();
    next.timestamp_ms = object["timestampMs"].as<uint32_t>();
    if (object["activeCommandId"].is<uint32_t>()) {
        next.active_command_id = object["activeCommandId"].as<uint32_t>();
    }
    if (object["distanceMm"].is<int32_t>()) {
        next.distance_mm = object["distanceMm"].as<int32_t>();
    }
    if (object["headingMdeg"].is<int32_t>()) {
        next.heading_mdeg = object["headingMdeg"].as<int32_t>();
    }
    if (object["lineErrorX100"].is<int16_t>()) {
        next.line_error_x100 = object["lineErrorX100"].as<int16_t>();
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

void GroundStationTcpServer::queueHelloAck() {
    StaticJsonDocument<192> response;
    response["v"] = 1;
    response["type"] = "hello_ack";
    response["role"] = "esp32";
    response["version"] = "2.1";
    response["groundStationSessionId"] = ground_station_session_id_;
    if (!queueJson(response)) {
        closeClient("hello_ack queue full");
    }
}

void GroundStationTcpServer::queueHeartbeat() {
    StaticJsonDocument<128> message;
    message["v"] = 1;
    message["type"] = "heartbeat";
    message["source"] = "esp32";
    message["uptimeMs"] = millis();
    if (!queueJson(message)) {
        Serial.println("Dropping ESP32 heartbeat because transmit queue is full");
    }
}

bool GroundStationTcpServer::queueJson(const JsonDocument &document) {
    if (transmit_count_ == kTransmitQueueLength) {
        return false;
    }

    const bool queue_was_empty = transmit_count_ == 0;
    PendingFrame &frame = transmit_queue_[
        (transmit_head_ + transmit_count_) % kTransmitQueueLength
    ];
    const size_t payload_length = serializeJson(
        document,
        frame.bytes + 4,
        kTransmitFrameLength - 4
    );
    if (payload_length == 0 || payload_length > kTransmitFrameLength - 4) {
        return false;
    }
    frame.bytes[0] = static_cast<uint8_t>(payload_length & 0xFF);
    frame.bytes[1] = static_cast<uint8_t>((payload_length >> 8) & 0xFF);
    frame.bytes[2] = static_cast<uint8_t>((payload_length >> 16) & 0xFF);
    frame.bytes[3] = static_cast<uint8_t>((payload_length >> 24) & 0xFF);
    frame.length = static_cast<uint16_t>(payload_length + 4);
    frame.offset = 0;
    ++transmit_count_;
    if (queue_was_empty) {
        last_transmit_progress_ms_ = millis();
    }
    return true;
}

void GroundStationTcpServer::closeClient(const char *reason) {
    Serial.printf("Closing Nano TCP client: %s\n", reason);
    client_.stop();
    reader_.reset();
    transmit_head_ = 0;
    transmit_count_ = 0;
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
