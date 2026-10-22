/*
  Silvestre del Moro Park — Diorama Controller v4
  ESP32 + ILI9488 480×320 + XPT2046 touch + rotary encoder + WiFi HTTP server

  TFT scope  : Gate access screen  |  Lights (Basic / Sound / Adaptive)  |  Audio via slave ESP32
  Web App    : Full control via HTTP GET endpoints (same ESP32 state)

  ── Wiring ──────────────────────────────────────────────────────────────────
  TFT SPI      : SCK 18, MOSI 23, TFT_CS 15, DC 2, RST 4
  Touch        : XPT2046 shares SPI; T_CLK 18, T_DIN 23, T_DO 19, T_CS 21
  Note         : Leave ILI9488 SDO disconnected; only XPT2046 T_DO connects to GPIO 19.
  Rotary      : CLK 25, DT 26, SW 27
  TB6612FNG   : PWMA + PWMB -> GPIO 13 (shared pump speed); AIN1/BIN1 -> 3V3,
                AIN2/BIN2 -> GND, STBY -> 3V3. Use an external pump supply.
  Force sensor: connected to slave ESP32 GPIO 36 (VP / ADC1).
  SD/audio    : SD card and PAM8403 are on the slave ESP32; main proxies audio controls.
  Mic         : GPIO 35  (ADC1 input-only analog)
  RGB LEDs    : driven by the slave ESP32; m  ain forwards RGB/mode commands
  Shared I2C bus: ESP32 SDA=32, SCL=33 -> VEML7700 and PCA9685 SDA/SCL
  Color controller UART1: Nano TX -> ESP32 GPIO 34 (D34), 9600 baud
                         Level converter: HV=5V(Nano), HV1=Nano TX, HV-GND=Nano GND
                                          LV=3.3V(ESP32), LV1=GPIO34(D34), LV-GND=ESP32 GND
                         BOTH GND pins on the level converter must be connected!
  Color UART packet, one line at 9600 baud: JSON RGB or three RGB numbers
  Fountain pumps: TB6612FNG PWMA/PWMB <- main ESP32 GPIO 13 (shared PWM speed);
                  AIN1/BIN1 -> 3.3V, AIN2/BIN2 -> GND, STBY -> 3.3V
  Intake pump   : slave ESP32 GPIO 27 through a suitable motor driver.
  Drain pump    : slave ESP32 GPIO 33 through a suitable motor driver.
  Fingerprint : RX2=16 ← sensor TX,  TX2=17 → sensor RX  (UART2)
  Gate servo   : signal GPIO 22; power from a suitable external supply,
                 with the servo ground connected to ESP32 GND

  ── HTTP API (same endpoints as Web App) ────────────────────────────────────
  GET /api/light?state=on|off&brightness=0-100
  GET /api/mode?mode=Basic|Colorful|Custom+Color|Sound+Reactive|Color+Adaptive
  GET /api/color?r=0-255&g=0-255&b=0-255&target=left|right|center|all
                                               (all RGB zones share one color)
  GET /api/color/source?mode=scan|picker
  GET /api/fountain?state=on|off&strength=0-100&auxStrength=0-100
  GET /api/intake-pump?state=on|off
  GET /api/force-sensor?state=on|off
  GET /api/force                                → {"active":bool,"level":0-1023}
  GET /api/drainage-pump?state=on|off
  GET /api/gate?state=open|closed
  GET /api/gate/status                          → {"open":true|false}
  GET /api/audio/play?file=/DIORAMA/xxx.mp3
  GET /api/audio/pause
  GET /api/audio/stop
  GET /api/audio/files                          → {"files":["/DIORAMA/001.mp3",...]}
  Note: Audio endpoints proxy to the slave ESP32 and return an error if it is unavailable.
  GET /api/sound-reactive?state=on|off&intensity=0-100
  GET /api/sound                                → {"detected":bool,"level":0-1023}
*/

// ── Types — defined inline to avoid header include issues ───────────────────
struct RGB    { uint8_t r, g, b; };   // must be first — used in function signatures
enum Page     { P_LIGHTS = 0, P_AUDIO = 1, P_SETTINGS = 2 };
enum Mode     { M_BASIC = 0, M_COLOR = 1, M_SOUND = 2, M_ADAPT = 3 };
enum Drag     { D_NONE = 0, D_BR, D_HUE, D_SAT, D_SENS };
enum GateState { GS_WAITING = 0, GS_SCANNING = 1, GS_WELCOME = 2, GS_OPEN = 3, GS_GOODBYE = 4 };
enum EncoderMode { ENC_NAVIGATE, ENC_FOCUS, ENC_ADJUST };
enum EncoderControl {
  ENC_CONTROL_NONE,
  ENC_CONTROL_LIGHT_MODE,
  ENC_CONTROL_LIGHT_POWER,
  ENC_CONTROL_BRIGHTNESS,
  ENC_CONTROL_COLOR_SOURCE,
  ENC_CONTROL_HUE,
  ENC_CONTROL_SATURATION,
  ENC_CONTROL_SENSITIVITY,
  ENC_CONTROL_AUDIO_PREVIOUS,
  ENC_CONTROL_AUDIO_PLAY,
  ENC_CONTROL_AUDIO_NEXT,
  ENC_CONTROL_SETTINGS_ENROLL,
  ENC_CONTROL_SETTINGS_DELETE_0,
  ENC_CONTROL_SETTINGS_DELETE_1,
  ENC_CONTROL_SETTINGS_DELETE_2,
  ENC_CONTROL_SETTINGS_DELETE_3,
  ENC_CONTROL_SETTINGS_DELETE_4,
  ENC_CONTROL_SETTINGS_DELETE_5
};

// User structure for fingerprint management
struct FingerprintUser {
  int id;
  char name[32];
  bool active;
};

// ── Libraries ────────────────────────────────────────────────────────────────
#include <FS.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <Adafruit_Fingerprint.h>
#include "webapp_embed.h"

#if TFT_CS == TOUCH_CS
  #error "TFT_CS and TOUCH_CS must be connected to separate GPIO pins"
#endif

// ── WiFi — Access Point mode ──────────────────────────────────────────────────
// The ESP32 creates its own WiFi network.
// Phone/PC connects to this network, then opens http://192.168.4.1
// to reach the web app (if served from ESP32 SPIFFS) or uses the IP
// shown on the TFT to configure the web app's Settings page.
#define WIFI_AP_SSID  "Diorama-Park"
#define WIFI_AP_PASS  "diorama123"   // min 8 chars; set "" for open network

// ── Pins ─────────────────────────────────────────────────────────────────────
#define MIC_PIN   35
#define ENCODER_CLK_PIN 25
#define ENCODER_DT_PIN  26
#define ENCODER_SW_PIN  27

#define I2C_SDA_PIN 32  // Shared by the ambient sensor and PWM extender
#define I2C_SCL_PIN 33
#define COLOR_SENSOR_RX_PIN 34  // GPIO34 (D34) — UART1 RX from Nano via level converter
#define COLOR_SENSOR_TX_PIN -1  // TX not needed — ESP32 only receives from Nano
#define COLOR_SENSOR_BAUD   9600
#define COLOR_SENSOR_TIMEOUT_MS 5000
#define COLOR_SENSOR_INVERTED_PWM false // Nano sends direct color values (255=Bright, 0=Dark)
constexpr uint8_t PUMP_PWM_PIN = 13;
constexpr uint8_t GATE_SERVO_PIN = 22;
constexpr uint8_t GATE_SERVO_CHANNEL = 15;
constexpr uint32_t GATE_SERVO_FREQUENCY = 50;
constexpr uint8_t GATE_SERVO_RESOLUTION = 16;
constexpr uint16_t GATE_SERVO_MIN_PULSE_US = 1000;
constexpr uint16_t GATE_SERVO_MAX_PULSE_US = 2000;
constexpr uint16_t GATE_SERVO_PERIOD_US = 20000;

// Fingerprint sensor pins (UART2)
#define FP_RX_PIN    16
#define FP_TX_PIN    17

// Calibration
#define CALIBRATION_FILE "/DioramaCalData"
#define REPEAT_CAL       false  // Recalibrate after correcting the shared MISO wiring

// User data file
#define USERS_FILE "/fingerprint_users.json"
#define MAX_USERS 50

// Inner-circle motion is physically driven by the slave ESP32.
bool innerCircleMotorRunning = false;
uint32_t lastSoundDetectedMs = 0;
constexpr uint32_t SOUND_MOTOR_HOLD_MS = 600; // Keep rotating smoothly across audio pulses

void stopMotors() {
  innerCircleMotorRunning = false;
}

void runInnerCircle(bool = true) {
  innerCircleMotorRunning = true;
}

// ── Objects ───────────────────────────────────────────────────────────────────
TFT_eSPI   tft = TFT_eSPI();
WebServer  server(80);

// Fingerprint sensor
HardwareSerial fingerprintSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerprintSerial);
HardwareSerial colorSensorSerial(1);

// Fingerprint users array
FingerprintUser users[MAX_USERS];
int userCount = 0;
bool fpSensorAvailable = false;

// ── Colour palette (RGB565) ───────────────────────────────────────────────────
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t C_BG     = rgb565(245, 250, 247);
const uint16_t C_CARD   = rgb565(255, 255, 255);
const uint16_t C_HDR    = rgb565(255, 255, 255);
const uint16_t C_BORDER = rgb565(196, 217, 205);
const uint16_t C_TRACK  = rgb565(215, 228, 222);
const uint16_t C_TEXT   = rgb565( 20,  35,  30);
const uint16_t C_DIM    = rgb565(120, 140, 132);
const uint16_t C_WHITE  = rgb565(255, 255, 255);
const uint16_t C_GREEN  = rgb565( 16, 150,  95);
const uint16_t C_AMBER  = rgb565(230, 130,   0);
const uint16_t C_PURPLE = rgb565(124,  58, 237);
const uint16_t C_RED    = rgb565(220,  50,  60);
const uint16_t C_FOCUS  = rgb565(  0, 120, 215);

// ── Geometry ──────────────────────────────────────────────────────────────────
//  480 × 320 landscape.  Header = top 36px.  Usable area = y 36..319 (284px).
const int W = 480, H = 320;
const int HDR_H = 36;         // header bar height
const int BODY_Y = HDR_H;     // body starts here
const int BODY_H = H - HDR_H; // 284 px

// Slider rail X extents
const int SL_X0 = 20, SL_X1 = 460;

// ── Colour helpers ────────────────────────────────────────────────────────────

RGB hsv(int h, int s, int v) {
  float S = s / 100.f, V = v / 100.f;
  float C = V * S, X = C * (1 - fabsf(fmodf(h / 60.f, 2.f) - 1)), m = V - C;
  float r, g, b;
  switch ((h / 60) % 6) {
    case 0: r = C; g = X; b = 0; break;
    case 1: r = X; g = C; b = 0; break;
    case 2: r = 0; g = C; b = X; break;
    case 3: r = 0; g = X; b = C; break;
    case 4: r = X; g = 0; b = C; break;
    default: r = C; g = 0; b = X;
  }
  return { (uint8_t)((r + m) * 255 + .5f),
           (uint8_t)((g + m) * 255 + .5f),
           (uint8_t)((b + m) * 255 + .5f) };
}
uint16_t c565(RGB c) { return tft.color565(c.r, c.g, c.b); }

// Preset palette (12 swatches for Custom Color mode)
const RGB PAL[12] = {
  {255, 68, 68},{255,140,  0},{255,215,  0},{124,252,  0},
  {  0,191,255},{138, 43,226},{255,105,180},{255,255,255},
  {  0,206,209},{255, 99, 71},{ 65,105,225},{ 50,205, 50}
};

// ── Shared diorama state ──────────────────────────────────────────────────────
// Lights
bool    lightsOn    = false;   // start OFF until gate opens
int     brightness  = 75;      // 0-100
String  lightMode   = "Basic"; // Colorful is the internal ID for the Custom Color mode.
bool    soundReactive = false;

// Color zones (left, right, center)  — index 0=left 1=right 2=center
RGB     zone[3]     = { {255,255,255}, {255,255,255}, {255,255,255} };
RGB     cur = {255, 255, 255};
int     hue = 36, sat = 46;
bool    colorScanMode = true;

// Fountain
constexpr int MAX_FOUNTAIN_PWM = 150;
bool    fountainOn  = false;
int     fountainStr = 100;  // 0-100
int     fountainAux = 100;  // Mirrors shared speed for compatibility with the UI state.
bool    forceSensorOn = false;
int     manualFountainStr = 100;
int     forceSensorLevel = 0; // Nonlinear sensor level normalized to 0-1023 for the web UI.
bool    intakePumpOn = false;
bool    drainagePumpOn = false;

int fountainPumpDuty() {
  return fountainOn ? fountainStr * MAX_FOUNTAIN_PWM / 100 : 0;
}

void applyFountainPumpOutput() {
  analogWrite(PUMP_PWM_PIN, fountainPumpDuty());
}

// Gate
bool    gateOpen    = false;
GateState gateState = GS_WAITING;
uint32_t  gateMs    = 0;
bool      gateServoPwmReady = false;
char      matchedUserName[32] = "";
const uint32_t SCAN_MS    = 2000;
const uint32_t WELCOME_MS = 3000;
const uint32_t GOODBYE_MS = 3000;

// Audio
#define MAX_TRACKS 24
char     trackPath[MAX_TRACKS][96];
int      trackCount = 0, curTrack = 0;
bool     sdOk = false, audioPlaying = false, audioStarted = false;
uint32_t trackStartMs = 0;
bool     slaveAudioReachable = false;
uint32_t lastSlaveAudioSyncMs = 0;
uint32_t lastSlaveAudioListMs = 0;
bool     autoDefaultAudioAttempted = false;
bool     autoAudioResumeAfterSound = false;
bool     soundPauseAttempted = false;
bool     soundResumeAttempted = false;
uint32_t lastAutoAudioAttemptMs = 0;
uint32_t lastSoundPauseAttemptMs = 0;
uint32_t lastSoundResumeAttemptMs = 0;

// TFT page / mode
Page  page      = P_LIGHTS;
Mode  tftMode   = M_BASIC;   // TFT-local mode tab
Drag  dragging  = D_NONE;
EncoderMode encoderMode = ENC_NAVIGATE;
EncoderControl encoderControl = ENC_CONTROL_BRIGHTNESS;
uint8_t encoderControlIndex = 0;
volatile uint8_t encoderPreviousState = 0;
volatile int16_t encoderQuarterSteps = 0;
bool encoderButtonStableHigh = true;
bool encoderButtonCandidateHigh = true;
uint32_t encoderButtonChangeMs = 0;
uint32_t encoderButtonDownMs = 0;
bool encoderLongPressHandled = false;

// Sound reactive live level (mic)
int   sens    = 60;
float lvl     = 0;
int   hueBase = 0;
  RGB   live    = {255, 255, 255};

int micRawMin = 0;
int micRawMax = 0;
int micPeakToPeak = 0;
int micRms = 0;
int micLevel = 0;
bool micDetected = false;
uint32_t micLastSampleMs = 0;
uint32_t micLastLogMs = 0;

bool ambientSensorAvailable = false;
bool colorSensorAvailable = false;
bool colorSensorScanning = false;
bool colorSensorReceiveSeen = false;
bool ambientSensorConfigured = false;
float ambientLux = 0.0f;
RGB detectedColor = {255, 255, 255};
uint32_t colorSensorLastPacketMs = 0;
uint32_t ambientLastDrawMs = 0;

bool wasTouched = false;
uint16_t touchCalData[5] = {};
bool touchCalibrationReady = false;

// Forward declarations
void txt(const char* s, int x, int y, int font, uint16_t fg, uint16_t bg, uint8_t datum = TL_DATUM);
void drawColorPanel();
void applyScannedColor();
void rgbToHsvValues(uint8_t r, uint8_t g, uint8_t b, int* h, int* s);
void drawGateScreen();
void enterGateState(GateState s);
void drawHeader();
void drawLights();
void drawAudio();
void drawSettings();
void onDrag(int x);
void pushZones();
void applyHardwareGate(bool open);
void sendSlaveRGB(uint8_t group, RGB col, int br);
void pollEncoder();
void applyEncoderStep(int direction);
void syncEncoderControlIndex();
void selectEncoderControl(EncoderControl control);
void IRAM_ATTR encoderQuadratureISR();
void startEnrollment();
bool deleteUser(int fpID);
void togglePlay();
bool stepTrack(int direction, String* response = nullptr, int* statusCode = nullptr);
bool sendSlaveAudioCommand(const String& endpoint);

// ═══════════════════════════════════════════════════════════════════════════════
// SLAVE ESP32 COMMUNICATION
// ═══════════════════════════════════════════════════════════════════════════════
// The slave ESP32 (192.168.4.200) drives 3 physically shared RGB groups via MOSFET PWM.
// Master sends fire-and-forget HTTP GET requests — no blocking wait.
//
//   All three physical groups share one RGB value.
//
// Every /rgb (and /off) includes &mode=Basic|Colorful|Sound|Adaptive so the
// slave knows which light mode owns the output:
//   Basic     → white (255,255,255)
//   Colorful  → custom zone RGB
//   Sound     → live sound-reactive RGB
//   Adaptive  → zone RGB + ambient brightness
//
// The slave also uses inverted PWM (0=bright, 255=off) so we send logical
// RGB values (0-255) plus brightness (0-100) and let the slave invert.

#define SLAVE_IP "192.168.4.200"

// If the slave is offline / reboot-looping, connect() can block the main loop and
// make the web app unreachable. Back off hard after a failed attempt.
static uint32_t slaveBackoffUntilMs = 0;
static uint32_t lastSlaveOkMs       = 0;
static uint32_t lastSlavePushMs     = 0;

// Short mode tag for the slave (URL-safe, no spaces).
const char* slaveModeTag() {
  if (lightMode == "Basic")           return "Basic";
  if (lightMode == "Colorful")        return "Colorful";
  if (lightMode == "Sound Reactive")  return "Sound";
  if (lightMode == "Color Adaptive")  return "Adaptive";
  return "Basic";
}

// Mode-correct RGB for the slave: Basic is always white; other modes use zone colors.
RGB slaveRgbForMode(RGB col) {
  if (lightMode == "Basic") return {255, 255, 255};
  return col;
}

// Send one RGB group command to the slave.
// Fail-fast + backoff so a dead/rebooting slave cannot freeze the web server.
// Phone/PC and slave may both join the SoftAP — that is normal and supported.
void sendSlaveRGB(uint8_t group, RGB col, int br) {
  if (WiFi.softAPgetStationNum() == 0) return;

  const uint32_t now = millis();
  if (now < slaveBackoffUntilMs) return;  // slave recently unreachable — protect web UI

  const char* grpStr = (group == 0) ? "all"
                     : (group == 1) ? "1"
                     : (group == 2) ? "2" : "3";

  const RGB out = slaveRgbForMode(col);
  const char* mode = slaveModeTag();
  const int soundDetected = (lightMode == "Sound Reactive" && micDetected && micLevel > 0) ? 1 : 0;

  char url[220];
  if (lightsOn) {
    snprintf(url, sizeof(url),
             "GET /rgb?group=%s&r=%d&g=%d&b=%d&brightness=%d&mode=%s&sound=%d HTTP/1.0\r\nHost: " SLAVE_IP "\r\nConnection: close\r\n\r\n",
             grpStr, out.r, out.g, out.b, br, mode, soundDetected);
  } else {
    snprintf(url, sizeof(url),
             "GET /off?mode=%s HTTP/1.0\r\nHost: " SLAVE_IP "\r\nConnection: close\r\n\r\n",
             mode);
  }

  WiFiClient client;
  client.setTimeout(50);
  if (!client.connect(IPAddress(192, 168, 4, 200), 80, 200)) {
    // Slave down or rebooting — pause slave traffic so SoftAP stays free for the phone
    slaveBackoffUntilMs = now + 4000;
    Serial.println("[SLAVE] Unreachable — backing off 4s (web app stays available)");
    return;
  }

  client.print(url);
  unsigned long t = millis();
  while (client.connected() && millis() - t < 40) {
    while (client.available()) client.read();
  }
  client.stop();
  lastSlaveOkMs = millis();
  slaveBackoffUntilMs = 0;
}

// Occasional steady-mode refresh. Never blocks with delay().
// Does NOT treat phone connect as slave connect (that used to stall the web app).
void syncSlaveIfConnected() {
  static uint32_t lastSyncMs = 0;
  const uint8_t stations = WiFi.softAPgetStationNum();
  const uint32_t now = millis();

  if (stations == 0) return;
  if (now < slaveBackoffUntilMs) return;

  // Only gentle resync for steady modes; Sound Reactive already pushes live
  const bool steadyMode = (lightMode == "Basic" || lightMode == "Colorful" || lightMode == "Color Adaptive");
  if (!steadyMode) return;
  if (now - lastSyncMs < 2000) return;

  lastSyncMs = now;
  pushZones();
}

// ═══════════════════════════════════════════════════════════════════════════════
// HARDWARE OUTPUT
// ═══════════════════════════════════════════════════════════════════════════════

// Push logical RGB zone colors to the slave ESP32.
// Basic mode is locked to white so stale colors cannot override that mode.
void pushZones() {
  if (lightMode == "Basic") {
    zone[0] = zone[1] = zone[2] = {255, 255, 255};
  }
  zone[1] = zone[2] = zone[0];
  for (int i = 0; i < 3; i++) {
    zone[i].r = constrain(zone[i].r, 0, 255);
    zone[i].g = constrain(zone[i].g, 0, 255);
    zone[i].b = constrain(zone[i].b, 0, 255);
  }

  // Forward zones to slave ESP32, throttled to keep the SoftAP responsive.
  const uint32_t nowSlave = millis();
  // Sound Reactive can request updates every ~40ms — cap slave HTTP to ~8/s
  if (lightMode == "Sound Reactive" && (nowSlave - lastSlavePushMs) < 120) return;
  lastSlavePushMs = nowSlave;

  sendSlaveRGB(0, zone[0], brightness);  // group=0 → sends "all"
}

void setAll(RGB c) {
  zone[0] = zone[1] = zone[2] = c;
  pushZones();
}

void applyHardwareGate(bool open) {
  if (!gateServoPwmReady) {
    Serial.println("[GATE] ERROR: servo PWM is not initialized");
    return;
  }

  const uint8_t angle = open ? 180 : 0;
  const uint32_t pulseUs = GATE_SERVO_MIN_PULSE_US +
      (static_cast<uint32_t>(GATE_SERVO_MAX_PULSE_US - GATE_SERVO_MIN_PULSE_US) *
       angle) / 180;
  const uint32_t maxDuty = (1UL << GATE_SERVO_RESOLUTION) - 1;
  const uint32_t duty = pulseUs * maxDuty / GATE_SERVO_PERIOD_US;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  if (!ledcWrite(GATE_SERVO_PIN, duty)) {
    Serial.println("[GATE] ERROR: failed to write servo PWM");
    return;
  }
#else
  ledcWrite(GATE_SERVO_CHANNEL, duty);
#endif
  Serial.printf("[GATE] Servo: %s (%u degrees)\n",
                open ? "OPEN" : "CLOSED", angle);
}

bool setupGateServo() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  gateServoPwmReady = ledcAttach(
      GATE_SERVO_PIN, GATE_SERVO_FREQUENCY, GATE_SERVO_RESOLUTION);
#else
  gateServoPwmReady = ledcSetup(
      GATE_SERVO_CHANNEL, GATE_SERVO_FREQUENCY, GATE_SERVO_RESOLUTION) > 0;
  if (gateServoPwmReady) {
    ledcAttachPin(GATE_SERVO_PIN, GATE_SERVO_CHANNEL);
  }
#endif

  if (!gateServoPwmReady) {
    Serial.printf("[GATE] ERROR: unable to initialize servo PWM on GPIO %u\n",
                  GATE_SERVO_PIN);
    return false;
  }

  applyHardwareGate(false);
  Serial.printf("[GATE] Servo PWM ready on GPIO %u at %lu Hz\n",
                GATE_SERVO_PIN, static_cast<unsigned long>(GATE_SERVO_FREQUENCY));
  return true;
}

int parseColorSensorPacket(const char* packet, RGB* color) {
  // 1. JSON format: {"r":255,"g":100,"b":50}
  StaticJsonDocument<192> doc;
  if (!deserializeJson(doc, packet) &&
      doc["r"].is<int>() && doc["g"].is<int>() && doc["b"].is<int>()) {
    const int red = doc["r"].as<int>();
    const int green = doc["g"].as<int>();
    const int blue = doc["b"].as<int>();
    if (red < 0 || red > 255 || green < 0 || green > 255 ||
        blue < 0 || blue > 255) return -1;
    *color = {(uint8_t)red, (uint8_t)green, (uint8_t)blue};
    return 1;
  }

  // 2. Labeled tokens (e.g. "R:0", "G:0", "B:0" on separate lines or "R:255 G:120 B:50" on one line)
  static int stateR = -1;
  static int stateG = -1;
  static int stateB = -1;

  bool matchedLabel = false;
  const char* p = packet;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') p++;
    if (!*p) break;
    if ((*p == 'R' || *p == 'r' || *p == 'G' || *p == 'g' || *p == 'B' || *p == 'b') &&
        (p[1] == ':' || p[1] == '=' || p[1] == ' ' || (p[1] >= '0' && p[1] <= '9'))) {
      char chan = (*p >= 'a') ? (*p - 'a' + 'A') : *p;
      p++;
      while (*p == ':' || *p == '=' || *p == ' ' || *p == '\t') p++;
      char* end = nullptr;
      long val = strtol(p, &end, 10);
      if (end > p) {
        val = constrain(val, 0, 255);
        if (chan == 'R') stateR = (int)val;
        else if (chan == 'G') stateG = (int)val;
        else if (chan == 'B') stateB = (int)val;
        matchedLabel = true;
        p = end;
        continue;
      }
    }
    p++;
  }

  if (matchedLabel) {
    if (stateR >= 0 && stateG >= 0 && stateB >= 0) {
      *color = {(uint8_t)stateR, (uint8_t)stateG, (uint8_t)stateB};
      stateR = stateG = stateB = -1;
      return 1;
    }
    return 0; // Partial component received, waiting for the rest
  }

  // 3. Three plain comma/space-separated numbers: "120,80,200" or "120 80 200"
  int values[3];
  uint8_t valueCount = 0;
  const char* cursor = packet;
  while (*cursor) {
    const bool negative = *cursor == '-' && cursor[1] >= '0' && cursor[1] <= '9';
    if ((*cursor >= '0' && *cursor <= '9') || negative) {
      if (valueCount >= 3) return -1;
      char* end = nullptr;
      const long value = strtol(cursor, &end, 10);
      if (end == cursor || value < 0 || value > 255) return -1;
      values[valueCount++] = (int)value;
      cursor = end;
    } else {
      cursor++;
    }
  }
  if (valueCount == 3) {
    *color = {(uint8_t)values[0], (uint8_t)values[1], (uint8_t)values[2]};
    return 1;
  }

  return -1;
}

void updateColorSensorScanForMode() {
  if (lightMode == "Colorful" && colorScanMode) {
    if (!colorSensorScanning) {
      colorSensorScanning = true;
      colorSensorReceiveSeen = false;
      colorSensorLastPacketMs = millis();
      Serial.println("[COLOR UART] Color scan source selected; waiting for a manual capture");
    }
  } else if (colorSensorScanning) {
    colorSensorScanning = false;
    Serial.println("[COLOR UART] Color scan source disabled");
  }
}

void rgbToHsvValues(uint8_t r, uint8_t g, uint8_t b, int* h, int* s) {
  const float rf = r / 255.0f;
  const float gf = g / 255.0f;
  const float bf = b / 255.0f;
  const float maxVal = fmaxf(rf, fmaxf(gf, bf));
  const float minVal = fminf(rf, fminf(gf, bf));
  const float delta = maxVal - minVal;

  if (delta < 0.0001f) {
    *h = 0;
    *s = 0;
    return;
  }

  *s = (int)((delta / maxVal) * 100.0f);
  float hueVal;
  if (rf >= maxVal) hueVal = (gf - bf) / delta;
  else if (gf >= maxVal) hueVal = 2.0f + (bf - rf) / delta;
  else hueVal = 4.0f + (rf - gf) / delta;
  hueVal *= 60.0f;
  if (hueVal < 0.0f) hueVal += 360.0f;
  *h = constrain((int)hueVal, 0, 359);
  *s = constrain(*s, 0, 100);
}

void applyScannedColor() {
  // A captured color is held until the user requests another scan.
  if (lightMode != "Colorful" || !colorScanMode) {
    Serial.printf("[COLOR SCAN] Ignored (mode=%s scanMode=%s)\n",
                  lightMode.c_str(), colorScanMode ? "scan" : "picker");
    return;
  }
  cur = detectedColor;
  rgbToHsvValues(cur.r, cur.g, cur.b, &hue, &sat);
  const bool zonesChanged =
    zone[0].r != cur.r || zone[0].g != cur.g || zone[0].b != cur.b ||
    zone[1].r != cur.r || zone[1].g != cur.g || zone[1].b != cur.b ||
    zone[2].r != cur.r || zone[2].g != cur.g || zone[2].b != cur.b;
  zone[0] = zone[1] = zone[2] = cur;
  if (zonesChanged) pushZones();
  if (page == P_LIGHTS && tftMode == M_COLOR && zonesChanged) {
    drawColorPanel();
  }
  Serial.printf("[COLOR SCAN] Captured & Applied RGB=%u,%u,%u (#%02X%02X%02X)\n",
                cur.r, cur.g, cur.b, cur.r, cur.g, cur.b);
}

bool captureColorSensorColor() {
  if (!colorSensorAvailable) {
    Serial.println("[COLOR SCAN] Capture failed: no recent sensor reading");
    return false;
  }
  if (lightMode != "Colorful" || !colorScanMode) {
    Serial.printf("[COLOR SCAN] Capture ignored (mode=%s scanMode=%s)\n",
                  lightMode.c_str(), colorScanMode ? "scan" : "picker");
    return false;
  }
  applyScannedColor();
  return true;
}

void processColorSensorColor(const RGB& raw) {
#if COLOR_SENSOR_INVERTED_PWM
  const RGB received = {
    (uint8_t)(255 - raw.r),
    (uint8_t)(255 - raw.g),
    (uint8_t)(255 - raw.b)
  };
#else
  const RGB received = raw;
#endif

  detectedColor = received;
  const bool wasAvailable = colorSensorAvailable;
  colorSensorAvailable = true;
  colorSensorLastPacketMs = millis();

  static uint32_t validPacketLogMs = 0;
  if (colorSensorLastPacketMs - validPacketLogMs >= 1000) {
    validPacketLogMs = colorSensorLastPacketMs;
    Serial.printf("[COLOR UART] Raw=(%u,%u,%u) -> Logical RGB=(%u,%u,%u) #%02X%02X%02X\n",
                  raw.r, raw.g, raw.b,
                  detectedColor.r, detectedColor.g, detectedColor.b,
                  detectedColor.r, detectedColor.g, detectedColor.b);
  }

  if (!wasAvailable && page == P_LIGHTS && tftMode == M_COLOR) {
    drawColorPanel();
  }
}

void processColorSensorPacket(const char* packet) {
  RGB received;
  int status = parseColorSensorPacket(packet, &received);
  if (status == 1) {
    processColorSensorColor(received);
  } else if (status == -1) {
    static uint32_t invalidPacketLogMs = 0;
    const uint32_t now = millis();
    if (now - invalidPacketLogMs >= 1000) {
      invalidPacketLogMs = now;
      Serial.printf("[COLOR UART] Unrecognized line: '%s' (expected JSON RGB or values e.g. R:120, G:80, B:200 or 120,80,200)\n",
                    packet);
    }
  }
  // status == 0: accepted partial component (e.g. "R:0"), waiting for G and B
}

void updateColorSensor() {
  static char packet[128];
  static size_t packetLength = 0;
  static bool discardingLongPacket = false;
  // Some Nano sketches send text lines; others write three raw RGB bytes.
  static bool binaryMode = false;
  static uint8_t binaryColor[3];
  static size_t binaryColorLength = 0;
  static uint32_t noDataLogMs = 0;
  static uint32_t packetLastByteMs = 0;
  static bool firstByteLogged = false;

  while (colorSensorSerial.available()) {
    const char value = (char)colorSensorSerial.read();
    colorSensorReceiveSeen = true;
    if (!firstByteLogged) {
      firstByteLogged = true;
      Serial.printf("[COLOR UART] First byte received on GPIO%d: 0x%02X '%c'\n",
                    COLOR_SENSOR_RX_PIN, (uint8_t)value,
                    ((uint8_t)value >= 0x20 && (uint8_t)value <= 0x7E) ? value : '.');
    }
    const uint32_t byteMs = millis();
    if (binaryMode && byteMs - packetLastByteMs > 1000) {
      if (binaryColorLength > 0) {
        Serial.println("[COLOR UART] Dropped incomplete binary RGB packet");
      }
      binaryMode = false;
      binaryColorLength = 0;
    }
    packetLastByteMs = byteMs;

    if (binaryMode) {
      binaryColor[binaryColorLength++] = (uint8_t)value;
      if (binaryColorLength == sizeof(binaryColor)) {
        const RGB received = {
          binaryColor[0], binaryColor[1], binaryColor[2]
        };
        processColorSensorColor(received);
        binaryColorLength = 0;
      }
      continue;
    }

    if (value == '\n' || value == '\r') {
      if (discardingLongPacket) {
        Serial.println("[COLOR UART] Discarded oversized line (limit 127 bytes)");
      } else {
        packet[packetLength] = '\0';
        if (packetLength > 0) processColorSensorPacket(packet);
      }
      packetLength = 0;
      discardingLongPacket = false;
    } else if ((uint8_t)value < 0x20 || (uint8_t)value > 0x7E) {
      packetLength = 0;
      discardingLongPacket = false;
      binaryMode = true;
      binaryColor[0] = (uint8_t)value;
      binaryColorLength = 1;
    } else {
      if (!discardingLongPacket) {
        if (packetLength < sizeof(packet) - 1) packet[packetLength++] = value;
        else {
          packetLength = 0;
          discardingLongPacket = true;
        }
      }
    }
  }

  const uint32_t now = millis();
  if (!colorSensorReceiveSeen && now - noDataLogMs >= 5000) {
    noDataLogMs = now;
    Serial.printf("[COLOR UART] No bytes received on GPIO%d at %d baud; check Nano TX -> ESP32 RX, common GND, and level-shifter direction\n",
                  COLOR_SENSOR_RX_PIN, COLOR_SENSOR_BAUD);
  }
  if (!binaryMode && packetLength > 0 && now - packetLastByteMs >= 50) {
    packet[packetLength] = '\0';
    processColorSensorPacket(packet);
    packetLength = 0;
    discardingLongPacket = false;
  }
  if (colorSensorAvailable && now - colorSensorLastPacketMs > COLOR_SENSOR_TIMEOUT_MS) {
    colorSensorAvailable = false;
    Serial.println("[COLOR UART] Color data timed out");
    if (page == P_LIGHTS && tftMode == M_COLOR) drawColorPanel();
  }
}

// Ambient sensing stays local on I2C; color UART readings are received continuously.
constexpr uint8_t VEML7700_ADDR = 0x10;
constexpr uint8_t VEML7700_ALS_CONF = 0x00;
constexpr uint8_t VEML7700_ALS_DATA = 0x04;
constexpr float VEML7700_LUX_PER_COUNT = 0.0576f;

bool writeVemlRegister(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(VEML7700_ADDR);
  Wire.write(reg);
  Wire.write((uint8_t)(value & 0xFF));
  Wire.write((uint8_t)(value >> 8));
  return Wire.endTransmission() == 0;
}

bool readVemlLux(float* lux) {
  Wire.beginTransmission(VEML7700_ADDR);
  Wire.write(VEML7700_ALS_DATA);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(VEML7700_ADDR, (uint8_t)2) != 2) return false;

  const uint16_t raw = Wire.read() | (Wire.read() << 8);
  *lux = raw * VEML7700_LUX_PER_COUNT;
  return true;
}

void setupAmbientSensor() {
  // Initialize the shared bus; do not probe/read the sensor until Adaptive mode.
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  ambientSensorAvailable = false;
  ambientSensorConfigured = false;
  Serial.printf("[AMBIENT] I2C bus ready (SDA=%d, SCL=%d); sensor reads start in Color Adaptive mode\n",
                I2C_SDA_PIN, I2C_SCL_PIN);
}

void updateAmbientSensor() {
  static uint32_t lastReadMs = 0;
  static uint32_t lastLogMs = 0;
  const uint32_t now = millis();

  if (lightMode != "Color Adaptive") {
    ambientSensorAvailable = false;
    ambientSensorConfigured = false;
    ambientLux = 0.0f;
    return;
  }

  if (now - lastReadMs >= 500) {
    lastReadMs = now;
    if (!ambientSensorConfigured) {
      ambientSensorConfigured = writeVemlRegister(VEML7700_ALS_CONF, 0x0000);
      if (!ambientSensorConfigured && now - lastLogMs >= 1000) {
        lastLogMs = now;
        Serial.println("[AMBIENT] VEML7700 setup failed; check SDA=GPIO32, SCL=GPIO33, power and ground");
      }
      return;
    }

    float measuredLux;
    ambientSensorAvailable = readVemlLux(&measuredLux);
    if (ambientSensorAvailable) {
      ambientLux = measuredLux;
      if (now - lastLogMs >= 1000) {
        lastLogMs = now;
        Serial.printf("[AMBIENT] VEML7700 lux=%.2f, adaptiveBrightness=%d%%\n",
                      ambientLux, brightness);
      }
      if (lightMode == "Color Adaptive") {
        const int targetBrightness = constrain(100 - (int)(ambientLux * 80.0f / 500.0f), 20, 100);
        if (brightness != targetBrightness) {
          brightness = targetBrightness;
          pushZones();
          if (page == P_LIGHTS) drawBrightnessRow();
        }
      }
    } else if (now - lastLogMs >= 1000) {
      lastLogMs = now;
      Serial.println("[AMBIENT] VEML7700 read failed; check SDA=GPIO32, SCL=GPIO33, power and ground");
    }
  }

  if (page == P_LIGHTS && tftMode == M_ADAPT && now - ambientLastDrawMs >= 500) {
    ambientLastDrawMs = now;
    drawModePanel();
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// DRAWING HELPERS
// ═══════════════════════════════════════════════════════════════════════════════

bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// Draw text with given font, colours and datum
void txt(const char* s, int x, int y, int font, uint16_t fg, uint16_t bg,
         uint8_t datum) {
  tft.setTextColor(fg, bg);
  tft.setTextDatum(datum);
  tft.drawString(s, x, y, font);
}

// Rounded-rect card outline
void card(int x, int y, int w, int h, uint16_t fill, uint16_t border) {
  tft.fillRoundRect(x, y, w, h, 8, fill);
  tft.drawRoundRect(x, y, w, h, 8, border);
}

// Labelled button (filled when active)
void btn(int x, int y, int w, int h, const char* label, bool active,
         uint16_t accent, uint8_t font = 2) {
  card(x, y, w, h, active ? accent : C_CARD, active ? accent : C_BORDER);
  txt(label, x + w / 2, y + h / 2, font,
      active ? C_WHITE : C_TEXT,
      active ? accent  : C_CARD,
      MC_DATUM);
}

void drawFocusCue(int x, int y, int w, int h, uint8_t order, bool focused) {
  if (focused) {
    tft.drawRoundRect(x - 3, y - 3, w + 6, h + 6, 7, C_FOCUS);
    tft.drawRoundRect(x - 2, y - 2, w + 4, h + 4, 6, C_FOCUS);
  }

  const int badgeX = x + 10;
  const int badgeY = y + 9;
  tft.fillCircle(badgeX, badgeY, 7, focused ? C_FOCUS : C_PURPLE);
  char number[4];
  snprintf(number, sizeof(number), "%u", order);
  txt(number, badgeX, badgeY, 1, C_WHITE, focused ? C_FOCUS : C_PURPLE, MC_DATUM);
}

void drawEncoderFocusCue(int x, int y, int w, int h, bool focused) {
  if (focused && encoderMode != ENC_NAVIGATE) {
    tft.drawRoundRect(x - 4, y - 4, w + 8, h + 8, 8, C_FOCUS);
  }
}

// Toggle switch (44×22)
void toggleSw(int x, int y, bool on, uint16_t accent) {
  tft.fillRoundRect(x, y, 44, 22, 11, on ? accent : C_TRACK);
  tft.fillCircle(on ? x + 33 : x + 11, y + 11, 8, C_WHITE);
  tft.drawCircle(on ? x + 33 : x + 11, y + 11, 8, C_BORDER);
}

// Horizontal slider (rail + filled portion + thumb)
void drawSlider(int y, int val, uint16_t accent) {
  tft.fillRect(0, y - 18, W, 36, C_BG);
  tft.fillRoundRect(SL_X0, y - 4, SL_X1 - SL_X0, 8, 4, C_TRACK);
  int kx = SL_X0 + (long)(SL_X1 - SL_X0) * val / 100;
  if (kx > SL_X0)
    tft.fillRoundRect(SL_X0, y - 4, kx - SL_X0, 8, 4, accent);
  tft.fillCircle(kx, y, 12, accent);
  tft.drawCircle(kx, y, 12, C_TEXT);
}

int pctFromX(int x) {
  return constrain((long)(x - SL_X0) * 100 / (SL_X1 - SL_X0), 0, 100);
}

void drawPct(int xRight, int y, int val, uint16_t col) {
  char b[8]; snprintf(b, 8, "%d%%", val);
  tft.setTextPadding(52);
  tft.setTextColor(col, C_BG);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(b, xRight, y, 4);
  tft.setTextPadding(0);
}

// Small label badge (used for section headings)
void label(const char* s, int x, int y) {
  txt(s, x, y, 2, C_DIM, C_BG, TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════════════════
// HEADER
// ═══════════════════════════════════════════════════════════════════════════════

void drawHeader() {
  tft.fillRect(0, 0, W, HDR_H, C_HDR);
  tft.drawFastHLine(0, HDR_H - 1, W, C_BORDER);

  // "Exit" tap zone — enlarged button with rounded corners
  if (gateState == GS_OPEN) {
    tft.fillRoundRect(4, 3, 76, HDR_H - 6, 6, C_RED);
    txt("Exit", 42, HDR_H / 2, 2, C_WHITE, C_RED, MC_DATUM);
  }

  // Park name (centred between Exit and tab buttons)
  txt("Silvestre del Moro", 92, HDR_H / 2, 2, C_TEXT, C_HDR, ML_DATUM);

  const char* encoderLabel = encoderMode == ENC_NAVIGATE ? "TAB"
                           : encoderMode == ENC_FOCUS ? "PICK" : "EDIT";
  const uint16_t encoderBadge = encoderMode == ENC_NAVIGATE ? C_GREEN : C_FOCUS;
  tft.fillRoundRect(208, 7, 46, 22, 5, encoderBadge);
  txt(encoderLabel, 231, 18, 1, C_WHITE, encoderBadge, MC_DATUM);

  // Page tabs - 3 tabs now (Lights, Audio, Settings)
  bool aL = (page == P_LIGHTS), aA = (page == P_AUDIO), aS = (page == P_SETTINGS);
  
  tft.fillRect(260, 0, 75, HDR_H - 1, aL ? C_AMBER  : C_HDR);
  txt("Lights", 297, HDR_H / 2, 2, aL ? C_WHITE : C_DIM, aL ? C_AMBER : C_HDR, MC_DATUM);
  
  tft.fillRect(335, 0, 70, HDR_H - 1, aA ? C_GREEN  : C_HDR);
  txt("Audio",  370, HDR_H / 2, 2, aA ? C_WHITE : C_DIM, aA ? C_GREEN : C_HDR, MC_DATUM);
  
  tft.fillRect(405, 0, 75, HDR_H - 1, aS ? C_PURPLE : C_HDR);
  txt("Settings", 442, HDR_H / 2, 2, aS ? C_WHITE : C_DIM, aS ? C_PURPLE : C_HDR, MC_DATUM);

  // WiFi AP dot (green = AP active) + SD dot
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool accessPointActive = wifiMode == WIFI_AP || wifiMode == WIFI_AP_STA;
  tft.fillCircle(474, 8,  4, accessPointActive ? C_GREEN : C_RED);
  tft.fillCircle(474, 22, 4, sdOk ? C_GREEN : C_RED);
}

// ═══════════════════════════════════════════════════════════════════════════════
// FINGERPRINT SENSOR MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════════

// Load users from SPIFFS
void loadUsers() {
  userCount = 0;
  if (!SPIFFS.exists(USERS_FILE)) {
    Serial.println("[FP] No users file found");
    return;
  }
  
  File file = SPIFFS.open(USERS_FILE, "r");
  if (!file) {
    Serial.println("[FP] Failed to open users file");
    return;
  }
  
  StaticJsonDocument<4096> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.printf("[FP] JSON parse error: %s\n", error.c_str());
    return;
  }
  
  JsonArray usersArray = doc["users"];
  for (JsonObject user : usersArray) {
    if (userCount >= MAX_USERS) break;
    users[userCount].id = user["id"];
    strlcpy(users[userCount].name, user["name"] | "Unknown", 32);
    users[userCount].active = user["active"] | true;
    userCount++;
  }
  
  Serial.printf("[FP] Loaded %d users\n", userCount);
}

// Save users to SPIFFS
void saveUsers() {
  StaticJsonDocument<4096> doc;
  JsonArray usersArray = doc.createNestedArray("users");
  
  for (int i = 0; i < userCount; i++) {
    JsonObject user = usersArray.createNestedObject();
    user["id"] = users[i].id;
    user["name"] = users[i].name;
    user["active"] = users[i].active;
  }
  
  File file = SPIFFS.open(USERS_FILE, "w");
  if (!file) {
    Serial.println("[FP] Failed to save users");
    return;
  }
  
  serializeJson(doc, file);
  file.close();
  Serial.println("[FP] Users saved");
}

// Find user by fingerprint ID
const char* getUserName(int fpID) {
  for (int i = 0; i < userCount; i++) {
    if (users[i].id == fpID && users[i].active) {
      return users[i].name;
    }
  }
  return "Unknown User";
}

// Find next available fingerprint ID
int findNextFreeID() {
  for (int id = 1; id <= 127; id++) {
    bool used = false;
    for (int i = 0; i < userCount; i++) {
      if (users[i].id == id) {
        used = true;
        break;
      }
    }
    if (!used) return id;
  }
  return -1; // All slots full
}

// Add new user
bool addUser(int fpID, const char* name) {
  if (userCount >= MAX_USERS) return false;
  
  users[userCount].id = fpID;
  strlcpy(users[userCount].name, name, 32);
  users[userCount].active = true;
  userCount++;
  
  saveUsers();
  return true;
}

// Delete user
bool deleteUser(int fpID) {
  for (int i = 0; i < userCount; i++) {
    if (users[i].id == fpID) {
      // Shift remaining users
      for (int j = i; j < userCount - 1; j++) {
        users[j] = users[j + 1];
      }
      userCount--;
      saveUsers();
      
      // Delete from sensor
      if (fpSensorAvailable) {
        finger.deleteModel(fpID);
      }
      
      Serial.printf("[FP] Deleted user ID #%d\n", fpID);
      return true;
    }
  }
  return false;
}

// Probe biometric sensor at given baud and pin combination
bool probeSensor(int baud, int rxPin, int txPin) {
  fingerprintSerial.end();
  delay(60);
  fingerprintSerial.begin(baud, SERIAL_8N1, rxPin, txPin);
  delay(120);
  return finger.verifyPassword();
}

// Probing & initializing optical fingerprint sensor
bool initFingerprintSensor() {
  Serial.println("[FP] Probing optical biometric sensor on UART2...");
  // Sensor boot-up delay
  delay(900);

  // 1. Try standard connection: Sensor TX -> GPIO 16, Sensor RX -> GPIO 17 @ 57600
  if (probeSensor(57600, 16, 17)) {
    fpSensorAvailable = true;
    Serial.println("[FP] ✓ SENSOR CONNECTED! (57600 baud, Sensor TX->GPIO16, Sensor RX->GPIO17)");
    finger.getTemplateCount();
    Serial.printf("[FP] %d template(s) stored in sensor flash\n", finger.templateCount);
    return true;
  }

  // 2. Try standard connection @ 9600 baud
  if (probeSensor(9600, 16, 17)) {
    fpSensorAvailable = true;
    Serial.println("[FP] ✓ SENSOR CONNECTED! (9600 baud, Sensor TX->GPIO16, Sensor RX->GPIO17)");
    finger.getTemplateCount();
    Serial.printf("[FP] %d template(s) stored in sensor flash\n", finger.templateCount);
    return true;
  }

  // 3. Try swapped connection: Sensor TX -> GPIO 17, Sensor RX -> GPIO 16 @ 57600
  if (probeSensor(57600, 17, 16)) {
    fpSensorAvailable = true;
    Serial.println("[FP] ✓ SENSOR CONNECTED! (57600 baud, swapped pins: RX:17, TX:16)");
    finger.getTemplateCount();
    Serial.printf("[FP] %d template(s) stored in sensor flash\n", finger.templateCount);
    return true;
  }

  // 4. Try swapped connection @ 9600 baud
  if (probeSensor(9600, 17, 16)) {
    fpSensorAvailable = true;
    Serial.println("[FP] ✓ SENSOR CONNECTED! (9600 baud, swapped pins: RX:17, TX:16)");
    finger.getTemplateCount();
    Serial.printf("[FP] %d template(s) stored in sensor flash\n", finger.templateCount);
    return true;
  }

  // 5. Try 115200 baud
  if (probeSensor(115200, 16, 17)) {
    fpSensorAvailable = true;
    Serial.println("[FP] ✓ SENSOR CONNECTED! (115200 baud, Sensor TX->GPIO16, Sensor RX->GPIO17)");
    finger.getTemplateCount();
    Serial.printf("[FP] %d template(s) stored in sensor flash\n", finger.templateCount);
    return true;
  }

  fpSensorAvailable = false;
  Serial.println("[FP] ✗ Sensor not found on UART2.");
  Serial.println("[FP]   Wiring guide:");
  Serial.println("[FP]     Sensor TX  --> ESP32 GPIO 16 (RX2)");
  Serial.println("[FP]     Sensor RX  --> ESP32 GPIO 17 (TX2)");
  Serial.println("[FP]     Sensor VCC --> ESP32 5V / VIN (recommended for optical sensors) or 3.3V");
  Serial.println("[FP]     Sensor GND --> ESP32 GND");
  return false;
}

void setupFingerprintSensor() {
  initFingerprintSensor();
}

// Enroll new fingerprint
// Returns: 0=success, 1=no finger, 2=error, 3=mismatch
int enrollFingerprint(int id, int step) {
  if (!fpSensorAvailable) {
    if (!initFingerprintSensor()) return 2;
  }
  
  if (step == 1) {
    // First capture
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) return 1;
    if (p != FINGERPRINT_OK) return 2;
    
    p = finger.image2Tz(1);
    if (p != FINGERPRINT_OK) return 2;
    
    Serial.println("[FP] First capture OK");
    return 0;
    
  } else if (step == 2) {
    // Second capture
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) return 1;
    if (p != FINGERPRINT_OK) return 2;
    
    p = finger.image2Tz(2);
    if (p != FINGERPRINT_OK) return 2;
    
    // Create model
    p = finger.createModel();
    if (p != FINGERPRINT_OK) {
      Serial.println("[FP] Fingerprints did not match");
      return 3;
    }
    
    // Store model
    p = finger.storeModel(id);
    if (p != FINGERPRINT_OK) return 2;
    
    Serial.printf("[FP] ✓ Enrolled at ID #%d\n", id);
    return 0;
  }
  
  return 2;
}

// Verify fingerprint
// Returns: fingerprint ID if match found, -1 if no match, -2 if error
int verifyFingerprint() {
  if (!fpSensorAvailable) {
    if (!initFingerprintSensor()) return -2;
  }
  
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -2;
  
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -2;
  
  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK) {
    Serial.printf("[FP] ✓ Match! ID #%d (confidence: %d)\n", 
                  finger.fingerID, finger.confidence);
    return finger.fingerID;
  } else {
    Serial.println("[FP] ✗ No match found");
    return -1;
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// GATE SCREEN
// ═══════════════════════════════════════════════════════════════════════════════

void drawGateScreen() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, W, 4, C_GREEN);

  txt("Silvestre del Moro Park", W / 2, 22, 2, C_DIM, C_BG, MC_DATUM);

  int cx = W / 2, cy = 110;

  switch (gateState) {
    case GS_WAITING: {
      // Park name accent line
      tft.fillRect(cx - 60, 40, 120, 2, C_GREEN);

      txt("Hi! Welcome!", cx, 80, 4, C_TEXT, C_BG, MC_DATUM);

      // Instruction lines — font 2 (16px, always available in TFT_eSPI)
      txt("Place your finger on the", cx, 148, 2, C_DIM, C_BG, MC_DATUM);
      txt("biometric scanner", cx, 178, 2, C_GREEN, C_BG, MC_DATUM);
      txt("to control your diorama.", cx, 208, 2, C_DIM, C_BG, MC_DATUM);
      break;
    }
    case GS_SCANNING: {
      txt("Scanning...", cx, 100, 4, C_AMBER, C_BG, MC_DATUM);
      txt("Verifying your fingerprint,", cx, 160, 2, C_DIM, C_BG, MC_DATUM);
      txt("please wait...", cx, 188, 2, C_DIM, C_BG, MC_DATUM);

      // Progress bar rail
      tft.fillRoundRect(cx - 140, 250, 280, 14, 7, C_TRACK);
      uint32_t elapsed = millis() - gateMs;
      int fw = constrain((int)(280L * elapsed / SCAN_MS), 0, 280);
      if (fw > 0) tft.fillRoundRect(cx - 140, 250, fw, 14, 7, C_AMBER);
      break;
    }
    case GS_WELCOME: {
      // Green accent bar
      tft.fillRoundRect(cx - 80, 55, 160, 4, 2, C_GREEN);

      txt("Welcome!", cx, 95, 4, C_GREEN, C_BG, MC_DATUM);

      txt("The gate is now open.", cx, 150, 2, C_TEXT, C_BG, MC_DATUM);
      txt("Enjoy your diorama experience.", cx, 178, 2, C_DIM, C_BG, MC_DATUM);

      // Open padlock icon
      tft.drawCircle(cx, 248, 16, C_GREEN);
      tft.fillRect(cx - 17, 248, 34, 16, C_BG); // erase lower half (open shackle)
      tft.fillRoundRect(cx - 16, 256, 32, 24, 4, C_GREEN);
      tft.fillCircle(cx, 265, 4, C_WHITE);
      break;
    }
    case GS_OPEN: {
      // This state draws nothing on the gate screen —
      // enterGateState(GS_OPEN) immediately calls drawHeader/drawLights.
      break;
    }
    case GS_GOODBYE: {
      // Closed padlock
      tft.drawCircle(cx, cy - 6, 18, C_RED);
      tft.fillRoundRect(cx - 18, cy + 6, 36, 28, 4, C_RED);
      tft.fillCircle(cx, cy + 17, 5, C_WHITE);

      txt("Goodbye!", cx, 178, 4, C_RED, C_BG, MC_DATUM);
      txt("Thank you for visiting.", cx, 208, 2, C_DIM, C_BG, MC_DATUM);
      break;
    }
  }
}

void enterGateState(GateState s) {
  const bool closingOpenGate =
      gateOpen && (s == GS_GOODBYE || s == GS_WAITING);
  if (closingOpenGate) {
    Serial.println("[GATE] Closing gate; stopping slave audio");
    if (!sendSlaveAudioCommand("/audio/stop")) {
      Serial.println("[GATE] WARNING: could not confirm audio stop on slave");
    }
    audioPlaying = false;
    audioStarted = false;
    autoDefaultAudioAttempted = false;
    autoAudioResumeAfterSound = false;
    soundPauseAttempted = false;
    soundResumeAttempted = false;
  }

  gateState = s;
  gateMs    = millis();

  switch (s) {
    case GS_WAITING:
      Serial.println("[GATE] WAITING");
      gateOpen = false;
      lightsOn = false;
      matchedUserName[0] = '\0';
      applyHardwareGate(false);
      pushZones();
      drawGateScreen();
      break;

    case GS_SCANNING:
      Serial.println("[GATE] SCANNING");
      drawGateScreen();
      break;

    case GS_WELCOME:
      Serial.println("[GATE] WELCOME — showing for 3s");
      drawGateScreen();
      break;

    case GS_OPEN:
      Serial.println("[GATE] OPEN — UI unlocked");
      gateOpen = true;
      lightsOn = true;
      applyHardwareGate(true);
      pushZones();
      drawHeader();
      drawLights();
      break;

    case GS_GOODBYE:
      Serial.println("[GATE] GOODBYE");
      gateOpen = false;
      lightsOn = false;
      fountainOn = false;
      applyFountainPumpOutput();
      matchedUserName[0] = '\0';
      applyHardwareGate(false);
      pushZones();
      drawGateScreen();
      break;
  }
}

void pollGate() {
  if (gateState == GS_WAITING) {
    // Poll physical biometric sensor on TX/RX
    static uint32_t lastFpPoll = 0;
    if (millis() - lastFpPoll >= 180) {
      lastFpPoll = millis();
      if (fpSensorAvailable) {
        uint8_t p = finger.getImage();
        if (p == FINGERPRINT_OK) {
          Serial.println("[FP] Finger detected on sensor!");
          enterGateState(GS_SCANNING);
          p = finger.image2Tz();
          if (p == FINGERPRINT_OK) {
            p = finger.fingerSearch();
            if (p == FINGERPRINT_OK) {
              const char* name = getUserName(finger.fingerID);
              Serial.printf("[FP] Match found! ID #%d (%s)\n", finger.fingerID, name);
              strncpy(matchedUserName, name ? name : "", sizeof(matchedUserName) - 1);
              matchedUserName[sizeof(matchedUserName) - 1] = '\0';
              enterGateState(GS_WELCOME);
              return;
            } else {
              Serial.println("[FP] Fingerprint not recognized");
              tft.fillRect(0, 140, W, 70, C_BG);
              txt("Fingerprint Not Recognized", W / 2, 160, 2, C_RED, C_BG, MC_DATUM);
              txt("Please register or try again", W / 2, 185, 2, C_DIM, C_BG, MC_DATUM);
              delay(1400);
              drawGateScreen();
              enterGateState(GS_WAITING);
              return;
            }
          } else {
            enterGateState(GS_WAITING);
            return;
          }
        }
      }
    }
  } else if (gateState == GS_SCANNING) {
    // Animate the progress bar while waiting
    uint32_t elapsed = millis() - gateMs;
    if (elapsed >= SCAN_MS) {
      enterGateState(GS_OPEN); // fallback: no real FP matched, e.g. touch-sim
    } else {
      int cx = W / 2;
      int fw = constrain((int)(280L * elapsed / SCAN_MS), 0, 280);
      tft.fillRoundRect(cx - 140, 250, fw, 14, 7, C_AMBER);
    }
  } else if (gateState == GS_WELCOME) {
    // Show greeting for WELCOME_MS then unlock the full UI
    if (millis() - gateMs >= WELCOME_MS) {
      enterGateState(GS_OPEN);
    }
  } else if (gateState == GS_GOODBYE) {
    if (millis() - gateMs >= GOODBYE_MS) enterGateState(GS_WAITING);
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// LIGHTS PAGE
// ═══════════════════════════════════════════════════════════════════════════════
//
//  Layout (480×284 body, y origin = HDR_H=36):
//
//  y  36..76  : Mode tabs  [ Basic ][ Custom Color ][ Sound ][ Adaptive ]  (4 tabs)
//  y  80..132 : Power card (on/off toggle + current mode label)
//  y 136..196 : Brightness slider (Basic/Custom Color) or read-only indicator (Adaptive)
//  Sound mode hides the brightness row and uses the freed space for sound controls.
//
// ─────────────────────────────────────────────────────────────────────────────

// Mode tabs — 4 tabs, each 116px wide with 3px gap, starting x=4
// 4×116 + 3×3 = 473px — fits in 480
void drawModeTabs() {
  const char* labels[4] = { "Basic", "Custom Color", "Sound", "Adaptive" };
  const Mode  modes[4]  = { M_BASIC, M_COLOR, M_SOUND, M_ADAPT };
  for (int i = 0; i < 4; i++) {
    bool act = (tftMode == modes[i]);
    int  bx  = 4 + i * 119;
    btn(bx, BODY_Y + 2, 116, 36, labels[i], act, C_AMBER, i == 1 ? 1 : 2);
    drawFocusCue(bx, BODY_Y + 2, 116, 36, i + 1, act);
    drawEncoderFocusCue(bx, BODY_Y + 2, 116, 36,
                        encoderControl == ENC_CONTROL_LIGHT_MODE && act);
  }
}

// Power toggle card
void drawPowerCard() {
  int cy = BODY_Y + 42;
  card(10, cy, 460, 48, C_CARD, lightsOn ? C_AMBER : C_BORDER);

  tft.fillCircle(36, cy + 24, 14, lightsOn ? C_AMBER : C_TRACK);
  tft.drawCircle(36, cy + 24,  9, lightsOn ? C_WHITE : C_DIM);
  tft.drawFastVLine(36, cy + 15, 9, lightsOn ? C_WHITE : C_DIM);

  txt(lightsOn ? "Lights  ON" : "Lights  OFF",
      60, cy + 10, 4, C_TEXT, C_CARD, TL_DATUM);
  const char* modeLabel = tftMode == M_COLOR ? "Custom Color" : lightMode.c_str();
  txt(modeLabel, 60, cy + 30, 2, C_DIM, C_CARD, TL_DATUM);

  toggleSw(410, cy + 13, lightsOn, C_AMBER);
  drawFocusCue(10, cy, 460, 48, 5, false);
  drawEncoderFocusCue(10, cy, 460, 48, encoderControl == ENC_CONTROL_LIGHT_POWER);
}

// Brightness row. Adaptive mode keeps this as a telemetry-only indicator.
void drawBrightnessRow() {
  if (tftMode == M_SOUND) return;

  int ly = BODY_Y + 100;
  tft.fillRect(0, ly, W, 64, C_BG);
  const bool readOnly = tftMode == M_ADAPT;
  label(readOnly ? "Auto brightness" : "Brightness", SL_X0, ly);
  drawPct(W - 10, ly + 4, brightness, readOnly ? C_DIM : C_AMBER);
  drawSlider(ly + 34, brightness, readOnly ? C_DIM : C_AMBER);
  if (!readOnly) {
    drawFocusCue(SL_X0, ly + 16, SL_X1 - SL_X0, 36, 6, false);
    drawEncoderFocusCue(SL_X0, ly + 16, SL_X1 - SL_X0, 36,
                        encoderControl == ENC_CONTROL_BRIGHTNESS);
  }
}

// ── Custom Color source and controls ──────────────────────────────────────────
void drawColorPanel() {
  int py = BODY_Y + 170;
  tft.fillRect(0, py, W, H - py, C_BG);

  label("Color source:", SL_X0, py + 2);
  btn(190, py, 130, 24, "SCAN COLOR", colorScanMode, C_AMBER, 1);
  btn(330, py, 140, 24, "HUE PICKER", !colorScanMode, C_AMBER, 1);
  drawFocusCue(190, py, 130, 24, 7, false);
  drawEncoderFocusCue(190, py, 130, 24,
                      encoderControl == ENC_CONTROL_COLOR_SOURCE);

  tft.fillRoundRect(10, py + 27, 28, 18, 4, c565(cur));
  tft.drawRoundRect(10, py + 27, 28, 18, 4, C_TEXT);
  char hex[8]; snprintf(hex, sizeof(hex), "#%02X%02X%02X", cur.r, cur.g, cur.b);
  txt(hex, 44, py + 31, 2, C_TEXT, C_BG, TL_DATUM);

  if (colorScanMode) {
    label(colorSensorAvailable ? "Captured color held; press Scan Color to update"
                               : "Waiting for color sensor reading...",
          SL_X0, py + 66);
    return;
  }

  label("Hue", SL_X0, py + 52);
  for (int x = SL_X0; x < SL_X1; x += 3) {
    const int value = (long)(x - SL_X0) * 359 / (SL_X1 - SL_X0);
    tft.fillRect(x, py + 62, 3, 8, c565(hsv(value, 100, 100)));
  }
  tft.drawRect(SL_X0, py + 62, SL_X1 - SL_X0, 8, C_BORDER);
  const int hueX = SL_X0 + (long)(SL_X1 - SL_X0) * hue / 359;
  tft.fillCircle(hueX, py + 66, 9, c565(hsv(hue, 100, 100)));
  tft.drawCircle(hueX, py + 66, 9, C_TEXT);
  drawFocusCue(SL_X0, py + 52, SL_X1 - SL_X0, 24, 8, false);
  drawEncoderFocusCue(SL_X0, py + 52, SL_X1 - SL_X0, 24,
                      encoderControl == ENC_CONTROL_HUE);

  label("Saturation", SL_X0, py + 78);
  drawPct(W - 10, py + 78, sat, C_DIM);
  for (int x = SL_X0; x < SL_X1; x += 3) {
    const int value = (long)(x - SL_X0) * 100 / (SL_X1 - SL_X0);
    tft.fillRect(x, py + 88, 3, 8, c565(hsv(hue, value, 100)));
  }
  tft.drawRect(SL_X0, py + 88, SL_X1 - SL_X0, 8, C_BORDER);
  const int satX = SL_X0 + (long)(SL_X1 - SL_X0) * sat / 100;
  tft.fillCircle(satX, py + 92, 9, c565(cur));
  tft.drawCircle(satX, py + 92, 9, C_TEXT);
  drawFocusCue(SL_X0, py + 78, SL_X1 - SL_X0, 24, 9, false);
  drawEncoderFocusCue(SL_X0, py + 78, SL_X1 - SL_X0, 24,
                      encoderControl == ENC_CONTROL_SATURATION);
}

// Mode-specific lower panel
void drawModePanel() {
  int py = tftMode == M_SOUND ? BODY_Y + 100 : BODY_Y + 168;
  tft.fillRect(0, py, W, H - py, C_BG);

  if (tftMode == M_BASIC) {
    label("Steady white light. Adjust brightness above.", SL_X0, py + 4);
    return;
  }

  if (tftMode == M_COLOR) {
    drawColorPanel();
    return;
  }

  if (tftMode == M_SOUND) {
    label("Sound level", SL_X0, py);
    int bw = (int)(lvl * (SL_X1 - SL_X0));
    tft.fillRect(SL_X0, py + 18, SL_X1 - SL_X0, 20, C_TRACK);
    if (bw > 0) tft.fillRect(SL_X0, py + 18, bw, 20, c565(live));
    label("Sensitivity", SL_X0, py + 50);
    drawPct(W - 10, py + 54, sens, C_AMBER);
    drawSlider(py + 80, sens, C_AMBER);
    drawFocusCue(SL_X0, py + 62, SL_X1 - SL_X0, 36, 7, false);
    drawEncoderFocusCue(SL_X0, py + 62, SL_X1 - SL_X0, 36,
                        encoderControl == ENC_CONTROL_SENSITIVITY);
    return;
  }

  if (tftMode == M_ADAPT) {
    char status[48];
    if (ambientSensorAvailable) {
      snprintf(status, sizeof(status), "Ambient light: %.1f lx", ambientLux);
      label(status, SL_X0, py + 4);
      snprintf(status, sizeof(status), "Adaptive brightness: %d%%", brightness);
      label(status, SL_X0, py + 30);
    } else {
      label("Ambient sensor offline — adaptive dimming paused", SL_X0, py + 4);
    }
    return;
  }
}

void drawLights() {
  tft.fillRect(0, BODY_Y, W, BODY_H, C_BG);
  drawModeTabs();
  drawPowerCard();
  if (tftMode != M_SOUND) drawBrightnessRow();
  drawModePanel();
}

// ═══════════════════════════════════════════════════════════════════════════════
// AUDIO PAGE
// ═══════════════════════════════════════════════════════════════════════════════
//
//  Layout (body y 36..319):
//
//  y  40..76  : Track title + counter
//  y  80..96  : Playback status
//  y 102..170 : Transport buttons  [  <<  ]  [ Play/Pause ]  [  >>  ]
// ─────────────────────────────────────────────────────────────────────────────

void drawAudio() {
  tft.fillRect(0, BODY_Y, W, BODY_H, C_BG);

  // Track info
  if (!sdOk) {
    txt("Slave SD unavailable", W / 2, BODY_Y + 28, 4, C_RED, C_BG, MC_DATUM);
  } else if (!trackCount) {
    txt("No MP3/WAV files in /DIORAMA", W / 2, BODY_Y + 28, 2, C_AMBER, C_BG, MC_DATUM);
  } else {
    // Show the current track filename without its folder and extension.
    char title[48];
    const char* filename = strrchr(trackPath[curTrack], '/');
    strlcpy(title, filename ? filename + 1 : trackPath[curTrack], sizeof(title));
    char* dot = strrchr(title, '.'); if (dot) *dot = 0;
    if (strlen(title) > 26) { title[24] = '.'; title[25] = '.'; title[26] = 0; }

    tft.setTextPadding(460);
    txt(title, W / 2, BODY_Y + 20, 4, C_TEXT, C_BG, MC_DATUM);
    tft.setTextPadding(0);

    char ctr[24];
    snprintf(ctr, sizeof(ctr), "Track %d / %d", curTrack + 1, trackCount);
    txt(ctr, W / 2, BODY_Y + 48, 2, C_DIM, C_BG, MC_DATUM);
  }

  // Playback status badge
  const char* statusStr = audioPlaying ? "Playing"
                        : audioStarted ? "Paused"
                        : "Stopped";
  uint16_t statusCol = audioPlaying ? C_GREEN : C_DIM;
  txt(statusStr, W / 2, BODY_Y + 70, 2, statusCol, C_BG, MC_DATUM);

  // Transport buttons  (centred block)
  //  [<<]   starts x=30,  w=110
  //  [Play] starts x=185, w=110
  //  [>>]   starts x=340, w=110
  btn( 30, BODY_Y + 92, 110, 58, "<<",                   false,        C_GREEN);
  drawFocusCue(30, BODY_Y + 92, 110, 58, 1, true);
  drawEncoderFocusCue(30, BODY_Y + 92, 110, 58,
                      encoderControl == ENC_CONTROL_AUDIO_PREVIOUS);
  btn(185, BODY_Y + 92, 110, 58, audioPlaying ? "Pause" : "Play",
      audioPlaying, C_GREEN);
  drawFocusCue(185, BODY_Y + 92, 110, 58, 2, false);
  drawEncoderFocusCue(185, BODY_Y + 92, 110, 58,
                      encoderControl == ENC_CONTROL_AUDIO_PLAY);
  btn(340, BODY_Y + 92, 110, 58, ">>",                   false,        C_GREEN);
  drawFocusCue(340, BODY_Y + 92, 110, 58, 3, false);
  drawEncoderFocusCue(340, BODY_Y + 92, 110, 58,
                      encoderControl == ENC_CONTROL_AUDIO_NEXT);

}

// ═══════════════════════════════════════════════════════════════════════════════
// SETTINGS PAGE — Fingerprint Management
// ═══════════════════════════════════════════════════════════════════════════════

void drawSettings() {
  tft.fillRect(0, BODY_Y, W, BODY_H, C_BG);

  // Title
  txt("Settings", W / 2, BODY_Y + 14, 4, C_TEXT, C_BG, MC_DATUM);

  // ── Drain Water card ───────────────────────────────────────────────────────
  int dy = BODY_Y + 44;
  card(10, dy, 460, 48, C_CARD, drainagePumpOn ? C_GREEN : C_BORDER);

  // Pump icon (simplified drop shape)
  tft.fillCircle(36, dy + 24, 12, drainagePumpOn ? C_GREEN : C_TRACK);
  tft.fillTriangle(28, dy + 22, 44, dy + 22, 36, dy + 10,
                   drainagePumpOn ? C_GREEN : C_TRACK);

  txt(drainagePumpOn ? "Drain: RUNNING" : "Drain: STOPPED",
      58, dy + 10, 4, C_TEXT, C_CARD, TL_DATUM);
  txt("Water drainage pump", 58, dy + 30, 2, C_DIM, C_CARD, TL_DATUM);

  // ON / OFF buttons
  uint16_t onFill  = drainagePumpOn  ? C_GREEN  : C_CARD;
  uint16_t offFill = !drainagePumpOn ? C_RED    : C_CARD;
  card(300, dy + 8, 64, 32, onFill,  drainagePumpOn  ? C_GREEN : C_BORDER);
  txt("ON",  332, dy + 24, 2, drainagePumpOn  ? C_WHITE : C_TEXT, onFill,  MC_DATUM);
  card(374, dy + 8, 64, 32, offFill, !drainagePumpOn ? C_RED   : C_BORDER);
  txt("OFF", 406, dy + 24, 2, !drainagePumpOn ? C_WHITE : C_TEXT, offFill, MC_DATUM);

  // ── Fingerprint Management ─────────────────────────────────────────────────
  label("Fingerprint Management", 20, BODY_Y + 104);

  // Sensor status
  const char* status = fpSensorAvailable ? "Connected" : "Dummy Mode";
  uint16_t statusColor = fpSensorAvailable ? C_GREEN : C_AMBER;
  txt(status, W / 2, BODY_Y + 120, 2, statusColor, C_BG, MC_DATUM);

  // Enroll button
  card(40, BODY_Y + 136, 180, 40, C_GREEN, C_GREEN);
  txt("Enroll New", 130, BODY_Y + 156, 2, C_WHITE, C_GREEN, MC_DATUM);
  drawFocusCue(40, BODY_Y + 136, 180, 40, 1, true);
  drawEncoderFocusCue(40, BODY_Y + 136, 180, 40,
                      encoderControl == ENC_CONTROL_SETTINGS_ENROLL);

  // List users section
  label("Enrolled Users:", 20, BODY_Y + 188);

  if (userCount == 0) {
    txt("No users enrolled yet", W / 2, BODY_Y + 210, 2, C_DIM, C_BG, MC_DATUM);
  } else {
    int y = BODY_Y + 206;
    for (int i = 0; i < min(userCount, 3); i++) {   // show 3 max (less space now)
      card(20, y, 440, 28, C_CARD, C_BORDER);

      tft.fillCircle(35, y + 14, 10, C_PURPLE);
      char idStr[4];
      snprintf(idStr, sizeof(idStr), "%d", users[i].id);
      txt(idStr, 35, y + 14, 2, C_WHITE, C_PURPLE, MC_DATUM);

      txt(users[i].name, 55, y + 14, 2, C_TEXT, C_CARD, ML_DATUM);

      tft.fillRoundRect(410, y + 6, 40, 16, 4, C_RED);
      txt("DEL", 430, y + 14, 1, C_WHITE, C_RED, MC_DATUM);
      drawFocusCue(390, y + 6, 60, 16, i + 2, false);
      drawEncoderFocusCue(390, y + 6, 60, 16,
          encoderControl == static_cast<EncoderControl>(ENC_CONTROL_SETTINGS_DELETE_0 + i));

      y += 32;
    }

    if (userCount > 3) {
      char more[24];
      snprintf(more, sizeof(more), "+%d more...", userCount - 3);
      txt(more, W / 2, BODY_Y + 310, 2, C_DIM, C_BG, MC_DATUM);
    }
  }
}

// Enrollment states
enum EnrollState { ENROLL_IDLE, ENROLL_STEP1, ENROLL_STEP1_WAIT, ENROLL_STEP2, ENROLL_STEP2_WAIT, ENROLL_SUCCESS, ENROLL_ERROR };
EnrollState enrollState = ENROLL_IDLE;
int enrollingID = -1;
uint32_t enrollStateMs = 0;

void redrawCurrentPage() {
  drawHeader();
  if (page == P_LIGHTS) drawLights();
  else if (page == P_AUDIO) drawAudio();
  else drawSettings();
}

int getEncoderControls(EncoderControl* controls) {
  int count = 0;
  if (page == P_LIGHTS) {
    controls[count++] = ENC_CONTROL_LIGHT_MODE;
    controls[count++] = ENC_CONTROL_LIGHT_POWER;
    if (tftMode != M_SOUND && tftMode != M_ADAPT) {
      controls[count++] = ENC_CONTROL_BRIGHTNESS;
    }
    if (tftMode == M_COLOR) {
      controls[count++] = ENC_CONTROL_COLOR_SOURCE;
      if (!colorScanMode) {
        controls[count++] = ENC_CONTROL_HUE;
        controls[count++] = ENC_CONTROL_SATURATION;
      }
    } else if (tftMode == M_SOUND) {
      controls[count++] = ENC_CONTROL_SENSITIVITY;
    }
  } else if (page == P_AUDIO) {
    controls[count++] = ENC_CONTROL_AUDIO_PREVIOUS;
    controls[count++] = ENC_CONTROL_AUDIO_PLAY;
    controls[count++] = ENC_CONTROL_AUDIO_NEXT;
  } else {
    controls[count++] = ENC_CONTROL_SETTINGS_ENROLL;
    for (int i = 0; i < min(userCount, 6); i++) {
      controls[count++] = static_cast<EncoderControl>(ENC_CONTROL_SETTINGS_DELETE_0 + i);
    }
  }
  return count;
}

void syncEncoderControlIndex() {
  EncoderControl controls[12];
  const int count = getEncoderControls(controls);
  for (int i = 0; i < count; i++) {
    if (controls[i] == encoderControl) {
      encoderControlIndex = i;
      return;
    }
  }
  encoderControlIndex = 0;
  if (count > 0) encoderControl = controls[0];
}

void moveEncoderFocus(int direction) {
  EncoderControl controls[12];
  const int count = getEncoderControls(controls);
  if (count == 0) return;
  syncEncoderControlIndex();
  encoderControlIndex = (encoderControlIndex + direction + count) % count;
  encoderControl = controls[encoderControlIndex];
  redrawCurrentPage();
}

bool encoderControlCanAdjust() {
  switch (encoderControl) {
    case ENC_CONTROL_LIGHT_MODE:
    case ENC_CONTROL_BRIGHTNESS:
    case ENC_CONTROL_HUE:
    case ENC_CONTROL_SATURATION:
    case ENC_CONTROL_SENSITIVITY:
      return true;
    default:
      return false;
  }
}

void selectEncoderControl(EncoderControl control) {
  encoderControl = control;
  syncEncoderControlIndex();
  encoderMode = encoderControlCanAdjust() ? ENC_ADJUST : ENC_FOCUS;
  drawHeader();
}

void activateEncoderControl() {
  switch (encoderControl) {
    case ENC_CONTROL_COLOR_SOURCE:
      colorScanMode = !colorScanMode;
      updateColorSensorScanForMode();
      if (colorScanMode) captureColorSensorColor();
      drawColorPanel();
      syncEncoderControlIndex();
      break;
    case ENC_CONTROL_LIGHT_POWER:
      lightsOn = !lightsOn;
      pushZones();
      Serial.printf("[LIGHTS] %s via encoder\n", lightsOn ? "ON" : "OFF");
      drawPowerCard();
      break;
    case ENC_CONTROL_AUDIO_PREVIOUS:
      stepTrack(-1);
      if (page == P_AUDIO) drawAudio();
      break;
    case ENC_CONTROL_AUDIO_PLAY:
      togglePlay();
      if (page == P_AUDIO) drawAudio();
      break;
    case ENC_CONTROL_AUDIO_NEXT:
      stepTrack(1);
      if (page == P_AUDIO) drawAudio();
      break;
    case ENC_CONTROL_SETTINGS_ENROLL:
      startEnrollment();
      break;
    default:
      if (encoderControl >= ENC_CONTROL_SETTINGS_DELETE_0 &&
          encoderControl <= ENC_CONTROL_SETTINGS_DELETE_5) {
        const int index = encoderControl - ENC_CONTROL_SETTINGS_DELETE_0;
        if (index < userCount) deleteUser(users[index].id);
        syncEncoderControlIndex();
        redrawCurrentPage();
      }
      break;
  }
}

void changePage(Page nextPage, int direction) {
  if (nextPage == page) return;

  const int stripWidth = 40;
  for (int i = 0; i < W / stripWidth; i++) {
    const int x = direction > 0 ? i * stripWidth : W - (i + 1) * stripWidth;
    tft.fillRect(x, 0, stripWidth, H, C_BG);
    delay(6);
  }

  page = nextPage;
  encoderMode = ENC_NAVIGATE;
  if (page == P_LIGHTS) encoderControl = ENC_CONTROL_BRIGHTNESS;
  else if (page == P_AUDIO) encoderControl = ENC_CONTROL_AUDIO_PLAY;
  else encoderControl = ENC_CONTROL_SETTINGS_ENROLL;
  syncEncoderControlIndex();
  redrawCurrentPage();
}

void setupEncoder() {
  pinMode(ENCODER_CLK_PIN, INPUT_PULLUP);
  pinMode(ENCODER_DT_PIN, INPUT_PULLUP);
  pinMode(ENCODER_SW_PIN, INPUT_PULLUP);
  encoderPreviousState = (digitalRead(ENCODER_CLK_PIN) << 1) |
                         digitalRead(ENCODER_DT_PIN);
  encoderButtonStableHigh = digitalRead(ENCODER_SW_PIN) == HIGH;
  encoderButtonCandidateHigh = encoderButtonStableHigh;
  encoderButtonChangeMs = millis();
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK_PIN), encoderQuadratureISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_DT_PIN), encoderQuadratureISR, CHANGE);
}

void IRAM_ATTR encoderQuadratureISR() {
  const uint8_t currentState =
      (gpio_get_level((gpio_num_t)ENCODER_CLK_PIN) << 1) |
      gpio_get_level((gpio_num_t)ENCODER_DT_PIN);
  const uint8_t transition = (encoderPreviousState << 2) | currentState;
  int8_t step = 0;

  switch (transition) {
    case 0b0001:
    case 0b0111:
    case 0b1110:
    case 0b1000:
      step = -1;
      break;
    case 0b0010:
    case 0b0100:
    case 0b1011:
    case 0b1101:
      step = 1;
      break;
    default:
      break;
  }

  encoderPreviousState = currentState;
  const int16_t nextSteps = encoderQuarterSteps + step;
  if (nextSteps > 16) encoderQuarterSteps = 16;
  else if (nextSteps < -16) encoderQuarterSteps = -16;
  else encoderQuarterSteps = nextSteps;
}

void applyEncoderStep(int direction) {
  if (encoderMode == ENC_NAVIGATE) {
    const int next = (static_cast<int>(page) + direction + 3) % 3;
    changePage(static_cast<Page>(next), direction);
    return;
  }
  if (encoderMode == ENC_FOCUS) {
    moveEncoderFocus(direction);
    return;
  }

  switch (encoderControl) {
    case ENC_CONTROL_LIGHT_MODE: {
      tftMode = static_cast<Mode>((static_cast<int>(tftMode) + direction + 4) % 4);
      if (tftMode == M_BASIC) {
        lightMode = "Basic";
        soundReactive = false;
        setAll({255, 255, 255});  // Basic → white on master + slave
      } else if (tftMode == M_COLOR) {
        lightMode = "Colorful";
        soundReactive = false;
        pushZones();              // tell slave mode + current custom RGB
      } else if (tftMode == M_SOUND) {
        lightMode = "Sound Reactive";
        soundReactive = true;
        pushZones();
      } else {
        lightMode = "Color Adaptive";
        soundReactive = false;
        pushZones();
      }
      updateColorSensorScanForMode();
      Serial.printf("[ENCODER] Lighting mode: %s → slave mode=%s\n",
                    lightMode.c_str(), slaveModeTag());
      drawLights();
      break;
    }

    case ENC_CONTROL_LIGHT_POWER:
      lightsOn = direction > 0;
      pushZones();
      drawPowerCard();
      break;

    case ENC_CONTROL_BRIGHTNESS:
      if (tftMode == M_SOUND || tftMode == M_ADAPT) break;
      brightness = constrain(brightness + direction * 5, 0, 100);
      pushZones();
      drawBrightnessRow();
      break;

    case ENC_CONTROL_HUE:
      if (colorScanMode) break;
      hue = constrain(hue + direction * 6, 0, 359);
      cur = hsv(hue, sat, 100);
      zone[0] = zone[1] = zone[2] = cur;
      pushZones();
      drawColorPanel();
      break;

    case ENC_CONTROL_SATURATION:
      if (colorScanMode) break;
      sat = constrain(sat + direction * 5, 0, 100);
      cur = hsv(hue, sat, 100);
      zone[0] = zone[1] = zone[2] = cur;
      pushZones();
      drawColorPanel();
      break;

    case ENC_CONTROL_SENSITIVITY: {
      sens = constrain(sens + direction * 5, 0, 100);
      drawModePanel();
      break;
    }

    default:
      break;
  }
}

void handleEncoderShortPress() {
  if (encoderMode == ENC_NAVIGATE) {
    syncEncoderControlIndex();
    encoderMode = ENC_FOCUS;
  } else if (encoderMode == ENC_FOCUS) {
    if (encoderControlCanAdjust()) encoderMode = ENC_ADJUST;
    else activateEncoderControl();
  } else {
    encoderMode = ENC_FOCUS;
  }
  Serial.printf("[ENCODER] Mode: %s\n",
                encoderMode == ENC_NAVIGATE ? "tab navigation"
                : encoderMode == ENC_FOCUS ? "control selection" : "value adjustment");
  if (enrollState == ENROLL_IDLE) redrawCurrentPage();
}

void pollEncoder() {
  if (gateState == GS_OPEN && enrollState == ENROLL_IDLE) {
    int8_t step = 0;
    noInterrupts();
    if (encoderQuarterSteps >= 4) {
      encoderQuarterSteps -= 4;
      step = 1;
    } else if (encoderQuarterSteps <= -4) {
      encoderQuarterSteps += 4;
      step = -1;
    }
    interrupts();

    if (step != 0) applyEncoderStep(step);
  } else {
    noInterrupts();
    encoderQuarterSteps = 0;
    interrupts();
  }

  const uint32_t now = millis();
  const bool buttonHigh = digitalRead(ENCODER_SW_PIN) == HIGH;
  if (buttonHigh != encoderButtonCandidateHigh) {
    encoderButtonCandidateHigh = buttonHigh;
    encoderButtonChangeMs = now;
  }

  if (encoderButtonCandidateHigh != encoderButtonStableHigh &&
      now - encoderButtonChangeMs >= 30) {
    encoderButtonStableHigh = encoderButtonCandidateHigh;
    if (!encoderButtonStableHigh && gateState == GS_OPEN &&
        enrollState == ENROLL_IDLE) {
      encoderButtonDownMs = now;
      encoderLongPressHandled = false;
    } else if (encoderButtonStableHigh && gateState == GS_OPEN &&
               enrollState == ENROLL_IDLE && !encoderLongPressHandled) {
      handleEncoderShortPress();
    }
  }

  if (!encoderButtonStableHigh && gateState == GS_OPEN &&
      enrollState == ENROLL_IDLE && !encoderLongPressHandled &&
      now - encoderButtonDownMs >= 700) {
    encoderLongPressHandled = true;
    encoderMode = ENC_NAVIGATE;
    Serial.println("[ENCODER] Mode: tab navigation");
    redrawCurrentPage();
  }
}

void drawEnrollmentScreen() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, W, 4, C_PURPLE);
  
  txt("Fingerprint Enrollment", W / 2, 22, 2, C_DIM, C_BG, MC_DATUM);
  
  int cx = W / 2;
  
  switch (enrollState) {
    case ENROLL_STEP1:
      txt("Step 1 of 2", cx, 80, 2, C_PURPLE, C_BG, MC_DATUM);
      txt("Place finger on sensor", cx, 120, 4, C_TEXT, C_BG, MC_DATUM);
      
      // Fingerprint icon
      tft.drawCircle(cx, 180, 35, C_PURPLE);
      tft.drawCircle(cx, 180, 28, C_PURPLE);
      tft.drawCircle(cx, 180, 21, C_PURPLE);
      tft.drawFastVLine(cx, 145, 70, C_PURPLE);
      tft.drawFastHLine(cx - 35, 180, 70, C_PURPLE);
      
      card(cx - 80, 250, 160, 40, C_BORDER, C_BORDER);
      txt("Cancel", cx, 270, 2, C_DIM, C_BG, MC_DATUM);
      break;
      
    case ENROLL_STEP1_WAIT:
      txt("Remove finger", cx, 120, 4, C_GREEN, C_BG, MC_DATUM);
      txt("Preparing for second scan...", cx, 160, 2, C_DIM, C_BG, MC_DATUM);
      break;
      
    case ENROLL_STEP2:
      txt("Step 2 of 2", cx, 80, 2, C_PURPLE, C_BG, MC_DATUM);
      txt("Place SAME finger again", cx, 120, 4, C_TEXT, C_BG, MC_DATUM);
      
      // Fingerprint icon
      tft.drawCircle(cx, 180, 35, C_PURPLE);
      tft.drawCircle(cx, 180, 28, C_PURPLE);
      tft.drawCircle(cx, 180, 21, C_PURPLE);
      tft.drawFastVLine(cx, 145, 70, C_PURPLE);
      tft.drawFastHLine(cx - 35, 180, 70, C_PURPLE);
      
      card(cx - 80, 250, 160, 40, C_BORDER, C_BORDER);
      txt("Cancel", cx, 270, 2, C_DIM, C_BG, MC_DATUM);
      break;
      
    case ENROLL_SUCCESS:
      tft.fillCircle(cx, 140, 40, C_GREEN);
      txt("✓", cx, 140, 7, C_WHITE, C_GREEN, MC_DATUM);
      txt("Success!", cx, 200, 4, C_GREEN, C_BG, MC_DATUM);
      txt("Fingerprint enrolled", cx, 235, 2, C_TEXT, C_BG, MC_DATUM);
      break;
      
    case ENROLL_ERROR:
      tft.fillCircle(cx, 140, 40, C_RED);
      txt("✗", cx, 140, 7, C_WHITE, C_RED, MC_DATUM);
      txt("Error", cx, 200, 4, C_RED, C_BG, MC_DATUM);
      txt("Fingerprints did not match", cx, 235, 2, C_TEXT, C_BG, MC_DATUM);
      
      card(cx - 80, 260, 160, 40, C_PURPLE, C_PURPLE);
      txt("Try Again", cx, 280, 2, C_WHITE, C_PURPLE, MC_DATUM);
      break;
      
    default:
      break;
  }
}

void startEnrollment() {
  enrollingID = findNextFreeID();
  if (enrollingID == -1) {
    Serial.println("[FP] All slots full!");
    return;
  }
  
  enrollState = ENROLL_STEP1;
  enrollStateMs = millis();
  drawEnrollmentScreen();
  Serial.printf("[FP] Starting enrollment for ID #%d\n", enrollingID);
}

void pollEnrollment() {
  if (enrollState == ENROLL_IDLE) return;
  
  if (enrollState == ENROLL_STEP1) {
    int result = enrollFingerprint(enrollingID, 1);
    if (result == 0) {
      // Success - move to wait state
      enrollState = ENROLL_STEP1_WAIT;
      enrollStateMs = millis();
      drawEnrollmentScreen();
    } else if (result == 2) {
      // Error
      enrollState = ENROLL_ERROR;
      drawEnrollmentScreen();
    }
    
  } else if (enrollState == ENROLL_STEP1_WAIT) {
    if (millis() - enrollStateMs > 2000) {
      enrollState = ENROLL_STEP2;
      drawEnrollmentScreen();
    }
    
  } else if (enrollState == ENROLL_STEP2) {
    int result = enrollFingerprint(enrollingID, 2);
    if (result == 0) {
      // Success!
      char newName[32];
      snprintf(newName, sizeof(newName), "User %d", enrollingID);
      addUser(enrollingID, newName);
      
      enrollState = ENROLL_SUCCESS;
      drawEnrollmentScreen();
      
      // Auto return to settings after 2s
      enrollStateMs = millis();
      
    } else if (result == 2 || result == 3) {
      // Error or mismatch
      enrollState = ENROLL_ERROR;
      drawEnrollmentScreen();
    }
    
  } else if (enrollState == ENROLL_SUCCESS) {
    if (millis() - enrollStateMs > 2000) {
      enrollState = ENROLL_IDLE;
      page = P_SETTINGS;
      drawHeader();
      drawSettings();
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// SOUND SENSOR
// ═══════════════════════════════════════════════════════════════════════════════

void updateMicSensor() {
  const uint32_t now = millis();
  if (lightMode != "Sound Reactive") {
    micRawMin = 0;
    micRawMax = 0;
    micPeakToPeak = 0;
    micRms = 0;
    micLevel = 0;
    micDetected = false;
    lvl = 0;
    return;
  }

  if (now - micLastSampleMs < 40) return;
  micLastSampleMs = now;

  constexpr int SAMPLE_COUNT = 96;
  int samples[SAMPLE_COUNT];
  uint32_t sum = 0;
  micRawMin = 4095;
  micRawMax = 0;

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    const int sample = analogRead(MIC_PIN);
    samples[i] = sample;
    sum += sample;
    if (sample < micRawMin) micRawMin = sample;
    if (sample > micRawMax) micRawMax = sample;
    delayMicroseconds(125);
  }

  const int mean = sum / SAMPLE_COUNT;
  uint64_t squaredErrorSum = 0;
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    const int centered = samples[i] - mean;
    squaredErrorSum += (uint64_t)(centered * centered);
  }

  micPeakToPeak = micRawMax - micRawMin;
  micRms = (int)sqrtf((float)squaredErrorSum / SAMPLE_COUNT);
  micLevel = constrain(map(micPeakToPeak, 0, 900, 0, 1023), 0, 1023);

  const int detectionThreshold = max(8, 90 - sens * 7 / 10);
  micDetected = micRms >= detectionThreshold;

  if (now - micLastLogMs >= 500) {
    micLastLogMs = now;
    Serial.printf("[SOUND] GPIO%d min=%d max=%d p-p=%d rms=%d level=%d/1023 detected=%s threshold=%d\n",
                  MIC_PIN,
                  micRawMin, micRawMax, micPeakToPeak, micRms, micLevel,
                  micDetected ? "yes" : "no", detectionThreshold);
  }
}

void updateLive() {
  static uint32_t last = 0;
  if (millis() - last < 40) return;
  last = millis();

  // Only Sound Reactive may animate RGB & rotate inner circle. Basic stays white; Custom/Adaptive are steady.
  if (lightMode != "Sound Reactive" || !lightsOn) {
    lvl = 0;
    if (innerCircleMotorRunning) {
      stopMotors();
    }
    return;
  }

  if (micDetected && micLevel > 0) {
    lvl     = max(micLevel / 1023.0f, lvl * 0.82f);
    hueBase = (hueBase + 2) % 360;
    live    = hsv((hueBase + (int)(lvl * 120)) % 360, 100, 12 + (int)(88 * lvl));
    lastSoundDetectedMs = millis();
    if (!innerCircleMotorRunning) {
      runInnerCircle(true);
      Serial.println("[MOTOR] Sound detected -> Inner Circle rotating");
    }
    setAll(live);
  } else {
    lvl = 0;
    if (innerCircleMotorRunning && (millis() - lastSoundDetectedMs >= SOUND_MOTOR_HOLD_MS)) {
      stopMotors();
      Serial.println("[MOTOR] Sound paused -> Inner Circle stopped");
      pushZones();
    }
  }
  // Refresh level bar if on lights page
  if (page == P_LIGHTS && tftMode == M_SOUND) {
    int py = BODY_Y + 100;
    int bw = (int)(lvl * (SL_X1 - SL_X0));
    tft.fillRect(SL_X0, py + 18, SL_X1 - SL_X0, 20, C_TRACK);
    if (bw > 0) tft.fillRect(SL_X0, py + 18, bw, 20, c565(live));
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// AUDIO HELPERS
// ═══════════════════════════════════════════════════════════════════════════════

bool requestSlaveAudio(const String& endpoint, String& body, int& statusCode) {
  WiFiClient client;
  HTTPClient http;
  const String url = "http://192.168.4.200" + endpoint;

  if (!http.begin(client, url)) {
    Serial.printf("[AUDIO] Could not initialize request: %s\n", url.c_str());
    return false;
  }
  http.setConnectTimeout(300);
  http.setTimeout(800);
  http.addHeader("Connection", "close");

  const int responseCode = http.GET();
  if (responseCode <= 0) {
    Serial.printf("[AUDIO] Request failed: %s (%s)\n",
                  url.c_str(), http.errorToString(responseCode).c_str());
    http.end();
    statusCode = 0;
    return false;
  }

  statusCode = responseCode;
  body = http.getString();
  http.end();
  return statusCode >= 200 && statusCode < 300;
}

String encodeAudioPath(const char* path) {
  String encoded;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p) {
    const unsigned char ch = *p;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      encoded += static_cast<char>(ch);
    } else {
      char escaped[4];
      snprintf(escaped, sizeof(escaped), "%%%02X", ch);
      encoded += escaped;
    }
  }
  return encoded;
}

bool sendSlaveAudioCommand(const String& endpoint) {
  String response;
  int statusCode = 0;
  const bool ok = requestSlaveAudio(endpoint, response, statusCode);
  if (!ok) Serial.printf("[AUDIO] Slave command failed: %s (HTTP %d)\n", endpoint.c_str(), statusCode);
  return ok;
}

bool parseSlaveAudioTracks(const String& response) {
  StaticJsonDocument<4096> doc;
  if (deserializeJson(doc, response)) {
    Serial.println("[AUDIO] ERROR: invalid track-list response from slave");
    return false;
  }
  if (!doc["files"].is<JsonArrayConst>()) {
    Serial.println("[AUDIO] ERROR: slave track-list response has no files array");
    return false;
  }

  slaveAudioReachable = true;
  sdOk = doc["sdCardReady"] | false;
  JsonArrayConst files = doc["files"].as<JsonArrayConst>();
  trackCount = 0;
  for (JsonVariantConst file : files) {
    if (trackCount >= MAX_TRACKS) break;
    const char* path = file.as<const char*>();
    if (path && *path) strlcpy(trackPath[trackCount++], path, sizeof(trackPath[0]));
  }
  lastSlaveAudioListMs = millis();
  return true;
}

void refreshSlaveAudioTracks() {
  lastSlaveAudioListMs = millis();
  String response;
  int statusCode = 0;
  if (!requestSlaveAudio("/audio/files", response, statusCode)) {
    slaveAudioReachable = false;
    sdOk = false;
    Serial.printf("[AUDIO] Track-list request to slave failed (HTTP %d)\n", statusCode);
    return;
  }
  parseSlaveAudioTracks(response);
}

void syncSlaveAudioState() {
  const uint32_t now = millis();
  if (now - lastSlaveAudioSyncMs < 3000) return;
  lastSlaveAudioSyncMs = now;

  if (!slaveAudioReachable && now - lastSlaveAudioListMs < 5000) return;
  if (!slaveAudioReachable || now - lastSlaveAudioListMs >= 5000) {
    refreshSlaveAudioTracks();
  }
  if (!slaveAudioReachable) return;

  String response;
  int statusCode = 0;
  if (!requestSlaveAudio("/audio/state", response, statusCode)) {
    slaveAudioReachable = false;
    sdOk = false;
    return;
  }

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, response)) {
    Serial.println("[AUDIO] ERROR: invalid playback-state response from slave");
    return;
  }

  const bool wasReady = sdOk;
  const bool wasPlaying = audioPlaying;
  const int oldTrack = curTrack;
  char previousTrack[sizeof(trackPath[0])];
  previousTrack[0] = '\0';
  if (audioStarted && curTrack >= 0 && curTrack < trackCount) {
    strlcpy(previousTrack, trackPath[curTrack], sizeof(previousTrack));
  }

  slaveAudioReachable = true;
  sdOk = doc["sdCardReady"] | false;
  audioPlaying = doc["playing"] | false;
  const char* currentPath = doc["track"] | "";
  audioStarted = currentPath[0] != '\0';
  if (audioStarted) {
    for (int i = 0; i < trackCount; ++i) {
      if (strcmp(trackPath[i], currentPath) == 0) {
        curTrack = i;
        break;
      }
    }
  }

  const bool trackChanged = audioStarted
    ? strcmp(previousTrack, currentPath) != 0
    : previousTrack[0] != '\0';
  if (page == P_AUDIO &&
      (wasReady != sdOk || wasPlaying != audioPlaying ||
       oldTrack != curTrack || trackChanged)) {
    drawAudio();
  }
}

bool startTrack(int i) {
  if (i < 0 || i >= trackCount) return false;
  const String endpoint = "/audio/play?file=" + encodeAudioPath(trackPath[i]);
  if (sendSlaveAudioCommand(endpoint)) {
    curTrack = i;
    audioPlaying = audioStarted = true;
    autoDefaultAudioAttempted = true;
    trackStartMs = millis();
    Serial.printf("[AUDIO] Playing %d/%d: %s\n", curTrack + 1, trackCount, trackPath[i]);
    return true;
  }
  return false;
}

bool stepTrack(int direction, String* response, int* statusCode) {
  if (!trackCount) return false;
  const int next = audioStarted
    ? (curTrack + direction + trackCount) % trackCount
    : (direction > 0 ? 0 : trackCount - 1);
  const char* endpoint = direction > 0 ? "/audio/next" : "/audio/previous";
  const bool commandAccepted = response && statusCode
    ? requestSlaveAudio(endpoint, *response, *statusCode)
    : sendSlaveAudioCommand(endpoint);
  if (!commandAccepted) return false;

  curTrack = next;
  audioPlaying = audioStarted = true;
  autoDefaultAudioAttempted = true;
  trackStartMs = millis();
  Serial.printf("[AUDIO] Stepped to %d/%d: %s\n",
                curTrack + 1, trackCount, trackPath[curTrack]);
  return true;
}

void togglePlay() {
  if (!trackCount) return;
  if (audioPlaying) {
    if (sendSlaveAudioCommand("/audio/pause")) audioPlaying = false;
  } else {
    const int selected = audioStarted ? curTrack : 0;
    startTrack(selected);
  }
}

void updateAutomaticAudio() {
  if (!gateOpen) return;

  if (lightMode == "Sound Reactive") {
    if (audioPlaying &&
        (!soundPauseAttempted || millis() - lastSoundPauseAttemptMs >= 3000)) {
      lastSoundPauseAttemptMs = millis();
      soundPauseAttempted = true;
      if (sendSlaveAudioCommand("/audio/pause")) {
        audioPlaying = false;
        autoAudioResumeAfterSound = true;
      }
    }
    return;
  }
  soundPauseAttempted = false;

  if (autoAudioResumeAfterSound) {
    if (soundResumeAttempted && millis() - lastSoundResumeAttemptMs < 3000) return;
    lastSoundResumeAttemptMs = millis();
    soundResumeAttempted = true;
    if (curTrack >= 0 && curTrack < trackCount && startTrack(curTrack)) {
      autoAudioResumeAfterSound = false;
      soundResumeAttempted = false;
    }
    return;
  }
  soundResumeAttempted = false;

  if (autoDefaultAudioAttempted || audioStarted || !slaveAudioReachable || !sdOk) return;
  if (millis() - lastAutoAudioAttemptMs < 3000) return;
  lastAutoAudioAttemptMs = millis();

  for (int i = 0; i < trackCount; ++i) {
    const char* filename = strrchr(trackPath[i], '/');
    filename = filename ? filename + 1 : trackPath[i];
    const String name(filename);
    if (name.equalsIgnoreCase("track01.mp3") || name.equalsIgnoreCase("track01.wav")) {
      if (startTrack(i)) {
        autoDefaultAudioAttempted = true;
      }
      return;
    }
  }

  Serial.println("[AUDIO] Default track track01.mp3/track01.wav was not found");
  autoDefaultAudioAttempted = true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// TOUCH — onPress & onDrag
// ═══════════════════════════════════════════════════════════════════════════════

void onPress(int x, int y) {
  Serial.printf("[TOUCH] Press x=%d y=%d gate=%d page=%d\n",
                x, y, (int)gateState, (int)page);

  // ── Gate screen (dummy biometric simulation) ────────────────────────────
  if (gateState != GS_OPEN) {
    // Any touch on the waiting screen simulates biometric sensor placement
    if (gateState == GS_WAITING) {
      enterGateState(GS_SCANNING);
    }
    return;
  }

  // ── Header taps ──────────────────────────────────────────────────────────
  if (y < HDR_H + 12) {
    // Exit tap zone (enlarged hit box: 0..95 px)
    if (x < 95) {
      enterGateState(GS_GOODBYE);
      return;
    }
    // Page tabs - 3 tabs now
    if (inRect(x, y, 250, 0, 85, HDR_H + 10)) {
      const int direction = ((static_cast<int>(P_LIGHTS) - static_cast<int>(page) + 3) % 3 == 1) ? 1 : -1;
      changePage(P_LIGHTS, direction);
      return;
    }
    if (inRect(x, y, 330, 0, 75, HDR_H + 10)) {
      const int direction = ((static_cast<int>(P_AUDIO) - static_cast<int>(page) + 3) % 3 == 1) ? 1 : -1;
      changePage(P_AUDIO, direction);
      return;
    }
    if (inRect(x, y, 400, 0, 80, HDR_H + 10)) {
      const int direction = ((static_cast<int>(P_SETTINGS) - static_cast<int>(page) + 3) % 3 == 1) ? 1 : -1;
      changePage(P_SETTINGS, direction);
      return;
    }
    return;
  }

  // ── Enrollment screen ──────────────────────────────────────────────────────
  if (enrollState != ENROLL_IDLE) {
    int cx = W / 2;
    
    // Cancel button during enrollment
    if ((enrollState == ENROLL_STEP1 || enrollState == ENROLL_STEP2) &&
        inRect(x, y, cx - 80, 250, 160, 40)) {
      enrollState = ENROLL_IDLE;
      page = P_SETTINGS;
      drawHeader();
      drawSettings();
      Serial.println("[FP] Enrollment cancelled");
      return;
    }
    
    // Try Again button on error
    if (enrollState == ENROLL_ERROR && inRect(x, y, cx - 80, 260, 160, 40)) {
      startEnrollment();
      return;
    }
    
    return; // Block all other touches during enrollment
  }

  // ── Settings page ──────────────────────────────────────────────────────────
  if (page == P_SETTINGS) {
    int dy = BODY_Y + 44;

    // Drain ON button
    if (inRect(x, y, 300, dy + 8, 64, 32)) {
      if (sendSlaveAudioCommand("/pump?type=drain&state=on")) {
        drainagePumpOn = true;
      } else {
        Serial.println("[DRAIN] ERROR: slave pump command failed");
      }
      drawSettings();
      return;
    }
    // Drain OFF button
    if (inRect(x, y, 374, dy + 8, 64, 32)) {
      if (sendSlaveAudioCommand("/pump?type=drain&state=off")) {
        drainagePumpOn = false;
      } else {
        Serial.println("[DRAIN] ERROR: slave pump command failed");
      }
      drawSettings();
      return;
    }

    // Enroll New button (shifted down to BODY_Y+136)
    if (inRect(x, y, 40, BODY_Y + 136, 180, 40)) {
      selectEncoderControl(ENC_CONTROL_SETTINGS_ENROLL);
      startEnrollment();
      return;
    }

    // Delete user buttons (shifted down to BODY_Y+206)
    if (userCount > 0) {
      int cy = BODY_Y + 206;
      for (int i = 0; i < min(userCount, 3); i++) {
        if (inRect(x, y, 410, cy + 6, 40, 16)) {
          const int deletedID = users[i].id;
          selectEncoderControl(static_cast<EncoderControl>(ENC_CONTROL_SETTINGS_DELETE_0 + i));
          deleteUser(deletedID);
          syncEncoderControlIndex();
          drawSettings();
          Serial.printf("[FP] Deleted user #%d via TFT\n", deletedID);
          return;
        }
        cy += 32;
      }
    }
    return;
  }

  // ── Audio page ────────────────────────────────────────────────────────────
  if (page == P_AUDIO) {
    // <<
    if (inRect(x, y, 30,  BODY_Y + 92, 110, 58)) {
      selectEncoderControl(ENC_CONTROL_AUDIO_PREVIOUS);
      stepTrack(-1); drawAudio(); return;
    }
    // Play/Pause
    if (inRect(x, y, 185, BODY_Y + 92, 110, 58)) {
      selectEncoderControl(ENC_CONTROL_AUDIO_PLAY);
      togglePlay(); drawAudio(); return;
    }
    // >>
    if (inRect(x, y, 340, BODY_Y + 92, 110, 58)) {
      selectEncoderControl(ENC_CONTROL_AUDIO_NEXT);
      stepTrack(1); drawAudio(); return;
    }
    return;
  }

  // ── Lights page ───────────────────────────────────────────────────────────
  if (page == P_LIGHTS) {
    // Mode tabs row  y = BODY_Y+2 .. BODY_Y+38
    if (inRect(x, y, 0, BODY_Y, W, 40)) {
      const Mode modes[4] = { M_BASIC, M_COLOR, M_SOUND, M_ADAPT };
      for (int i = 0; i < 4; i++) {
        if (inRect(x, y, 4 + i * 119, BODY_Y + 2, 116, 36)) {
          selectEncoderControl(ENC_CONTROL_LIGHT_MODE);
          if (tftMode != modes[i]) {
            tftMode = modes[i];
            if (tftMode == M_BASIC)  { lightMode = "Basic";          soundReactive = false; setAll({255,255,255}); }
            if (tftMode == M_COLOR)  { lightMode = "Colorful";        soundReactive = false; pushZones(); }
            if (tftMode == M_SOUND)  { lightMode = "Sound Reactive";  soundReactive = true;  pushZones(); }
            if (tftMode == M_ADAPT)  { lightMode = "Color Adaptive";  soundReactive = false; pushZones(); }
            updateColorSensorScanForMode();
            Serial.printf("[MODE] %s → slave mode=%s\n", lightMode.c_str(), slaveModeTag());
            drawLights();
          } else {
            drawModeTabs();
          }
          return;
        }
      }
    }

    // Power toggle card  y = BODY_Y+42 .. BODY_Y+90
    if (inRect(x, y, 10, BODY_Y + 42, 460, 48)) {
      selectEncoderControl(ENC_CONTROL_LIGHT_POWER);
      lightsOn = !lightsOn;
      Serial.printf("[LIGHTS] %s\n", lightsOn ? "ON" : "OFF");
      pushZones();
      drawPowerCard();
      return;
    }

    // Brightness is adjustable only in Basic and Custom Color modes.
    if (tftMode != M_SOUND && tftMode != M_ADAPT &&
        inRect(x, y, SL_X0, BODY_Y + 116, SL_X1 - SL_X0, 52)) {
      selectEncoderControl(ENC_CONTROL_BRIGHTNESS);
      drawBrightnessRow();
      dragging = D_BR; onDrag(x); return;
    }

    // Custom Color is set by a one-shot sensor capture or the hue picker.
    if (tftMode == M_COLOR) {
      const int py = BODY_Y + 170;
      if (inRect(x, y, 190, py, 130, 24)) {
        selectEncoderControl(ENC_CONTROL_COLOR_SOURCE);
        colorScanMode = true;
        updateColorSensorScanForMode();
        captureColorSensorColor();
        drawColorPanel();
        syncEncoderControlIndex();
        return;
      }
      if (inRect(x, y, 330, py, 140, 24)) {
        selectEncoderControl(ENC_CONTROL_COLOR_SOURCE);
        colorScanMode = false;
        updateColorSensorScanForMode();
        drawColorPanel();
        syncEncoderControlIndex();
        return;
      }
      if (!colorScanMode && inRect(x, y, SL_X0, py + 54, SL_X1 - SL_X0, 24)) {
        selectEncoderControl(ENC_CONTROL_HUE);
        dragging = D_HUE;
        onDrag(x);
        return;
      }
      if (!colorScanMode && inRect(x, y, SL_X0, py + 80, SL_X1 - SL_X0, 24)) {
        selectEncoderControl(ENC_CONTROL_SATURATION);
        dragging = D_SAT;
        onDrag(x);
        return;
      }
      return;
    }

    // Sensitivity slider (Sound mode only)  y = py+62 .. py+118
    if (tftMode == M_SOUND) {
      int py = BODY_Y + 100;
      if (inRect(x, y, SL_X0, py + 62, SL_X1 - SL_X0, 56)) {
        selectEncoderControl(ENC_CONTROL_SENSITIVITY);
        drawModePanel();
        dragging = D_SENS; onDrag(x); return;
      }
    }
  }
}

void onDrag(int x) {
  int p = pctFromX(x);
  switch (dragging) {
    case D_BR:
      if (tftMode == M_SOUND || tftMode == M_ADAPT) {
        dragging = D_NONE;
        break;
      }
      if (p != brightness) {
        brightness = p;
        Serial.printf("[LIGHTS] Brightness %d%%\n", brightness);
        drawPct(W - 10, BODY_Y + 104, p, C_AMBER);
        drawSlider(BODY_Y + 134, p, C_AMBER);
        pushZones();
      }
      break;

    case D_HUE: {
      if (colorScanMode) break;
      const int nextHue = (long)p * 359 / 100;
      if (nextHue != hue) {
        hue = nextHue;
        cur = hsv(hue, sat, 100);
        zone[0] = zone[1] = zone[2] = cur;
        pushZones();
        drawColorPanel();
      }
      break;
    }

    case D_SAT:
      if (colorScanMode) break;
      if (p != sat) {
        sat = p;
        cur = hsv(hue, sat, 100);
        zone[0] = zone[1] = zone[2] = cur;
        pushZones();
        drawColorPanel();
      }
      break;

    case D_SENS:
      if (p != sens) {
        sens = p;
        Serial.printf("[SOUND] Sensitivity %d%%\n", sens);
        drawModePanel();
      }
      break;

    default: break;
  }
}

bool readCalibratedTouch(uint16_t* x, uint16_t* y) {
  static uint32_t invalidRawSinceMs = 0;
  static uint32_t badRawLogMs = 0;

  uint16_t pressure = tft.getTouchRawZ();
  if (pressure <= 350) {
    invalidRawSinceMs = 0;
    return false;
  }

  delayMicroseconds(1000);
  pressure = tft.getTouchRawZ();
  if (pressure <= 350) {
    invalidRawSinceMs = 0;
    return false;
  }

  uint16_t rawX, rawY;
  tft.getTouchRaw(&rawX, &rawY);
  if (rawX == 0 || rawY == 0 || rawX >= 4095 || rawY >= 4095) {
    const uint32_t now = millis();
    if (invalidRawSinceMs == 0) invalidRawSinceMs = now;
    if (now - invalidRawSinceMs >= 1000 && now - badRawLogMs >= 5000) {
      badRawLogMs = millis();
      Serial.printf("[TOUCH] Sustained invalid samples x=%u y=%u; check TFT SDO is disconnected and XPT T_DO is on GPIO 19\n",
                    rawX, rawY);
    }
    return false;
  }
  invalidRawSinceMs = 0;

  const int32_t x0 = touchCalData[0];
  const int32_t xSpan = touchCalData[1];
  const int32_t y0 = touchCalData[2];
  const int32_t ySpan = touchCalData[3];
  if (!touchCalibrationReady || xSpan == 0 || ySpan == 0) return false;

  int32_t screenX, screenY;
  if (touchCalData[4] & 0x01) {
    screenX = (rawY - x0) * (W - 1) / xSpan;
    screenY = (rawX - y0) * (H - 1) / ySpan;
  } else {
    screenX = (rawX - x0) * (W - 1) / xSpan;
    screenY = (rawY - y0) * (H - 1) / ySpan;
  }

  if (touchCalData[4] & 0x02) screenX = (W - 1) - screenX;
  if (touchCalData[4] & 0x04) screenY = (H - 1) - screenY;
  if (screenX < 0 || screenX >= W || screenY < 0 || screenY >= H) {
    static uint32_t outOfRangeLogMs = 0;
    if (millis() - outOfRangeLogMs >= 500) {
      outOfRangeLogMs = millis();
      Serial.printf("[TOUCH] Raw (%u,%u) maps outside screen to (%ld,%ld); calibration invalid\n",
                    rawX, rawY, (long)screenX, (long)screenY);
    }
    return false;
  }

  *x = screenX;
  *y = screenY;
  static uint32_t touchLogMs = 0;
  if (millis() - touchLogMs >= 300) {
    touchLogMs = millis();
    Serial.printf("[TOUCH] Mapped x=%u y=%u pressure=%u\n", *x, *y, pressure);
  }
  return true;
}

void pollTouch() {
  static uint8_t miss = 0;
  uint16_t x, y;
  if (readCalibratedTouch(&x, &y)) {
    miss = 0;
    if (!wasTouched) { wasTouched = true; onPress(x, y); }
    else if (dragging) onDrag(x);
  } else {
    if (wasTouched && ++miss >= 3) {
      wasTouched = false; miss = 0;
      dragging = D_NONE;
    }

  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// HTTP SERVER — endpoint handlers
// All endpoints match the web app's esp32Api.js exactly.
// ═══════════════════════════════════════════════════════════════════════════════

// Helper: send JSON OK
void jsonOk(const char* extra = "") {
  char buf[128];
  if (strlen(extra) == 0)
    snprintf(buf, sizeof(buf), "{\"ok\":true}");
  else
    snprintf(buf, sizeof(buf), "{\"ok\":true,%s}", extra);
  server.send(200, "application/json", buf);
}

// ── GET /api/light?state=on|off&brightness=0-100 ─────────────────────────────
void handleLight() {
  if (server.hasArg("state")) {
    lightsOn = (server.arg("state") == "on");
    if (!lightsOn) stopMotors();
  }
  if (server.hasArg("brightness")) {
    brightness = constrain(server.arg("brightness").toInt(), 0, 100);
  }
  Serial.printf("[HTTP /api/light] state=%s brightness=%d\n",
                lightsOn ? "on" : "off", brightness);
  pushZones();
  if (page == P_LIGHTS && gateState == GS_OPEN) {
    drawPowerCard();
    drawBrightnessRow();
  }
  jsonOk();
}

// ── GET /api/mode?mode=Basic|Sound+Reactive|Color+Adaptive|Colorful|Custom+Color
void handleMode() {
  if (server.hasArg("mode")) {
    lightMode = server.arg("mode");
    lightMode.replace("+", " ");  // URL-encoded spaces
    if (lightMode == "Custom Color") lightMode = "Colorful";
    Serial.printf("[HTTP /api/mode] mode=%s\n", lightMode.c_str());
    // Sync TFT mode tab
    if (lightMode == "Basic")                tftMode = M_BASIC;
    else if (lightMode == "Colorful")        tftMode = M_COLOR;
    else if (lightMode == "Sound Reactive")  tftMode = M_SOUND;
    else if (lightMode == "Color Adaptive")  tftMode = M_ADAPT;
    else                                     tftMode = M_BASIC;

    if (tftMode != M_SOUND) {
      stopMotors();
    }

    updateColorSensorScanForMode();
    syncEncoderControlIndex();
    // Push mode-correct RGB to the slave for every mode change.
    if (tftMode == M_BASIC) {
      soundReactive = false;          // stop any leftover sound-reactive flag
      setAll({255, 255, 255});        // Basic → white only
    } else {
      pushZones();
    }
    if (page == P_LIGHTS && gateState == GS_OPEN) drawLights();
  }
  jsonOk();
}

// ── GET /api/color?r=&g=&b=&target=left|right|center|all ────────────────────
void handleColor() {
  // Basic = locked white. Sound/Adaptive own their output. Only Custom Color accepts picks.
  if (lightMode == "Basic") {
    Serial.println("[HTTP /api/color] Ignored — Basic mode is locked to white");
    setAll({255, 255, 255});
    jsonOk();
    return;
  }
  if (lightMode != "Colorful") {
    Serial.printf("[HTTP /api/color] Ignored — mode=%s (Custom Color only)\n", lightMode.c_str());
    jsonOk();
    return;
  }

  uint8_t r = server.hasArg("r") ? constrain(server.arg("r").toInt(), 0, 255) : 255;
  uint8_t g = server.hasArg("g") ? constrain(server.arg("g").toInt(), 0, 255) : 180;
  uint8_t b = server.hasArg("b") ? constrain(server.arg("b").toInt(), 0, 255) : 90;
  String  target = server.hasArg("target") ? server.arg("target") : "all";

  Serial.printf("[HTTP /api/color] r=%d g=%d b=%d target=%s\n", r, g, b, target.c_str());

  RGB col = {r, g, b};
  cur = col;
  rgbToHsvValues(cur.r, cur.g, cur.b, &hue, &sat);
  zone[0] = zone[1] = zone[2] = col;

  pushZones();
  if (page == P_LIGHTS && tftMode == M_COLOR) drawColorPanel();
  jsonOk();
}

// ── GET /api/color/source?mode=scan|picker ────────────────────────────────────
void handleColorSource() {
  if (!server.hasArg("mode") ||
      (server.arg("mode") != "scan" && server.arg("mode") != "picker")) {
    server.send(400, "application/json",
                "{\"error\":\"missing or invalid mode; use mode=scan or mode=picker\"}");
    return;
  }

  colorScanMode = server.arg("mode") == "scan";
  updateColorSensorScanForMode();
  syncEncoderControlIndex();
  if (page == P_LIGHTS && tftMode == M_COLOR) {
    drawColorPanel();
  }
  Serial.printf("[COLOR] Source changed to %s\n", colorScanMode ? "scan" : "picker");
  jsonOk(colorScanMode ? "\"scanMode\":\"scan\"" : "\"scanMode\":\"picker\"");
}

// ── GET /api/color/scan → triggers color capture from sensor & syncs to zones/TFT ──
void handleColorScan() {
  colorScanMode = true;
  updateColorSensorScanForMode();
  if (lightMode != "Colorful") {
    server.send(409, "application/json",
                "{\"error\":\"switch to Custom Color mode before scanning\"}");
    return;
  }
  if (!captureColorSensorColor()) {
    server.send(503, "application/json",
                "{\"error\":\"color sensor has no recent reading\"}");
    return;
  }
  if (page == P_LIGHTS && tftMode == M_COLOR) drawColorPanel();
  char buf[128];
  snprintf(buf, sizeof(buf),
    "{\"success\":true,\"r\":%u,\"g\":%u,\"b\":%u,\"hex\":\"#%02X%02X%02X\"}",
    cur.r, cur.g, cur.b, cur.r, cur.g, cur.b
  );
  server.send(200, "application/json", buf);
}

// ── GET /api/fountain?state=on|off&strength=0-100&auxStrength=0-100 ──────────
void handleFountain() {
  if (server.hasArg("state"))       fountainOn  = (server.arg("state") == "on");
  if (!forceSensorOn && server.hasArg("strength")) {
    fountainStr = constrain(server.arg("strength").toInt(), 0, 100);
  } else if (!forceSensorOn && server.hasArg("auxStrength")) {
    fountainStr = constrain(server.arg("auxStrength").toInt(), 0, 100);
  }
  fountainAux = fountainStr;
  applyFountainPumpOutput();

  Serial.printf("[PUMPS] state=%s sharedSpeed=%d%% pwm=%d/255\n",
                fountainOn ? "on" : "off", fountainStr, fountainPumpDuty());
  jsonOk();
}

bool readSlaveForceSensorLevel(int& level) {
  WiFiClient client;
  client.setTimeout(100);
  if (!client.connect(IPAddress(192, 168, 4, 200), 80, 100)) return false;

  client.print("GET /force HTTP/1.0\r\nHost: 192.168.4.200\r\nConnection: close\r\n\r\n");
  const uint32_t startedAt = millis();
  while (!client.available() && client.connected() && millis() - startedAt < 250) {
    delay(1);
  }
  if (!client.available()) {
    client.stop();
    return false;
  }

  const String statusLine = client.readStringUntil('\n');
  if (statusLine.indexOf(" 200 ") < 0) {
    client.stop();
    return false;
  }
  while (client.connected() || client.available()) {
    if (!client.available()) {
      if (millis() - startedAt >= 300) {
        client.stop();
        return false;
      }
      delay(1);
      continue;
    }
    const String header = client.readStringUntil('\n');
    if (header == "\r" || header.length() == 0) break;
  }

  String body;
  while (client.connected() || client.available()) {
    while (client.available()) body += static_cast<char>(client.read());
    if (millis() - startedAt >= 300) break;
    delay(1);
  }
  client.stop();
  StaticJsonDocument<64> response;
  const DeserializationError error = deserializeJson(response, body);
  if (error || !response["level"].is<int>()) return false;

  level = constrain(response["level"].as<int>(), 0, 1023);
  return true;
}

// ── GET /api/force-sensor?state=on|off ───────────────────────────────────────
void handleForceSensorControl() {
  if (!server.hasArg("state") ||
      (server.arg("state") != "on" && server.arg("state") != "off")) {
    server.send(400, "application/json",
                "{\"error\":\"missing or invalid state; use state=on or state=off\"}");
    return;
  }

  const bool requestedState = server.arg("state") == "on";
  if (requestedState && !fountainOn) {
    server.send(409, "application/json",
                "{\"error\":\"turn the fountain on before enabling force sensor control\"}");
    return;
  }

  if (requestedState != forceSensorOn) {
    if (requestedState) {
      manualFountainStr = fountainStr;
      forceSensorOn = true;
    } else {
      forceSensorOn = false;
      forceSensorLevel = 0;
      fountainStr = manualFountainStr;
      fountainAux = fountainStr;
      applyFountainPumpOutput();
    }
  }

  Serial.printf("[FORCE SENSOR] state=%s strength=%d%%\n",
                forceSensorOn ? "on" : "off", fountainStr);
  jsonOk();
}

// ── GET /api/force → {"active":bool,"level":0-1023} ──────────────────────────
void handleForceSensorLevel() {
  if (forceSensorOn) {
    int level;
    if (!readSlaveForceSensorLevel(level)) {
      static uint32_t lastReadErrorMs = 0;
      if (millis() - lastReadErrorMs >= 5000) {
        Serial.println("[FORCE SENSOR] ERROR: could not read sensor from slave ESP32");
        lastReadErrorMs = millis();
      }
      server.send(503, "application/json",
                  "{\"error\":\"force sensor unavailable on slave ESP32\"}");
      return;
    }
    forceSensorLevel = level;
    fountainStr = (level * 100 + 511) / 1023;
    fountainAux = fountainStr;
    applyFountainPumpOutput();
  }

  char response[64];
  snprintf(response, sizeof(response), "{\"active\":%s,\"level\":%d}",
           forceSensorOn ? "true" : "false", forceSensorLevel);
  server.send(200, "application/json", response);
}

// ── GET /api/intake-pump?state=on|off ────────────────────────────────────────
void handleIntakePump() {
  if (!server.hasArg("state") ||
      (server.arg("state") != "on" && server.arg("state") != "off")) {
    server.send(400, "application/json",
                "{\"error\":\"missing or invalid state; use state=on or state=off\"}");
    return;
  }

  const bool requestedState = server.arg("state") == "on";
  const String endpoint = String("/pump?type=intake&state=") +
                          (requestedState ? "on" : "off");
  if (!sendSlaveAudioCommand(endpoint)) {
    server.send(503, "application/json",
                "{\"error\":\"slave ESP32 unavailable; intake pump command failed\"}");
    return;
  }

  intakePumpOn = requestedState;
  Serial.printf("[INTAKE PUMP] %s on slave GPIO 27\n",
                intakePumpOn ? "ON" : "OFF");
  server.send(200, "application/json",
              intakePumpOn
                ? "{\"ok\":true,\"on\":true,\"gpio\":27,\"board\":\"slave\"}"
                : "{\"ok\":true,\"on\":false,\"gpio\":27,\"board\":\"slave\"}");
}

// ── GET /api/drainage-pump?state=on|off ──────────────────────────────────────
void handleDrainagePump() {
  if (!server.hasArg("state") ||
      (server.arg("state") != "on" && server.arg("state") != "off")) {
    server.send(400, "application/json",
                "{\"error\":\"missing or invalid state; use state=on or state=off\"}");
    return;
  }

  const bool requestedState = server.arg("state") == "on";
  const String endpoint = String("/pump?type=drain&state=") +
                          (requestedState ? "on" : "off");
  if (!sendSlaveAudioCommand(endpoint)) {
    server.send(503, "application/json",
                "{\"error\":\"slave ESP32 unavailable; drainage pump command failed\"}");
    return;
  }

  drainagePumpOn = requestedState;
  Serial.printf("[DRAINAGE PUMP] %s on slave GPIO 33\n",
                drainagePumpOn ? "ON" : "OFF");
  server.send(200, "application/json",
              drainagePumpOn
                ? "{\"ok\":true,\"requested\":true,\"hardwareConfigured\":true,\"gpio\":33,\"board\":\"slave\"}"
                : "{\"ok\":true,\"requested\":false,\"hardwareConfigured\":true,\"gpio\":33,\"board\":\"slave\"}");
}

// ── GET /api/gate?state=open|closed ─────────────────────────────────────────
void handleGate() {
  bool wantOpen = server.hasArg("state") && server.arg("state") == "open";
  Serial.printf("[HTTP /api/gate] state=%s\n", wantOpen ? "open" : "closed");

  if (wantOpen && !gateOpen) {
    enterGateState(GS_OPEN);
  } else if (!wantOpen && gateOpen) {
    enterGateState(GS_GOODBYE);
  }
  jsonOk();
}

// ── GET /api/gate/status  → {"open":true|false} ──────────────────────────────
void handleGateStatus() {
  char buf[32];
  snprintf(buf, sizeof(buf), "{\"open\":%s}", gateOpen ? "true" : "false");
  server.send(200, "application/json", buf);
}

// ── GET /api/audio/play?file=xxx.mp3 ─────────────────────────────────────────
void handleAudioPlay() {
  if (!server.hasArg("file")) {
    Serial.println("[AUDIO] Play request missing file parameter");
    server.send(400, "application/json", "{\"error\":\"missing file\"}");
    return;
  }
  const String requested = server.arg("file");
  Serial.printf("[AUDIO] Web play request: %s\n", requested.c_str());
  int index = -1;
  for (int i = 0; i < trackCount; ++i) {
    const char* name = strrchr(trackPath[i], '/');
    if (requested == trackPath[i] || requested == (name ? name + 1 : trackPath[i])) {
      index = i;
      break;
    }
  }
  if (index < 0) {
    Serial.println("[AUDIO] Play request rejected: track not in cached slave playlist");
    server.send(404, "application/json", "{\"error\":\"track not found in slave /DIORAMA playlist\"}");
    return;
  }

  const String endpoint = "/audio/play?file=" + encodeAudioPath(trackPath[index]);
  String response;
  int statusCode = 0;
  if (!requestSlaveAudio(endpoint, response, statusCode)) {
    Serial.printf("[AUDIO] Slave rejected/failed play request (HTTP %d): %s\n",
                  statusCode, response.c_str());
    server.send(statusCode ? statusCode : 503, "application/json",
                response.length() ? response : "{\"error\":\"slave audio controller unavailable\"}");
    return;
  }
  Serial.printf("[AUDIO] Slave accepted play request for track %d/%d: %s\n",
                index + 1, trackCount, trackPath[index]);
  curTrack = index;
  audioPlaying = audioStarted = true;
  trackStartMs = millis();
  if (page == P_AUDIO && gateState == GS_OPEN) drawAudio();
  server.send(statusCode, "application/json", response);
}

// ── GET /api/audio/pause ─────────────────────────────────────────────────────
void handleAudioPause() {
  Serial.println("[HTTP /api/audio/pause]");
  String response;
  int statusCode = 0;
  if (!requestSlaveAudio("/audio/pause", response, statusCode)) {
    server.send(statusCode ? statusCode : 503, "application/json",
                response.length() ? response : "{\"error\":\"slave audio controller unavailable\"}");
    return;
  }
  audioPlaying = false;
  if (page == P_AUDIO && gateState == GS_OPEN) drawAudio();
  server.send(statusCode, "application/json", response);
}

void handleAudioStep(int direction) {
  Serial.printf("[HTTP /api/audio/%s]\n", direction > 0 ? "next" : "previous");
  if (!slaveAudioReachable || !sdOk) refreshSlaveAudioTracks();
  if (!sdOk || !trackCount) {
    server.send(503, "application/json",
                "{\"error\":\"slave SD/audio playlist unavailable\"}");
    return;
  }
  String response;
  int statusCode = 0;
  if (!stepTrack(direction, &response, &statusCode)) {
    server.send(statusCode ? statusCode : 503, "application/json",
                response.length() ? response : "{\"error\":\"slave rejected audio track change\"}");
    return;
  }
  if (page == P_AUDIO && gateState == GS_OPEN) drawAudio();
  server.send(statusCode, "application/json", response);
}

// ── GET /api/audio/stop ──────────────────────────────────────────────────────
void handleAudioStop() {
  Serial.println("[HTTP /api/audio/stop]");
  String response;
  int statusCode = 0;
  if (!requestSlaveAudio("/audio/stop", response, statusCode)) {
    server.send(statusCode ? statusCode : 503, "application/json",
                response.length() ? response : "{\"error\":\"slave audio controller unavailable\"}");
    return;
  }
  audioPlaying = audioStarted = false;
  autoDefaultAudioAttempted = true;
  autoAudioResumeAfterSound = false;
  soundPauseAttempted = false;
  soundResumeAttempted = false;
  if (page == P_AUDIO && gateState == GS_OPEN) drawAudio();
  server.send(statusCode, "application/json", response);
}

// ── GET /api/audio/files → list of files found on the slave SD card ──────────
void handleAudioFiles() {
  String response;
  int statusCode = 0;
  if (!requestSlaveAudio("/audio/files", response, statusCode)) {
    server.send(statusCode ? statusCode : 503, "application/json",
                response.length() ? response : "{\"error\":\"slave SD/audio controller unavailable\",\"files\":[]}");
    return;
  }
  if (!parseSlaveAudioTracks(response)) {
    server.send(502, "application/json", "{\"error\":\"invalid track-list response from slave\",\"files\":[]}");
    return;
  }
  Serial.printf("[AUDIO] Proxied %d track(s) from slave; SD=%s\n",
                trackCount, sdOk ? "ready" : "not ready");
  server.send(statusCode, "application/json", response);
}

// ── GET /api/sound-reactive?state=on|off&intensity=0-100 ─────────────────────
void handleSoundReactive() {
  if (server.hasArg("state"))     soundReactive   = (server.arg("state") == "on");
  if (server.hasArg("intensity")) sens = constrain(server.arg("intensity").toInt(), 0, 100);
  Serial.printf("[HTTP /api/sound-reactive] state=%s intensity=%d\n",
                soundReactive ? "on" : "off", sens);
  if (soundReactive) {
    lightMode = "Sound Reactive";
    tftMode   = M_SOUND;
    updateColorSensorScanForMode();
    if (page == P_LIGHTS && gateState == GS_OPEN) drawLights();
  } else {
    stopMotors();
  }
  jsonOk();
}

// ── GET /api/sound  → {"detected":bool,"level":0-1023} ───────────────────────
void handleSound() {
  char buf[160];
  snprintf(buf, sizeof(buf), "{\"detected\":%s,\"level\":%d,\"rms\":%d,\"peakToPeak\":%d,\"r\":%u,\"g\":%u,\"b\":%u,\"motorRunning\":%s}",
           micDetected ? "true" : "false", micLevel, micRms, micPeakToPeak,
           live.r, live.g, live.b, innerCircleMotorRunning ? "true" : "false");
  server.send(200, "application/json", buf);
}

// ── GET /api/motor?state=on|off&dir=fwd|rev ──────────────────────────────────
void handleMotor() {
  if (server.hasArg("state")) {
    String st = server.arg("state");
    bool fwd = server.hasArg("dir") ? (!server.arg("dir").equalsIgnoreCase("rev")) : true;
    if (st.equalsIgnoreCase("on") || st.equalsIgnoreCase("run") || st == "1") {
      runInnerCircle(fwd);
      Serial.println("[HTTP /api/motor] Manual RUN");
    } else {
      stopMotors();
      Serial.println("[HTTP /api/motor] Manual STOP");
    }
  }
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"motorRunning\":%s}", innerCircleMotorRunning ? "true" : "false");
  server.send(200, "application/json", buf);
}

// ── GET /api/state  → full diorama state snapshot ────────────────────────────
// The web app polls this every few seconds to sync changes made on the TFT.
void handleState() {
  // Zone colour hex strings
  char leftHex[8], rightHex[8], centerHex[8];
  snprintf(leftHex,   sizeof(leftHex),   "#%02X%02X%02X", zone[0].r, zone[0].g, zone[0].b);
  snprintf(rightHex,  sizeof(rightHex),  "#%02X%02X%02X", zone[1].r, zone[1].g, zone[1].b);
  snprintf(centerHex, sizeof(centerHex), "#%02X%02X%02X", zone[2].r, zone[2].g, zone[2].b);

  // Preserve the full SD path so the web audio page can match its playlist item.
  char trackName[96] = "";
  if (audioStarted && curTrack >= 0 && curTrack < trackCount) {
    strlcpy(trackName, trackPath[curTrack], sizeof(trackName));
  }

  // Escape lightMode string for JSON (replace " with \")
  char modeEsc[48];
  strlcpy(modeEsc, lightMode.c_str(), sizeof(modeEsc));

  char json[640];
  snprintf(json, sizeof(json),
    "{"
    "\"gateOpen\":%s,"
    "\"lightsOn\":%s,"
    "\"brightness\":%d,"
    "\"lightingMode\":\"%s\","
    "\"soundReactive\":%s,"
    "\"soundIntensity\":%d,"
    "\"fountainOn\":%s,"
    "\"fountainStr\":%d,"
    "\"fountainAux\":%d,"
    "\"fountainForceSensorOn\":%s,"
    "\"intakePumpOn\":%s,"
    "\"drainagePumpOn\":%s,"
    "\"audioPlaying\":%s,"
    "\"audioTrack\":\"%s\","
    "\"fountainColor\":\"%s\","
    "\"fountainAuxColor\":\"%s\","
    "\"circleColor\":\"%s\","
    "\"innerCircleMotor\":%s"
    "}",
    gateOpen       ? "true" : "false",
    lightsOn       ? "true" : "false",
    brightness,
    modeEsc,
    soundReactive  ? "true" : "false",
    sens,
    fountainOn     ? "true" : "false",
    fountainStr,
    fountainAux,
    forceSensorOn  ? "true" : "false",
    intakePumpOn   ? "true" : "false",
    drainagePumpOn ? "true" : "false",
    audioPlaying   ? "true" : "false",
    trackName,
    leftHex,
    rightHex,
    centerHex,
    innerCircleMotorRunning ? "true" : "false"
  );

  server.send(200, "application/json", json);
}

// ── GET /api/sensors  → live reading for hardware sensors ────────────────────
void handleSensors() {
  // Microphone values are sampled once in loop and shared with the sound API.
  const int micPct = constrain(map(micLevel, 0, 1023, 0, 100), 0, 100);

  // Biometric / Gate state
  const char* gateStateStr = (gateState == GS_OPEN)     ? "OPEN"
                           : (gateState == GS_SCANNING) ? "SCANNING"
                           : (gateState == GS_GOODBYE)  ? "GOODBYE"
                           : "WAITING";

  char luxJson[20];
  if (ambientSensorAvailable) snprintf(luxJson, sizeof(luxJson), "%.2f", ambientLux);
  else strlcpy(luxJson, "null", sizeof(luxJson));

  char buf[560];
  snprintf(buf, sizeof(buf),
    "{"
    "\"ambientLight\":{\"connected\":%s,\"lux\":%s,\"brightness\":%d,\"source\":\"VEML7700 I2C\"},"
    "\"colorSensor\":{\"connected\":%s,\"scanning\":%s,\"scanMode\":\"%s\",\"r\":%u,\"g\":%u,\"b\":%u,\"source\":\"Serial Color Sensor\"},"
    "\"mic\":{\"level\":%d,\"percent\":%d,\"rms\":%d,\"peakToPeak\":%d,\"detected\":%s},"
    "\"innerCircleMotor\":{\"running\":%s},"
    "\"biometric\":{\"state\":\"%s\",\"open\":%s,\"authorized\":%s}"
    "}",
    ambientSensorAvailable ? "true" : "false", luxJson, brightness,
    colorSensorAvailable ? "true" : "false",
    colorSensorScanning ? "true" : "false",
    colorScanMode ? "scan" : "picker",
    detectedColor.r, detectedColor.g, detectedColor.b,
    micLevel, micPct, micRms, micPeakToPeak, micDetected ? "true" : "false",
    innerCircleMotorRunning ? "true" : "false",
    gateStateStr,
    gateOpen ? "true" : "false",
    (gateState == GS_OPEN) ? "true" : "false"
  );

  server.send(200, "application/json", buf);
}

// ── GET /api/fingerprint/users  → list all enrolled users ────────────────────
void handleFingerprintUsers() {
  StaticJsonDocument<4096> doc;
  JsonArray usersArray = doc.createNestedArray("users");
  
  for (int i = 0; i < userCount; i++) {
    JsonObject user = usersArray.createNestedObject();
    user["id"] = users[i].id;
    user["name"] = users[i].name;
    user["active"] = users[i].active;
  }
  
  doc["sensorAvailable"] = fpSensorAvailable;
  doc["totalSlots"] = 127;
  doc["usedSlots"] = userCount;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// ── GET/POST /api/fingerprint/enroll?name=xxx&step=1|2&id=X  ────────────────
void handleFingerprintEnroll() {
  if (!server.hasArg("name")) {
    server.send(400, "application/json", "{\"error\":\"missing name parameter\"}");
    return;
  }
  
  String name = server.arg("name");
  int step = server.hasArg("step") ? server.arg("step").toInt() : 0;
  int id = server.hasArg("id") ? server.arg("id").toInt() : findNextFreeID();

  if (id <= 0 || id > 127) {
    server.send(400, "application/json", "{\"error\":\"no valid slot ID available\"}");
    return;
  }

  // If sensor not yet available, try to re-probe before giving up
  if (!fpSensorAvailable) {
    Serial.println("[API] Sensor not ready — attempting re-probe...");
    if (!initFingerprintSensor()) {
      server.send(503, "application/json", "{\"success\":false,\"status\":\"error\",\"message\":\"Sensor not found. Check wiring: Sensor TX -> GPIO16, Sensor RX -> GPIO17.\"}");
      return;
    }
  }

  // Real optical biometric sensor enrollment steps
  if (step == 1) {
    // Step 1: First capture
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      server.send(200, "application/json", "{\"success\":false,\"status\":\"waiting_finger\",\"message\":\"Waiting for finger on sensor...\"}");
      return;
    }
    if (p != FINGERPRINT_OK) {
      server.send(200, "application/json", "{\"success\":false,\"status\":\"error\",\"message\":\"Failed to capture image\"}");
      return;
    }
    p = finger.image2Tz(1);
    if (p != FINGERPRINT_OK) {
      server.send(200, "application/json", "{\"success\":false,\"status\":\"error\",\"message\":\"Failed to convert image\"}");
      return;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"success\":true,\"status\":\"step1_ok\",\"id\":%d,\"message\":\"First scan OK. Remove and place same finger again.\"}", id);
    server.send(200, "application/json", buf);
    Serial.printf("[API] FP Step 1 captured for ID #%d\n", id);
    return;
  } else if (step == 2) {
    // Step 2: Second capture & model creation
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      server.send(200, "application/json", "{\"success\":false,\"status\":\"waiting_finger\",\"message\":\"Place same finger on sensor again...\"}");
      return;
    }
    if (p != FINGERPRINT_OK) {
      server.send(200, "application/json", "{\"success\":false,\"status\":\"error\",\"message\":\"Failed to capture confirmation image\"}");
      return;
    }
    p = finger.image2Tz(2);
    if (p != FINGERPRINT_OK) {
      server.send(200, "application/json", "{\"success\":false,\"status\":\"error\",\"message\":\"Failed to convert confirmation image\"}");
      return;
    }
    p = finger.createModel();
    if (p != FINGERPRINT_OK) {
      server.send(200, "application/json", "{\"success\":false,\"status\":\"mismatch\",\"message\":\"Prints did not match\"}");
      Serial.println("[API] FP Step 2 prints did not match");
      return;
    }
    p = finger.storeModel(id);
    if (p != FINGERPRINT_OK) {
      server.send(200, "application/json", "{\"success\":false,\"status\":\"error\",\"message\":\"Failed to store model in sensor\"}");
      return;
    }
    addUser(id, name.c_str());
    char buf[180];
    snprintf(buf, sizeof(buf), "{\"success\":true,\"status\":\"enrolled\",\"id\":%d,\"name\":\"%s\",\"message\":\"Fingerprint enrolled successfully!\"}", id, name.c_str());
    server.send(200, "application/json", buf);
    Serial.printf("[API] FP Enrolled user '%s' at ID #%d\n", name.c_str(), id);
    return;
  }

  // step parameter is required (must be 1 or 2)
  server.send(400, "application/json", "{\"success\":false,\"status\":\"error\",\"message\":\"Missing or invalid step parameter. Use step=1 or step=2.\"}");
}

// ── DELETE /api/fingerprint/delete?id=X  → delete fingerprint ────────────────
void handleFingerprintDelete() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"missing id parameter\"}");
    return;
  }
  
  int id = server.arg("id").toInt();
  
  if (deleteUser(id)) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"success\":true,\"message\":\"User #%d deleted\"}", id);
    server.send(200, "application/json", buf);
    Serial.printf("[API] Deleted user ID #%d\n", id);
  } else {
    server.send(404, "application/json", "{\"error\":\"user not found\"}");
  }
}

// ── CORS preflight (OPTIONS) ──────────────────────────────────────────────────
void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

// ── Attach CORS headers to every response ────────────────────────────────────
void addCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void setupRoutes() {
  // CORS preflight
  server.on("/api/light",            HTTP_OPTIONS, handleOptions);
  server.on("/api/mode",             HTTP_OPTIONS, handleOptions);
  server.on("/api/color",            HTTP_OPTIONS, handleOptions);
  server.on("/api/color/source",     HTTP_OPTIONS, handleOptions);
  server.on("/api/color/scan",       HTTP_OPTIONS, handleOptions);
  server.on("/api/fountain",         HTTP_OPTIONS, handleOptions);
  server.on("/api/force-sensor",     HTTP_OPTIONS, handleOptions);
  server.on("/api/force",            HTTP_OPTIONS, handleOptions);
  server.on("/api/intake-pump",      HTTP_OPTIONS, handleOptions);
  server.on("/api/drainage-pump",   HTTP_OPTIONS, handleOptions);
  server.on("/api/gate",             HTTP_OPTIONS, handleOptions);
  server.on("/api/gate/status",      HTTP_OPTIONS, handleOptions);
  server.on("/api/audio/play",       HTTP_OPTIONS, handleOptions);
  server.on("/api/audio/pause",      HTTP_OPTIONS, handleOptions);
  server.on("/api/audio/next",       HTTP_OPTIONS, handleOptions);
  server.on("/api/audio/previous",   HTTP_OPTIONS, handleOptions);
  server.on("/api/audio/stop",       HTTP_OPTIONS, handleOptions);
  server.on("/api/audio/files",      HTTP_OPTIONS, handleOptions);
  server.on("/api/sound-reactive",   HTTP_OPTIONS, handleOptions);
  server.on("/api/sound",            HTTP_OPTIONS, handleOptions);
  server.on("/api/motor",            HTTP_OPTIONS, handleOptions);
  server.on("/api/sensors",          HTTP_OPTIONS, handleOptions);
  server.on("/api/state",            HTTP_OPTIONS, handleOptions);
  server.on("/api/fingerprint/users",   HTTP_OPTIONS, handleOptions);
  server.on("/api/fingerprint/enroll",  HTTP_OPTIONS, handleOptions);
  server.on("/api/fingerprint/delete",  HTTP_OPTIONS, handleOptions);

  // GET handlers (wrap with CORS header injection)
  server.on("/api/light",  HTTP_GET, []() { addCORSHeaders(); handleLight();        });
  server.on("/api/mode",   HTTP_GET, []() { addCORSHeaders(); handleMode();         });
  server.on("/api/color",  HTTP_GET, []() { addCORSHeaders(); handleColor();        });
  server.on("/api/color/source", HTTP_GET, []() { addCORSHeaders(); handleColorSource(); });
  server.on("/api/color/scan", HTTP_GET, []() { addCORSHeaders(); handleColorScan();    });
  server.on("/api/fountain",HTTP_GET,[]() { addCORSHeaders(); handleFountain();     });
  server.on("/api/force-sensor", HTTP_GET, []() { addCORSHeaders(); handleForceSensorControl(); });
  server.on("/api/force", HTTP_GET, []() { addCORSHeaders(); handleForceSensorLevel(); });
  server.on("/api/intake-pump", HTTP_GET, []() { addCORSHeaders(); handleIntakePump(); });
  server.on("/api/drainage-pump",HTTP_GET,[]() { addCORSHeaders(); handleDrainagePump(); });
  server.on("/api/gate",   HTTP_GET, []() { addCORSHeaders(); handleGate();         });
  server.on("/api/gate/status", HTTP_GET, []() { addCORSHeaders(); handleGateStatus(); });
  server.on("/api/audio/play",  HTTP_GET, []() { addCORSHeaders(); handleAudioPlay();  });
  server.on("/api/audio/pause", HTTP_GET, []() { addCORSHeaders(); handleAudioPause(); });
  server.on("/api/audio/next", HTTP_GET, []() { addCORSHeaders(); handleAudioStep(1); });
  server.on("/api/audio/previous", HTTP_GET, []() { addCORSHeaders(); handleAudioStep(-1); });
  server.on("/api/audio/stop",  HTTP_GET, []() { addCORSHeaders(); handleAudioStop();  });
  server.on("/api/audio/files", HTTP_GET, []() { addCORSHeaders(); handleAudioFiles(); });
  server.on("/api/sound-reactive",HTTP_GET,[]() { addCORSHeaders(); handleSoundReactive(); });
  server.on("/api/sound",  HTTP_GET, []() { addCORSHeaders(); handleSound();        });
  server.on("/api/motor",  HTTP_GET, []() { addCORSHeaders(); handleMotor();        });
  server.on("/api/sensors",HTTP_GET, []() { addCORSHeaders(); handleSensors();      });
  server.on("/api/state",      HTTP_GET,[]() { addCORSHeaders(); handleState();       });
  
  // Fingerprint API
  server.on("/api/fingerprint/users",  HTTP_GET, []() { addCORSHeaders(); handleFingerprintUsers(); });
  server.on("/api/fingerprint/enroll", HTTP_GET, []() { addCORSHeaders(); handleFingerprintEnroll(); });
  server.on("/api/fingerprint/delete", HTTP_GET, []() { addCORSHeaders(); handleFingerprintDelete(); });

  // ── Static Web App Files (SPIFFS) ──────────────────────────────────────────
  server.serveStatic("/", SPIFFS, "/");

  server.onNotFound([]() {
    addCORSHeaders();
    String uri = server.uri();
    if (uri.startsWith("/api/")) {
      server.send(404, "application/json", "{\"error\":\"not found\"}");
      return;
    }

    // Check if the requested file exists in SPIFFS (plain or gzipped)
    if (SPIFFS.exists(uri) || SPIFFS.exists(uri + ".gz")) {
      String path = SPIFFS.exists(uri + ".gz") ? uri + ".gz" : uri;
      String contentType = "text/plain";
      if (uri.endsWith(".html") || uri == "/") contentType = "text/html";
      else if (uri.endsWith(".css")) contentType = "text/css";
      else if (uri.endsWith(".js")) contentType = "application/javascript";
      else if (uri.endsWith(".png")) contentType = "image/png";
      else if (uri.endsWith(".svg")) contentType = "image/svg+xml";
      else if (uri.endsWith(".ico")) contentType = "image/x-icon";
      else if (uri.endsWith(".json")) contentType = "application/json";
      else if (uri.endsWith(".stl")) contentType = "model/stl";

      File file = SPIFFS.open(path, "r");
      if (file) {
        if (path.endsWith(".gz")) {
          server.sendHeader("Content-Encoding", "gzip");
        }
        server.streamFile(file, contentType);
        file.close();
        return;
      }
    }

    // Fallback to index.html for Single Page Application navigation
    if (SPIFFS.exists("/index.html.gz") || SPIFFS.exists("/index.html")) {
      File file = SPIFFS.open(SPIFFS.exists("/index.html.gz") ? "/index.html.gz" : "/index.html", "r");
      if (file) {
        if (SPIFFS.exists("/index.html.gz")) {
          server.sendHeader("Content-Encoding", "gzip");
        }
        server.streamFile(file, "text/html");
        file.close();
        return;
      }
    }

    // Direct embedded web app fallback (PROGMEM) — runs without SPIFFS upload
    if (serveEmbeddedWebApp(server)) {
      return;
    }

    server.send(404, "text/plain", "404: Not Found");
  });
}

// ═══════════════════════════════════════════════════════════════════════════════
// TOUCH CALIBRATION
// ═══════════════════════════════════════════════════════════════════════════════

bool captureTouchCalibrationPoint(uint8_t point, int targetX, int targetY,
                                 int32_t& rawX, int32_t& rawY) {
  const int marker = 12;
  tft.fillScreen(C_BG);
  txt("Touch and hold each marker", W / 2, 28, 2, C_TEXT, C_BG, MC_DATUM);
  txt("Lift your finger after it is recorded", W / 2, 52, 2, C_DIM, C_BG, MC_DATUM);
  tft.drawCircle(targetX, targetY, marker, C_RED);
  tft.drawFastHLine(targetX - marker, targetY, marker * 2 + 1, C_RED);
  tft.drawFastVLine(targetX, targetY - marker, marker * 2 + 1, C_RED);
  Serial.printf("[TOUCH] Calibration point %u: touch target (%d,%d)\n",
                point + 1, targetX, targetY);

  while (tft.getTouchRawZ() <= 350) delay(10);

  int32_t sumX = 0;
  int32_t sumY = 0;
  for (uint8_t sample = 0; sample < 12; sample++) {
    while (tft.getTouchRawZ() <= 350) delay(5);
    uint16_t rawX, rawY;
    tft.getTouchRaw(&rawX, &rawY);
    if (rawX == 0 || rawY == 0 || rawX >= 4095 || rawY >= 4095) {
      Serial.printf("[TOUCH] Invalid calibration sample x=%u y=%u; check TFT SDO/T_DO wiring\n",
                    rawX, rawY);
      while (tft.getTouchRawZ() > 350) delay(10);
      return false;
    }
    sumX += rawX;
    sumY += rawY;
    delay(12);
  }

  rawX = sumX / 12;
  rawY = sumY / 12;
  Serial.printf("[TOUCH] Calibration point %u raw=(%ld,%ld)\n",
                point + 1, (long)rawX, (long)rawY);

  while (tft.getTouchRawZ() > 350) delay(10);
  delay(150);
  return true;
}

bool calibrateTouchRaw() {
  int32_t rawX[4], rawY[4];
  const int inset = 20;
  const int targetX[4] = { inset, inset, W - 1 - inset, W - 1 - inset };
  const int targetY[4] = { inset, H - 1 - inset, inset, H - 1 - inset };

  for (uint8_t point = 0; point < 4; point++) {
    if (!captureTouchCalibrationPoint(point, targetX[point], targetY[point],
                                      rawX[point], rawY[point])) {
      return false;
    }
  }

  const bool rotated =
      abs((long)(rawX[0] - rawX[1])) > abs((long)(rawY[0] - rawY[1]));
  int32_t x0 = rotated ? (rawY[0] + rawY[1]) / 2 : (rawX[0] + rawX[1]) / 2;
  int32_t x1 = rotated ? (rawY[2] + rawY[3]) / 2 : (rawX[2] + rawX[3]) / 2;
  int32_t y0 = rotated ? (rawX[0] + rawX[2]) / 2 : (rawY[0] + rawY[2]) / 2;
  int32_t y1 = rotated ? (rawX[1] + rawX[3]) / 2 : (rawY[1] + rawY[3]) / 2;

  uint16_t flags = rotated ? 1 : 0;
  if (x0 > x1) {
    const int32_t tmp = x0;
    x0 = x1;
    x1 = tmp;
    flags |= 0x02;
  }
  if (y0 > y1) {
    const int32_t tmp = y0;
    y0 = y1;
    y1 = tmp;
    flags |= 0x04;
  }

  const int32_t xSpan = x1 - x0;
  const int32_t ySpan = y1 - y0;
  if (xSpan < 100 || ySpan < 100 || x1 > 4095 || y1 > 4095) {
    Serial.printf("[TOUCH] Calibration failed: spans x=%ld y=%ld\n",
                  (long)xSpan, (long)ySpan);
    return false;
  }

  touchCalData[0] = x0;
  touchCalData[1] = xSpan;
  touchCalData[2] = y0;
  touchCalData[3] = ySpan;
  touchCalData[4] = flags;
  Serial.printf("[TOUCH] Calibration result: %u %u %u %u flags=%u\n",
                touchCalData[0], touchCalData[1], touchCalData[2],
                touchCalData[3], touchCalData[4]);
  return true;
}

void setupTouch() {
  uint8_t  calOK = 0;

  Serial.printf("[TOUCH] TFT_CS=%d TOUCH_CS=%d REPEAT_CAL=%d\n",
                TFT_CS, TOUCH_CS, REPEAT_CAL);

  if (!SPIFFS.begin(true)) {
    Serial.println("[SPIFFS] Mount failed");
  }

  if (SPIFFS.exists(CALIBRATION_FILE) && !REPEAT_CAL) {
    File f = SPIFFS.open(CALIBRATION_FILE, "r");
    if (f) {
      if (f.readBytes((char*)touchCalData, sizeof(touchCalData)) == sizeof(touchCalData)) calOK = 1;
      f.close();
    }
  }

  if (calOK &&
      (touchCalData[0] > 4095 || touchCalData[1] < 100 || touchCalData[1] > 4095 ||
       touchCalData[2] > 4095 || touchCalData[3] < 100 || touchCalData[3] > 4095 ||
       touchCalData[4] > 7)) {
    Serial.println("[TOUCH] Saved calibration is invalid; recalibrating");
    calOK = 0;
  }

  if (calOK) {
    tft.setTouch(touchCalData);
    Serial.printf("[TOUCH] Calibration loaded: %u %u %u %u flags=%u\n",
                  touchCalData[0], touchCalData[1], touchCalData[2],
                  touchCalData[3], touchCalData[4]);
  } else {
    Serial.println(REPEAT_CAL
                     ? "[TOUCH] Forced recalibration starting"
                     : "[TOUCH] Calibration missing; starting calibration");
    while (!calibrateTouchRaw()) {
      tft.fillScreen(C_BG);
      txt("Touch calibration failed", W / 2, H / 2 - 12, 2, C_RED, C_BG, MC_DATUM);
      txt("Retrying in 2 seconds", W / 2, H / 2 + 16, 2, C_DIM, C_BG, MC_DATUM);
      delay(2000);
    }

    File f = SPIFFS.open(CALIBRATION_FILE, "w");
    if (!f) {
      Serial.println("[TOUCH] ERROR: could not open calibration file for writing");
    } else {
      const size_t written = f.write((const unsigned char*)touchCalData, sizeof(touchCalData));
      f.close();
      if (written != sizeof(touchCalData)) {
        Serial.println("[TOUCH] ERROR: calibration file write was incomplete");
      }
    }
    tft.setTouch(touchCalData);
    Serial.printf("[TOUCH] Calibration complete: %u %u %u %u flags=%u\n",
                  touchCalData[0], touchCalData[1], touchCalData[2],
                  touchCalData[3], touchCalData[4]);
  }
  touchCalibrationReady = touchCalData[1] != 0 && touchCalData[3] != 0;
  if (!touchCalibrationReady) Serial.println("[TOUCH] Invalid calibration spans");
}

// ═══════════════════════════════════════════════════════════════════════════════
// SETUP / LOOP
// ═══════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  pinMode(PUMP_PWM_PIN, OUTPUT);
  analogWrite(PUMP_PWM_PIN, 0);
  setupGateServo();
  setupEncoder();
  Serial.printf("[ENCODER] CLK=%d DT=%d SW=%d\n",
                ENCODER_CLK_PIN, ENCODER_DT_PIN, ENCODER_SW_PIN);
  colorSensorSerial.begin(COLOR_SENSOR_BAUD, SERIAL_8N1,
                          COLOR_SENSOR_RX_PIN, COLOR_SENSOR_TX_PIN);
  Serial.printf("[COLOR UART] Listening RX=%d, TX disabled, at %d baud\n",
                COLOR_SENSOR_RX_PIN, COLOR_SENSOR_BAUD);
  Serial.println("[COLOR UART] Receive-only: accepts labeled/numeric RGB text or raw 3-byte RGB packets");
  setupAmbientSensor();

  stopMotors();
  pushZones();

  // ── Display ─────────────────────────────────────────────────────────────
  tft.init();
  tft.setRotation(1);
  setupTouch();
  tft.fillScreen(C_BG);

  // Audio and SD playback run on the slave; this ESP32 only proxies controls/state.
  sdOk = false;
  trackCount = 0;
  Serial.println("[AUDIO] SD card and playback are handled by slave ESP32");

  // ── Mic ──────────────────────────────────────────────────────────────────
  pinMode(MIC_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);

  // ── Fingerprint sensor ────────────────────────────────────────────────────
  setupFingerprintSensor();
  loadUsers();
  Serial.printf("[FP] User system initialized: %d users\n", userCount);

  // ── WiFi — Access Point ───────────────────────────────────────────────────
  tft.fillScreen(C_BG);
  txt("Starting WiFi AP...", W / 2, H / 2 - 20, 2, C_DIM, C_BG, MC_DATUM);

  WiFi.mode(WIFI_AP);
  // SoftAP can host phone/PC + slave ESP together (max 8 stations).
  // Sharing the hotspot is expected — it does NOT block the web app by itself.
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS, 1 /*channel*/, 0 /*not hidden*/, 8 /*max conn*/);
  delay(500); // give AP time to start

  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("[WiFi] AP started — SSID: %s  IP: %s  (phone + slave OK on same AP)\n",
                WIFI_AP_SSID, apIP.toString().c_str());
  Serial.println("[WiFi] Open web app at http://192.168.4.1");

  // Show connection info on TFT
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, W, 4, C_GREEN);
  txt("WiFi Ready!", W / 2, 30, 4, C_GREEN, C_BG, MC_DATUM);

  txt("Connect your phone/PC to:", W / 2, 80, 2, C_DIM, C_BG, MC_DATUM);
  card(40, 98, 400, 36, C_GREEN, C_GREEN);
  txt(WIFI_AP_SSID, W / 2, 116, 4, C_WHITE, C_GREEN, MC_DATUM);

  txt("Password:", W / 2, 155, 2, C_DIM, C_BG, MC_DATUM);
  txt(WIFI_AP_PASS, W / 2, 173, 2, C_TEXT, C_BG, MC_DATUM);

  txt("Then set ESP32 IP in Settings to:", W / 2, 210, 2, C_DIM, C_BG, MC_DATUM);
  char ipLine[32];
  snprintf(ipLine, sizeof(ipLine), "%s", apIP.toString().c_str());
  card(40, 228, 400, 36, C_AMBER, C_AMBER);
  txt(ipLine, W / 2, 246, 4, C_WHITE, C_AMBER, MC_DATUM);

  delay(4000); // show for 4 seconds then continue to gate screen

  // ── HTTP routes ──────────────────────────────────────────────────────────
  setupRoutes();
  server.begin();
  Serial.println("[HTTP] Server started");

  // ── Gate start ───────────────────────────────────────────────────────────
  enterGateState(GS_WAITING);
}

void loop() {
  // Serve web clients first so SoftAP stays responsive even if slave is flaky
  server.handleClient();

  updateColorSensor();
  updateAmbientSensor();
  updateMicSensor();
  pollEncoder();

  server.handleClient();

  // Slave sync is fail-fast + backoff — never delay() here
  syncSlaveIfConnected();

  server.handleClient();

  // Gate state machine (timed transitions)
  pollGate();

  // Touch polling every 25 ms
  static uint32_t touchMs = 0;
  if (millis() - touchMs >= 25) { touchMs = millis(); pollTouch(); }

  // Sensor / animation updates (lights only when gate is open)
  if (gateState == GS_OPEN) {  
    updateLive();
  }
  syncSlaveAudioState();
  updateAutomaticAudio();
  
  // Enrollment polling
  pollEnrollment();

  server.handleClient();
}
