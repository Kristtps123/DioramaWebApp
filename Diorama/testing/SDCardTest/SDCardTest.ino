#include <SPI.h>
#include <SdFat.h>

constexpr uint8_t SD_SCK_PIN = 18;
constexpr uint8_t SD_MOSI_PIN = 23;
constexpr uint8_t SD_MISO_PIN = 19;
constexpr uint8_t SD_CS_PIN = 21;
constexpr uint32_t SD_SPI_FREQUENCY = 1000000;
constexpr uint32_t SD_SPI_FALLBACK_FREQUENCY = 250000;
constexpr char AUDIO_DIRECTORY[] = "/DIORAMA";

SPIClass sdSPI(VSPI);
SdFs sd;

void readDirectory() {
  FsFile directory;
  if (!directory.open(&sd, AUDIO_DIRECTORY, O_RDONLY) || !directory.isDirectory()) {
    Serial.printf("ERROR: Cannot open directory %s\n", AUDIO_DIRECTORY);
    Serial.println("RESULT: SD MOUNTED, AUDIO DIRECTORY FAILED");
    directory.close();
    return;
  }

  Serial.printf("Quick read check for files in %s (beginning, middle, end)\n\n",
                AUDIO_DIRECTORY);
  uint8_t buffer[512];
  uint16_t fileCount = 0;
  uint16_t failedSamples = 0;

  FsFile entry = directory.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      char name[96];
      const bool hasName = entry.getName(name, sizeof(name));
      const uint32_t fileSize = entry.fileSize();
      const uint16_t sampleSize = fileSize < sizeof(buffer)
        ? static_cast<uint16_t>(fileSize)
        : sizeof(buffer);

      if (hasName) {
        Serial.printf("%s  (%lu bytes)\n", name,
                      static_cast<unsigned long>(fileSize));
      } else {
        Serial.println("Could not read filename");
      }

      if (sampleSize == 0) {
        Serial.println("    Quick read check: FAILED, empty file");
        ++failedSamples;
        ++fileCount;
      } else {
        const uint32_t offsets[] = {
          0,
          (fileSize - sampleSize) / 2,
          fileSize - sampleSize
        };
        bool filePassed = true;
        for (uint8_t sample = 0; sample < 3; ++sample) {
          if (!entry.seekSet(offsets[sample])) {
            Serial.printf("    Sample %u: seek to %lu failed\n",
                          sample + 1, static_cast<unsigned long>(offsets[sample]));
            filePassed = false;
            break;
          }

          const int bytesRead = entry.read(buffer, sampleSize);
          if (bytesRead != sampleSize) {
            Serial.printf("    Sample %u: read %d / %u bytes at %lu\n",
                          sample + 1, bytesRead, sampleSize,
                          static_cast<unsigned long>(offsets[sample]));
            if (sd.card()) {
              Serial.printf("    SdFat card error: code=0x%02X data=0x%02X\n",
                            sd.card()->errorCode(), sd.card()->errorData());
            }
            filePassed = false;
            break;
          }
        }

        if (filePassed) {
          Serial.printf("    Quick read check: OK, 3 x %u-byte samples\n", sampleSize);
        } else {
          ++failedSamples;
        }
        ++fileCount;
      }
    }
    entry.close();
    entry = directory.openNextFile();
  }

  directory.close();
  Serial.printf("\nFinished: %u file(s), %u quick read failure(s)\n",
                fileCount, failedSamples);
  if (fileCount == 0) {
    Serial.println("RESULT: NO FILES FOUND IN /DIORAMA");
  } else if (failedSamples == 0) {
    Serial.println("RESULT: ALL FILES PASSED QUICK READ CHECK");
    Serial.println("Note: this samples each file; it does not read every byte.");
  } else {
    Serial.println("RESULT: ONE OR MORE QUICK READ CHECKS FAILED");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("ESP32 SD adapter test");
  Serial.printf("SPI: SCK=%u MOSI=%u MISO=%u CS=%u at %lu Hz\n",
                SD_SCK_PIN, SD_MOSI_PIN, SD_MISO_PIN, SD_CS_PIN,
                static_cast<unsigned long>(SD_SPI_FREQUENCY));

  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  const uint32_t sdFrequencies[] = {
    SD_SPI_FREQUENCY,
    SD_SPI_FALLBACK_FREQUENCY,
    SD_SPI_FALLBACK_FREQUENCY
  };
  bool mounted = false;
  for (uint8_t attempt = 0; attempt < sizeof(sdFrequencies) / sizeof(sdFrequencies[0]); ++attempt) {
    const uint32_t frequency = sdFrequencies[attempt];
    Serial.printf("Mount attempt %u at %lu Hz...\n",
                  attempt + 1, static_cast<unsigned long>(frequency));
    if (sd.begin(SdSpiConfig(
          SD_CS_PIN, DEDICATED_SPI, SD_SCK_HZ(frequency), &sdSPI))) {
      mounted = true;
      break;
    }
    if (sd.card()) {
      Serial.printf("SdFat error: code=0x%02X data=0x%02X\n",
                    sd.card()->errorCode(), sd.card()->errorData());
    } else {
      Serial.println("SdFat did not return a card object.");
    }
    delay(100);
  }

  if (!mounted) {
    Serial.println("RESULT: SD mount FAILED");
    Serial.println("Check wiring, card seating, 3.3V logic, and FAT32 formatting.");
    return;
  }

  const uint8_t filesystemType = sd.fatType();
  if (filesystemType == 64) {
    Serial.println("Mounted filesystem: exFAT");
  } else {
    Serial.printf("Mounted filesystem: FAT%u\n", filesystemType);
  }
  Serial.println("A full read at 250 kHz can take several minutes per large MP3.");

  readDirectory();
}

void loop() {
}
