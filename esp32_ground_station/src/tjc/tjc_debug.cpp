#include "tjc_debug.h"

#include <Arduino.h>
#include <driver/uart.h>

#include "app_config.h"

namespace {

constexpr size_t kCaptureCapacity = 512;
constexpr uint32_t kResponseTimeoutMs = 450;
constexpr uint32_t kPowerOnCaptureTimeoutMs = 5000;

const uint32_t kSupportedBauds[] = {
    115200,
    9600,
    19200,
    38400,
    57600,
    230400,
    256000,
    512000,
    921600,
    4800,
    2400,
};

struct Capture {
    uint8_t bytes[kCaptureCapacity];
    size_t length = 0;
    bool overflow = false;
};

struct Summary {
    bool uart1_exact_echo = false;
    bool uart2_exact_echo = false;
    bool gpio_static_coupling = false;
    bool power_on_frame_seen = false;
    bool connect_probe_passed = false;
    uint32_t detected_baud = 0;
    bool ascii_parser_passed = false;
    int current_page = -1;
    bool main_page_passed = false;
    bool t_ap_passed = false;
    bool t_nano_passed = false;
    bool t_ti_passed = false;
    bool cross_page_t_ap_passed = false;
    bool text_write_readback_passed = false;
    bool dim_readback_passed = false;
    int32_t address = -1;
};

Summary summary;
uint32_t current_baud = kGroundStationBaud;

void printHexByte(uint8_t byte) {
    if (byte < 0x10) {
        Serial.print('0');
    }
    Serial.print(byte, HEX);
}

void appendByte(Capture &capture, uint8_t byte) {
    if (capture.length < kCaptureCapacity) {
        capture.bytes[capture.length++] = byte;
    } else {
        capture.overflow = true;
    }
}

void printCapture(const char *label, const Capture &capture) {
    Serial.printf(
        "[%s][%lu ms] length=%u%s\n",
        label,
        static_cast<unsigned long>(millis()),
        static_cast<unsigned int>(capture.length),
        capture.overflow ? " OVERFLOW" : ""
    );
    Serial.print("  HEX: ");
    if (capture.length == 0) {
        Serial.println("<none>");
    } else {
        for (size_t index = 0; index < capture.length; index++) {
            if (index > 0) {
                Serial.print(' ');
            }
            printHexByte(capture.bytes[index]);
        }
        Serial.println();
    }

    Serial.print("  ASCII: ");
    if (capture.length == 0) {
        Serial.println("<none>");
        return;
    }
    for (size_t index = 0; index < capture.length; index++) {
        const uint8_t byte = capture.bytes[index];
        Serial.write(byte >= 0x20 && byte <= 0x7E ? byte : '.');
    }
    Serial.println();
}

bool containsBytes(
    const Capture &capture,
    const uint8_t *pattern,
    size_t pattern_length
) {
    if (pattern_length == 0 || capture.length < pattern_length) {
        return false;
    }
    for (
        size_t start = 0;
        start + pattern_length <= capture.length;
        start++
    ) {
        if (
            memcmp(
                capture.bytes + start,
                pattern,
                pattern_length
            ) == 0
        ) {
            return true;
        }
    }
    return false;
}

bool isFrameStart(const Capture &capture, size_t index) {
    return index == 0
        || (
            index >= 3
            && capture.bytes[index - 3] == 0xFF
            && capture.bytes[index - 2] == 0xFF
            && capture.bytes[index - 1] == 0xFF
        );
}

bool containsAscii(const Capture &capture, const char *text) {
    return containsBytes(
        capture,
        reinterpret_cast<const uint8_t *>(text),
        strlen(text)
    );
}

bool containsResultCode(const Capture &capture, uint8_t code) {
    for (size_t index = 0; index + 3 < capture.length; index++) {
        if (
            isFrameStart(capture, index)
            && capture.bytes[index] == code
            && capture.bytes[index + 1] == 0xFF
            && capture.bytes[index + 2] == 0xFF
            && capture.bytes[index + 3] == 0xFF
        ) {
            return true;
        }
    }
    return false;
}

void clearInput() {
    while (Serial1.available() > 0) {
        Serial1.read();
    }
}

Capture captureUntilIdle(uint32_t timeout_ms, uint32_t idle_ms = 40) {
    Capture capture;
    const uint32_t started_ms = millis();
    uint32_t last_byte_ms = started_ms;
    bool received = false;

    while (millis() - started_ms < timeout_ms) {
        while (Serial1.available() > 0) {
            appendByte(
                capture,
                static_cast<uint8_t>(Serial1.read())
            );
            received = true;
            last_byte_ms = millis();
        }
        if (received && millis() - last_byte_ms >= idle_ms) {
            break;
        }
        delay(1);
    }
    return capture;
}

Capture capturePortUntilIdle(
    HardwareSerial &port,
    uint32_t timeout_ms,
    uint32_t idle_ms
) {
    Capture capture;
    const uint32_t started_ms = millis();
    uint32_t last_byte_ms = started_ms;
    bool received = false;

    while (millis() - started_ms < timeout_ms) {
        while (port.available() > 0) {
            appendByte(capture, static_cast<uint8_t>(port.read()));
            received = true;
            last_byte_ms = millis();
        }
        if (received && millis() - last_byte_ms >= idle_ms) {
            break;
        }
        delay(1);
    }
    return capture;
}

bool captureEquals(
    const Capture &capture,
    const uint8_t *expected,
    size_t expected_length
) {
    return capture.length == expected_length
        && memcmp(capture.bytes, expected, expected_length) == 0;
}

bool testUartEcho(
    HardwareSerial &port,
    uart_port_t uart_number,
    const char *label
) {
    static const uint8_t pattern[] = {
        0xA5, 0x5A, 0x13, 0x7C, 0xC3, 0x3C, 0x96,
    };

    port.end();
    delay(20);
    const esp_err_t loopback_result =
        uart_set_loop_back(uart_number, false);
    Serial.printf(
        "[%s] disable internal loopback: %s (0x%lX)\n",
        label,
        esp_err_to_name(loopback_result),
        static_cast<unsigned long>(loopback_result)
    );

    port.begin(
        kGroundStationBaud,
        SERIAL_8N1,
        kTjcRxPin,
        kTjcTxPin
    );
    delay(50);
    while (port.available() > 0) {
        port.read();
    }

    Serial.printf("[%s TX PATTERN] ", label);
    for (uint8_t byte : pattern) {
        printHexByte(byte);
        Serial.print(' ');
    }
    Serial.println();
    port.write(pattern, sizeof(pattern));
    port.flush();

    Capture response = capturePortUntilIdle(port, 500, 50);
    printCapture(label, response);
    const bool exact_echo =
        captureEquals(response, pattern, sizeof(pattern));
    Serial.printf(
        "[%s EXACT ECHO] %s\n",
        label,
        exact_echo ? "DETECTED" : "NOT DETECTED"
    );
    port.end();
    delay(30);
    return exact_echo;
}

void testGpioStaticCoupling() {
    Serial1.end();
    Serial2.end();
    delay(30);

    pinMode(kTjcRxPin, INPUT);
    pinMode(kTjcTxPin, OUTPUT);

    digitalWrite(kTjcTxPin, HIGH);
    delay(10);
    const int rx_when_tx_high = digitalRead(kTjcRxPin);

    digitalWrite(kTjcTxPin, LOW);
    delay(10);
    const int rx_when_tx_low = digitalRead(kTjcRxPin);

    digitalWrite(kTjcTxPin, HIGH);
    delay(10);
    const int rx_when_tx_high_again = digitalRead(kTjcRxPin);

    summary.gpio_static_coupling =
        rx_when_tx_high == HIGH
        && rx_when_tx_low == LOW
        && rx_when_tx_high_again == HIGH;

    Serial.printf(
        "[GPIO COUPLING] TX4=HIGH -> RX5=%d, "
        "TX4=LOW -> RX5=%d, TX4=HIGH -> RX5=%d\n",
        rx_when_tx_high,
        rx_when_tx_low,
        rx_when_tx_high_again
    );
    Serial.printf(
        "[GPIO STATIC TX->RX COUPLING] %s\n",
        summary.gpio_static_coupling ? "DETECTED" : "NOT DETECTED"
    );

    pinMode(kTjcTxPin, INPUT);
    pinMode(kTjcRxPin, INPUT);
    delay(30);
}

void testEchoSource() {
    Serial.println();
    Serial.println("=== DEBUG 0: UART ECHO SOURCE ===");

    summary.uart1_exact_echo =
        testUartEcho(Serial1, UART_NUM_1, "UART1 RX");
    summary.uart2_exact_echo =
        testUartEcho(Serial2, UART_NUM_2, "UART2 RX");
    testGpioStaticCoupling();
}

void startSerial(uint32_t baud) {
    Serial1.end();
    delay(20);
    uart_set_loop_back(UART_NUM_1, false);
    Serial1.begin(baud, SERIAL_8N1, kTjcRxPin, kTjcTxPin);
    current_baud = baud;
    delay(50);
    clearInput();
}

void printTx(const char *command) {
    Serial.printf(
        "[TJC TX][%lu baud][%lu ms] %s\n",
        static_cast<unsigned long>(current_baud),
        static_cast<unsigned long>(millis()),
        command
    );
    Serial.print("  HEX: ");
    const uint8_t *bytes =
        reinterpret_cast<const uint8_t *>(command);
    const size_t length = strlen(command);
    for (size_t index = 0; index < length; index++) {
        printHexByte(bytes[index]);
        Serial.print(' ');
    }
    Serial.println("FF FF FF");
}

void sendCommand(const char *command) {
    printTx(command);
    Serial1.print(command);
    Serial1.write(0xFF);
    Serial1.write(0xFF);
    Serial1.write(0xFF);
    Serial1.flush();
}

Capture transact(const char *command) {
    clearInput();
    sendCommand(command);
    Capture response = captureUntilIdle(kResponseTimeoutMs);
    printCapture("TJC RX", response);

    const size_t command_length = strlen(command);
    bool exact_echo = response.length == command_length + 3;
    if (exact_echo) {
        exact_echo =
            memcmp(response.bytes, command, command_length) == 0
            && response.bytes[command_length] == 0xFF
            && response.bytes[command_length + 1] == 0xFF
            && response.bytes[command_length + 2] == 0xFF;
    }
    if (exact_echo) {
        Serial.println(
            "[EXACT ECHO] RX is identical to the complete TX byte stream"
        );
    }
    return response;
}

bool extractNumber(const Capture &capture, uint32_t &value) {
    for (size_t index = 0; index + 7 < capture.length; index++) {
        if (
            isFrameStart(capture, index)
            && capture.bytes[index] == 0x71
            && capture.bytes[index + 5] == 0xFF
            && capture.bytes[index + 6] == 0xFF
            && capture.bytes[index + 7] == 0xFF
        ) {
            value =
                static_cast<uint32_t>(capture.bytes[index + 1])
                | (
                    static_cast<uint32_t>(capture.bytes[index + 2])
                    << 8
                )
                | (
                    static_cast<uint32_t>(capture.bytes[index + 3])
                    << 16
                )
                | (
                    static_cast<uint32_t>(capture.bytes[index + 4])
                    << 24
                );
            return true;
        }
    }
    return false;
}

bool extractPage(const Capture &capture, uint8_t &page) {
    for (size_t index = 0; index + 4 < capture.length; index++) {
        if (
            isFrameStart(capture, index)
            && capture.bytes[index] == 0x66
            && capture.bytes[index + 2] == 0xFF
            && capture.bytes[index + 3] == 0xFF
            && capture.bytes[index + 4] == 0xFF
        ) {
            page = capture.bytes[index + 1];
            return true;
        }
    }
    return false;
}

bool extractText(
    const Capture &capture,
    char *text,
    size_t capacity
) {
    if (capacity == 0) {
        return false;
    }
    for (size_t start = 0; start < capture.length; start++) {
        if (
            !isFrameStart(capture, start)
            || capture.bytes[start] != 0x70
        ) {
            continue;
        }
        size_t output_index = 0;
        for (size_t index = start + 1; index < capture.length; index++) {
            if (
                index + 2 < capture.length
                && capture.bytes[index] == 0xFF
                && capture.bytes[index + 1] == 0xFF
                && capture.bytes[index + 2] == 0xFF
            ) {
                text[output_index] = '\0';
                return true;
            }
            if (output_index + 1 >= capacity) {
                return false;
            }
            text[output_index++] =
                static_cast<char>(capture.bytes[index]);
        }
    }
    return false;
}

void capturePowerOnFrame() {
    Serial.println();
    Serial.println("=== DEBUG 1: RAW POWER-ON RX ===");
    Serial.println(
        "Power-cycle the TJC display now; listening for 5 seconds."
    );

    clearInput();
    Capture capture;
    const uint32_t started_ms = millis();
    uint32_t last_byte_ms = started_ms;
    const uint8_t ready_frame[] = {0x88, 0xFF, 0xFF, 0xFF};

    while (millis() - started_ms < kPowerOnCaptureTimeoutMs) {
        while (Serial1.available() > 0) {
            appendByte(
                capture,
                static_cast<uint8_t>(Serial1.read())
            );
            last_byte_ms = millis();
        }
        if (
            containsBytes(capture, ready_frame, sizeof(ready_frame))
            && millis() - last_byte_ms >= 100
        ) {
            break;
        }
        delay(1);
    }

    printCapture("TJC RAW RX", capture);
    summary.power_on_frame_seen =
        containsBytes(capture, ready_frame, sizeof(ready_frame));
}

Capture probeAtBaud(uint32_t baud) {
    startSerial(baud);

    static const uint8_t probe[] = {
        'D', 'R', 'A', 'K', 'J', 'H', 'S', 'U',
        'Y', 'D', 'G', 'B', 'N', 'C', 'J', 'H',
        'G', 'J', 'K', 'S', 'H', 'B', 'D', 'N',
        0xFF, 0xFF, 0xFF,
        0x00, 0xFF, 0xFF, 0xFF,
        'c', 'o', 'n', 'n', 'e', 'c', 't',
        0xFF, 0xFF, 0xFF,
    };

    Serial.printf(
        "\n[BAUD PROBE] %lu\n",
        static_cast<unsigned long>(baud)
    );
    Serial.print("[TJC TX RAW] ");
    for (uint8_t byte : probe) {
        printHexByte(byte);
        Serial.print(' ');
    }
    Serial.println();

    Serial1.write(probe, sizeof(probe));
    Serial1.flush();

    uint32_t timeout_ms = 1000000UL / baud + 330;
    if (timeout_ms < 700) {
        timeout_ms = 700;
    }
    Capture response = captureUntilIdle(timeout_ms, 80);
    printCapture("TJC RX PROBE", response);
    return response;
}

void scanBauds() {
    Serial.println();
    Serial.println("=== DEBUG 2: OFFICIAL CONNECT/COMOK BAUD SCAN ===");

    for (uint32_t baud : kSupportedBauds) {
        Capture response = probeAtBaud(baud);
        if (containsAscii(response, "comok")) {
            summary.connect_probe_passed = true;
            summary.detected_baud = baud;
            Serial.printf(
                "[BAUD] PASS: %lu, comok detected\n",
                static_cast<unsigned long>(baud)
            );
            startSerial(baud);
            return;
        }
    }

    summary.connect_probe_passed = false;
    summary.detected_baud = 0;
    Serial.println("[BAUD] FAIL: no comok at any supported baud");
    startSerial(kGroundStationBaud);
}

void testAsciiParser() {
    Serial.println();
    Serial.println("=== DEBUG 3: ASCII COMMAND PARSER ===");

    transact("recmod=0");
    transact("bkcmd=3");
    Capture set_response = transact("sys0=305419896");
    Capture get_response = transact("get sys0");

    uint32_t value = 0;
    const bool readback_passed =
        extractNumber(get_response, value)
        && value == 0x12345678UL;
    summary.ascii_parser_passed =
        readback_passed
        && (
            containsResultCode(set_response, 0x01)
            || !containsResultCode(set_response, 0x00)
        );
    Serial.printf(
        "[ASCII PARSER] %s, readback=0x%08lX\n",
        summary.ascii_parser_passed ? "PASS" : "FAIL",
        static_cast<unsigned long>(value)
    );
}

int queryPage() {
    Capture response = transact("sendme");
    uint8_t page = 0;
    if (!extractPage(response, page)) {
        Serial.println("[PAGE] FAIL: no 0x66 response");
        return -1;
    }
    Serial.printf("[PAGE] PASS: id=%u\n", page);
    return page;
}

void testPage() {
    Serial.println();
    Serial.println("=== DEBUG 4: PAGE NAME AND PAGE ID ===");

    summary.current_page = queryPage();
    Capture page_response = transact("page main");
    delay(150);
    summary.current_page = queryPage();

    if (summary.current_page != 1) {
        Serial.println("[PAGE] page main failed; trying page 1");
        transact("page 1");
        delay(150);
        summary.current_page = queryPage();
    }
    summary.main_page_passed =
        summary.current_page == 1
        && !containsResultCode(page_response, 0x03);
}

bool readText(
    const char *component,
    char *text,
    size_t capacity
) {
    char command[80];
    snprintf(command, sizeof(command), "get %s.txt", component);
    Capture response = transact(command);
    const bool passed = extractText(response, text, capacity);

    Serial.printf(
        "[CONTROL %s] %s",
        component,
        passed ? "PASS" : "FAIL"
    );
    if (passed) {
        Serial.printf(", text=\"%s\"", text);
    } else if (containsResultCode(response, 0x02)) {
        Serial.print(", 0x02 invalid component ID");
    } else if (containsResultCode(response, 0x1A)) {
        Serial.print(", 0x1A invalid variable name");
    }
    Serial.println();
    return passed;
}

bool setAndReadText(
    const char *component,
    const char *new_text,
    const char *restore_text
) {
    char command[96];
    snprintf(
        command,
        sizeof(command),
        "%s.txt=\"%s\"",
        component,
        new_text
    );
    Capture set_response = transact(command);

    char readback[96];
    const bool passed =
        readText(component, readback, sizeof(readback))
        && strcmp(readback, new_text) == 0
        && !containsResultCode(set_response, 0x02);

    Serial.printf(
        "[SET/READBACK %s] %s\n",
        component,
        passed ? "PASS" : "FAIL"
    );

    snprintf(
        command,
        sizeof(command),
        "%s.txt=\"%s\"",
        component,
        restore_text
    );
    transact(command);
    return passed;
}

void testControls() {
    Serial.println();
    Serial.println("=== DEBUG 5: HMI PAGE/CONTROL CONTENT ===");

    if (queryPage() != 1) {
        transact("page 1");
        delay(150);
    }

    char ap_text[96] = "";
    char nano_text[96] = "";
    char ti_text[96] = "";
    summary.t_ap_passed =
        readText("tAp", ap_text, sizeof(ap_text));
    summary.t_nano_passed =
        readText("tNano", nano_text, sizeof(nano_text));
    summary.t_ti_passed =
        readText("tTi", ti_text, sizeof(ti_text));

    char cross_page_text[96] = "";
    summary.cross_page_t_ap_passed =
        readText(
            "main.tAp",
            cross_page_text,
            sizeof(cross_page_text)
        );

    bool ap_write = false;
    bool nano_write = false;
    bool ti_write = false;
    if (summary.t_ap_passed) {
        ap_write =
            setAndReadText("tAp", "DBG_READY", ap_text);
    }
    if (summary.t_nano_passed) {
        nano_write =
            setAndReadText("tNano", "DBG_NANO", nano_text);
    }
    if (summary.t_ti_passed) {
        ti_write =
            setAndReadText("tTi", "DBG_TI", ti_text);
    }
    summary.text_write_readback_passed =
        ap_write && nano_write && ti_write;
}

bool queryNumber(const char *variable, uint32_t &value) {
    char command[64];
    snprintf(command, sizeof(command), "get %s", variable);
    return extractNumber(transact(command), value);
}

void testDimAndAddress() {
    Serial.println();
    Serial.println("=== DEBUG 6: VISIBLE DIM AND ADDRESS ===");

    transact("dim=30");
    delay(700);
    uint32_t dim_low = UINT32_MAX;
    const bool low_passed =
        queryNumber("dim", dim_low) && dim_low == 30;

    transact("dim=100");
    delay(300);
    uint32_t dim_restored = UINT32_MAX;
    const bool restore_passed =
        queryNumber("dim", dim_restored) && dim_restored == 100;
    summary.dim_readback_passed = low_passed && restore_passed;

    uint32_t address = 0;
    if (queryNumber("addr", address)) {
        summary.address = static_cast<int32_t>(address);
    }
}

const char *passFail(bool passed) {
    return passed ? "PASS" : "FAIL";
}

const char *linkVerdict() {
    if (
        summary.uart1_exact_echo
        && summary.uart2_exact_echo
        && summary.gpio_static_coupling
    ) {
        return "EXTERNAL_TX_TO_RX_COUPLING";
    }
    if (summary.uart1_exact_echo && summary.uart2_exact_echo) {
        return "EXTERNAL_ACTIVE_ECHO_OR_SHARED_DEVICE";
    }
    if (summary.uart1_exact_echo && !summary.uart2_exact_echo) {
        return "UART1_SPECIFIC_LOOPBACK_OR_DRIVER_STATE";
    }
    if (
        summary.connect_probe_passed
        || summary.ascii_parser_passed
    ) {
        return "REAL_TJC_RESPONSE";
    }
    return "TJC_NO_RESPONSE";
}

const char *hmiVerdict() {
    if (
        summary.t_ap_passed
        && summary.t_nano_passed
        && summary.t_ti_passed
    ) {
        return "HMI_COMPONENTS_VALID";
    }
    if (summary.ascii_parser_passed && summary.main_page_passed) {
        return "HMI_COMPONENT_MISSING_OR_RENAMED";
    }
    return "HMI_NOT_YET_VERIFIED";
}

void printSummary() {
    Serial.println();
    Serial.println("========== TJC DEBUG SUMMARY ==========");
    Serial.println("ESP USB serial              PASS");
    Serial.printf(
        "UART mapping                Serial1 RX=%d TX=%d\n",
        kTjcRxPin,
        kTjcTxPin
    );
    Serial.printf(
        "UART1 exact TX echo         %s\n",
        summary.uart1_exact_echo ? "DETECTED" : "NOT DETECTED"
    );
    Serial.printf(
        "UART2 exact TX echo         %s\n",
        summary.uart2_exact_echo ? "DETECTED" : "NOT DETECTED"
    );
    Serial.printf(
        "GPIO4->GPIO5 coupling       %s\n",
        summary.gpio_static_coupling ? "DETECTED" : "NOT DETECTED"
    );
    Serial.printf(
        "TJC power-on frame 0x88     %s\n",
        summary.power_on_frame_seen ? "PASS" : "NOT SEEN"
    );
    Serial.printf(
        "Official connect/comok      %s\n",
        passFail(summary.connect_probe_passed)
    );
    Serial.printf(
        "Detected baud               %lu\n",
        static_cast<unsigned long>(summary.detected_baud)
    );
    Serial.printf(
        "ASCII command parser        %s\n",
        passFail(summary.ascii_parser_passed)
    );
    Serial.printf("Current page                %d\n", summary.current_page);
    Serial.printf(
        "Main page                   %s\n",
        passFail(summary.main_page_passed)
    );
    Serial.printf(
        "Component tAp               %s\n",
        passFail(summary.t_ap_passed)
    );
    Serial.printf(
        "Component tNano             %s\n",
        passFail(summary.t_nano_passed)
    );
    Serial.printf(
        "Component tTi               %s\n",
        passFail(summary.t_ti_passed)
    );
    Serial.printf(
        "Cross-page main.tAp         %s\n",
        passFail(summary.cross_page_t_ap_passed)
    );
    Serial.printf(
        "Text write/readback         %s\n",
        passFail(summary.text_write_readback_passed)
    );
    Serial.printf(
        "Visible dim/readback        %s\n",
        passFail(summary.dim_readback_passed)
    );
    if (summary.address >= 0) {
        Serial.printf(
            "TJC address                 %ld\n",
            static_cast<long>(summary.address)
        );
    } else {
        Serial.println("TJC address                 UNKNOWN");
    }
    Serial.printf("LINK VERDICT                %s\n", linkVerdict());
    Serial.printf("HMI VERDICT                 %s\n", hmiVerdict());
    Serial.println("=======================================");
}

}  // namespace

void runTjcDebug() {
    summary = Summary();

    delay(1200);
    Serial.println();
    Serial.println("=======================================");
    Serial.println("INTEGRATED TJC FULL DEBUG");
    Serial.println("No Wi-Fi, TCP, Nano, MSPM0, or motor traffic yet.");
    Serial.println("=======================================");

    testEchoSource();
    startSerial(kGroundStationBaud);
    capturePowerOnFrame();
    scanBauds();
    testAsciiParser();
    testPage();
    testControls();
    testDimAndAddress();
    printSummary();

    Serial.println(
        "TJC debug complete; returning Serial1 to the normal application."
    );
    Serial1.end();
    delay(50);
}
