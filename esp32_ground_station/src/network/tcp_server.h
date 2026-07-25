#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "framed_json.h"

enum class NanoLinkState {
    DOWN,
    CONNECTING,
    UP,
    DEGRADED,
};

class GroundStationTcpServer {
public:
    explicit GroundStationTcpServer(uint16_t port);

    void begin();
    void poll();

    NanoLinkState linkState() const;
    uint32_t groundStationSessionId() const;

private:
    static const size_t kReceiveChunkLength = 256;
    static const size_t kTransmitFrameLength = 512;
    static const size_t kTransmitQueueLength = 4;

    struct PendingFrame {
        uint8_t bytes[kTransmitFrameLength];
        uint16_t length;
        uint16_t offset;
    };

    void acceptClient();
    void readClient();
    void flushTransmitQueue();
    void handleMessage(const String &json);
    void queueHelloAck();
    void queueHeartbeat();
    bool queueJson(const JsonDocument &document);
    void closeClient(const char *reason);
    void updateLinkTimeout();
    void setLinkState(NanoLinkState state);

    WiFiServer server_;
    WiFiClient client_;
    FramedJsonReader reader_;
    PendingFrame transmit_queue_[kTransmitQueueLength] = {};
    size_t transmit_head_ = 0;
    size_t transmit_count_ = 0;
    bool hello_complete_ = false;
    uint32_t ground_station_session_id_ = 0;
    uint32_t connected_at_ms_ = 0;
    uint32_t last_valid_message_ms_ = 0;
    uint32_t next_heartbeat_ms_ = 0;
    NanoLinkState link_state_ = NanoLinkState::DOWN;
};
