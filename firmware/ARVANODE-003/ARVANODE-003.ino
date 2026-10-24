#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include "esp_system.h"
#include "esp_mac.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// ============================================================
// ARVACORE / ARVANODE SENSOR NODE FIRMWARE
// Board: ESP32 Dev Module / ESP32-WROOM-32
//
// GPIO 34: Soil moisture analog output
// GPIO 5 : Ultrasonic TRIG
// GPIO 18: Ultrasonic ECHO (voltage divider required for 5V)
// GPIO 19: Activation button (active LOW)
// GPIO 23: Pairing button (active LOW)
// GPIO 26: System LED (master, errors)
// GPIO 25: Sensor/telemetry activity LED
//
// DIAGNOSTICS:
// - Checks actual Wi-Fi connectivity.
// - Checks soil ADC electrical response.
// - Warns about a potentially disconnected or saturated ADC.
// - Checks actual ultrasonic echo pulses.
// - Does not require a 20 cm minimum distance.
// - Reports individual failures in Serial Monitor.
// - Telemetry interval: 2 seconds.
//
// NOTE:
// A stable soil reading is not automatically a failure.
// Definitive sensor validation may require changing the input.
// ============================================================

// ---------------- BUILD-TIME / PROVISIONED CONFIGURATION -----
#if __has_include("node_config.h")
#include "node_config.h"
#endif

// ============================================================
// MANUAL WI-FI CONFIGURATION (EDIT DIRECTLY HERE)
// Simple single-network connection — no fallback switching.
// ============================================================
const char* WIFI_SSID     = "Cream";
const char* WIFI_PASSWORD = "hatdog123";

// ---------------- NETWORK & IDENTITY CONFIGURATION ----------

#ifdef ARVA_CONFIG_INJECTED
const char* BACKEND_BASE_URL = NODE_BACKEND_URL;
const char* GATEWAY_KEY = NODE_GATEWAY_KEY;

#ifdef NODE_DEVICE_KEY
const char* BOOTSTRAP_DEVICE_KEY = NODE_DEVICE_KEY;
#else
const char* BOOTSTRAP_DEVICE_KEY = "dev_sec_1efa16c86709da60e5766074b3f8c0b924b13df1de343e8b2233a13c";
#endif

const char* PROVISIONED_NODE_ID = NODE_LOGICAL_ID;
const char* PROVISIONED_DISPLAY_NAME = NODE_DISPLAY_NAME;
const char* PROVISIONED_FIRMWARE_VER = NODE_FIRMWARE_VERSION;
const char* PROVISIONED_BUILD_ID = NODE_BUILD_ID;
constexpr float SENSOR_DATUM_OFFSET_CM = NODE_DATUM_OFFSET_CM;
#else
const char* BACKEND_BASE_URL =
    "https://api-aquaflow.gladwindr.me/api/v1";

const char* GATEWAY_KEY =
    "ed630e681ef735f36954469b48f832f210286246ca95fff62e62086cab74e8a9";

const char* BOOTSTRAP_DEVICE_KEY = "dev_sec_1efa16c86709da60e5766074b3f8c0b924b13df1de343e8b2233a13c";

const char* PROVISIONED_NODE_ID = "ARVANODE-003";
const char* PROVISIONED_DISPLAY_NAME = "ArvaNode Field 03";
const char* PROVISIONED_FIRMWARE_VER = "1.2.0";
const char* PROVISIONED_BUILD_ID = "std-20261018";
constexpr float SENSOR_DATUM_OFFSET_CM = 20.0f;
#endif


// ---------------- GPIO DEFINITIONS ---------------------------

constexpr uint8_t SOIL_PIN = 34;
constexpr uint8_t TRIG_PIN = 5;
constexpr uint8_t ECHO_PIN = 18;

constexpr uint8_t BTN_ACTIVATE = 19;
constexpr uint8_t BTN_PAIRING = 23;

constexpr uint8_t LED_PAIRING = 25;
constexpr uint8_t LED_ACTIVATE = 26;

// ---------------- TIMING -------------------------------------

constexpr unsigned long DEBOUNCE_MS = 50;
constexpr unsigned long WIFI_TIMEOUT_MS = 10000;
constexpr unsigned long HTTP_TIMEOUT_MS = 10000;

constexpr unsigned long TELEMETRY_INTERVAL_MS =
    2UL * 1000UL;
constexpr unsigned long INITIAL_TELEMETRY_DELAY_MS =
    2UL * 1000UL;
constexpr unsigned long TELEMETRY_BLINK_INTERVAL_MS = 100;
constexpr unsigned long PAIRING_POLL_INTERVAL_MS = 5000;
constexpr unsigned long AUTH_RECHECK_MS = 60000;
constexpr unsigned long WIFI_RETRY_MS = 10000;
unsigned long lastAuthRecheckMs = 0;
unsigned long lastWifiRetryMs = 0;
constexpr unsigned long PAIRING_WAIT_TIMEOUT_MS = 60000;

// HTTP 401 backoff to avoid continuous request hammering on auth errors
unsigned long lastAuthErrorMs = 0;
unsigned long authErrorBackoffMs = 5000;
constexpr unsigned long MAX_AUTH_BACKOFF_MS = 60000;

constexpr uint8_t SOIL_SAMPLES = 4;
constexpr uint8_t ULTRASONIC_SAMPLES = 3;

constexpr unsigned long ECHO_TIMEOUT_US = 30000;

// Broad operational bounds, not target values.
// No 20 cm minimum distance requirement.
constexpr float MIN_DISTANCE_CM = 2.0f;
constexpr float MAX_DISTANCE_CM = 450.0f;

constexpr float SOUND_SPEED_CM_PER_US = 0.0343f;

constexpr int SOIL_AIR_VALUE = 3200;
constexpr int SOIL_WATER_VALUE = 1500;

// ---------------- STATE --------------------------------------

enum PairingState {
    PAIRING_UNVERIFIED,
    PAIRING_PENDING,
    PAIRING_APPROVED,
    PAIRING_REJECTED
};

Preferences prefs;

String nodeId;
String macAddress;

String storedOperationalKeyId = "";
String storedOperationalSecret = "";
long storedPairingRequestId = 0;

PairingState pairingState = PAIRING_APPROVED;

bool isDevicePoweredOn = false;
bool isPairedAndApproved = true;
bool isMonitoringActive = false;
bool isInitialTelemetryPending = false;
unsigned long activationTimestamp = 0;

bool isSendingTelemetry = false;
unsigned long telemetryBlinkLastMs = 0;
bool telemetryBlinkState = false;

bool isWaitingForPairing = false;
unsigned long pairingWaitStartMs = 0;
unsigned long lastPairingPollMs = 0;
unsigned long pairingBlinkLastMs = 0;
bool pairingBlinkState = false;

unsigned long lastTransmissionTime = 0;
unsigned long telemetrySequence = 0;

unsigned long auditCount = 0;
unsigned long pairingAttempts = 0;
unsigned long pairingSuccesses = 0;
unsigned long pairingFailures = 0;
unsigned long statusChecks = 0;
unsigned long telemetryAttempts = 0;
unsigned long telemetrySuccesses = 0;
unsigned long telemetryFailures = 0;

String currentSoilHealth = "UNKNOWN";
String currentUltrasonicHealth = "UNKNOWN";
String currentTelemetryStatus = "UNKNOWN";

// Button debounce state
int lastActivateReading = HIGH;
int activateStableState = HIGH;
unsigned long activateDebounceAt = 0;

int lastPairingReading = HIGH;
int pairingStableState = HIGH;
unsigned long pairingDebounceAt = 0;

// ============================================================
// LOGGING
// ============================================================

void printSection(const char* title) {
    Serial.println();
    Serial.println(F("=================================================="));
    Serial.print(F("  "));
    Serial.println(title);
    Serial.println(F("=================================================="));
}

void info(const String& message) {
    Serial.print(F("[INFO] "));
    Serial.println(message);
}

void success(const String& message) {
    Serial.print(F("[SUCCESS] "));
    Serial.println(message);
}

void warning(const String& message) {
    Serial.print(F("[WARNING] "));
    Serial.println(message);
}

void errorLog(const String& message) {
    Serial.print(F("[ERROR] "));
    Serial.println(message);
}

void action(const String& message) {
    Serial.print(F("[ACTION] "));
    Serial.println(message);
}

const char* pairingStateName(PairingState state) {
    switch (state) {
        case PAIRING_PENDING:  return "PENDING";
        case PAIRING_APPROVED: return "APPROVED";
        case PAIRING_REJECTED: return "REJECTED";
        default:               return "UNVERIFIED";
    }
}

// ============================================================
// LED SIGNALS (non-blocking to HTTP/sensor code via FreeRTOS)
// LED 1 GPIO26: master ON=3 flashes, error=1 flash
// LED 2 GPIO25: soil, ultrasonic, telemetry acknowledgement
// ============================================================
constexpr float ULTRA_CLOSE_CM = 10.0f;
constexpr float ULTRA_MIDDLE_CM = 50.0f;
constexpr float SOIL_DRY_PERCENT = 30.0f;
constexpr float SOIL_WET_PERCENT = 70.0f;

struct LedPattern {
    uint8_t count;
    uint16_t onMs;
    uint16_t offMs;
    uint16_t gapMs;
};

QueueHandle_t systemLedQueue = nullptr;
QueueHandle_t activityLedQueue = nullptr;

void ledTask(void* argument) {
    const uint8_t pin = (uint8_t)(uintptr_t)argument;
    QueueHandle_t queue = pin == LED_ACTIVATE ? systemLedQueue : activityLedQueue;
    LedPattern pattern;
    for (;;) {
        if (xQueueReceive(queue, &pattern, portMAX_DELAY) == pdTRUE) {
            for (uint8_t i = 0; i < pattern.count; ++i) {
                digitalWrite(pin, HIGH);
                vTaskDelay(pdMS_TO_TICKS(pattern.onMs));
                digitalWrite(pin, LOW);
                vTaskDelay(pdMS_TO_TICKS(pattern.offMs));
            }
            vTaskDelay(pdMS_TO_TICKS(pattern.gapMs));
        }
    }
}

void queuePattern(QueueHandle_t queue, uint8_t count,
                  uint16_t onMs = 85, uint16_t offMs = 85,
                  uint16_t gapMs = 160) {
    if (!queue || !count) return;
    LedPattern p = {count, onMs, offMs, gapMs};
    // Keep freshest sensor indication if the queue ever backs up.
    if (xQueueSend(queue, &p, 0) != pdTRUE) {
        LedPattern stale;
        xQueueReceive(queue, &stale, 0);
        xQueueSend(queue, &p, 0);
    }
}

void startLedController() {
    systemLedQueue = xQueueCreate(16, sizeof(LedPattern));
    activityLedQueue = xQueueCreate(32, sizeof(LedPattern));
    if (!systemLedQueue || !activityLedQueue) {
        Serial.println("[LED] Could not create LED queues");
        return;
    }
    xTaskCreate(ledTask, "system-led", 2048,
                (void*)(uintptr_t)LED_ACTIVATE, 1, nullptr);
    xTaskCreate(ledTask, "activity-led", 2048,
                (void*)(uintptr_t)LED_PAIRING, 1, nullptr);
}

void updateLeds() {
    // LEDs are normally OFF. Only the worker tasks control the pins.
}

void flashActivationLed(uint8_t count) {
    queuePattern(systemLedQueue, count, 120, 120, 220);
}

// Kept as a compatibility wrapper: pairing/error signals use LED 1.
void flashPairingLed(uint8_t count) {
    queuePattern(systemLedQueue, count == 1 ? 5 : 1,
                 count == 1 ? 65 : 180,
                 count == 1 ? 65 : 180, 220);
}

void flashSystemError() {
    queuePattern(systemLedQueue, 1, 180, 180, 180);
}

void flashPairSuccess() {
    queuePattern(systemLedQueue, 5, 65, 65, 220);
}

void flashSensorSuccess() {
    // 1 brief flash on activity LED for successful telemetry transmission
    queuePattern(activityLedQueue, 1, 85, 85, 160);
}

void flashSensorError() {
    // Communication/transmission error: 1 flash on system LED (GPIO 26).
    queuePattern(systemLedQueue, 1, 180, 180, 180);
}

void flashSoilLevel(float percent, bool valid) {
    // Soil Moisture LED (GPIO 25):
    // 1 flash = Dry (< 30%)
    // 2 flashes = Moderate (30–70%)
    // 3 flashes = Wet (> 70%)
    // 4 flashes = Sensor error / invalid
    // gapMs = 600 ms to create a clear visual break before the ultrasonic
    // sequence that immediately follows on the same activity LED.
    if (!valid || !isfinite(percent) || percent <= 0.0f) {
        queuePattern(activityLedQueue, 4, 80, 80, 600);
    } else if (percent < SOIL_DRY_PERCENT) {
        queuePattern(activityLedQueue, 1, 100, 100, 600);
    } else if (percent <= SOIL_WET_PERCENT) {
        queuePattern(activityLedQueue, 2, 100, 100, 600);
    } else {
        queuePattern(activityLedQueue, 3, 100, 100, 600);
    }
}

void flashUltrasonicLevel(float distanceCm, bool valid) {
    // Ultrasonic LED (GPIO 25, sequenced after soil):
    // 1 flash = Very close (< 10 cm)
    // 2 flashes = Moderate distance (10–50 cm)
    // 3 flashes = Farthest (> 50 cm)
    // 4 flashes = Sensor error / invalid
    if (!valid || !isfinite(distanceCm) || distanceCm <= 0.0f) {
        queuePattern(activityLedQueue, 4, 80, 80, 200);
    } else if (distanceCm < ULTRA_CLOSE_CM) {
        queuePattern(activityLedQueue, 1, 100, 100, 200);
    } else if (distanceCm <= ULTRA_MIDDLE_CM) {
        queuePattern(activityLedQueue, 2, 100, 100, 200);
    } else {
        queuePattern(activityLedQueue, 3, 100, 100, 200);
    }
}

void flashTelemetrySent() {
    flashSensorSuccess();
}

void applyPairingState(PairingState newState) {
    bool newlyApproved = (newState == PAIRING_APPROVED && pairingState != PAIRING_APPROVED);
    pairingState = newState;

    switch (newState) {
        case PAIRING_APPROVED:
            isPairedAndApproved = true;
            isWaitingForPairing = false;
            prefs.putBool("paired", true);
            break;

        case PAIRING_PENDING:
            isPairedAndApproved = false;
            prefs.putBool("paired", false);

            isMonitoringActive = false;
            isInitialTelemetryPending = false;
            prefs.putBool("active", false);
            break;

        case PAIRING_REJECTED:
            isPairedAndApproved = false;
            isWaitingForPairing = false;
            prefs.putBool("paired", false);

            isMonitoringActive = false;
            isInitialTelemetryPending = false;
            prefs.putBool("active", false);
            break;

        case PAIRING_UNVERIFIED:
        default:
            // Preserve the last known local approval, but do not
            // treat it as proof of current backend authorization.
            break;
    }

    if (newlyApproved) flashPairSuccess();
    updateLeds();

    Serial.printf(
        "[PAIRING] State: %s\n",
        pairingStateName(pairingState)
    );
}

// ============================================================
// WI-FI CONNECTIVITY (SIMPLE DIRECT CONNECTION — NO FALLBACKS)
// ============================================================

constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000; // 20s timeout

bool connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }

    action(String("Connecting to Wi-Fi (") + WIFI_SSID + ")...");

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        delay(500);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        success(
            String("Wi-Fi connected: ") + WIFI_SSID +
            " (IP: " + WiFi.localIP().toString() + ")"
        );
        Serial.printf(
            "[WIFI] Active SSID: %s | RSSI: %d dBm | Channel: %d\n",
            WIFI_SSID,
            WiFi.RSSI(),
            WiFi.channel()
        );
        if (time(nullptr) < 1700000000) {
            syncDeviceTime();
        }
        return true;
    }

    flashSystemError();
    errorLog(
        String("Wi-Fi connection failed for SSID '") + WIFI_SSID +
        "' (status: " + String((int)WiFi.status()) + ")."
    );
    return false;
}


// ============================================================
// HTTPS INITIALIZATION
//
// setInsecure() disables certificate verification.
// Retained here as requested; use verified TLS certificates
// before deploying on an untrusted production network.
// ============================================================

bool beginSecureHttp(
    HTTPClient& http,
    WiFiClientSecure& client,
    const String& url
) {
    if (!connectWiFi()) {
        errorLog("HTTP aborted: Wi-Fi unavailable.");
        return false;
    }

    client.setInsecure();
    client.setTimeout(HTTP_TIMEOUT_MS);

    if (!http.begin(client, url)) {
        errorLog(String("HTTP initialization failed: ") + url);
        return false;
    }

    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("X-ArvaCore-Gateway-Key", GATEWAY_KEY);

    return true;
}

// ============================================================
// SIMPLE JSON FIELD EXTRACTION
// Suitable only for simple string fields without escaped quotes.
// Use ArduinoJson for general JSON parsing.
// ============================================================

bool extractJsonString(
    const String& body,
    const char* key,
    String& output
) {
    String searchKey = String("\"") + key + "\"";

    int keyPos = body.indexOf(searchKey);
    if (keyPos < 0) return false;

    int colonPos = body.indexOf(':', keyPos + searchKey.length());
    if (colonPos < 0) return false;

    int firstQuote = body.indexOf('"', colonPos + 1);
    if (firstQuote < 0) return false;

    int secondQuote = body.indexOf('"', firstQuote + 1);
    if (secondQuote < 0) return false;

    output = body.substring(firstQuote + 1, secondQuote);
    return true;
}

bool extractJsonNumber(const String& body, const char* key, long& output) {
    String searchKey = String("\"") + key + "\"";
    int keyPos = body.indexOf(searchKey);
    if (keyPos < 0) return false;
    int colonPos = body.indexOf(':', keyPos + searchKey.length());
    if (colonPos < 0) return false;
    int start = colonPos + 1;
    while (start < (int)body.length() && (body[start] == ' ' || body[start] == '\t' || body[start] == '\n' || body[start] == '\r')) {
        start++;
    }
    int end = start;
    while (end < (int)body.length() && ((body[end] >= '0' && body[end] <= '9') || body[end] == '-')) {
        end++;
    }
    if (end > start) {
        output = body.substring(start, end).toInt();
        return true;
    }
    return false;
}

bool extractJsonBool(const String& body, const char* key, bool& output) {
    String searchKey = String("\"") + key + "\"";
    int keyPos = body.indexOf(searchKey);
    if (keyPos < 0) return false;
    int colonPos = body.indexOf(':', keyPos + searchKey.length());
    if (colonPos < 0) return false;
    int start = colonPos + 1;
    while (start < (int)body.length() && (body[start] == ' ' || body[start] == '\t' || body[start] == '\n' || body[start] == '\r')) {
        start++;
    }
    if (body.substring(start, start + 4) == "true") {
        output = true;
        return true;
    } else if (body.substring(start, start + 5) == "false") {
        output = false;
        return true;
    }
    return false;
}

// ============================================================
// DEVICE AUTHENTICATION & HMAC CONTRACT (ARVA-HMAC-V1)
// ============================================================

bool syncDeviceTime() {
    info("Synchronizing device time via SNTP...");
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
    time_t now = time(nullptr);
    unsigned long start = millis();
    while (now < 1700000000 && (millis() - start < 8000)) {
        delay(250);
        now = time(nullptr);
    }
    if (now >= 1700000000) {
        Serial.printf("[NTP] Device time synchronized: epoch %ld\n", (long)now);
        return true;
    } else {
        warning("SNTP time synchronization pending or timed out; will retry on next connection.");
        return false;
    }
}

void clearOperationalCredentials() {
    storedOperationalKeyId = "";
    storedOperationalSecret = "";
    storedPairingRequestId = 0;
    prefs.remove("op_key_id");
    prefs.remove("op_secret");
    prefs.remove("pairing_req_id");
    prefs.putBool("paired", false);
    isPairedAndApproved = false;
    info("Cleared device operational credentials from Preferences.");
}

String computeSha256Hex(const String& payload) {
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256
    mbedtls_sha256_update(&ctx, (const unsigned char*)payload.c_str(), payload.length());
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    char hexStr[65];
    for (int i = 0; i < 32; i++) {
        sprintf(hexStr + (i * 2), "%02x", hash[i]);
    }
    hexStr[64] = '\0';
    return String(hexStr);
}

String base64UrlEncode(const uint8_t* data, size_t length) {
    static const char base64url_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    String result = "";
    int i = 0;
    int j = 0;
    uint8_t char_array_3[3];
    uint8_t char_array_4[4];

    while (length--) {
        char_array_3[i++] = *(data++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (i = 0; (i < 4); i++)
                result += base64url_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for (j = i; j < 3; j++)
            char_array_3[j] = '\0';

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;

        for (j = 0; (j < i + 1); j++)
            result += base64url_chars[char_array_4[j]];
    }

    return result;
}

String computeHmacSha256Base64Url(const String& key, const String& data) {
    uint8_t hmacResult[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
    mbedtls_md_hmac_update(&ctx, (const unsigned char*)data.c_str(), data.length());
    mbedtls_md_hmac_finish(&ctx, hmacResult);
    mbedtls_md_free(&ctx);
    return base64UrlEncode(hmacResult, 32);
}

String resolveBootstrapDeviceKey(const String& id) {
    if (strlen(BOOTSTRAP_DEVICE_KEY) > 0) {
        return String(BOOTSTRAP_DEVICE_KEY);
    }
    if (id == "ARVANODE-001") return "dev_sec_e62959d17ffe8ad6f98246918bd61fa4e87b7b8963cf44cc15cb5c18";
    if (id == "ARVANODE-002") return "dev_sec_67c3f4b5b6f18e4c9d5e7d0a03a1ef1da7d5aafc56dfe643e39083f5";
    if (id == "ARVANODE-003") return "dev_sec_1efa16c86709da60e5766074b3f8c0b924b13df1de343e8b2233a13c";
    if (id == "ARVANODE-004") return "dev_sec_64f495c247d9c387b82654402c239df12f3271456ab7f61e0ba81b1f";
    if (id == "ARVANODE-005") return "dev_sec_28137003e478fdde0e3309cf818632c91c4942d5693042aa617ad783";
    if (id == "ARVANODE-006") return "dev_sec_d6f2f0528826852eb61f4f4ffd9abc05883b4b87ec52a086623453e6";
    if (id == "ARVANODE-007") return "dev_sec_7fd6eee6d715538406b2104103dcad259ae1ad583373c1485f1b3ead";
    if (id == "ARVANODE-008") return "dev_sec_d19181c75649b99744dc47a9a74a60f0b55519c98b2ab4af290ee342";
    if (id == "ARVANODE-009") return "dev_sec_ec64333bbf7cb13dcf86192a0fd30e8bfeb0e89db7a4d7adad4370d6";
    if (id == "ARVANODE-010") return "dev_sec_a7465fd7d18dfcc1b1a45e4ee3548055b9c0b14a554111674c028a7f";
    return "dev_sec_e62959d17ffe8ad6f98246918bd61fa4e87b7b8963cf44cc15cb5c18";
}

void signAndAttachDeviceAuthHeaders(
    HTTPClient& http,
    const String& method,
    const String& requestPath,
    const String& payload,
    const String& customKeyId = "",
    const String& customSecret = ""
) {
    if (time(nullptr) < 1700000000) {
        warning("[DEVICE-AUTH] Clock skew detected before signing; resynchronizing NTP...");
        syncDeviceTime();
    }
    time_t nowSec = time(nullptr);
    String timestamp = String((long)nowSec);
    String nonce = String(esp_random(), HEX) + String(millis(), HEX);

    String keyId = customKeyId;
    String secret = customSecret;

    if (keyId.length() == 0) {
        if (storedOperationalKeyId.length() > 0 && storedOperationalSecret.length() > 0 && isPairedAndApproved) {
            keyId = storedOperationalKeyId;
            secret = storedOperationalSecret;
        } else {
            keyId = "bootstrap-" + nodeId;
            secret = resolveBootstrapDeviceKey(nodeId);
        }
    }

    String bodyHash = computeSha256Hex(payload);

    String canonicalRequest = "ARVA-HMAC-V1\n" +
        method + "\n" +
        requestPath + "\n" +
        bodyHash + "\n" +
        timestamp + "\n" +
        nonce + "\n" +
        nodeId + "\n" +
        keyId;

    String signature = computeHmacSha256Base64Url(secret, canonicalRequest);

    http.addHeader("X-Arva-Node-ID", nodeId);
    http.addHeader("X-Arva-Key-ID", keyId);
    http.addHeader("X-Arva-Timestamp", timestamp);
    http.addHeader("X-Arva-Nonce", nonce);
    http.addHeader("X-Arva-Signature", signature);

    Serial.printf("[DEVICE-AUTH] Signed %s %s with %s (nonce=%s)\n",
        method.c_str(), requestPath.c_str(), keyId.c_str(), nonce.c_str());
}

bool provisionAndCompletePairing(long reqId) {
    if (reqId <= 0) {
        errorLog("Invalid pairing request ID for provisioning: " + String(reqId));
        return false;
    }

    printSection("OPERATIONAL PROVISIONING");
    info("Requesting operational credentials for pairing request #" + String(reqId));

    WiFiClientSecure client;
    HTTPClient http;
    String path = "/api/v1/pairing/requests/" + String(reqId) + "/provision";
    String url = String("https://api-aquaflow.gladwindr.me") + path;

    if (!beginSecureHttp(http, client, url)) {
        errorLog("Failed to initialize provisioning HTTP request.");
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");

    // Sign with bootstrap key
    signAndAttachDeviceAuthHeaders(
        http,
        "POST",
        path,
        "",
        "bootstrap-" + nodeId,
        resolveBootstrapDeviceKey(nodeId)
    );

    int httpCode = http.POST("");
    String response = httpCode > 0 ? http.getString() : "";
    http.end();

    Serial.printf("[PROVISION] HTTP status: %d\n", httpCode);
    if (response.length() > 0) {
        Serial.printf("[PROVISION] Response: %s\n", response.c_str());
    }

    if (httpCode < 200 || httpCode >= 300) {
        errorLog("Operational credential provisioning failed with HTTP " + String(httpCode));
        return false;
    }

    String opKeyId, opSecret;
    if (!extractJsonString(response, "operationalKeyId", opKeyId) ||
        !extractJsonString(response, "operationalSecret", opSecret)) {
        errorLog("Provisioning response missing required credential fields.");
        return false;
    }

    opKeyId.trim();
    opSecret.trim();

    info("Provisioning credentials received. Storing to non-volatile preferences...");
    storedOperationalKeyId = opKeyId;
    storedOperationalSecret = opSecret;
    prefs.putString("op_key_id", storedOperationalKeyId);
    prefs.putString("op_secret", storedOperationalSecret);

    // Now prove possession: POST /api/v1/pairing/requests/{reqId}/complete signed with operational credential
    printSection("PAIRING COMPLETION HANDSHAKE");
    info("Proving possession of operational credential to backend...");

    WiFiClientSecure completeClient;
    HTTPClient completeHttp;
    String completePath = "/api/v1/pairing/requests/" + String(reqId) + "/complete";
    String completeUrl = String("https://api-aquaflow.gladwindr.me") + completePath;

    if (!beginSecureHttp(completeHttp, completeClient, completeUrl)) {
        errorLog("Failed to initialize completion HTTP request.");
        return false;
    }

    completeHttp.addHeader("Content-Type", "application/json");
    completeHttp.addHeader("Accept", "application/json");

    String completePayload = "{\"operationalKeyId\":\"" + storedOperationalKeyId + "\"}";

    // Sign with new OPERATIONAL key!
    signAndAttachDeviceAuthHeaders(
        completeHttp,
        "POST",
        completePath,
        completePayload,
        storedOperationalKeyId,
        storedOperationalSecret
    );

    int completeCode = completeHttp.POST(completePayload);
    String completeResp = completeCode > 0 ? completeHttp.getString() : "";
    completeHttp.end();

    Serial.printf("[COMPLETE] HTTP status: %d\n", completeCode);
    if (completeResp.length() > 0) {
        Serial.printf("[COMPLETE] Response: %s\n", completeResp.c_str());
    }

    if (completeCode < 200 || completeCode >= 300) {
        errorLog("Pairing completion failed with HTTP " + String(completeCode));
        return false;
    }

    applyPairingState(PAIRING_APPROVED);
    isPairedAndApproved = true;
    isWaitingForPairing = false;
    prefs.putBool("paired", true);
    success("Node successfully provisioned, verified, and paired!");
    /* Success handled by applyPairingState transition. */
    return true;
}

// ============================================================
// SOIL MOISTURE SENSOR DIAGNOSTICS
//
// Tests whether ADC samples are available and whether the input
// is electrically plausible.
//
// A single constant ADC value is not proof of failure.
// A disconnected analog input can float, and a real sensor can
// produce stable readings. This function reports suspicious
// conditions instead of claiming more than it can prove.
// ============================================================

int readSoilADC() {
    uint32_t total = 0;
    int minimum = 4095;
    int maximum = 0;
    uint8_t validSamples = 0;

    // Discard the first conversion after selecting the channel.
    analogRead(SOIL_PIN);
    delayMicroseconds(200);

    for (uint8_t i = 0; i < SOIL_SAMPLES; i++) {
        int value = analogRead(SOIL_PIN);

        if (value >= 0 && value <= 4095) {
            total += value;

            if (value < minimum) minimum = value;
            if (value > maximum) maximum = value;

            validSamples++;
        }

        delayMicroseconds(500);
    }

    if (validSamples == 0) {
        errorLog("SOIL: No usable ADC samples.");
        return -1;
    }

    int average = total / validSamples;

    Serial.printf(
        "[SOIL] Samples: %u/%u | Min: %d | Max: %d | "
        "Average ADC: %d | Spread: %d\n",
        validSamples,
        SOIL_SAMPLES,
        minimum,
        maximum,
        average,
        maximum - minimum
    );

    if (average <= 10) {
        errorLog(
            "SOIL: ADC is near zero. Check sensor power, GND, "
            "AO wiring, and GPIO 34."
        );
    } else if (average >= 4085) {
        errorLog(
            "SOIL: ADC is near full scale. Check wiring, "
            "sensor output voltage, and GPIO 34."
        );
    } else {
        success(
            "SOIL: ADC is producing a non-railed reading."
        );
    }

    if (minimum == maximum) {
        warning(
            "SOIL: All sampled values are identical. "
            "This may be normal, but verify the sensor by "
            "changing its input and observing ADC changes."
        );
    }

    return average;
}

float soilPercentFromADC(int adcValue) {
    if (adcValue < 0) return -1.0f;

    float percentage =
        (float)(SOIL_AIR_VALUE - adcValue) * 100.0f /
        (float)(SOIL_AIR_VALUE - SOIL_WATER_VALUE);

    if (percentage < 0.0f) percentage = 0.0f;
    if (percentage > 100.0f) percentage = 100.0f;

    return percentage;
}

bool checkSoilSensor(int& rawADC, float& percentage, String& healthStatus) {
    printSection("SOIL SENSOR FUNCTIONAL CHECK");

    rawADC = readSoilADC();

    if (rawADC < 0) {
        percentage = 0.0f;
        healthStatus = "ERROR";
        errorLog("SOIL RESULT: FAIL - No ADC samples (using fallback 0.00%).");
        return false;
    }

    // Do not use moisture percentage as a hardware health test.
    // Only report obvious electrical rail conditions as failures.
    bool electricalResponse =
        rawADC > 10 && rawADC < 4085;

    if (!electricalResponse) {
        percentage = 0.0f;
        healthStatus = (rawADC <= 10 || rawADC >= 4085) ? "DISCONNECTED" : "ERROR";
        Serial.printf(
            "[SOIL RESULT] ADC=%d, fallback moisture=0.00%%, "
            "electrical check=FAIL (%s)\n",
            rawADC,
            healthStatus.c_str()
        );
        return false;
    }

    percentage = soilPercentFromADC(rawADC);
    if (!isfinite(percentage) || percentage < 0.0f) {
        percentage = 0.0f;
        healthStatus = "ERROR";
        return false;
    }
    if (percentage > 100.0f) {
        percentage = 100.0f;
    }
    healthStatus = "OK";

    Serial.printf(
        "[SOIL RESULT] ADC=%d, estimated moisture=%.2f%%, "
        "electrical check=PASS (OK)\n",
        rawADC,
        percentage
    );

    return true;
}

bool checkSoilSensor(int& rawADC, float& percentage) {
    String unusedHealth;
    return checkSoilSensor(rawADC, percentage, unusedHealth);
}

// ============================================================
// ULTRASONIC SENSOR DIAGNOSTICS
//
// Checks for an actual echo pulse. Does not require a minimum
// distance of 20 cm.
//
// A valid echo confirms a response, not necessarily the
// correctness of the physical installation or target surface.
// ============================================================

bool measureUltrasonicSingle(
    float& distanceCm,
    unsigned long& durationUs
) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(5);

    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);

    digitalWrite(TRIG_PIN, LOW);

    durationUs = pulseIn(
        ECHO_PIN,
        HIGH,
        ECHO_TIMEOUT_US
    );

    if (durationUs == 0) {
        return false;
    }

    distanceCm =
        (durationUs * SOUND_SPEED_CM_PER_US) / 2.0f;

    // Broad sanity limits for pulse measurements.
    // In-range readings are not proof of correct targeting.
    if (
        !isfinite(distanceCm) ||
        distanceCm < MIN_DISTANCE_CM ||
        distanceCm > MAX_DISTANCE_CM
    ) {
        return false;
    }

    return true;
}

bool readUltrasonicAverage(float& result, String& healthStatus) {
    float total = 0.0f;
    uint8_t valid = 0;
    uint8_t timeouts = 0;
    uint8_t invalidPulses = 0;

    float minimum = MAX_DISTANCE_CM;
    float maximum = MIN_DISTANCE_CM;

    printSection("ULTRASONIC FUNCTIONAL CHECK");

    for (uint8_t i = 0; i < ULTRASONIC_SAMPLES; i++) {
        float distance = 0.0f;
        unsigned long duration = 0;

        bool ok = measureUltrasonicSingle(
            distance,
            duration
        );

        if (ok) {
            total += distance;
            valid++;

            if (distance < minimum) minimum = distance;
            if (distance > maximum) maximum = distance;

            Serial.printf(
                "[ULTRASONIC] Sample %u: echo=%lu us, "
                "distance=%.2f cm, PASS\n",
                i + 1,
                duration,
                distance
            );
        } else {
            if (duration == 0) {
                timeouts++;
            } else {
                invalidPulses++;
            }

            Serial.printf(
                "[ULTRASONIC] Sample %u: FAIL - "
                "no valid echo or out-of-range pulse (duration=%lu us)\n",
                i + 1,
                duration
            );
        }

        delayMicroseconds(2000);
    }

    Serial.printf(
        "[ULTRASONIC] Valid samples: %u/%u, "
        "timeouts: %u, invalid: %u\n",
        valid,
        ULTRASONIC_SAMPLES,
        timeouts,
        invalidPulses
    );

    if (valid == 0) {
        result = 0.0f;
        healthStatus = (timeouts == ULTRASONIC_SAMPLES) ? "TIMEOUT" : "ERROR";
        errorLog(
            "ULTRASONIC RESULT: FAIL (" + healthStatus + "). Reporting fallback 0.00 cm. "
            "Check sensor power, GND, TRIG GPIO 5, ECHO GPIO 18, voltage divider, and target placement."
        );
        return false;
    }

    result = total / valid;
    if (!isfinite(result) || result < 0.0f) {
        result = 0.0f;
        healthStatus = "ERROR";
        return false;
    }
    healthStatus = "OK";

    Serial.printf(
        "[ULTRASONIC] Average=%.2f cm, spread=%.2f cm\n",
        result,
        maximum - minimum
    );

    if (valid < ULTRASONIC_SAMPLES) {
        warning(
            "ULTRASONIC: Intermittent echoes detected. "
            "Inspect wiring, power stability, and target surface."
        );
    } else {
        success(
            "ULTRASONIC: Echo responses received for every sample."
        );
    }

    return true;
}

bool readUltrasonicAverage(float& result) {
    String unusedHealth;
    return readUltrasonicAverage(result, unusedHealth);
}

// ============================================================
// BACKEND PAIRING STATUS - FIXED NODE: Always returns true
// ============================================================

bool verifyBackendPairingStatus() {
    // Fixed node architecture: node is pre-provisioned, permanently paired and approved.
    // Zone assignment: ARVANODE-003 = Zone 3 (ArvaNode Field 03)
    pairingState = PAIRING_APPROVED;
    isPairedAndApproved = true;
    return true;
}

// ============================================================
// COMPREHENSIVE ACTIVATION AUDIT
// ============================================================

bool runActivationAudit() {
    auditCount++;

    printSection("HARDWARE AND CONNECTIVITY AUDIT");

    Serial.printf(
        "[AUDIT #%lu] Node=%s, MAC=%s\n",
        auditCount,
        nodeId.c_str(),
        macAddress.c_str()
    );

    info("Checking Wi-Fi connection.");
    bool wifiOk = connectWiFi();

    Serial.printf(
        "[AUDIT] Wi-Fi: %s\n",
        wifiOk ? "PASS" : "FAIL"
    );

    int soilADC = -1;
    float soilPercent = 0.0f;
    String soilHealth = "UNKNOWN";

    bool soilOk = checkSoilSensor(
        soilADC,
        soilPercent,
        soilHealth
    );

    float distanceCm = 0.0f;
    String ultrasonicHealth = "UNKNOWN";
    bool ultrasonicOk = readUltrasonicAverage(
        distanceCm,
        ultrasonicHealth
    );

    currentSoilHealth = soilHealth;
    currentUltrasonicHealth = ultrasonicHealth;
    if (soilOk && ultrasonicOk) {
        currentTelemetryStatus = "OK";
    } else if (soilOk || ultrasonicOk) {
        currentTelemetryStatus = "PARTIAL_FAILURE";
    } else {
        currentTelemetryStatus = "SENSOR_FAILURE";
    }

    printSection("AUDIT SUMMARY");

    Serial.printf(
        "[AUDIT] Wi-Fi:             %s\n",
        wifiOk ? "PASS" : "FAIL"
    );

    Serial.printf(
        "[AUDIT] Soil ADC:          %s (%s)\n",
        soilOk ? "PASS" : "FAIL",
        soilHealth.c_str()
    );

    Serial.printf(
        "[AUDIT] Ultrasonic echo:   %s (%s)\n",
        ultrasonicOk ? "PASS" : "FAIL",
        ultrasonicHealth.c_str()
    );

    Serial.printf(
        "[AUDIT] TELEMETRY STATUS:  %s\n",
        currentTelemetryStatus.c_str()
    );

    if (!soilOk) {
        warning(
            "Soil sensor check failed (" + soilHealth + "); node will transmit numeric 0.00% with sensorHealth=" + soilHealth + "."
        );
    }

    if (!ultrasonicOk) {
        warning(
            "Ultrasonic sensor check failed (" + ultrasonicHealth + "); node will transmit numeric 0.00 cm with sensorHealth=" + ultrasonicHealth + "."
        );
    }

    // Individual sensor failures must NEVER block node activation or telemetry loop.
    // Only Wi-Fi connectivity is required to transmit telemetry to the backend.
    return wifiOk;
}

// ============================================================
// PAIRING REQUEST - DEPRECATED: Not used for fixed nodes
// ============================================================

void requestPairing() {
    // Fixed nodes do not use pairing request functionality.
    // This function is kept for compatibility but should never be called.
    warning("Pairing request is disabled for fixed nodes.");
    flashSystemError();
}

// ============================================================
// TELEMETRY TRANSMISSION
//
// POST /api/v1/telemetry/uplink/{nodeId}
//
// Battery values remain placeholders because this firmware
// does not measure battery voltage using a voltage divider.
// ============================================================

bool sendTelemetry(
    float soilPercent,
    bool soilValid,
    const String& soilHealth,
    float distanceCm,
    bool distanceValid,
    const String& ultrasonicHealth
) {
    printSection("TELEMETRY TRANSMISSION");

    if (!isMonitoringActive) {
        warning("Telemetry blocked: monitoring inactive.");
        return false;
    }

    // Sanitize numeric values: failed sensors must always transmit 0.00 (never NaN/Inf/negative)
    float safeSoilPercent = (soilValid && isfinite(soilPercent) && soilPercent >= 0.0f)
        ? (soilPercent > 100.0f ? 100.0f : soilPercent)
        : 0.0f;

    float safeDistanceCm = (distanceValid && isfinite(distanceCm) && distanceCm >= MIN_DISTANCE_CM && distanceCm <= MAX_DISTANCE_CM)
        ? distanceCm
        : 0.0f;

    float safeWaterLevelCm = 0.0f;
    if (distanceValid && safeDistanceCm > 0.0f) {
        float calculatedWaterLevel = SENSOR_DATUM_OFFSET_CM - safeDistanceCm;
        if (isfinite(calculatedWaterLevel) && calculatedWaterLevel > 0.0f) {
            safeWaterLevelCm = calculatedWaterLevel > 91.44f ? 91.44f : calculatedWaterLevel;
        }
    }

    String safeSoilHealth = soilValid ? "OK" : (soilHealth.length() > 0 ? soilHealth : "ERROR");
    String safeUltrasonicHealth = distanceValid ? "OK" : (ultrasonicHealth.length() > 0 ? ultrasonicHealth : "TIMEOUT");
    String telemetryStatus = (soilValid && distanceValid)
        ? "OK"
        : ((soilValid || distanceValid) ? "PARTIAL_FAILURE" : "SENSOR_FAILURE");

    currentSoilHealth = safeSoilHealth;
    currentUltrasonicHealth = safeUltrasonicHealth;
    currentTelemetryStatus = telemetryStatus;

    isSendingTelemetry = true;
    updateLeds();

    WiFiClientSecure client;
    HTTPClient http;

    String telemetryPath = "/api/v1/telemetry/uplink/" + nodeId;
    String url = String("https://api-aquaflow.gladwindr.me") + telemetryPath;

    if (!beginSecureHttp(http, client, url)) {
        isSendingTelemetry = false;
        updateLeds();
        telemetryFailures++;
        errorLog("Telemetry HTTP initialization failed.");
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");
    if (strlen(GATEWAY_KEY) > 0) {
        http.addHeader("X-ArvaCore-Gateway-Key", GATEWAY_KEY);
    }

    telemetrySequence++;
    telemetryAttempts++;

    String payload = "{";
    payload += "\"nodeId\":\"" + nodeId + "\",";
    payload += "\"seq\":" + String(telemetrySequence) + ",";

    // PLACEHOLDERS: replace with actual battery measurements
    // before using these values for battery monitoring.
    payload += "\"batteryVoltage\":4.15,";
    payload += "\"batteryLevel\":95.0,";

    payload +=
        "\"soilMoisturePercent\":" +
        String(safeSoilPercent, 2) + ",";
    payload +=
        "\"waterLevelCm\":" +
        String(safeWaterLevelCm, 2) + ",";
    payload +=
        "\"telemetryStatus\":\"" + telemetryStatus + "\",";
    payload +=
        "\"sensorHealth\":{"
        "\"soilMoisture\":\"" + safeSoilHealth + "\","
        "\"ultrasonic\":\"" + safeUltrasonicHealth + "\""
        "},";

    payload += "\"measurements\":{";
    payload +=
        "\"soil_moisture\":" +
        String(safeSoilPercent, 2) + ",";
    payload +=
        "\"water_level_distance\":" +
        String(safeDistanceCm, 2) + ",";
    payload +=
        "\"water_level_cm\":" +
        String(safeWaterLevelCm, 2);
    payload += "}}";

    // Attach operational HMAC credentials (ARVA-HMAC-V1)
    signAndAttachDeviceAuthHeaders(http, "POST", telemetryPath, payload);

    Serial.printf(
        "[TELEMETRY] Sequence #%lu (status=%s, soil=%s, ultrasonic=%s)\n",
        telemetrySequence,
        telemetryStatus.c_str(),
        safeSoilHealth.c_str(),
        safeUltrasonicHealth.c_str()
    );

    Serial.printf(
        "[TELEMETRY] POST %s\n",
        url.c_str()
    );

    Serial.printf(
        "[TELEMETRY] Payload: %s\n",
        payload.c_str()
    );

    int httpCode = http.POST(payload);

    String response =
        httpCode > 0 ? http.getString() : "";

    String transportError =
        httpCode < 0 ? http.errorToString(httpCode) : "";

    http.end();

    Serial.printf(
        "[TELEMETRY] HTTP status: %d\n",
        httpCode
    );

    if (response.length() > 0) {
        Serial.printf(
            "[TELEMETRY] Response: %s\n",
            response.c_str()
        );
    }

    isSendingTelemetry = false;
    updateLeds();

    if (httpCode >= 200 && httpCode < 300) {
        telemetrySuccesses++;
        flashTelemetrySent();

        success(
            String("Telemetry accepted. Sequence #") +
            String(telemetrySequence)
        );

        return true;
    }

    telemetryFailures++;
    flashSensorError();

    errorLog(
        String("Telemetry rejected or failed. HTTP ") +
        String(httpCode)
    );

    if ((httpCode == 401 || httpCode == 403 || httpCode == 400) &&
        (response.indexOf("REVOKED") >= 0 || response.indexOf("UNREGISTERED") >= 0 || response.indexOf("EXPIRED") >= 0)) {
        warning("Node has been revoked or unpaired by backend. Clearing pairing state and operational credentials.");
        clearOperationalCredentials();
        applyPairingState(PAIRING_REJECTED);
        flashSystemError();
    }

    if (httpCode < 0) {
        errorLog(
            String("Transport error: ") + transportError
        );
    }

    return false;
}

bool sendTelemetry(
    float soilPercent,
    float distanceCm,
    bool distanceValid
) {
    bool soilValid = isfinite(soilPercent) && soilPercent >= 0.0f;
    return sendTelemetry(
        soilValid ? soilPercent : 0.0f,
        soilValid,
        soilValid ? "OK" : "ERROR",
        distanceValid ? distanceCm : 0.0f,
        distanceValid,
        distanceValid ? "OK" : "TIMEOUT"
    );
}

// ============================================================
// MONITORING CYCLE
// ============================================================

void runMonitoringCycle() {
    printSection("MONITORING CYCLE");

    if (!isMonitoringActive) {
        info("Monitoring inactive.");
        return;
    }

    Serial.printf(
        "[CYCLE] Node ID: %s\n",
        nodeId.c_str()
    );

    // Revalidate periodically, not on every telemetry cycle.
    // Transient backend failures must not erase valid locally stored pairing.
    if (millis() - lastAuthRecheckMs >= AUTH_RECHECK_MS) {
        lastAuthRecheckMs = millis();
        verifyBackendPairingStatus();
    }

    if (!isPairedAndApproved) {
        errorLog(
            "Monitoring stopped: current pairing authorization "
            "could not be confirmed."
        );

        isMonitoringActive = false;
        isInitialTelemetryPending = false;
        prefs.putBool("active", false);

        flashSystemError();
        updateLeds();
        return;
    }

    info("Checking soil sensor response.");

    int soilADC = -1;
    float soilPercent = 0.0f;
    String soilHealth = "UNKNOWN";

    bool soilOk = checkSoilSensor(
        soilADC,
        soilPercent,
        soilHealth
    );

    info("Checking ultrasonic echo response.");

    float distanceCm = 0.0f;
    String ultrasonicHealth = "UNKNOWN";

    bool distanceOk = readUltrasonicAverage(
        distanceCm,
        ultrasonicHealth
    );

    if (!soilOk) {
        soilPercent = 0.0f;
    }
    if (!distanceOk) {
        distanceCm = 0.0f;
    }

    // Report both measurements, with 4 flashes for error/zero.
    flashSoilLevel(soilPercent, soilOk);
    flashUltrasonicLevel(distanceCm, distanceOk);

    printSection("SENSOR HEALTH SUMMARY");

    Serial.printf(
        "[HEALTH] Soil sensor: %s (%s, value=%.2f%%)\n",
        soilOk ? "PASS" : "FAIL",
        soilHealth.c_str(),
        soilPercent
    );

    Serial.printf(
        "[HEALTH] Ultrasonic:  %s (%s, distance=%.2f cm)\n",
        distanceOk ? "PASS" : "FAIL",
        ultrasonicHealth.c_str(),
        distanceCm
    );

    if (!soilOk) {
        warning(
            "Soil sensor failure (" + soilHealth + ") detected; transmitting numeric 0.00% with sensorHealth=" + soilHealth + "."
        );
    }

    if (!distanceOk) {
        warning(
            "Ultrasonic sensor failure (" + ultrasonicHealth + ") detected; transmitting numeric 0.00 cm with sensorHealth=" + ultrasonicHealth + "."
        );
    }

    info("Transmitting telemetry payload to backend (2s interval policy).");

    bool sent = sendTelemetry(
        soilPercent,
        soilOk,
        soilHealth,
        distanceCm,
        distanceOk,
        ultrasonicHealth
    );

    if (!sent) {
        warning("Telemetry transmission failed.");
    }

    printSection("CYCLE COMPLETE");

    Serial.printf(
        "[SUMMARY] Telemetry successes: %lu\n",
        telemetrySuccesses
    );

    Serial.printf(
        "[SUMMARY] Telemetry failures: %lu\n",
        telemetryFailures
    );

    Serial.printf(
        "[SUMMARY] Next cycle in %lu ms\n",
        TELEMETRY_INTERVAL_MS
    );
}

// ============================================================
// ACTIVATION BUTTON
// ============================================================

void handleActivationButton() {
    unsigned long now = millis();

    int reading = digitalRead(BTN_ACTIVATE);

    if (reading != lastActivateReading) {
        activateDebounceAt = now;
    }

    if (
        now - activateDebounceAt >= DEBOUNCE_MS &&
        reading != activateStableState
    ) {
        activateStableState = reading;

        if (activateStableState == LOW) {
            printSection("ACTIVATION BUTTON PRESSED");

            if (!isDevicePoweredOn) {
                isDevicePoweredOn = true;
                flashActivationLed(3);  // Master ON: exactly three flashes

                success(
                    "Device logically activated. "
                    "Monitoring is now enabled."
                );
            }

            if (isMonitoringActive) {
                isMonitoringActive = false;
                isInitialTelemetryPending = false;
                prefs.putBool("active", false);

                updateLeds();

                success(
                    "Monitoring stopped. Device remains powered."
                );
                return;
            }

            info("Running hardware functional checks.");

            bool hardwareOk = runActivationAudit();

            if (!hardwareOk) {
                errorLog(
                    "Activation denied: one or more hardware "
                    "checks failed."
                );

                isMonitoringActive = false;
                isInitialTelemetryPending = false;
                prefs.putBool("active", false);

                flashSystemError();
                updateLeds();
                return;
            }

            success(
                "Activation passed: hardware checks OK."
            );

            isMonitoringActive = true;
            isInitialTelemetryPending = true;
            activationTimestamp = millis();
            prefs.putBool("active", true);

            updateLeds();

            info(
                "Monitoring active. First telemetry cycle "
                "will run in 2 seconds (repeats every 2 seconds)."
            );
        }
    }

    lastActivateReading = reading;
}

// ============================================================
// PAIRING BUTTON - DEPRECATED: Only runs diagnostics
// ============================================================

void handlePairingButton() {
    unsigned long now = millis();

    int reading = digitalRead(BTN_PAIRING);

    if (reading != lastPairingReading) {
        pairingDebounceAt = now;
    }

    if (
        now - pairingDebounceAt >= DEBOUNCE_MS &&
        reading != pairingStableState
    ) {
        pairingStableState = reading;

        if (pairingStableState == LOW) {
            printSection("PAIRING BUTTON PRESSED");

            if (!isDevicePoweredOn) {
                warning(
                    "Diagnostics disabled. Press Activation first."
                );
                return;
            }

            info("On-demand local diagnostics triggered via GPIO 23.");
            runActivationAudit();
        }
    }

    lastPairingReading = reading;
}

// ============================================================
// SETUP
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(700);

    Serial.println();
    Serial.println(
        F("==============================================")
    );
    Serial.println(
        F("       ARVACORE / ARVANODE SENSOR NODE")
    );
    Serial.println(
        F("       Fixed Node - No Pairing Required")
    );
    Serial.println(
        F("==============================================")
    );

    printSection("GPIO INITIALIZATION");

    pinMode(LED_PAIRING, OUTPUT);
    pinMode(LED_ACTIVATE, OUTPUT);

    digitalWrite(LED_PAIRING, LOW);
    digitalWrite(LED_ACTIVATE, LOW);
    startLedController();

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    digitalWrite(TRIG_PIN, LOW);

    pinMode(BTN_ACTIVATE, INPUT_PULLUP);
    pinMode(BTN_PAIRING, INPUT_PULLUP);

    analogReadResolution(12);
    analogSetPinAttenuation(SOIL_PIN, ADC_11db);

    success("GPIO initialized.");

    Serial.println(F("GPIO 34: Soil sensor AO"));
    Serial.println(F("GPIO 5 : Ultrasonic TRIG"));
    Serial.println(F("GPIO 18: Ultrasonic ECHO"));
    Serial.println(F("GPIO 19: Activation button"));
    Serial.println(F("GPIO 23: Pairing button (diagnostics only)"));
    Serial.println(F("GPIO 25: Sensor / telemetry activity LED"));
    Serial.println(F("GPIO 26: System / error LED"));

    printSection("PREFERENCES STORAGE");

    if (!prefs.begin("arvanode", false)) {
        errorLog("Could not initialize Preferences.");

        while (true) {
            digitalWrite(LED_ACTIVATE, LOW);
            delay(1000);
        }
    }

    // Restore approval and automatically resume monitoring after reboot.
    // Fixed nodes: permanently provisioned and active.
    isDevicePoweredOn = true;
    isMonitoringActive = false;  // Will be enabled by activation button
    isInitialTelemetryPending = false;
    activationTimestamp = millis();

    pairingState = PAIRING_APPROVED;
    isPairedAndApproved = true; // Permanently provisioned fixed node
    storedOperationalKeyId = prefs.getString("op_key_id", "");
    storedOperationalSecret = prefs.getString("op_secret", "");
    storedPairingRequestId = prefs.getLong("pairing_req_id", 0);
    if (isPairedAndApproved) {
        pairingState = PAIRING_APPROVED;
    }

    printSection("NODE IDENTITY");

    WiFi.mode(WIFI_STA);

    uint8_t mac[6] = {0};

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char buffer[18];

        snprintf(
            buffer,
            sizeof(buffer),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2],
            mac[3], mac[4], mac[5]
        );

        macAddress = String(buffer);
    } else {
        macAddress = WiFi.macAddress();
    }

    String compactMac = macAddress;
    compactMac.replace(":", "");

    if (strlen(PROVISIONED_NODE_ID) > 0) {
        nodeId = String(PROVISIONED_NODE_ID);
    } else {
        nodeId = compactMac.length() >= 4
            ? "NODE-" +
              compactMac.substring(compactMac.length() - 4)
            : "NODE-01";
    }

    Serial.printf(
        "[BOOT] Node ID: %s (Zone 1)\n",
        nodeId.c_str()
    );

    Serial.printf(
        "[BOOT] Display Name: %s\n",
        PROVISIONED_DISPLAY_NAME
    );

    Serial.printf(
        "[BOOT] Firmware Ver: %s\n",
        PROVISIONED_FIRMWARE_VER
    );

    Serial.printf(
        "[BOOT] Build ID: %s\n",
        PROVISIONED_BUILD_ID
    );

    Serial.printf(
        "[BOOT] MAC: %s\n",
        macAddress.c_str()
    );

    Serial.printf(
        "[BOOT] Backend: %s\n",
        BACKEND_BASE_URL
    );

    printSection("INITIAL WI-FI CONNECTION");
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) {
        syncDeviceTime();
    }

    printSection("BUTTON INITIALIZATION");

    lastActivateReading = digitalRead(BTN_ACTIVATE);
    activateStableState = lastActivateReading;
    activateDebounceAt = millis();

    lastPairingReading = digitalRead(BTN_PAIRING);
    pairingStableState = lastPairingReading;
    pairingDebounceAt = millis();

    updateLeds();

    printSection("SYSTEM READY");

    Serial.println(F("[STATUS] Mode: STANDBY"));
    Serial.println(F("[STATUS] Monitoring: OFF"));
    Serial.println(F("[STATUS] Telemetry interval: 2 seconds (initial: 2 seconds)"));
    Serial.println(F("[STATUS] Fixed node: ARVANODE-003 -> Zone 3 (TDMA Slot 20s)"));

    Serial.println();
    Serial.println(F("1. Open Serial Monitor at 115200 baud."));
    Serial.println(F("2. Press GPIO 19 to activate and start monitoring."));
    Serial.println(F("3. Check sensor diagnostic results."));
    Serial.println(F("4. Telemetry will transmit every 2 seconds."));
    Serial.println();
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
    handleActivationButton();
    handlePairingButton();

    unsigned long now = millis();

    // Retry Wi-Fi independently of sensor failures.
    if (WiFi.status() != WL_CONNECTED && now - lastWifiRetryMs >= WIFI_RETRY_MS) {
        lastWifiRetryMs = now;
        WiFi.reconnect();
    }

    // Telemetry cycle timing
    if (isMonitoringActive) {
        if (isInitialTelemetryPending) {
            if (now - activationTimestamp >= INITIAL_TELEMETRY_DELAY_MS) {
                isInitialTelemetryPending = false;
                lastTransmissionTime = now;
                info("Initial activation timer elapsed (2s). Triggering telemetry.");
                runMonitoringCycle();
            }
        } else if (now - lastTransmissionTime >= TELEMETRY_INTERVAL_MS) {
            lastTransmissionTime = now;
            runMonitoringCycle();
        }
    }

    // Continuously service LED state changes and non-blocking blinking
    updateLeds();
}
