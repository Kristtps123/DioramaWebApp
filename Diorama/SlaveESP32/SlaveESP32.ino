#include <SPI.h>
#include <SD.h>

constexpr uint8_t SD_SCK_PIN  = 21;
constexpr uint8_t SD_MISO_PIN = 34;
constexpr uint8_t SD_MOSI_PIN = 2;
constexpr uint8_t SD_CS_PIN   = 15;
constexpr uint32_t SD_SPEED   = 1000000;  // 1 MHz

SPIClass sdSPI(VSPI);

void listDirectory(File directory, uint8_t depth = 0) {
  while (true) {
    File entry = directory.openNextFile();
    if (!entry) break;

    for (uint8_t i = 0; i < depth; i++) {
      Serial.print("  ");
    }

    Serial.print(entry.name());

    if (entry.isDirectory()) {
      Serial.println("/");
      listDirectory(entry, depth + 1);
    } else {
      Serial.print("  (");
      Serial.print(entry.size());
      Serial.println(" bytes)");
    }

    entry.close();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32 SD adapter test");
  Serial.println("SPI: SCK=21, MISO=34, MOSI=2, CS=15");

  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  if (!SD.begin(SD_CS_PIN, sdSPI, SD_SPEED)) {
    Serial.println("RESULT: SD mount FAILED");
    Serial.println("Check wiring, card format, adapter, and signal voltage.");
    return;
  }

  Serial.println("RESULT: SD mounted successfully");

  uint8_t cardType = SD.cardType();
  Serial.print("Card type: ");
  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SDHC/SDXC");
  } else {
    Serial.println("Unknown");
  }

  Serial.print("Card size: ");
  Serial.print(SD.cardSize() / (1024 * 1024));
  Serial.println(" MB");

  Serial.println("\nFiles on card:");
  File root = SD.open("/");
  if (!root) {
    Serial.println("Could not open card root directory.");
    return;
  }
  listDirectory(root);
  root.close();

  Serial.println("\nFiles in /DIORAMA:");
  File musicFolder = SD.open("/DIORAMA");
  if (!musicFolder || !musicFolder.isDirectory()) {
    Serial.println("Folder /DIORAMA not found.");
    if (musicFolder) musicFolder.close();
  } else {
    listDirectory(musicFolder);
    musicFolder.close();
  }
}

void loop() {
  // Test runs once after startup.
}