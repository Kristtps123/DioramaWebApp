#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>

// Arduino Nano wiring:
// Sensor TX -> D2 (Nano SoftwareSerial RX)
// Sensor RX -> D3 (Nano SoftwareSerial TX)
// Sensor GND -> Nano GND
// Sensor VCC -> supply voltage specified for your sensor.
// If the sensor RX is not 5V tolerant, use a level shifter or voltage divider
// between Nano D3 and sensor RX.
constexpr uint8_t FINGERPRINT_RX_PIN = 2;
constexpr uint8_t FINGERPRINT_TX_PIN = 3;
constexpr uint32_t FINGERPRINT_BAUD = 57600;
constexpr uint32_t DEBUG_BAUD = 115200;

SoftwareSerial fingerprintSerial(FINGERPRINT_RX_PIN, FINGERPRINT_TX_PIN);
Adafruit_Fingerprint finger(&fingerprintSerial);
bool sensorReady = false;
bool fingerWasDetected = false;

void setup() {
  Serial.begin(DEBUG_BAUD);
  delay(500);

  Serial.println();
  Serial.println(F("Fingerprint sensor hardware test"));
  Serial.print(F("Nano SoftwareSerial: RX=D"));
  Serial.print(FINGERPRINT_RX_PIN);
  Serial.print(F(", TX=D"));
  Serial.print(FINGERPRINT_TX_PIN);
  Serial.print(F(", baud="));
  Serial.println(FINGERPRINT_BAUD);
  Serial.println(F("Wiring: sensor TX -> D2, sensor RX -> D3"));
  Serial.println(F("Connect sensor GND to Nano GND."));
  Serial.println(F("This test does not enroll or delete fingerprints."));

  fingerprintSerial.begin(FINGERPRINT_BAUD);
  delay(1000);

  if (!finger.verifyPassword()) {
    Serial.println(F("RESULT: Sensor did not respond."));
    Serial.println(F("Check power, common GND, TX/RX wiring, and baud rate."));
    Serial.println(F("The project commonly uses 57600 baud; some sensors use 9600."));
    return;
  }
  sensorReady = true;

  Serial.println(F("Sensor communication: OK"));
  if (finger.getTemplateCount() == FINGERPRINT_OK) {
    Serial.print(F("Stored templates: "));
    Serial.println(finger.templateCount);
  } else {
    Serial.println(F("Could not read the stored-template count."));
  }
  Serial.println(F("Place a finger on the sensor to test image capture."));
}

void loop() {
  if (!sensorReady) {
    delay(1000);
    return;
  }

  const uint8_t imageResult = finger.getImage();
  if (imageResult == FINGERPRINT_NOFINGER) {
    if (fingerWasDetected) {
      fingerWasDetected = false;
      Serial.println(F("Finger removed. Place a finger to test again."));
    }
    delay(100);
    return;
  }

  if (fingerWasDetected) {
    delay(100);
    return;
  }

  fingerWasDetected = true;
  if (imageResult != FINGERPRINT_OK) {
    Serial.print(F("Image capture failed (sensor code 0x"));
    Serial.print(imageResult, HEX);
    Serial.println(F(")."));
    Serial.println(F("Lift your finger before trying again."));
    return;
  }

  Serial.println(F("Image capture: OK"));

  const uint8_t conversionResult = finger.image2Tz(1);
  if (conversionResult == FINGERPRINT_OK) {
    Serial.println(F("Image processing: OK"));
    Serial.println(F("RESULT: Sensor is responding and reading fingerprints."));
  } else {
    Serial.print(F("Image processing failed (sensor code 0x"));
    Serial.print(conversionResult, HEX);
    Serial.println(F(")."));
  }
}
