#include <WiFi.h>

#include "app_config.h"
#include "network/tcp_server.h"
#include "tjc/tjc_display.h"

namespace {

GroundStationTcpServer tcp_server(kGroundStationTcpPort);
TjcDisplay tjc_display;
bool soft_ap_ready = false;

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
    tjc_display.begin();

    soft_ap_ready = startSoftAp();
    tjc_display.showAp(soft_ap_ready);
    if (soft_ap_ready) {
        tcp_server.begin();
    }
}

void loop() {
    if (soft_ap_ready) {
        tcp_server.poll();
    }
    tjc_display.showNano(tcp_server.linkState());
}
