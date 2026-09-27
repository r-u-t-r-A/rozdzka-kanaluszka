// wrozka-kanalozka: single-button VTX channel switcher for ExpressLRS.
//
// Sends MSP_SET_VTX_CONFIG over ESP-NOW to the ExpressLRS TX backpack, which
// forwards it to the TX module. The TX module only takes band/channel from this
// packet (power and pitmode stay as configured) and pushes it to the quad via
// VTX Admin. When disarmed, the TX echoes the new config back through the
// backpack, which we use as confirmation.
//
// Requirements:
//  - ESP32-based ELRS TX module (ELRS 3.0.0+), TX backpack firmware 1.4.0+
//  - MY_BINDING_PHRASE (platformio.ini) equal to the TX/backpack binding phrase
//  - change channels while disarmed

#include <Arduino.h>
#include <MD5Builder.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#ifndef MY_BINDING_PHRASE
#error "Set MY_BINDING_PHRASE in platformio.ini build_flags"
#endif

// ---- Configuration ---------------------------------------------------------

static const uint8_t NUM_PRESETS = 4;
static const uint8_t BUTTON_PIN = BUTTON;  // onboard BOOT button (GPIO9), cycles presets
static const char *PRESET_NAMES[NUM_PRESETS] = {"R1", "R3", "R6", "R7"};
// VTX table index = (band - 1) * 8 + (channel - 1), Raceband is band 5
static const uint8_t PRESET_IDX[NUM_PRESETS] = {32, 34, 37, 38};
static const uint8_t PRESET_COLORS[NUM_PRESETS][3] = {
    {0, 255, 255},  // R1 cyan
    {0, 255, 0},    // R3 green
    {0, 0, 255},    // R6 blue
    {255, 0, 255},  // R7 magenta
};

static const uint8_t LED_PIN = PIN_NEOPIXEL;
static const uint8_t LED_DIM = 16;     // idle brightness (0-255 scale)
static const uint8_t LED_BRIGHT = 96;  // feedback brightness

static const uint8_t ESPNOW_CHANNEL = 1;
static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t PRESS_LOCKOUT_MS = 150;
static const uint8_t MAX_SEND_ATTEMPTS = 3;
static const uint32_t RETRY_DELAY_MS = 50;
static const uint32_t ECHO_TIMEOUT_MS = 3500;  // TX waits ~1 s before sending
static const uint32_t FEEDBACK_MS = 1500;
static const uint32_t BOOT_REQUEST_WINDOW_MS = 5000;

// ---- MSP -------------------------------------------------------------------

static const uint16_t MSP_ELRS_REQU_VTX_PKT = 0x0B;
static const uint16_t MSP_SET_VTX_CONFIG = 0x59;
static const uint16_t MSP_MAX_PAYLOAD = 64;

static uint8_t crc8_dvb_s2(uint8_t crc, uint8_t a)
{
    crc ^= a;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x80) ? (crc << 1) ^ 0xD5 : crc << 1;
    return crc;
}

// Builds an MSPv2 command frame; returns its length
static size_t buildMspFrame(uint8_t *out, uint16_t function, const uint8_t *payload, uint16_t len)
{
    size_t n = 0;
    out[n++] = '$';
    out[n++] = 'X';
    out[n++] = '<';
    out[n++] = 0;  // flags
    out[n++] = function & 0xFF;
    out[n++] = function >> 8;
    out[n++] = len & 0xFF;
    out[n++] = len >> 8;
    memcpy(&out[n], payload, len);
    n += len;
    uint8_t crc = 0;
    for (size_t i = 3; i < n; i++)
        crc = crc8_dvb_s2(crc, out[i]);
    out[n++] = crc;
    return n;
}

// Validates one MSPv2 frame; on success sets function/payload/len
static bool parseMspFrame(const uint8_t *data, int dataLen, uint16_t &function, const uint8_t *&payload,
                          uint16_t &len)
{
    if (dataLen < 9 || data[0] != '$' || data[1] != 'X' || (data[2] != '<' && data[2] != '>'))
        return false;
    function = data[4] | (data[5] << 8);
    len = data[6] | (data[7] << 8);
    if (len > MSP_MAX_PAYLOAD || dataLen < 9 + len)
        return false;
    uint8_t crc = 0;
    for (int i = 3; i < 8 + len; i++)
        crc = crc8_dvb_s2(crc, data[i]);
    if (crc != data[8 + len])
        return false;
    payload = &data[8];
    return true;
}

// ---- State -----------------------------------------------------------------

static uint8_t uid[6];

// Written from the WiFi task callbacks
static volatile bool echoReceived = false;
static volatile uint8_t echoIdx = 0;
static volatile bool sendDone = false;
static volatile bool sendOk = false;

enum class Feedback { None, Pending, Confirmed, Unconfirmed, Failed };

static int16_t currentIdx = -1;  // last known VTX index, -1 = unknown
static Feedback feedback = Feedback::None;
static uint32_t feedbackSince = 0;

static struct {
    bool active;
    uint8_t preset;
    uint8_t attempts;
    bool waitingForSendCb;
    bool acked;
    uint32_t lastSendAt;
} pending = {};

static bool buttonStable;
static bool buttonLastRead;
static uint32_t buttonChangedAt;
static uint32_t lastPressAt = 0;
static uint32_t lastRequestAt = 0;

// ---- ESP-NOW ---------------------------------------------------------------

static void onDataSent(const esp_now_send_info_t *, esp_now_send_status_t status)
{
    sendOk = status == ESP_NOW_SEND_SUCCESS;
    sendDone = true;
}

static void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (memcmp(info->src_addr, uid, 6) != 0)
        return;
    uint16_t function, payloadLen;
    const uint8_t *payload;
    if (!parseMspFrame(data, len, function, payload, payloadLen))
        return;
    if (function == MSP_SET_VTX_CONFIG && payloadLen >= 1) {
        echoIdx = payload[0];
        echoReceived = true;
    }
}

static bool sendMsp(uint16_t function, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[9 + MSP_MAX_PAYLOAD];
    size_t n = buildMspFrame(frame, function, payload, len);
    return esp_now_send(uid, frame, n) == ESP_OK;
}

static void deriveUid()
{
    // Same as ExpressLRS build_flags.py: md5 of the literal define line
    const char *define = "-DMY_BINDING_PHRASE=\"" MY_BINDING_PHRASE "\"";
    MD5Builder md5;
    md5.begin();
    md5.add((const uint8_t *)define, strlen(define));
    md5.calculate();
    uint8_t hash[16];
    md5.getBytes(hash);
    memcpy(uid, hash, 6);
    // MAC must be unicast, like the backpack does
    uid[0] &= ~0x01;
}

static bool initEspNow()
{
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_protocol(WIFI_IF_STA,
                          WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (esp_wifi_set_mac(WIFI_IF_STA, uid) != ESP_OK)
        return false;
    if (esp_now_init() != ESP_OK)
        return false;

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, uid, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK)
        return false;

    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);
    return true;
}

// ---- Logic -----------------------------------------------------------------

static int presetForIdx(int16_t idx)
{
    for (int i = 0; i < NUM_PRESETS; i++)
        if (PRESET_IDX[i] == idx)
            return i;
    return -1;
}

static void setFeedback(Feedback f)
{
    feedback = f;
    feedbackSince = millis();
}

static void sendPendingVtx()
{
    uint8_t payload[2] = {PRESET_IDX[pending.preset], 0};
    pending.attempts++;
    pending.lastSendAt = millis();
    sendDone = false;
    pending.waitingForSendCb = sendMsp(MSP_SET_VTX_CONFIG, payload, sizeof(payload));
    Serial.printf("Sending %s (idx %u), attempt %u\n", PRESET_NAMES[pending.preset], payload[0],
                  pending.attempts);
}

static void startPreset(uint8_t preset)
{
    pending = {};
    pending.active = true;
    pending.preset = preset;
    setFeedback(Feedback::Pending);
    sendPendingVtx();
}

static void handleEcho()
{
    if (!echoReceived)
        return;
    echoReceived = false;
    uint8_t idx = echoIdx;
    currentIdx = idx;
    Serial.printf("VTX config from backpack: idx %u (band %u, ch %u)\n", idx, idx / 8 + 1, idx % 8 + 1);
    if (pending.active && PRESET_IDX[pending.preset] == idx) {
        Serial.printf("%s confirmed by TX\n", PRESET_NAMES[pending.preset]);
        pending.active = false;
        setFeedback(Feedback::Confirmed);
    }
}

static void handlePending()
{
    if (!pending.active)
        return;
    uint32_t now = millis();

    if (pending.waitingForSendCb && sendDone) {
        pending.waitingForSendCb = false;
        if (sendOk) {
            pending.acked = true;
            Serial.println("Backpack ACKed, waiting for TX echo");
        }
    }

    bool sendFailed = !pending.acked && !pending.waitingForSendCb;
    if (sendFailed && now - pending.lastSendAt >= RETRY_DELAY_MS) {
        if (pending.attempts < MAX_SEND_ATTEMPTS) {
            sendPendingVtx();
        } else {
            Serial.println("Send failed: no ACK from backpack (binding phrase? backpack in WiFi mode?)");
            pending.active = false;
            setFeedback(Feedback::Failed);
        }
        return;
    }

    if (pending.acked && now - pending.lastSendAt >= ECHO_TIMEOUT_MS) {
        // Backpack got it but the TX didn't echo (e.g. armed); assume it was applied
        Serial.println("No echo from TX (armed?), assuming applied");
        currentIdx = PRESET_IDX[pending.preset];
        pending.active = false;
        setFeedback(Feedback::Unconfirmed);
    }
}

static uint8_t nextPreset()
{
    // Step from the channel being set, else the current one; unknown starts at the first preset
    int preset = pending.active ? pending.preset : presetForIdx(currentIdx);
    return preset < 0 ? 0 : (preset + 1) % NUM_PRESETS;
}

static void handleButton()
{
    uint32_t now = millis();
    bool pressed = digitalRead(BUTTON_PIN) == LOW;
    if (pressed != buttonLastRead) {
        buttonLastRead = pressed;
        buttonChangedAt = now;
    }
    if (pressed == buttonStable || now - buttonChangedAt < DEBOUNCE_MS)
        return;
    buttonStable = pressed;
    if (pressed && now - lastPressAt >= PRESS_LOCKOUT_MS) {
        lastPressAt = now;
        startPreset(nextPreset());
    }
}

static void requestCurrentVtx()
{
    // Ask the backpack for its cached VTX packet so we can show the current channel
    uint32_t now = millis();
    if (currentIdx >= 0 || pending.active || now > BOOT_REQUEST_WINDOW_MS || now - lastRequestAt < 1000)
        return;
    lastRequestAt = now;
    uint8_t payload[1] = {0};
    sendMsp(MSP_ELRS_REQU_VTX_PKT, payload, sizeof(payload));
}

static void ledColor(const uint8_t *rgb, uint8_t brightness)
{
    rgbLedWrite(LED_PIN, rgb[0] * brightness / 255, rgb[1] * brightness / 255, rgb[2] * brightness / 255);
}

static void updateLed()
{
    static const uint8_t WHITE[3] = {255, 255, 255};
    static const uint8_t RED[3] = {255, 0, 0};
    static const uint8_t ORANGE[3] = {255, 80, 0};
    static const uint8_t OFF[3] = {0, 0, 0};

    uint32_t now = millis();
    uint32_t elapsed = now - feedbackSince;
    bool blink = (now / 100) % 2;

    if (feedback != Feedback::None && feedback != Feedback::Pending && elapsed >= FEEDBACK_MS)
        feedback = Feedback::None;

    switch (feedback) {
    case Feedback::Pending:
        ledColor(blink ? PRESET_COLORS[pending.preset] : OFF, LED_BRIGHT);
        return;
    case Feedback::Confirmed:
        ledColor(PRESET_COLORS[pending.preset], LED_BRIGHT);
        return;
    case Feedback::Unconfirmed:
        ledColor(blink ? PRESET_COLORS[pending.preset] : ORANGE, LED_BRIGHT);
        return;
    case Feedback::Failed:
        ledColor(blink ? RED : OFF, LED_BRIGHT);
        return;
    case Feedback::None:
        break;
    }

    int preset = presetForIdx(currentIdx);
    ledColor(preset >= 0 ? PRESET_COLORS[preset] : WHITE, LED_DIM);
}

// ---- Arduino ---------------------------------------------------------------

void setup()
{
    Serial.begin(115200);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    buttonStable = buttonLastRead = digitalRead(BUTTON_PIN) == LOW;

    deriveUid();
    Serial.printf("wrozka-kanalozka, backpack MAC %02X:%02X:%02X:%02X:%02X:%02X\n", uid[0], uid[1], uid[2],
                  uid[3], uid[4], uid[5]);

    if (!initEspNow()) {
        Serial.println("ESP-NOW init failed");
        static const uint8_t RED[3] = {255, 0, 0};
        ledColor(RED, LED_BRIGHT);
        while (true)
            delay(1000);
    }
}

void loop()
{
    handleEcho();
    handlePending();
    handleButton();
    requestCurrentVtx();
    updateLed();
    delay(5);
}
