/*
 * ==============================================================================
 *  DIORAMA AUXILIARY CONTROLLER FIRMWARE
 *  Board: ESP32 NodeMCU-32S (ESP-32S Kit)
 * ==============================================================================
 * 
 *  FEATURES & SPECIFICATIONS:
 *  - LEDC Channels 7 to 15: Custom 9-MOSFET module for 3x RGB Light Zones
 *    • Inverse PWM logic: 0 = Maximum Brightness, 255 = Completely OFF
 *    • Zone 0 (Left):   R1 (Ch 7),  G1 (Ch 8),  B1 (Ch 9)
 *    • Zone 1 (Right):  R2 (Ch 10), G2 (Ch 11), B2 (Ch 12)
 *    • Zone 2 (Center): R3 (Ch 13), G3 (Ch 14), B3 (Ch 15)
 * 
 *  - LEDC Channels 0 to 3: Motor PWM for TB6612FNG Dual H-Bridge Driver
 *    • Motor A (Fountain / Main Pump): PWMA (Ch 0), AIN1, AIN2
 *    • Motor B (Aux Pump / Carousel):  PWMB (Ch 1), BIN1, BIN2
 *    • Standby Control (STBY)
 * 
 *  - Communication:
 *    • UART2 (Hardware Serial) linked to Main ESP32
 *    • USB Serial (Serial0 @ 115200) for real-time monitoring and manual testing
 *    • Supports both plain text commands (RGB, MOTOR, etc.) and JSON payloads
 * ==============================================================================
 */

#include <Arduino.h>
#include <ArduinoJson.h>

// ─── PIN ASSIGNMENTS (NodeMCU-32S) ─────────────────────────────────────────────

// Custom 9-MOSFET RGB LED Module Pins
// Group 1: Left Zone
#define PIN_R1       4
#define PIN_G1       5
#define PIN_B1       18

// Group 2: Right Zone
#define PIN_R2       19
#define PIN_G2       23
#define PIN_B2       25

// Group 3: Center Zone
#define PIN_R3       26
#define PIN_G3       27
#define PIN_B3       33

// TB6612FNG Dual Motor Driver Pins
#define PIN_MOTOR_STBY 15   // HIGH to enable TB6612FNG driver
#define PIN_PWMA       13   // Motor A Speed (PWM)
#define PIN_AIN1       12   // Motor A Direction 1
#define PIN_AIN2       14   // Motor A Direction 2

#define PIN_PWMB       32   // Motor B Speed (PWM)
#define PIN_BIN1       22   // Motor B Direction 1
#define PIN_BIN2       21   // Motor B Direction 2

// Inter-ESP32 Serial Communication Pins (UART2)
#define PIN_UART_RX    16   // Connect to Main ESP32 TX pin
#define PIN_UART_TX    17   // Connect to Main ESP32 RX pin

// ─── LEDC PWM CHANNELS ALLOCATION ─────────────────────────────────────────────
// Motor PWM (LEDC Channels 0 to 3)
#define CH_MOTOR_A     0
#define CH_MOTOR_B     1
#define CH_MOTOR_C     2   // Spare
#define CH_MOTOR_D     3   // Spare

// LED PWM (LEDC Channels 7 to 15)
#define CH_R1          7
#define CH_G1          8
#define CH_B1          9

#define CH_R2          10
#define CH_G2          11
#define CH_B2          12

#define CH_R3          13
#define CH_G3          14
#define CH_B3          15

// PWM Parameters
#define PWM_FREQ_LED   5000  // 5 kHz for flicker-free LED dimming
#define PWM_FREQ_MOTOR 1000  // 1 kHz for TB6612FNG motor control
#define PWM_RESOLUTION 8     // 8-bit resolution (0 to 255)
#define MAX_FOUNTAIN_PWM 100 // Limit pump PWM to 100/255

// ─── HARDWARE STATE ────────────────────────────────────────────────────────────
struct RGBColor {
  uint8_t r = 255;
  uint8_t g = 180;
  uint8_t b = 90;
};

// 3 Zones: 0=Left, 1=Right, 2=Center
RGBColor zones[3];
bool     masterLightsOn = true;
uint8_t  masterBrightness = 100; // 0-100%

// Motor States
int      motorSpeedA = 0;        // 0-MAX_FOUNTAIN_PWM
int      motorDirA   = 1;        // 0=STOP, 1=FORWARD, 2=REVERSE
int      motorSpeedB = 0;        // 0-MAX_FOUNTAIN_PWM
int      motorDirB   = 1;

// Hardware Serial 2 for Main ESP32 Link
HardwareSerial MainESPComm(2);

// ─── LEDC HELPER WRAPPERS (Core 2.x and 3.x Compatible) ────────────────────────
void attachLEDC(uint8_t pin, uint8_t channel, uint32_t freq, uint8_t resolution) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcAttachChannel(pin, freq, resolution, channel);
#else
  ledcSetup(channel, freq, resolution);
  ledcAttachPin(pin, channel);
#endif
}

void writeLEDC(uint8_t pin, uint8_t channel, uint32_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcWriteChannel(channel, duty);
#else
  ledcWrite(channel, duty);
#endif
}

// ─── INVERSE PWM LED DRIVER FUNCTION ──────────────────────────────────────────
// Rule: Inverse PWM — Value of 0 for MAXIMUM brightness, 255 for OFF.
void applySingleChannel(uint8_t pin, uint8_t channel, uint8_t rawVal) {
  uint8_t effectiveVal = 0;
  if (masterLightsOn) {
    // Scale by master brightness percentage (0-100%)
    effectiveVal = (uint16_t)rawVal * masterBrightness / 100;
  } else {
    effectiveVal = 0; // OFF
  }

  // Inverse PWM formula:
  // Full Brightness (effectiveVal = 255) -> Duty = 0
  // Completely OFF  (effectiveVal = 0)   -> Duty = 255
  uint8_t duty = 255 - effectiveVal;
  writeLEDC(pin, channel, duty);
}

void updateAllLEDs() {
  // Zone 0: Left
  applySingleChannel(PIN_R1, CH_R1, zones[0].r);
  applySingleChannel(PIN_G1, CH_G1, zones[0].g);
  applySingleChannel(PIN_B1, CH_B1, zones[0].b);

  // Zone 1: Right
  applySingleChannel(PIN_R2, CH_R2, zones[1].r);
  applySingleChannel(PIN_G2, CH_G2, zones[1].g);
  applySingleChannel(PIN_B2, CH_B2, zones[1].b);

  // Zone 2: Center
  applySingleChannel(PIN_R3, CH_R3, zones[2].r);
  applySingleChannel(PIN_G3, CH_G3, zones[2].g);
  applySingleChannel(PIN_B3, CH_B3, zones[2].b);
}

// ─── TB6612FNG MOTOR DRIVER FUNCTIONS ─────────────────────────────────────────
void setMotorA(int speed, int direction) {
  motorSpeedA = constrain(speed, 0, MAX_FOUNTAIN_PWM);
  motorDirA   = direction;

  if (motorSpeedA == 0 || motorDirA == 0) {
    digitalWrite(PIN_AIN1, LOW);
    digitalWrite(PIN_AIN2, LOW);
    writeLEDC(PIN_PWMA, CH_MOTOR_A, 0);
  } else if (motorDirA == 1) { // Forward / CW
    digitalWrite(PIN_AIN1, HIGH);
    digitalWrite(PIN_AIN2, LOW);
    writeLEDC(PIN_PWMA, CH_MOTOR_A, motorSpeedA);
  } else {                     // Reverse / CCW
    digitalWrite(PIN_AIN1, LOW);
    digitalWrite(PIN_AIN2, HIGH);
    writeLEDC(PIN_PWMA, CH_MOTOR_A, motorSpeedA);
  }
}

void setMotorB(int speed, int direction) {
  motorSpeedB = constrain(speed, 0, MAX_FOUNTAIN_PWM);
  motorDirB   = direction;

  if (motorSpeedB == 0 || motorDirB == 0) {
    digitalWrite(PIN_BIN1, LOW);
    digitalWrite(PIN_BIN2, LOW);
    writeLEDC(PIN_PWMB, CH_MOTOR_B, 0);
  } else if (motorDirB == 1) { // Forward / CW
    digitalWrite(PIN_BIN1, HIGH);
    digitalWrite(PIN_BIN2, LOW);
    writeLEDC(PIN_PWMB, CH_MOTOR_B, motorSpeedB);
  } else {                     // Reverse / CCW
    digitalWrite(PIN_BIN1, LOW);
    digitalWrite(PIN_BIN2, HIGH);
    writeLEDC(PIN_PWMB, CH_MOTOR_B, motorSpeedB);
  }
}

// ─── COMMAND PARSER ───────────────────────────────────────────────────────────
void processCommand(String line, Stream &replyStream) {
  line.trim();
  if (line.length() == 0) return;

  // 1. JSON Payload Support
  if (line.startsWith("{") && line.endsWith("}")) {
    StaticJsonDocument<300> doc;
    DeserializationError err = deserializeJson(doc, line);
    if (!err) {
      if (doc.containsKey("lights")) {
        masterLightsOn = doc["lights"].as<bool>();
      }
      if (doc.containsKey("brightness")) {
        masterBrightness = constrain(doc["brightness"].as<int>(), 0, 100);
      }
      if (doc.containsKey("zone")) {
        int z = doc["zone"].as<int>();
        uint8_t r = doc["r"] | 255;
        uint8_t g = doc["g"] | 255;
        uint8_t b = doc["b"] | 255;
        if (z >= 0 && z < 3) {
          zones[z] = { r, g, b };
        } else if (z == 3 || z == -1) { // All zones
          for (int i = 0; i < 3; i++) zones[i] = { r, g, b };
        }
      }
      if (doc.containsKey("motorA")) {
        int spd = doc["motorA"]["speed"] | 0;
        int dir = doc["motorA"]["dir"] | 1;
        setMotorA(spd, dir);
      }
      if (doc.containsKey("motorB")) {
        int spd = doc["motorB"]["speed"] | 0;
        int dir = doc["motorB"]["dir"] | 1;
        setMotorB(spd, dir);
      }
      updateAllLEDs();
      replyStream.println("{\"status\":\"ok\"}");
      return;
    }
  }

  // 2. Text / ASCII Protocol Support
  // Examples:
  // RGB <zone 0..2 | 3=all> <r> <g> <b>
  // BRIGHTNESS <0..100>
  // LIGHTS <0|1>
  // MOTOR <0|1> <speed 0..215> <dir 0..2>
  // FOUNTAIN <speed 0..215>
  // TEST
  // STATUS
  char cmd[32] = {0};
  int p1 = 0, p2 = 0, p3 = 0, p4 = 0;
  int parsed = sscanf(line.c_str(), "%31s %d %d %d %d", cmd, &p1, &p2, &p3, &p4);

  if (strcasecmp(cmd, "RGB") == 0) {
    int zoneIdx = p1;
    uint8_t r = constrain(p2, 0, 255);
    uint8_t g = constrain(p3, 0, 255);
    uint8_t b = constrain(p4, 0, 255);
    if (zoneIdx >= 0 && zoneIdx < 3) {
      zones[zoneIdx] = { r, g, b };
    } else { // 3 or invalid = apply to all
      for (int i = 0; i < 3; i++) zones[i] = { r, g, b };
    }
    updateAllLEDs();
    replyStream.printf("OK RGB Zone:%d R:%d G:%d B:%d\n", zoneIdx, r, g, b);

  } else if (strcasecmp(cmd, "LIGHTS") == 0) {
    masterLightsOn = (p1 != 0);
    updateAllLEDs();
    replyStream.printf("OK LIGHTS %s\n", masterLightsOn ? "ON" : "OFF");

  } else if (strcasecmp(cmd, "BRIGHTNESS") == 0) {
    masterBrightness = constrain(p1, 0, 100);
    updateAllLEDs();
    replyStream.printf("OK BRIGHTNESS %d%%\n", masterBrightness);

  } else if (strcasecmp(cmd, "MOTOR") == 0) {
    int motorId = p1; // 0=A, 1=B
    int speed   = constrain(p2, 0, MAX_FOUNTAIN_PWM);
    int dir     = (parsed >= 4) ? p3 : 1;
    if (motorId == 0) {
      setMotorA(speed, dir);
      replyStream.printf("OK MOTOR A Speed:%d Dir:%d\n", speed, dir);
    } else {
      setMotorB(speed, dir);
      replyStream.printf("OK MOTOR B Speed:%d Dir:%d\n", speed, dir);
    }

  } else if (strcasecmp(cmd, "FOUNTAIN") == 0) {
    int speed = constrain(p1, 0, MAX_FOUNTAIN_PWM);
    setMotorA(speed, 1);
    replyStream.printf("OK FOUNTAIN Speed:%d\n", speed);

  } else if (strcasecmp(cmd, "TEST") == 0) {
    replyStream.println("Starting Hardware Self-Test...");
    // Flash all LEDs in sequence
    for (int i = 0; i < 3; i++) {
      zones[0] = (i == 0) ? RGBColor{255, 0, 0} : (i == 1) ? RGBColor{0, 255, 0} : RGBColor{0, 0, 255};
      zones[1] = zones[0];
      zones[2] = zones[0];
      updateAllLEDs();
      delay(400);
    }
    // Test Motors
    setMotorA(180, 1);
    delay(500);
    setMotorA(0, 0);
    setMotorB(180, 1);
    delay(500);
    setMotorB(0, 0);
    replyStream.println("Self-Test Finished.");

  } else if (strcasecmp(cmd, "STATUS") == 0) {
    replyStream.printf("STATUS: Lights=%s, Brightness=%d%%\n", masterLightsOn ? "ON" : "OFF", masterBrightness);
    for (int i = 0; i < 3; i++) {
      replyStream.printf("  Zone %d: R=%d G=%d B=%d\n", i, zones[i].r, zones[i].g, zones[i].b);
    }
    replyStream.printf("  Motor A: Spd=%d Dir=%d | Motor B: Spd=%d Dir=%d\n", motorSpeedA, motorDirA, motorSpeedB, motorDirB);

  } else {
    replyStream.println("ERR: Unknown Command");
  }
}

// ─── SETUP ────────────────────────────────────────────────────────────────────
void setup() {
  // 1. USB Serial for Debug / Direct Terminal Control
  Serial.begin(115200);
  delay(500);
  Serial.println("\n========================================================");
  Serial.println("   DIORAMA AUXILIARY CONTROLLER (NodeMCU-32S)");
  Serial.println("   9-MOSFET LEDC Ch 7..15 (Inverse PWM: 0=Max, 255=Off)");
  Serial.println("   TB6612FNG Motor LEDC Ch 0..3");
  Serial.println("========================================================");

  // 2. Hardware UART2 for Communication with Main ESP32
  MainESPComm.begin(115200, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
  Serial.printf("[AUX] UART2 started on RX:GPIO%d, TX:GPIO%d\n", PIN_UART_RX, PIN_UART_TX);

  // 3. Configure TB6612FNG Motor Control Pins
  pinMode(PIN_MOTOR_STBY, OUTPUT);
  digitalWrite(PIN_MOTOR_STBY, HIGH); // Enable H-Bridge

  pinMode(PIN_AIN1, OUTPUT);
  pinMode(PIN_AIN2, OUTPUT);
  pinMode(PIN_BIN1, OUTPUT);
  pinMode(PIN_BIN2, OUTPUT);

  attachLEDC(PIN_PWMA, CH_MOTOR_A, PWM_FREQ_MOTOR, PWM_RESOLUTION);
  attachLEDC(PIN_PWMB, CH_MOTOR_B, PWM_FREQ_MOTOR, PWM_RESOLUTION);

  setMotorA(0, 0);
  setMotorB(0, 0);
  Serial.println("[AUX] Motor controller configured (LEDC Ch 0..1).");

  // 4. Configure 9-MOSFET Inverse PWM LED Channels (LEDC 7..15)
  attachLEDC(PIN_R1, CH_R1, PWM_FREQ_LED, PWM_RESOLUTION);
  attachLEDC(PIN_G1, CH_G1, PWM_FREQ_LED, PWM_RESOLUTION);
  attachLEDC(PIN_B1, CH_B1, PWM_FREQ_LED, PWM_RESOLUTION);

  attachLEDC(PIN_R2, CH_R2, PWM_FREQ_LED, PWM_RESOLUTION);
  attachLEDC(PIN_G2, CH_G2, PWM_FREQ_LED, PWM_RESOLUTION);
  attachLEDC(PIN_B2, CH_B2, PWM_FREQ_LED, PWM_RESOLUTION);

  attachLEDC(PIN_R3, CH_R3, PWM_FREQ_LED, PWM_RESOLUTION);
  attachLEDC(PIN_G3, CH_G3, PWM_FREQ_LED, PWM_RESOLUTION);
  attachLEDC(PIN_B3, CH_B3, PWM_FREQ_LED, PWM_RESOLUTION);

  // Initial default warm colors
  zones[0] = {255, 180, 90}; // Left
  zones[1] = {255, 180, 90}; // Right
  zones[2] = {255, 180, 90}; // Center
  updateAllLEDs();
  Serial.println("[AUX] 9-MOSFET LED channels configured (LEDC Ch 7..15).");
  Serial.println("[AUX] Ready for commands from Main ESP32 or Serial Monitor.");
}

// ─── LOOP ─────────────────────────────────────────────────────────────────────
String serialBuffer = "";
String uartBuffer = "";

void loop() {
  // 1. Read incoming commands from Main ESP32 (UART2)
  while (MainESPComm.available()) {
    char c = (char)MainESPComm.read();
    if (c == '\n' || c == '\r') {
      if (uartBuffer.length() > 0) {
        processCommand(uartBuffer, MainESPComm);
        uartBuffer = "";
      }
    } else {
      uartBuffer += c;
    }
  }

  // 2. Read incoming commands from USB Serial Monitor (for developer testing)
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        processCommand(serialBuffer, Serial);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
    }
  }
}
