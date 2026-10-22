/*
  Standalone SD-card speaker test for a classic ESP32.

  SD SPI: SCK=18, MOSI=23, MISO=19, CS=21
  PAM8403 input: GPIO25 (DAC1) -> L, GPIO26 (DAC2) -> R, shared GND
  Put MP3 or WAV files directly inside /DIORAMA on the SD card. For WAV,
  use 8- or 16-bit PCM, stereo, 44100 Hz (22050 Hz if playback stutters).
  The ESP32 DAC feeds an amplifier input; do not connect a speaker directly
  to GPIO25 or GPIO26.
*/

#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>
#include <AudioFileSource.h>
#include <AudioGenerator.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <strings.h>

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
#if __has_include(<AudioOutputInternalDAC.h>)
#include <AudioOutputInternalDAC.h>
#else
#error "Update ESP8266Audio to a version that provides AudioOutputInternalDAC.h"
#endif
#else
#include <AudioOutputI2S.h>
#endif

constexpr uint8_t SD_SCK_PIN = 18;
constexpr uint8_t SD_MOSI_PIN = 23;
constexpr uint8_t SD_MISO_PIN = 19;
constexpr uint8_t SD_CS_PIN = 21;
constexpr uint32_t SD_SPI_FREQUENCY = 8000000;
constexpr uint32_t SD_SPI_FALLBACK_FREQUENCY = 4000000;
constexpr uint32_t SD_SPI_SAFE_FREQUENCY = 1000000;
constexpr char AUDIO_DIRECTORY[] = "/DIORAMA";
constexpr uint8_t MAX_AUDIO_TRACKS = 24;
constexpr uint32_t AUDIO_SAMPLE_RATE_HZ = 44100;
constexpr float AUDIO_DIGITAL_BOOST = 1.5f; // About +3.5 dB; peaks are limited to 16-bit range.
constexpr uint32_t AUDIO_WAV_BUFFER_BYTES = 4096;
constexpr uint16_t AUDIO_DAC_BUFFER_BYTES = 2048;
constexpr int AUDIO_I2S_DMA_BUFFER_COUNT = 16;

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
class BoostedInternalDAC : public AudioOutputInternalDAC {
public:
  explicit BoostedInternalDAC(uint16_t bufferSize)
      : AudioOutputInternalDAC(bufferSize) {}

  bool ConsumeSample(int16_t samples[2]) override {
    for (uint8_t channel = 0; channel < 2; ++channel) {
      const float amplified = samples[channel] * AUDIO_DIGITAL_BOOST;
      samples[channel] = static_cast<int16_t>(
          constrain(amplified, -32767.0f, 32767.0f));
    }
    return AudioOutputInternalDAC::ConsumeSample(samples);
  }
};
#else
class BoostedAudioOutputI2S : public AudioOutputI2S {
public:
  BoostedAudioOutputI2S(int port, int outputMode, int dmaBufferCount, int useApll)
      : AudioOutputI2S(port, outputMode, dmaBufferCount, useApll) {}

  bool ConsumeSample(int16_t samples[2]) override {
    for (uint8_t channel = 0; channel < 2; ++channel) {
      const float amplified = samples[channel] * AUDIO_DIGITAL_BOOST;
      samples[channel] = static_cast<int16_t>(
          constrain(amplified, -32767.0f, 32767.0f));
    }
    return AudioOutputI2S::ConsumeSample(samples);
  }
};
#endif

SPIClass sdSPI(VSPI);
SdFs sd;
char audioTracks[MAX_AUDIO_TRACKS][96];
uint8_t audioTrackCount = 0;
int8_t currentAudioTrack = -1;
bool sdCardReady = false;
AudioFileSource* audioSource = nullptr;
AudioGenerator* audioDecoder = nullptr;
AudioOutput* audioOutput = nullptr;

class AudioFileSourceSdFat : public AudioFileSource {
public:
  explicit AudioFileSourceSdFat(const char* filename) {
    open(filename);
  }

  bool open(const char* filename) override {
    close();
    return file.open(&sd, filename, O_RDONLY);
  }

  uint32_t read(void* data, uint32_t length) override {
    return file.isOpen() ? file.read(data, length) : 0;
  }

  bool seek(int32_t offset, int direction) override {
    if (!file.isOpen()) return false;

    uint64_t base;
    if (direction == SEEK_SET) {
      base = 0;
    } else if (direction == SEEK_CUR) {
      base = file.curPosition();
    } else if (direction == SEEK_END) {
      base = file.fileSize();
    } else {
      return false;
    }

    uint64_t target;
    if (offset < 0) {
      const uint64_t distance = static_cast<uint64_t>(-static_cast<int64_t>(offset));
      if (base < distance) return false;
      target = base - distance;
    } else {
      target = base + static_cast<uint32_t>(offset);
      if (target < base) return false;
    }
    return file.seekSet(target);
  }

  bool close() override {
    return !file.isOpen() || file.close();
  }

  bool isOpen() override {
    return file.isOpen();
  }

  uint32_t getSize() override {
    const uint64_t size = file.isOpen() ? file.fileSize() : 0;
    return size > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(size);
  }

  uint32_t getPos() override {
    const uint64_t position = file.isOpen() ? file.curPosition() : 0;
    return position > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(position);
  }

private:
  FsFile file;
};

bool isSupportedAudioFile(const char* name) {
  const size_t length = strlen(name);
  if (length < 4) return false;
  const char* extension = name + length - 4;
  return strcasecmp(extension, ".mp3") == 0 ||
         strcasecmp(extension, ".wav") == 0;
}

bool readWavU16(FsFile& file, uint16_t& value) {
  uint8_t bytes[2];
  if (file.read(bytes, sizeof(bytes)) != sizeof(bytes)) return false;
  value = static_cast<uint16_t>(bytes[0]) |
          (static_cast<uint16_t>(bytes[1]) << 8);
  return true;
}

bool readWavU32(FsFile& file, uint32_t& value) {
  uint8_t bytes[4];
  if (file.read(bytes, sizeof(bytes)) != sizeof(bytes)) return false;
  value = static_cast<uint32_t>(bytes[0]) |
          (static_cast<uint32_t>(bytes[1]) << 8) |
          (static_cast<uint32_t>(bytes[2]) << 16) |
          (static_cast<uint32_t>(bytes[3]) << 24);
  return true;
}

bool validateWavFormat(const char* path) {
  FsFile file;
  if (!file.open(&sd, path, O_RDONLY)) {
    Serial.printf("[AUDIO] ERROR: cannot inspect %s\n", path);
    return false;
  }

  char riff[4], wave[4];
  uint32_t riffSize;
  bool valid = file.read(riff, sizeof(riff)) == sizeof(riff) &&
               readWavU32(file, riffSize) &&
               file.read(wave, sizeof(wave)) == sizeof(wave) &&
               memcmp(riff, "RIFF", 4) == 0 &&
               memcmp(wave, "WAVE", 4) == 0;
  if (!valid) {
    Serial.printf("[AUDIO] ERROR: %s is not a valid RIFF/WAVE file\n", path);
    file.close();
    return false;
  }

  bool foundFormat = false;
  uint16_t encoding = 0, channels = 0, bitsPerSample = 0;
  uint32_t sampleRate = 0;
  const uint64_t fileSize = file.fileSize();
  while (file.curPosition() + 8 <= fileSize) {
    char chunkId[4];
    uint32_t chunkSize;
    if (file.read(chunkId, sizeof(chunkId)) != sizeof(chunkId) ||
        !readWavU32(file, chunkSize)) {
      break;
    }

    const uint64_t chunkData = file.curPosition();
    if (memcmp(chunkId, "fmt ", 4) == 0) {
      uint32_t byteRate;
      uint16_t blockAlign;
      if (chunkSize < 16 ||
          !readWavU16(file, encoding) ||
          !readWavU16(file, channels) ||
          !readWavU32(file, sampleRate) ||
          !readWavU32(file, byteRate) ||
          !readWavU16(file, blockAlign) ||
          !readWavU16(file, bitsPerSample)) {
        break;
      }
      foundFormat = true;
    } else if (memcmp(chunkId, "data", 4) == 0) {
      break;
    }

    const uint64_t nextChunk = chunkData + chunkSize + (chunkSize & 1U);
    if (nextChunk > fileSize || !file.seekSet(nextChunk)) break;
  }
  file.close();

  if (!foundFormat || encoding != 1 || channels != 2 ||
      (bitsPerSample != 8 && bitsPerSample != 16) ||
      (sampleRate != 44100 && sampleRate != 22050)) {
    Serial.printf(
        "[AUDIO] ERROR: %s must be 8- or 16-bit PCM stereo at 44100 or 22050 Hz"
        " (found PCM=%u, channels=%u, bits=%u, rate=%lu Hz)\n",
        path, encoding, channels, bitsPerSample,
        static_cast<unsigned long>(sampleRate));
    return false;
  }

  Serial.printf("[AUDIO] WAV format OK: %u-bit PCM stereo, %lu Hz\n",
                bitsPerSample,
                static_cast<unsigned long>(sampleRate));
  return true;
}

bool mountSdCard() {
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  const uint32_t frequencies[] = {
    SD_SPI_FREQUENCY,
    SD_SPI_FALLBACK_FREQUENCY,
    SD_SPI_SAFE_FREQUENCY
  };
  for (uint8_t attempt = 0; attempt < sizeof(frequencies) / sizeof(frequencies[0]); ++attempt) {
    Serial.printf("[SD] Mount attempt %u at %lu Hz\n",
                  attempt + 1, static_cast<unsigned long>(frequencies[attempt]));
    if (sd.begin(SdSpiConfig(
          SD_CS_PIN, DEDICATED_SPI, SD_SCK_HZ(frequencies[attempt]), &sdSPI))) {
      sdCardReady = true;
      const uint8_t filesystemType = sd.fatType();
      if (filesystemType == 64) {
        Serial.println("[SD] Mounted exFAT card");
      } else {
        Serial.printf("[SD] Mounted FAT%u card\n", filesystemType);
      }
      return true;
    }

    if (sd.card()) {
      Serial.printf("[SD] Mount failed: code=0x%02X data=0x%02X\n",
                    sd.card()->errorCode(), sd.card()->errorData());
    } else {
      Serial.println("[SD] Mount failed: no card object; check card and wiring");
    }
    delay(100);
  }
  return false;
}

bool scanAudioTracks() {
  FsFile directory;
  if (!directory.open(&sd, AUDIO_DIRECTORY, O_RDONLY) || !directory.isDirectory()) {
    Serial.printf("[AUDIO] ERROR: create %s and put MP3/WAV files inside it\n",
                  AUDIO_DIRECTORY);
    directory.close();
    return false;
  }

  FsFile entry = directory.openNextFile();
  while (entry && audioTrackCount < MAX_AUDIO_TRACKS) {
    if (!entry.isDirectory()) {
      char name[sizeof(audioTracks[0])];
      if (entry.getName(name, sizeof(name)) && isSupportedAudioFile(name)) {
        snprintf(audioTracks[audioTrackCount], sizeof(audioTracks[0]),
                 "%s/%s", AUDIO_DIRECTORY, name);
        ++audioTrackCount;
      }
    }
    entry.close();
    entry = directory.openNextFile();
  }
  directory.close();

  for (uint8_t i = 0; i + 1 < audioTrackCount; ++i) {
    for (uint8_t j = i + 1; j < audioTrackCount; ++j) {
      if (strcasecmp(audioTracks[i], audioTracks[j]) > 0) {
        char path[sizeof(audioTracks[0])];
        strlcpy(path, audioTracks[i], sizeof(path));
        strlcpy(audioTracks[i], audioTracks[j], sizeof(audioTracks[i]));
        strlcpy(audioTracks[j], path, sizeof(audioTracks[j]));
      }
    }
  }

  Serial.printf("[AUDIO] Found %u MP3/WAV file(s) in %s\n",
                audioTrackCount, AUDIO_DIRECTORY);
  for (uint8_t i = 0; i < audioTrackCount; ++i) {
    Serial.printf("  %u: %s\n", i + 1, audioTracks[i]);
  }
  if (audioTrackCount == 0) {
    Serial.println("[AUDIO] ERROR: no supported audio files found");
    return false;
  }
  return true;
}

void stopAudioTrack() {
  if (audioDecoder) {
    audioDecoder->stop();
    delete audioDecoder;
    audioDecoder = nullptr;
  }
  if (audioSource) {
    audioSource->close();
    delete audioSource;
    audioSource = nullptr;
  }
}

bool startAudioTrack(uint8_t index) {
  if (!sdCardReady || index >= audioTrackCount) return false;
  stopAudioTrack();
  currentAudioTrack = -1;

  audioSource = new AudioFileSourceSdFat(audioTracks[index]);
  if (!audioSource || !audioSource->isOpen()) {
    Serial.printf("[AUDIO] ERROR: cannot open %s\n", audioTracks[index]);
    stopAudioTrack();
    return false;
  }

  const char* path = audioTracks[index];
  const size_t length = strlen(path);
  const char* extension = length >= 4 ? path + length - 4 : "";
  if (strcasecmp(extension, ".mp3") == 0) {
    audioDecoder = new AudioGeneratorMP3();
  } else if (strcasecmp(extension, ".wav") == 0) {
    if (!validateWavFormat(audioTracks[index])) {
      stopAudioTrack();
      return false;
    }
    AudioGeneratorWAV* wavDecoder = new AudioGeneratorWAV();
    wavDecoder->SetBufferSize(AUDIO_WAV_BUFFER_BYTES);
    audioDecoder = wavDecoder;
  }

  if (!audioDecoder || !audioDecoder->begin(audioSource, audioOutput)) {
    Serial.printf("[AUDIO] ERROR: decoder could not start for %s\n",
                  audioTracks[index]);
    stopAudioTrack();
    return false;
  }

  currentAudioTrack = index;
  Serial.printf("[AUDIO] Playing %u/%u: %s\n",
                index + 1, audioTrackCount, audioTracks[index]);
  return true;
}

void serviceAudioPlayback() {
  if (!audioDecoder || audioDecoder->loop()) return;

  const uint32_t position = audioSource ? audioSource->getPos() : 0;
  const uint32_t size = audioSource ? audioSource->getSize() : 0;
  const bool reachedEndOfFile = audioSource && position >= size;
  Serial.printf("[AUDIO] Decoder stopped at %lu/%lu bytes (%s)\n",
                static_cast<unsigned long>(position),
                static_cast<unsigned long>(size),
                reachedEndOfFile ? "end of file" : "before end of file");
  stopAudioTrack();

  if (!reachedEndOfFile) {
    if (sd.card()) {
      Serial.printf("[AUDIO] SD error: code=0x%02X data=0x%02X\n",
                    sd.card()->errorCode(), sd.card()->errorData());
    }
    return;
  }

  const uint8_t nextTrack = currentAudioTrack + 1;
  currentAudioTrack = -1;
  if (nextTrack < audioTrackCount) {
    startAudioTrack(nextTrack);
  } else {
    Serial.println("[AUDIO] Test complete; all tracks played");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("ESP32 SD-card speaker test");
  Serial.println("MP3 and WAV files in /DIORAMA will be played in name order.");
  Serial.println("WAV format: 8- or 16-bit PCM stereo, 44100 Hz (22050 Hz if playback stutters).");
  Serial.println("DAC output: GPIO25 -> PAM8403 L input, GPIO26 -> PAM8403 R input");
  Serial.println("Use a PAM8403/amplifier; do not connect a speaker directly to the ESP32.");

  if (!mountSdCard()) {
    Serial.println("[SD] ERROR: card mount failed; stopping test");
    return;
  }
  if (!scanAudioTracks()) {
    Serial.println("[AUDIO] ERROR: no playable tracks; stopping test");
    return;
  }

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  audioOutput = new BoostedInternalDAC(AUDIO_DAC_BUFFER_BYTES);
#else
  audioOutput = new BoostedAudioOutputI2S(
      0, AudioOutputI2S::INTERNAL_DAC, AUDIO_I2S_DMA_BUFFER_COUNT,
      AudioOutputI2S::APLL_ENABLE);
#endif
  if (!audioOutput) {
    Serial.println("[AUDIO] ERROR: could not allocate internal DAC output");
    return;
  }
  if (!audioOutput->SetGain(1.0f)) {
    Serial.println("[AUDIO] ERROR: could not configure output gain");
    return;
  }
  Serial.printf("[AUDIO] Internal DAC output ready, default %lu Hz, digital boost %.2fx\n",
                static_cast<unsigned long>(AUDIO_SAMPLE_RATE_HZ), AUDIO_DIGITAL_BOOST);
  Serial.println("[AUDIO] Decoder will set the actual sample rate and channels from the track");

  if (!startAudioTrack(0)) {
    Serial.println("[AUDIO] ERROR: first track could not be started");
  }
}

void loop() {
  serviceAudioPlayback();
  delay(1);
}
