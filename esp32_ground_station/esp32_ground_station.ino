#include <WiFi.h>
#include <string.h>

#include "app_config.h"
#include "src/network/tcp_server.h"
#include "src/tjc/tjc_display.h"

namespace {

GroundStationTcpServer tcp_server(kGroundStationTcpPort);
TjcDisplay tjc_display;
bool soft_ap_ready = false;
uint32_t last_tjc_visual_test_ms = 0;
char tjc_visual_test_rx[32];
size_t tjc_visual_test_rx_length = 0;
uint8_t tjc_visual_test_ff_count = 0;
bool tjc_visual_test_rx_overflow = false;

void finishTjcVisualTestFrame() {
    Serial.write(0xFF);
    Serial.write(0xFF);
    Serial.write(0xFF);
}

void sendTjcVisualTestFrame() {
    Serial.print("tAp.txt=\"UART OK\"");
    finishTjcVisualTestFrame();
}

void showTjcVisualTestRx(const char *value) {
    Serial.print("tRx.txt=\"");
    Serial.print(value);
    Serial.print("\"");
    finishTjcVisualTestFrame();
}

void handleTjcVisualTestToken(const char *token) {
    struct TokenResponse {
        const char *token;
        const char *response;
    };
    static const TokenResponse responses[] = {
        {kTjcSyncEventToken, "RX:SYNC"},
        {kTjcArmEventToken, "RX:ARM"},
        {kTjcForward500EventToken, "RX:FWD_500"},
        {kTjcBackward500EventToken, "RX:BACK_500"},
        {kTjcLeft90EventToken, "RX:LEFT_90"},
        {kTjcRight90EventToken, "RX:RIGHT_90"},
        {kTjcStopEventToken, "RX:STOP"},
    };

    for (const TokenResponse &response : responses) {
        if (strcmp(token, response.token) == 0) {
            showTjcVisualTestRx(response.response);
            return;
        }
    }
}

void pollTjcVisualTestRx() {
    while (Serial.available() > 0) {
        const int received = Serial.read();
        if (received < 0) {
            return;
        }

        if (received == 0xFF) {
            ++tjc_visual_test_ff_count;
            if (tjc_visual_test_ff_count == 3) {
                if (!tjc_visual_test_rx_overflow) {
                    tjc_visual_test_rx[tjc_visual_test_rx_length] = '\0';
                    handleTjcVisualTestToken(tjc_visual_test_rx);
                }
                tjc_visual_test_rx_length = 0;
                tjc_visual_test_ff_count = 0;
                tjc_visual_test_rx_overflow = false;
            }
            continue;
        }

        if (tjc_visual_test_ff_count != 0) {
            tjc_visual_test_rx_length = 0;
            tjc_visual_test_ff_count = 0;
            tjc_visual_test_rx_overflow = false;
        }
        if (tjc_visual_test_rx_length < sizeof(tjc_visual_test_rx) - 1) {
            tjc_visual_test_rx[tjc_visual_test_rx_length++] =
                static_cast<char>(received);
        } else {
            tjc_visual_test_rx_overflow = true;
        }
    }
}

bool startSoftAp() {
    const IPAddress ap_ip(192, 168, 4, 1);
    const IPAddress gateway(192, 168, 4, 1);
    const IPAddress subnet(255, 255, 255, 0);

    WiFi.mode(WIFI_AP);
    if (!WiFi.softAPConfig(ap_ip, gateway, subnet)) {
        Serial.println("SoftAP IP configuration failed");
        return false;
    }
    if (!WiFi.softAP(CAR_GS_WIFI_SSID, CAR_GS_WIFI_PASSWORD, 1, false, 1)) {
        Serial.println("SoftAP start failed");
        return false;
    }
    Serial.printf(
        "SoftAP READY: ssid=%s ip=%s\n",
        CAR_GS_WIFI_SSID,
        WiFi.softAPIP().toString().c_str()
    );
    return true;
}

}  // namespace

void setup() {
    Serial.begin(kGroundStationBaud);
    if (kRunTjcTxVisualTest) {
        sendTjcVisualTestFrame();
        last_tjc_visual_test_ms = millis();
        return;
    }

    tjc_display.begin();

    soft_ap_ready = startSoftAp();
    tjc_display.showAp(soft_ap_ready);
    if (soft_ap_ready) {
        tcp_server.begin();
    }
}

void loop() {
    if (kRunTjcTxVisualTest) {
        pollTjcVisualTestRx();
        const uint32_t now = millis();
        if (now - last_tjc_visual_test_ms >= kTjcRefreshIntervalMs) {
            sendTjcVisualTestFrame();
            last_tjc_visual_test_ms = now;
        }
        return;
    }

    TjcInputEvent input_event;
    if (tjc_display.pollEvent(input_event)) {
        if (input_event.type == TjcInputEventType::SYNC) {
            tjc_display.forceRefresh();
        } else {
            tcp_server.requestCommand(input_event.command);
        }
        tjc_display.showInputReceipt(input_event);
    }
    if (soft_ap_ready) {
        tcp_server.poll();
    }
    tjc_display.showNano(tcp_server.linkState());
    tjc_display.showTi(tcp_server.tiOnline());
    tjc_display.showVehicleStatus(tcp_server.vehicleStatus());
    tjc_display.showCommandResult(tcp_server.commandResult());
    tjc_display.poll();
}
