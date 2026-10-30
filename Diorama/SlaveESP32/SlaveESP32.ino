/*
  Diorama Slave ESP32 — RGB Light & Inner Circle Motor Controller
  ─────────────────────────────────────────────────────────────────────────────
  Connects to the master ESP32 AP (Diorama-Park) as a WiFi station.
  Master sends mode + RGB + sound detection state; this board drives:
  1. Shared MOSFET PWM outputs for the three RGB zones
  2. ULN2003 stepper driver (IN1-IN4) for Inner Circle rotation on sound detection

  ── IMPORTANT (classic ESP32) ────────────────────────────────────────────────
  Do NOT use GPIO 6–11. They are wired to internal SPI flash.

  ── Safe wiring ──────────────────────────────────────────────────────────────
  All groups share RGB inputs: R=GPIO4, G=GPIO22, B=GPIO32
  Force sensor            : analog output=GPIO36 (VP / ADC1)
  Water sensor            : analog output=GPIO34 (ADC1); calibrate threshold below
                            Keep the sensor output at or below 3.3 V.
  Intake pump driver      : control input=GPIO27
  Drain pump driver       : control input=GPIO33

  ── SPI microSD adapter ─────────────────────────────────────────────────────
  SCK=GPIO18, MOSI=GPIO23, MISO=GPIO19, CS=GPIO21
  SdFat handles FAT16/FAT32 and exFAT; install the SdFat library.

  Audio playback requires the ESP8266Audio library and supports MP3/WAV files
  directly inside /DIORAMA. WAV tracks must be 8- or 16-bit PCM stereo at
  44.1 or 22.05 kHz; output uses the ESP32 built-in DAC.

  ── PAM8403 analog audio input ───────────────────────────────────────────────
  Built-in DAC output: 44,100 Hz default, DAC1=GPIO25 -> Left input,
  DAC2=GPIO26 -> Right input

  ── Motor Driver (ULN2003 + 28BYJ-48) ────────────────────────────────────────
  Stepper inputs: IN1=GPIO13 , IN2=GPIO14, IN3=GPIO16, IN4=GPIO17
  Intake pump driver input: GPIO27
  Drain pump driver input: GPIO33

  ── API (master calls these) ─────────────────────────────────────────────────
  GET /rgb?group=1|2|3|all&r=&g=&b=&brightness=&mode=Basic|Colorful|Sound|Adaptive&sound=0|1
  GET /off?mode=...
  GET /motor?state=on|off&dir=fwd|rev
  GET /status
  GET /pump?type=intake|drain&state=on|off
  Intake pump automatically stops and is blocked while water is detected.
  GET /audio/files, /audio/state, /audio/volume[?value=0-100],
      /audio/play?file=..., /audio/pause, /audio/next, /audio/previous
  GET /audio/stop, /audio/delete?file=...
  POST /audio/upload (multipart form field "audio", MP3/WAV)
*/

#include <SdFat.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <AudioFileSource.h>
#include <AudioGenerator.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
#if __has_include(<AudioOutputInternalDAC.h>)
#include <AudioOutputInternalDAC.h>
#else
#error "Update ESP8266Audio to a version that provides AudioOutputInternalDAC.h"
#endif
#else
#include <AudioOutputI2S.h>
#endif
#include <strings.h>

// ── WiFi (must match main ESP SoftAP) ─────────────────────────────────────────
const char* WIFI_SSID     = "Diorama-Park";
const char* WIFI_PASSWORD = "diorama123";

const IPAddress SLAVE_IP    (192, 168, 4, 200);
const IPAddress GATEWAY_IP  (192, 168, 4,   1);
const IPAddress SUBNET_MASK (255, 255, 255,  0);
const IPAddress DNS_IP      (192, 168, 4,   1);

// The three physical RGB groups share one red, green, and blue driver input.
constexpr uint8_t RGB_R_PIN = 4;
constexpr uint8_t RGB_G_PIN = 22;
constexpr uint8_t RGB_B_PIN = 32;

// ── ULN2003 / 28BYJ-48 stepper input pins ─────────────────────────────────────
constexpr uint8_t IN1_PIN = 13;
constexpr uint8_t IN2_PIN = 14;
constexpr uint8_t IN3_PIN = 16;
constexpr uint8_t IN4_PIN = 17;

constexpr uint8_t INTAKE_PUMP_PIN = 27;
constexpr uint8_t DRAIN_PUMP_PIN = 33;
bool intakePumpOn = false;
bool drainPumpOn = false;

constexpr uint8_t FORCE_SENSOR_PIN = 36; // VP, ADC1 input-only pin
constexpr uint8_t WATER_SENSOR_PIN = 34; // ADC1 input-only pin
// Use the midpoint of measured dry/wet readings; set false if wet reads lower.
constexpr int WATER_SENSOR_THRESHOLD = 2048;
constexpr bool WATER_SENSOR_WET_WHEN_ABOVE_THRESHOLD = true;
constexpr uint32_t WATER_SENSOR_SAMPLE_INTERVAL_MS = 100;
constexpr uint8_t WATER_SENSOR_SAMPLE_COUNT = 8;
int waterSensorAdc = 0;
bool waterSensorDetected = false;
uint32_t lastWaterSensorSampleMs = 0;
constexpr uint32_t AUDIO_DEFAULT_SAMPLE_RATE_HZ = 22050;
constexpr uint8_t AUDIO_OUTPUT_CHANNELS = 2;
constexpr float AUDIO_FIXED_GAIN = 0.5f;
constexpr uint8_t AUDIO_DEFAULT_VOLUME_PERCENT = 50;
constexpr float AUDIO_DIGITAL_BOOST = 0.7f; // About +3.5 dB; peaks are limited to 16-bit range.
constexpr uint32_t AUDIO_WAV_BUFFER_BYTES = 4096;
constexpr uint16_t AUDIO_DAC_BUFFER_BYTES = 2048;
constexpr int AUDIO_I2S_DMA_BUFFER_COUNT = 16;
uint32_t audioOutputSampleRateHz = AUDIO_DEFAULT_SAMPLE_RATE_HZ;
volatile uint8_t audioVolumePercent = AUDIO_DEFAULT_VOLUME_PERCENT;

constexpr uint8_t SD_SCK_PIN  = 18;
constexpr uint8_t SD_MOSI_PIN = 23;
constexpr uint8_t SD_MISO_PIN = 19;
constexpr uint8_t SD_CS_PIN  = 21;
constexpr uint32_t SD_SPI_FREQUENCY = 8000000;
constexpr uint32_t SD_SPI_FALLBACK_FREQUENCY = 4000000;
constexpr uint32_t SD_SPI_SAFE_FREQUENCY = 1000000;
SPIClass sdSPI(VSPI);
SdFs sd;
bool sdCardReady = false;
volatile bool audioCommandPending = false;
volatile bool audioStopComplete = false;

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
class GainAdjustedInternalDAC : public AudioOutputInternalDAC {
public:
  explicit GainAdjustedInternalDAC(uint16_t bufferSize)
      : AudioOutputInternalDAC(bufferSize) {}

  bool SetRate(int rate) override {
    if (!AudioOutputInternalDAC::SetRate(rate)) return false;
    audioOutputSampleRateHz = rate;
    return true;
  }

  bool begin() override {
    return AudioOutputInternalDAC::begin() &&
           AudioOutputInternalDAC::SetRate(audioOutputSampleRateHz);
  }

  bool SetGain(float gain) override {
    outputGain = constrain(gain, 0.0f, 1.0f);
    return AudioOutputInternalDAC::SetGain(1.0f);
  }

  bool ConsumeSample(int16_t samples[2]) override {
    if (audioCommandPending) return false;
    for (uint8_t channel = 0; channel < AUDIO_OUTPUT_CHANNELS; ++channel) {
      const float amplified = samples[channel] * outputGain * AUDIO_DIGITAL_BOOST;
      samples[channel] = static_cast<int16_t>(
          constrain(amplified, -32767.0f, 32767.0f));
    }
    return AudioOutputInternalDAC::ConsumeSample(samples);
  }

private:
  float outputGain = AUDIO_FIXED_GAIN;
};
#else
class RateTrackingAudioOutputI2S : public AudioOutputI2S {
public:
  RateTrackingAudioOutputI2S(int port, int outputMode, int dmaBufferCount, int useApll)
      : AudioOutputI2S(port, outputMode, dmaBufferCount, useApll) {}

  bool SetGain(float gain) override {
    outputGain = constrain(gain, 0.0f, 1.0f);
    return AudioOutputI2S::SetGain(1.0f);
  }

  bool SetRate(int rate) override {
    if (!AudioOutputI2S::SetRate(rate)) return false;
    audioOutputSampleRateHz = rate;
    return true;
  }

  bool ConsumeSample(int16_t samples[2]) override {
    if (audioCommandPending) return false;
    for (uint8_t channel = 0; channel < AUDIO_OUTPUT_CHANNELS; ++channel) {
      const float amplified = samples[channel] * outputGain * AUDIO_DIGITAL_BOOST;
      samples[channel] = static_cast<int16_t>(
          constrain(amplified, -32767.0f, 32767.0f));
    }
    return AudioOutputI2S::ConsumeSample(samples);
  }

private:
  float outputGain = AUDIO_FIXED_GAIN;
};
#endif

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

constexpr uint8_t MAX_AUDIO_TRACKS = 24;
char audioTracks[MAX_AUDIO_TRACKS][96];
uint8_t audioTrackCount = 0;
int8_t currentAudioTrack = -1;
int8_t pendingAudioTrack = -1;
bool audioPlaying = false;
bool audioOutputPaused = false;
portMUX_TYPE audioStateMux = portMUX_INITIALIZER_UNLOCKED;
AudioFileSourceSdFat* audioSource = nullptr;
AudioGenerator* audioDecoder = nullptr;
AudioOutput* audioOutput = nullptr;
FsFile audioUploadFile;
String audioUploadPath;
String audioUploadError;
size_t audioUploadBytesWritten = 0;
uint16_t audioUploadStatus = 400;
bool audioUploadFileCreated = false;
bool audioUploadInProgress = false;

enum AudioCommandType : uint8_t {
  AUDIO_COMMAND_PLAY,
  AUDIO_COMMAND_PAUSE,
  AUDIO_COMMAND_STEP,
  AUDIO_COMMAND_STOP,
  AUDIO_COMMAND_VOLUME
};

struct AudioCommand {
  AudioCommandType type;
  int8_t value;
};

QueueHandle_t audioCommandQueue = nullptr;
volatile bool audioTaskRunning = false;

void processAudioCommand(const AudioCommand& command);
void addCORS();

void setAudioState(int8_t track, bool playing) {
  portENTER_CRITICAL(&audioStateMux);
  currentAudioTrack = track;
  audioPlaying = playing;
  portEXIT_CRITICAL(&audioStateMux);
}

void getAudioState(int8_t& track, bool& playing) {
  portENTER_CRITICAL(&audioStateMux);
  track = currentAudioTrack;
  playing = audioPlaying;
  portEXIT_CRITICAL(&audioStateMux);
}

bool queueAudioCommand(AudioCommandType type, int8_t value = 0) {
  if (!audioCommandQueue) return false;
  const AudioCommand command = {type, value};
  if (xQueueSend(audioCommandQueue, &command, 0) != pdTRUE) return false;
  audioCommandPending = true;
  return true;
}

void serviceAudioCommands() {
  AudioCommand command;
  while (xQueueReceive(audioCommandQueue, &command, 0) == pdTRUE) {
    processAudioCommand(command);
  }
  audioCommandPending = false;
  if (uxQueueMessagesWaiting(audioCommandQueue) > 0) {
    audioCommandPending = true;
  }
}

bool innerCircleMotorRunning = false;
bool manualMotorRunning = false;
bool motorForward = true;
uint8_t motorStepIndex = 0;
unsigned long lastMotorStepMs = 0;
constexpr unsigned long MOTOR_STEP_INTERVAL_MS = 2;
constexpr uint8_t MOTOR_PHASES[8][4] = {
  {1, 0, 0, 0},
  {1, 1, 0, 0},
  {0, 1, 0, 0},
  {0, 1, 1, 0},
  {0, 0, 1, 0},
  {0, 0, 1, 1},
  {0, 0, 0, 1},
  {1, 0, 0, 1},
};
unsigned long lastSoundDetectedMs = 0;
constexpr unsigned long SOUND_MOTOR_HOLD_MS = 600; // Hold motor briefly across audio pulses for smooth rotation

struct GroupState { uint8_t r, g, b, brightness; bool on; };
GroupState groups[3] = {
  {255, 255, 255, 100, false},
  {255, 255, 255, 100, false},
  {255, 255, 255, 100, false},
};

String lightMode = "Basic";
WebServer server(80);

unsigned long lastReconnectMs = 0;
unsigned long lastStatusMs    = 0;
bool wasConnected = false;
bool httpStarted  = false;

// ── Motor Control Functions ───────────────────────────────────────────────────
void writeMotorPhase() {
  digitalWrite(IN1_PIN, MOTOR_PHASES[motorStepIndex][0] ? HIGH : LOW);
  digitalWrite(IN2_PIN, MOTOR_PHASES[motorStepIndex][1] ? HIGH : LOW);
  digitalWrite(IN3_PIN, MOTOR_PHASES[motorStepIndex][2] ? HIGH : LOW);
  digitalWrite(IN4_PIN, MOTOR_PHASES[motorStepIndex][3] ? HIGH : LOW);
}

void stopMotors() {
  digitalWrite(IN1_PIN, LOW);
  digitalWrite(IN2_PIN, LOW);
  digitalWrite(IN3_PIN, LOW);
  digitalWrite(IN4_PIN, LOW);
  innerCircleMotorRunning = false;
  manualMotorRunning = false;
}

void runInnerCircle(bool forward = true) {
  motorForward = forward;
  if (!innerCircleMotorRunning) {
    writeMotorPhase();
    lastMotorStepMs = millis();
  }
  innerCircleMotorRunning = true;
}

void serviceInnerCircleMotor(unsigned long now) {
  if (!innerCircleMotorRunning || now - lastMotorStepMs < MOTOR_STEP_INTERVAL_MS) return;

  motorStepIndex = motorForward
    ? (motorStepIndex + 1) % 8
    : (motorStepIndex + 7) % 8;
  writeMotorPhase();
  lastMotorStepMs = now;
}

bool isSupportedAudioFile(const String& path) {
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".mp3") || lower.endsWith(".wav");
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
  const bool validHeader =
      file.read(riff, sizeof(riff)) == sizeof(riff) &&
      readWavU32(file, riffSize) &&
      file.read(wave, sizeof(wave)) == sizeof(wave) &&
      memcmp(riff, "RIFF", 4) == 0 &&
      memcmp(wave, "WAVE", 4) == 0;
  if (!validHeader) {
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

  if (!foundFormat || encoding != 1 || channels != AUDIO_OUTPUT_CHANNELS ||
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
                bitsPerSample, static_cast<unsigned long>(sampleRate));
  return true;
}

void scanAudioTracks() {
  audioTrackCount = 0;
  if (!sdCardReady) return;

  FsFile directory;
  if (!directory.open(&sd, "/DIORAMA", O_RDONLY) || !directory.isDirectory()) {
    Serial.println("[AUDIO] ERROR: /DIORAMA directory not found");
    directory.close();
    return;
  }

  FsFile entry = directory.openNextFile();
  while (entry && audioTrackCount < MAX_AUDIO_TRACKS) {
    if (!entry.isDirectory()) {
      char name[sizeof(audioTracks[0])];
      if (entry.getName(name, sizeof(name))) {
        String path = String("/DIORAMA/") + name;
        if (isSupportedAudioFile(path)) {
          strlcpy(audioTracks[audioTrackCount++], path.c_str(), sizeof(audioTracks[0]));
        }
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
  Serial.printf("[AUDIO] Found %u MP3/WAV file(s) in /DIORAMA\n", audioTrackCount);
}

void stopAudioDecoder() {
  setAudioState(currentAudioTrack, false);
  if (audioDecoder) {
    audioDecoder->stop();
    delete audioDecoder;
    audioDecoder = nullptr;
  }
  if (audioOutput) audioOutput->stop();
  audioOutputPaused = false;
  if (audioSource) {
    audioSource->close();
    delete audioSource;
    audioSource = nullptr;
  }
}

bool startAudioTrack(uint8_t index) {
  if (!sdCardReady || index >= audioTrackCount) return false;
  stopAudioDecoder();
  setAudioState(-1, false);

  audioSource = new AudioFileSourceSdFat(audioTracks[index]);
  if (!audioSource || !audioSource->isOpen()) {
    Serial.printf("[AUDIO] ERROR: cannot open %s\n", audioTracks[index]);
    stopAudioDecoder();
    return false;
  }

  String path = audioTracks[index];
  path.toLowerCase();
  if (path.endsWith(".mp3")) audioDecoder = new AudioGeneratorMP3();
  else if (path.endsWith(".wav")) {
    if (!validateWavFormat(audioTracks[index])) {
      stopAudioDecoder();
      return false;
    }
    AudioGeneratorWAV* wavDecoder = new AudioGeneratorWAV();
    wavDecoder->SetBufferSize(AUDIO_WAV_BUFFER_BYTES);
    audioDecoder = wavDecoder;
  }

  if (!audioDecoder || !audioOutput ||
      !audioOutput->SetGain(audioVolumePercent / 100.0f) ||
      !audioDecoder->begin(audioSource, audioOutput)) {
    Serial.printf("[AUDIO] ERROR: decoder could not start for %s\n", audioTracks[index]);
    stopAudioDecoder();
    return false;
  }

  audioOutputPaused = false;
  setAudioState(index, true);
  Serial.printf("[AUDIO] Playing %u/%u: %s\n",
                currentAudioTrack + 1, audioTrackCount, audioTracks[currentAudioTrack]);
  return true;
}

String jsonQuoted(const char* value) {
  String escaped = "\"";
  while (*value) {
    const unsigned char ch = static_cast<unsigned char>(*value++);
    if (ch == '"' || ch == '\\') {
      escaped += '\\';
      escaped += static_cast<char>(ch);
    } else if (ch >= 0x20) {
      escaped += static_cast<char>(ch);
    }
  }
  escaped += '"';
  return escaped;
}

bool makeAudioUploadPath(const String& uploadedName, char* path, size_t pathSize) {
  int basenameStart = max(uploadedName.lastIndexOf('/'), uploadedName.lastIndexOf('\\')) + 1;
  const String filename = uploadedName.substring(basenameStart);
  if (filename.isEmpty() || filename.length() > 80 ||
      filename[0] == '.' || filename.endsWith(".") || filename.endsWith(" ")) {
    return false;
  }

  for (size_t i = 0; i < filename.length(); ++i) {
    const char ch = filename[i];
    const bool allowed = (ch >= 'a' && ch <= 'z') ||
                         (ch >= 'A' && ch <= 'Z') ||
                         (ch >= '0' && ch <= '9') ||
                         ch == ' ' || ch == '-' || ch == '_' ||
                         ch == '.' || ch == '(' || ch == ')';
    if (!allowed) return false;
  }

  String lowerName = filename;
  lowerName.toLowerCase();
  if (!lowerName.endsWith(".mp3") && !lowerName.endsWith(".wav")) return false;

  const int written = snprintf(path, pathSize, "/DIORAMA/%s", filename.c_str());
  return written > 0 && static_cast<size_t>(written) < pathSize;
}

void failAudioUpload(uint16_t status, const char* message) {
  if (audioUploadError.length()) return;
  audioUploadStatus = status;
  audioUploadError = message;
  if (audioUploadFile.isOpen()) audioUploadFile.close();
  if (audioUploadFileCreated && audioUploadPath.length()) {
    sd.remove(audioUploadPath.c_str());
  }
  audioUploadFileCreated = false;
}

void handleAudioUploadData() {
  HTTPUpload& upload = server.upload();
  switch (upload.status) {
    case UPLOAD_FILE_START: {
      audioUploadPath = "";
      audioUploadError = "";
      audioUploadBytesWritten = 0;
      audioUploadStatus = 400;
      audioUploadFileCreated = false;

      if (!sdCardReady) {
        failAudioUpload(503, "SD card is not mounted");
        break;
      }
      if (audioUploadInProgress || audioDecoder || audioSource ||
          pendingAudioTrack >= 0 ||
          (audioCommandQueue && uxQueueMessagesWaiting(audioCommandQueue) > 0)) {
        failAudioUpload(409, "Stop audio playback before uploading");
        break;
      }

      char path[96];
      if (!makeAudioUploadPath(upload.filename, path, sizeof(path))) {
        failAudioUpload(400, "Use an MP3 or WAV filename with basic English characters");
        break;
      }
      if (!sd.exists("/DIORAMA") && !sd.mkdir("/DIORAMA")) {
        failAudioUpload(500, "Could not create the /DIORAMA folder");
        break;
      }

      scanAudioTracks();
      if (audioTrackCount >= MAX_AUDIO_TRACKS) {
        failAudioUpload(409, "The playlist is full (maximum 24 tracks)");
        break;
      }
      audioUploadPath = path;
      if (sd.exists(audioUploadPath.c_str())) {
        failAudioUpload(409, "A file with that name already exists");
        break;
      }
      if (!audioUploadFile.open(&sd, audioUploadPath.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL)) {
        failAudioUpload(507, "Could not create the file on the SD card");
        break;
      }

      audioUploadFileCreated = true;
      audioUploadInProgress = true;
      Serial.printf("[AUDIO] Upload started: %s\n", audioUploadPath.c_str());
      break;
    }

    case UPLOAD_FILE_WRITE:
      if (!audioUploadError.length() && audioUploadFile.isOpen()) {
        const size_t written = audioUploadFile.write(upload.buf, upload.currentSize);
        audioUploadBytesWritten += written;
        if (written != upload.currentSize) {
          failAudioUpload(507, "SD card write failed or the card is full");
        }
      }
      break;

    case UPLOAD_FILE_END: {
      if (audioUploadError.length() || !audioUploadFile.isOpen()) break;
      audioUploadFile.flush();
      if (!audioUploadFile.close()) {
        failAudioUpload(507, "Could not finish writing the file");
        break;
      }
      if (audioUploadBytesWritten == 0) {
        failAudioUpload(400, "The selected audio file is empty");
        break;
      }
      String lowerPath = audioUploadPath;
      lowerPath.toLowerCase();
      if (lowerPath.endsWith(".wav") &&
          !validateWavFormat(audioUploadPath.c_str())) {
        failAudioUpload(400, "WAV must be 8- or 16-bit PCM stereo at 22050 or 44100 Hz");
        break;
      }
      scanAudioTracks();
      audioUploadStatus = 201;
      audioUploadFileCreated = false;
      Serial.printf("[AUDIO] Upload complete: %s (%u bytes)\n",
                    audioUploadPath.c_str(),
                    static_cast<unsigned>(audioUploadBytesWritten));
      break;
    }

    case UPLOAD_FILE_ABORTED:
      failAudioUpload(400, "Upload was interrupted");
      break;
  }
}

void handleAudioUploadComplete() {
  addCORS();
  audioUploadInProgress = false;
  if (audioUploadError.length()) {
    String response = "{\"error\":";
    response += jsonQuoted(audioUploadError.c_str());
    response += "}";
    server.send(audioUploadStatus, "application/json", response);
    Serial.printf("[AUDIO] Upload failed: %s\n", audioUploadError.c_str());
    return;
  }

  String response = "{\"ok\":true,\"file\":";
  response += jsonQuoted(audioUploadPath.c_str());
  response += ",\"bytes\":";
  response += String(static_cast<unsigned long>(audioUploadBytesWritten));
  response += "}";
  server.send(201, "application/json", response);
}

void serviceAudioPlayback() {
  if (pendingAudioTrack >= 0) {
    const uint8_t requestedTrack = static_cast<uint8_t>(pendingAudioTrack);
    pendingAudioTrack = -1;
    if (!startAudioTrack(requestedTrack)) {
      setAudioState(-1, false);
      Serial.printf("[AUDIO] ERROR: queued track %u failed to start\n",
                    requestedTrack + 1);
    }
    return;
  }

  if (!audioPlaying || !audioDecoder) return;
  if (audioDecoder->loop()) return;

  const uint32_t sourcePosition = audioSource ? audioSource->getPos() : 0;
  const uint32_t sourceSize = audioSource ? audioSource->getSize() : 0;
  const bool reachedEndOfFile = audioSource && sourcePosition >= sourceSize;
  Serial.printf("[AUDIO] Decoder stopped at %lu/%lu bytes (%s)\n",
                static_cast<unsigned long>(sourcePosition),
                static_cast<unsigned long>(sourceSize),
                reachedEndOfFile ? "end of file" : "before end of file");
  if (!reachedEndOfFile) {
    if (sd.card()) {
      Serial.printf("[AUDIO] SD card error after decoder stop: code=0x%02X data=0x%02X\n",
                    sd.card()->errorCode(), sd.card()->errorData());
    }
    stopAudioDecoder();
    setAudioState(-1, false);
    return;
  }

  const int8_t nextTrack = currentAudioTrack + 1;
  if (nextTrack >= 0 && nextTrack < audioTrackCount) {
    if (!startAudioTrack(static_cast<uint8_t>(nextTrack))) {
      stopAudioDecoder();
      setAudioState(-1, false);
    }
  } else {
    stopAudioDecoder();
    setAudioState(-1, false);
  }
}

void processAudioCommand(const AudioCommand& command) {
  switch (command.type) {
    case AUDIO_COMMAND_PLAY: {
      const int8_t index = command.value;
      if (index < 0 || index >= audioTrackCount || pendingAudioTrack == index) return;
      if (currentAudioTrack == index && audioDecoder) {
        if (!audioOutput || !audioOutput->SetGain(audioVolumePercent / 100.0f)) {
          Serial.println("[AUDIO] ERROR: could not restore audio output gain");
          return;
        }
        if (audioOutputPaused && !audioOutput->begin()) {
          Serial.println("[AUDIO] ERROR: could not restart audio output for resume");
          return;
        }
        audioOutputPaused = false;
        setAudioState(currentAudioTrack, true);
      } else {
        pendingAudioTrack = index;
      }
      break;
    }
    case AUDIO_COMMAND_PAUSE:
      pendingAudioTrack = -1;
      if (!audioDecoder) {
        Serial.println("[AUDIO] Pause ignored: no active audio track");
        return;
      }
      if (!audioOutput || !audioOutput->SetGain(0.0f)) {
        Serial.println("[AUDIO] ERROR: could not mute audio output for pause");
        return;
      }
      audioOutput->flush();
      if (!audioOutput->stop()) {
        Serial.println("[AUDIO] ERROR: could not stop audio output for pause");
        return;
      }
      audioOutputPaused = true;
      setAudioState(currentAudioTrack, false);
      Serial.println("[AUDIO] Playback paused; output stopped and buffer flushed");
      break;
    case AUDIO_COMMAND_STEP: {
      if (!sdCardReady || !audioTrackCount) return;
      const int8_t direction = command.value;
      const int8_t baseTrack = pendingAudioTrack >= 0
        ? pendingAudioTrack
        : currentAudioTrack;
      const uint8_t nextTrack = baseTrack >= 0
        ? static_cast<uint8_t>((baseTrack + direction + audioTrackCount) % audioTrackCount)
        : (direction > 0 ? 0 : audioTrackCount - 1);
      pendingAudioTrack = static_cast<int8_t>(nextTrack);
      break;
    }
    case AUDIO_COMMAND_STOP:
      pendingAudioTrack = -1;
      stopAudioDecoder();
      setAudioState(-1, false);
      audioStopComplete = true;
      Serial.println("[AUDIO] Playback stopped");
      break;
    case AUDIO_COMMAND_VOLUME: {
      const uint8_t volume = static_cast<uint8_t>(command.value);
      if (!audioOutput || !audioOutput->SetGain(volume / 100.0f)) {
        Serial.printf("[AUDIO] ERROR: could not set volume to %u%%\n", volume);
        return;
      }
      audioVolumePercent = volume;
      Serial.printf("[AUDIO] Volume set to %u%%\n", audioVolumePercent);
      break;
    }
  }
}

void audioPlaybackTask(void*) {
  // Keep decoder work out of the HTTP loop so controls stay responsive during playback.
  for (;;) {
    serviceAudioCommands();
    serviceAudioPlayback();
    vTaskDelay(1);
  }
}

void resolveRgbForMode(uint8_t& r, uint8_t& g, uint8_t& b) {
  if (lightMode == "Basic") {
    r = 255;
    g = 255;
    b = 255;
  }
}

void setModeFromArg() {
  if (!server.hasArg("mode")) return;
  String m = server.arg("mode");
  m.trim();
  if (m.equalsIgnoreCase("Basic"))          lightMode = "Basic";
  else if (m.equalsIgnoreCase("Colorful"))  lightMode = "Colorful";
  else if (m.equalsIgnoreCase("Sound"))     lightMode = "Sound";
  else if (m.equalsIgnoreCase("Adaptive"))  lightMode = "Adaptive";

  if (lightMode != "Sound") {
    lastSoundDetectedMs = 0;
    if (innerCircleMotorRunning && !manualMotorRunning) stopMotors();
  }
}

void writeRGB(uint8_t pinR, uint8_t pinG, uint8_t pinB,
              uint8_t r, uint8_t g, uint8_t b, uint8_t br) {
  // Inverted MOSFET: 0 = full on, 255 = off
  analogWrite(pinR, 255 - (uint8_t)((uint32_t)r * br / 100));
  analogWrite(pinG, 255 - (uint8_t)((uint32_t)g * br / 100));
  analogWrite(pinB, 255 - (uint8_t)((uint32_t)b * br / 100));
}

void pushAllGroups() {
  const GroupState& g = groups[0];
  uint8_t br = g.on ? g.brightness : 0;
  writeRGB(RGB_R_PIN, RGB_G_PIN, RGB_B_PIN, g.r, g.g, g.b, br);
}

void allOff() {
  for (int i = 0; i < 3; i++) groups[i].on = false;
  pushAllGroups();
}

void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleRGB() {
  addCORS();
  if (!server.hasArg("group")) {
    server.send(400, "application/json", "{\"error\":\"missing group\"}");
    return;
  }

  setModeFromArg();

  uint8_t r  = server.hasArg("r")          ? constrain(server.arg("r").toInt(),          0, 255) : 255;
  uint8_t g  = server.hasArg("g")          ? constrain(server.arg("g").toInt(),          0, 255) : 255;
  uint8_t b  = server.hasArg("b")          ? constrain(server.arg("b").toInt(),          0, 255) : 255;
  uint8_t br = server.hasArg("brightness") ? constrain(server.arg("brightness").toInt(), 0, 100) : 100;

  resolveRgbForMode(r, g, b);

  bool soundParam = false;
  if (server.hasArg("sound")) {
    String s = server.arg("sound");
    soundParam = (s == "1" || s.equalsIgnoreCase("true") || s.equalsIgnoreCase("on"));
  }
  if (lightMode == "Sound") {
    lastSoundDetectedMs = soundParam ? millis() : 0;
  }

  String grp = server.arg("group");
  grp.toLowerCase();

  if (grp == "all") {
    for (int i = 0; i < 3; i++) {
      groups[i] = {r, g, b, br, true};
    }
    pushAllGroups();
    Serial.printf("[RGB] mode=%s ALL #%02X%02X%02X br=%d sound=%d\n",
                  lightMode.c_str(), r, g, b, br, soundParam ? 1 : 0);
  } else {
    int idx = grp.toInt() - 1;
    if (idx < 0 || idx > 2) {
      server.send(400, "application/json", "{\"error\":\"group must be 1,2,3 or all\"}");
      return;
    }
    for (int i = 0; i < 3; i++) {
      groups[i] = {r, g, b, br, true};
    }
    pushAllGroups();
    Serial.printf("[RGB] mode=%s G%d #%02X%02X%02X br=%d sound=%d\n",
                  lightMode.c_str(), idx + 1, r, g, b, br, soundParam ? 1 : 0);
  }

  char ok[160];
  snprintf(ok, sizeof(ok),
           "{\"ok\":true,\"mode\":\"%s\",\"r\":%d,\"g\":%d,\"b\":%d,\"ip\":\"%s\",\"motor\":%s}",
           lightMode.c_str(), r, g, b, WiFi.localIP().toString().c_str(),
           innerCircleMotorRunning ? "true" : "false");
  server.send(200, "application/json", ok);
}

void handleOff() {
  addCORS();
  setModeFromArg();
  allOff();
  stopMotors();
  lastSoundDetectedMs = 0;
  Serial.printf("[RGB] mode=%s All OFF\n", lightMode.c_str());
  server.send(200, "application/json", "{\"ok\":true,\"motor\":false}");
}

void handleMotor() {
  addCORS();
  if (server.hasArg("state")) {
    String st = server.arg("state");
    bool fwd = server.hasArg("dir") ? (!server.arg("dir").equalsIgnoreCase("rev")) : true;
    if (st.equalsIgnoreCase("on") || st.equalsIgnoreCase("run") || st == "1") {
      manualMotorRunning = true;
      runInnerCircle(fwd);
      Serial.println("[MOTOR] Manual RUN");
    } else {
      stopMotors();
      lastSoundDetectedMs = 0;
      Serial.println("[MOTOR] Manual STOP");
    }
  }
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"motorRunning\":%s}", innerCircleMotorRunning ? "true" : "false");
  server.send(200, "application/json", buf);
}

void handleForceSensor() {
  uint32_t rawTotal = 0;
  for (uint8_t sample = 0; sample < 8; ++sample) {
    rawTotal += analogRead(FORCE_SENSOR_PIN);
  }
  const uint16_t raw = static_cast<uint16_t>((rawTotal + 4) / 8);
  const uint16_t level = static_cast<uint16_t>((raw * 1023U + 2047U) / 4095U);

  char response[48];
  snprintf(response, sizeof(response), "{\"raw\":%u,\"level\":%u}", raw, level);
  server.send(200, "application/json", response);
}

void handleAudioFiles() {
  addCORS();
  String json = "{\"sdCardReady\":";
  json += sdCardReady ? "true,\"files\":[" : "false,\"files\":[";
  for (uint8_t i = 0; i < audioTrackCount; ++i) {
    if (i) json += ',';
    json += jsonQuoted(audioTracks[i]);
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleAudioDelete() {
  addCORS();
  if (!sdCardReady) {
    server.send(503, "application/json", "{\"error\":\"SD card is not mounted\"}");
    return;
  }
  if (!server.hasArg("file")) {
    server.send(400, "application/json", "{\"error\":\"missing file\"}");
    return;
  }
  if (audioUploadInProgress) {
    server.send(409, "application/json",
                "{\"error\":\"Cannot delete a file while an upload is in progress\"}");
    return;
  }

  const String requested = server.arg("file");
  bool trackFound = false;
  for (uint8_t i = 0; i < audioTrackCount; ++i) {
    if (requested == audioTracks[i]) {
      trackFound = true;
      break;
    }
  }
  if (!trackFound) {
    server.send(404, "application/json", "{\"error\":\"track not found in /DIORAMA\"}");
    return;
  }

  audioStopComplete = false;
  if (!queueAudioCommand(AUDIO_COMMAND_STOP)) {
    server.send(503, "application/json",
                "{\"error\":\"audio command queue is full; could not stop playback\"}");
    return;
  }

  const uint32_t stopStartedMs = millis();
  while (!audioStopComplete && millis() - stopStartedMs < 2500) {
    if (!audioTaskRunning) serviceAudioCommands();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  if (!audioStopComplete) {
    server.send(503, "application/json",
                "{\"error\":\"audio playback did not stop; file was not deleted\"}");
    return;
  }

  if (!sd.remove(requested.c_str())) {
    Serial.printf("[AUDIO] ERROR: could not delete %s (SD error code=0x%02X data=0x%02X)\n",
                  requested.c_str(),
                  sd.card() ? sd.card()->errorCode() : 0,
                  sd.card() ? sd.card()->errorData() : 0);
    server.send(500, "application/json", "{\"error\":\"Could not delete the file from the SD card\"}");
    return;
  }

  scanAudioTracks();
  Serial.printf("[AUDIO] Deleted: %s\n", requested.c_str());
  String response = "{\"ok\":true,\"file\":";
  response += jsonQuoted(requested.c_str());
  response += "}";
  server.send(200, "application/json", response);
}

void handleAudioState() {
  addCORS();
  int8_t track;
  bool playing;
  getAudioState(track, playing);
  const char* path = track >= 0
    ? audioTracks[track]
    : "";
  String title = path;
  const int slash = title.lastIndexOf('/');
  if (slash >= 0) title.remove(0, slash + 1);
  const int dot = title.lastIndexOf('.');
  if (dot > 0) title.remove(dot);

  String json = "{\"sdCardReady\":";
  json += sdCardReady ? "true" : "false";
  json += ",\"track\":";
  json += jsonQuoted(path);
  json += ",\"title\":";
  json += jsonQuoted(title.c_str());
  json += ",\"playing\":";
  json += playing ? "true" : "false";
  json += ",\"trackCount\":";
  json += String(audioTrackCount);
  json += ",\"volume\":";
  json += String(audioVolumePercent);
  json += "}";
  server.send(200, "application/json", json);
}

void handleAudioVolume() {
  addCORS();
  if (!server.hasArg("value")) {
    String response = "{\"volume\":";
    response += String(audioVolumePercent);
    response += "}";
    server.send(200, "application/json", response);
    return;
  }

  const String requested = server.arg("value");
  if (requested.isEmpty()) {
    server.send(400, "application/json", "{\"error\":\"volume must be an integer from 0 to 100\"}");
    return;
  }
  for (size_t i = 0; i < requested.length(); ++i) {
    if (requested[i] < '0' || requested[i] > '9') {
      server.send(400, "application/json", "{\"error\":\"volume must be an integer from 0 to 100\"}");
      return;
    }
  }
  const long volume = requested.toInt();
  if (volume < 0 || volume > 100) {
    server.send(400, "application/json", "{\"error\":\"volume must be an integer from 0 to 100\"}");
    return;
  }
  if (!audioOutput) {
    server.send(503, "application/json", "{\"error\":\"audio output is not available\"}");
    return;
  }
  if (!queueAudioCommand(AUDIO_COMMAND_VOLUME, static_cast<int8_t>(volume))) {
    server.send(503, "application/json", "{\"error\":\"audio command queue is full\"}");
    return;
  }

  String response = "{\"ok\":true,\"queued\":true,\"volume\":";
  response += String(volume);
  response += "}";
  server.send(202, "application/json", response);
}

void handlePump() {
  if (!server.hasArg("type") || !server.hasArg("state") ||
      (server.arg("type") != "intake" && server.arg("type") != "drain") ||
      (server.arg("state") != "on" && server.arg("state") != "off")) {
    addCORS();
    server.send(400, "application/json",
                "{\"error\":\"use type=intake|drain and state=on|off\"}");
    return;
  }

  const bool enabled = server.arg("state") == "on";
  const bool intake = server.arg("type") == "intake";
  const uint8_t pin = intake ? INTAKE_PUMP_PIN : DRAIN_PUMP_PIN;
  if (intake && enabled && waterSensorDetected) {
    digitalWrite(INTAKE_PUMP_PIN, LOW);
    intakePumpOn = false;
    addCORS();
    server.send(409, "application/json",
                "{\"error\":\"water detected; intake pump is blocked\",\"waterDetected\":true,\"on\":false}");
    return;
  }

  digitalWrite(pin, enabled ? HIGH : LOW);
  if (intake) intakePumpOn = enabled;
  else drainPumpOn = enabled;

  Serial.printf("[PUMP] %s %s on GPIO %u\n",
                intake ? "INTAKE" : "DRAIN",
                enabled ? "ON" : "OFF",
                static_cast<unsigned>(pin));
  addCORS();
  char response[96];
  snprintf(response, sizeof(response),
           "{\"ok\":true,\"type\":\"%s\",\"on\":%s,\"gpio\":%u}",
           intake ? "intake" : "drain",
           enabled ? "true" : "false",
           static_cast<unsigned>(pin));
  server.send(200, "application/json", response);
}

void handleAudioPlay() {
  addCORS();
  Serial.printf("[MASTER CMD] PLAY received: %s\n",
                server.hasArg("file") ? server.arg("file").c_str() : "(missing file)");
  if (!sdCardReady) {
    Serial.println("[AUDIO] Play request rejected: SD card is not mounted");
    server.send(503, "application/json", "{\"error\":\"SD card is not mounted\"}");
    return;
  }
  if (!server.hasArg("file")) {
    Serial.println("[AUDIO] Play request rejected: missing file parameter");
    server.send(400, "application/json", "{\"error\":\"missing file\"}");
    return;
  }

  const String requested = server.arg("file");
  for (uint8_t i = 0; i < audioTrackCount; ++i) {
    if (requested == audioTracks[i]) {
      if (!queueAudioCommand(AUDIO_COMMAND_PLAY, static_cast<int8_t>(i))) {
        server.send(503, "application/json", "{\"error\":\"audio command queue is full\"}");
        return;
      }
      Serial.printf("[AUDIO] Play/resume queued %u/%u: %s\n",
                    i + 1, audioTrackCount, audioTracks[i]);
      server.send(202, "application/json", "{\"ok\":true,\"queued\":true}");
      return;
    }
  }
  Serial.println("[AUDIO] Play request rejected: track not found in playlist");
  server.send(404, "application/json", "{\"error\":\"track not found in /DIORAMA\"}");
}

void handleAudioPause() {
  addCORS();
  int8_t track;
  bool playing;
  getAudioState(track, playing);
  Serial.printf("[MASTER CMD] PAUSE received (playing=%s, track=%d/%u)\n",
                playing ? "yes" : "no",
                track >= 0 ? track + 1 : 0,
                audioTrackCount);
  if (track < 0) {
    server.send(409, "application/json", "{\"error\":\"no active audio track\"}");
    return;
  }
  if (!queueAudioCommand(AUDIO_COMMAND_PAUSE)) {
    server.send(503, "application/json", "{\"error\":\"audio command queue is full\"}");
    return;
  }
  server.send(202, "application/json", "{\"ok\":true,\"queued\":true}");
}

void handleAudioStep(int8_t direction) {
  addCORS();
  Serial.printf("[MASTER CMD] %s received\n",
                direction > 0 ? "NEXT" : "PREV");
  if (!sdCardReady || !audioTrackCount) {
    Serial.println("[AUDIO] Track-step request rejected: playlist unavailable");
    server.send(503, "application/json", "{\"error\":\"audio playlist unavailable\"}");
    return;
  }

  if (!queueAudioCommand(AUDIO_COMMAND_STEP, direction)) {
    server.send(503, "application/json", "{\"error\":\"audio command queue is full\"}");
    return;
  }
  server.send(202, "application/json", "{\"ok\":true,\"queued\":true}");
}

void handleAudioStop() {
  addCORS();
  Serial.println("[AUDIO] Stop request received");
  if (!queueAudioCommand(AUDIO_COMMAND_STOP)) {
    server.send(503, "application/json", "{\"error\":\"audio command queue is full\"}");
    return;
  }
  Serial.println("[AUDIO] Stop command queued for playback task");
  server.send(202, "application/json", "{\"ok\":true,\"queued\":true}");
}

void handleStatus() {
  addCORS();
  char buf[800];
  snprintf(buf, sizeof(buf),
    "{\"connected\":%s,\"ip\":\"%s\",\"mode\":\"%s\",\"ssid\":\"%s\","
    "\"motorRunning\":%s,\"sdCardReady\":%s,"
    "\"intakePumpOn\":%s,\"drainagePumpOn\":%s,"
    "\"waterSensorAvailable\":true,\"waterDetected\":%s,\"waterSensorAdc\":%d,"
    "\"groups\":["
    "{\"id\":1,\"r\":%d,\"g\":%d,\"b\":%d,\"brightness\":%d,\"on\":%s},"
    "{\"id\":2,\"r\":%d,\"g\":%d,\"b\":%d,\"brightness\":%d,\"on\":%s},"
    "{\"id\":3,\"r\":%d,\"g\":%d,\"b\":%d,\"brightness\":%d,\"on\":%s}"
    "]}",
    WiFi.status() == WL_CONNECTED ? "true" : "false",
    WiFi.localIP().toString().c_str(),
    lightMode.c_str(),
    WIFI_SSID,
    innerCircleMotorRunning ? "true" : "false",
    sdCardReady ? "true" : "false",
    intakePumpOn ? "true" : "false",
    drainPumpOn ? "true" : "false",
    waterSensorDetected ? "true" : "false",
    waterSensorAdc,
    groups[0].r, groups[0].g, groups[0].b, groups[0].brightness, groups[0].on ? "true" : "false",
    groups[1].r, groups[1].g, groups[1].b, groups[1].brightness, groups[1].on ? "true" : "false",
    groups[2].r, groups[2].g, groups[2].b, groups[2].brightness, groups[2].on ? "true" : "false"
  );
  server.send(200, "application/json", buf);
}

void startHttpServer() {
  if (httpStarted) return;
  server.on("/",       HTTP_GET,     []() {
    addCORS();
    server.send(200, "text/plain", "Diorama Slave RGB & Motor Controller");
  });
  server.on("/rgb",    HTTP_GET,     handleRGB);
  server.on("/off",    HTTP_GET,     handleOff);
  server.on("/motor",  HTTP_GET,     handleMotor);
  server.on("/force",  HTTP_GET,     handleForceSensor);
  server.on("/pump",   HTTP_GET,     handlePump);
  server.on("/status", HTTP_GET,     handleStatus);
  server.on("/audio/files", HTTP_GET, handleAudioFiles);
  server.on("/audio/delete", HTTP_GET, handleAudioDelete);
  server.on("/audio/state", HTTP_GET, handleAudioState);
  server.on("/audio/volume", HTTP_GET, handleAudioVolume);
  server.on("/audio/play", HTTP_GET, handleAudioPlay);
  server.on("/audio/pause", HTTP_GET, handleAudioPause);
  server.on("/audio/next", HTTP_GET, []() { handleAudioStep(1); });
  server.on("/audio/previous", HTTP_GET, []() { handleAudioStep(-1); });
  server.on("/audio/stop", HTTP_GET, handleAudioStop);
  server.on("/audio/upload", HTTP_POST, handleAudioUploadComplete, handleAudioUploadData);
  server.on("/rgb",    HTTP_OPTIONS, []() { addCORS(); server.send(204); });
  server.on("/off",    HTTP_OPTIONS, []() { addCORS(); server.send(204); });
  server.on("/motor",  HTTP_OPTIONS, []() { addCORS(); server.send(204); });
  server.on("/pump",   HTTP_OPTIONS, []() { addCORS(); server.send(204); });
  server.on("/status", HTTP_OPTIONS, []() { addCORS(); server.send(204); });
  server.on("/audio/files", HTTP_OPTIONS, []() { addCORS(); server.send(204); });
  server.on("/audio/state", HTTP_OPTIONS, []() { addCORS(); server.send(204); });
  server.on("/audio/volume", HTTP_OPTIONS, []() { addCORS(); server.send(204); });
  server.on("/audio/play", HTTP_OPTIONS, []() { addCORS(); server.send(204); });
  server.on("/audio/pause", HTTP_OPTIONS, []() { addCORS(); server.send(204); });
  server.on("/audio/next", HTTP_OPTIONS, []() { addCORS(); server.send(204); });
  server.on("/audio/previous", HTTP_OPTIONS, []() { addCORS(); server.send(204); });
  server.on("/audio/stop", HTTP_OPTIONS, []() { addCORS(); server.send(204); });
  server.on("/audio/upload", HTTP_OPTIONS, []() { addCORS(); server.send(204); });
  server.begin();
  httpStarted = true;
  Serial.printf("[HTTP] Listening on http://%s/status\n", WiFi.localIP().toString().c_str());
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // keep radio awake for SoftAP reliability
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  // Static IP so master always finds us at 192.168.4.200
  if (!WiFi.config(SLAVE_IP, GATEWAY_IP, SUBNET_MASK, DNS_IP)) {
    Serial.println("[WiFi] Static IP config FAILED");
  } else {
    Serial.println("[WiFi] Static IP 192.168.4.200 configured");
  }

  WiFi.disconnect(false, true);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WiFi] Connecting to \"%s\" ...\n", WIFI_SSID);
  lastReconnectMs = millis();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("[SLAVE] Diorama RGB & Inner Circle Motor Slave starting...");
  Serial.println("[SLAVE] Shared RGB pins: R=4, G=22, B=32");
  Serial.println("[AUDIO] PAM8403 analog inputs: GPIO25/26 (built-in DAC)");
  Serial.println("[SLAVE] Motor pins: IN1=13, IN2=14, IN3=16, IN4=17");
  Serial.println("[SLAVE] Pump driver pins: intake=27, drain=33");
  Serial.println("[SD] SPI pins: SCK=18, MOSI=23, MISO=19, CS=21");

  // Init PWM pins OFF (inverted)
  const uint8_t pins[] = {RGB_R_PIN, RGB_G_PIN, RGB_B_PIN};
  for (uint8_t p : pins) {
    pinMode(p, OUTPUT);
    analogWrite(p, 255);  // off
  }
  Serial.println("[PWM] 3 shared RGB outputs ready, all OFF");

  // Init Motor Pins OFF
  pinMode(IN1_PIN, OUTPUT);
  pinMode(IN2_PIN, OUTPUT);
  pinMode(IN3_PIN, OUTPUT);
  pinMode(IN4_PIN, OUTPUT);
  stopMotors();
  Serial.println("[MOTOR] ULN2003 stepper outputs ready, all OFF");

  pinMode(INTAKE_PUMP_PIN, OUTPUT);
  digitalWrite(INTAKE_PUMP_PIN, LOW);
  pinMode(DRAIN_PUMP_PIN, OUTPUT);
  digitalWrite(DRAIN_PUMP_PIN, LOW);
  Serial.println("[PUMP] Intake and drain outputs ready, both OFF");

  pinMode(FORCE_SENSOR_PIN, INPUT);
  pinMode(WATER_SENSOR_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(FORCE_SENSOR_PIN, ADC_11db);
  analogSetPinAttenuation(WATER_SENSOR_PIN, ADC_11db);
  Serial.println("[FORCE SENSOR] Analog input ready on GPIO36 (VP)");
  for (uint8_t sample = 0; sample < WATER_SENSOR_SAMPLE_COUNT; ++sample) {
    waterSensorAdc += analogRead(WATER_SENSOR_PIN);
  }
  waterSensorAdc /= WATER_SENSOR_SAMPLE_COUNT;
  waterSensorDetected = WATER_SENSOR_WET_WHEN_ABOVE_THRESHOLD
      ? waterSensorAdc >= WATER_SENSOR_THRESHOLD
      : waterSensorAdc <= WATER_SENSOR_THRESHOLD;
  lastWaterSensorSampleMs = millis();
  Serial.printf("[WATER SENSOR] Analog input ready on GPIO34: ADC=%d, threshold=%d, detected=%s\n",
                waterSensorAdc, WATER_SENSOR_THRESHOLD,
                waterSensorDetected ? "yes" : "no");

  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  const uint32_t sdFrequencies[] = {
    SD_SPI_FREQUENCY,
    SD_SPI_FALLBACK_FREQUENCY,
    SD_SPI_SAFE_FREQUENCY
  };
  for (uint8_t attempt = 0; attempt < sizeof(sdFrequencies) / sizeof(sdFrequencies[0]); ++attempt) {
    const uint32_t frequency = sdFrequencies[attempt];
    Serial.printf("[SD] Mount attempt %u at %lu Hz\n",
                  attempt + 1, static_cast<unsigned long>(frequency));
    sdCardReady = sd.begin(SdSpiConfig(
      SD_CS_PIN, DEDICATED_SPI, SD_SCK_HZ(frequency), &sdSPI));
    if (sdCardReady) break;

    if (sd.card()) {
      Serial.printf("[SD] Attempt failed: code=0x%02X data=0x%02X\n",
                    sd.card()->errorCode(), sd.card()->errorData());
    } else {
      Serial.println("[SD] Attempt failed: no card object returned");
    }
    delay(100);
  }
  if (sdCardReady) {
    const uint8_t filesystemType = sd.fatType();
    if (filesystemType == 64) {
      Serial.println("[SD] Card mounted; filesystem: exFAT");
    } else {
      Serial.printf("[SD] Card mounted; filesystem: FAT%u\n", filesystemType);
    }
  } else {
    Serial.println("[SD] ERROR: card mount failed");
    Serial.printf("[SD] SPI pins: SCK=%u MOSI=%u MISO=%u CS=%u; attempted %lu, %lu, and %lu Hz\n",
                  SD_SCK_PIN, SD_MOSI_PIN, SD_MISO_PIN, SD_CS_PIN,
                  static_cast<unsigned long>(SD_SPI_FREQUENCY),
                  static_cast<unsigned long>(SD_SPI_FALLBACK_FREQUENCY),
                  static_cast<unsigned long>(SD_SPI_SAFE_FREQUENCY));
    if (sd.card()) {
      Serial.printf("[SD] SdFat card error: code=0x%02X data=0x%02X\n",
                    sd.card()->errorCode(), sd.card()->errorData());
    } else {
      Serial.println("[SD] SdFat did not return a card object; check card power, seating, and SPI wiring");
    }
  }
  if (sdCardReady) {
    scanAudioTracks();
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    audioOutput = new GainAdjustedInternalDAC(AUDIO_DAC_BUFFER_BYTES);
#else
    audioOutput = new RateTrackingAudioOutputI2S(
      0, AudioOutputI2S::INTERNAL_DAC, AUDIO_I2S_DMA_BUFFER_COUNT,
      AudioOutputI2S::APLL_ENABLE);
#endif
    if (!audioOutput) {
      Serial.println("[AUDIO] ERROR: could not allocate internal DAC output");
    } else if (!audioOutput->SetRate(AUDIO_DEFAULT_SAMPLE_RATE_HZ) ||
               !audioOutput->SetChannels(AUDIO_OUTPUT_CHANNELS)) {
      Serial.println("[AUDIO] ERROR: could not configure default 44100 Hz stereo output");
      delete audioOutput;
      audioOutput = nullptr;
    } else {
      audioOutput->SetGain(AUDIO_DEFAULT_VOLUME_PERCENT / 100.0f);
      Serial.printf("[AUDIO] Internal DAC ready on GPIO25/26; default=%lu Hz stereo, digital boost %.2fx\n",
                    static_cast<unsigned long>(AUDIO_DEFAULT_SAMPLE_RATE_HZ),
                    AUDIO_DIGITAL_BOOST);
      Serial.println("[AUDIO] Decoder will set the stream rate from each file's metadata");
    }
  }

  audioCommandQueue = xQueueCreate(8, sizeof(AudioCommand));
  if (!audioCommandQueue) {
    Serial.println("[AUDIO] ERROR: could not create audio command queue");
  } else {
    audioTaskRunning = true;
    if (xTaskCreatePinnedToCore(
            audioPlaybackTask, "audioPlayback", 12288, nullptr, 1, nullptr, 1) != pdPASS) {
      audioTaskRunning = false;
      Serial.println("[AUDIO] WARNING: playback task unavailable; using main-loop fallback");
    }
  }

  connectWifi();
}

void loop() {
  const uint32_t now = millis();
  if (now - lastWaterSensorSampleMs >= WATER_SENSOR_SAMPLE_INTERVAL_MS) {
    lastWaterSensorSampleMs = now;
    uint32_t sampleTotal = 0;
    for (uint8_t sample = 0; sample < WATER_SENSOR_SAMPLE_COUNT; ++sample) {
      sampleTotal += analogRead(WATER_SENSOR_PIN);
    }
    waterSensorAdc = sampleTotal / WATER_SENSOR_SAMPLE_COUNT;

    const bool detected = WATER_SENSOR_WET_WHEN_ABOVE_THRESHOLD
        ? waterSensorAdc >= WATER_SENSOR_THRESHOLD
        : waterSensorAdc <= WATER_SENSOR_THRESHOLD;
    if (detected != waterSensorDetected) {
      waterSensorDetected = detected;
      Serial.printf("[WATER SENSOR] ADC=%d water=%s\n",
                    waterSensorAdc, waterSensorDetected ? "detected" : "clear");
      if (waterSensorDetected && intakePumpOn) {
        digitalWrite(INTAKE_PUMP_PIN, LOW);
        intakePumpOn = false;
        Serial.println("[PUMP] Intake stopped: water detected");
      }
    }
  }

  if (httpStarted) server.handleClient();
  if (!audioTaskRunning && audioCommandQueue) {
    serviceAudioCommands();
    serviceAudioPlayback();
  }

  const bool connected = (WiFi.status() == WL_CONNECTED);
  // ── Inner circle sound-reactive rotation ─────────────────────────
  if (!manualMotorRunning && lightMode == "Sound" &&
      lastSoundDetectedMs != 0 && now - lastSoundDetectedMs < SOUND_MOTOR_HOLD_MS) {
    if (!innerCircleMotorRunning) {
      runInnerCircle(true);
      Serial.println("[MOTOR] Sound detected -> Inner Circle rotating");
    }
  } else if (!manualMotorRunning && innerCircleMotorRunning) {
    if (lightMode != "Sound" || lastSoundDetectedMs == 0 ||
        now - lastSoundDetectedMs >= SOUND_MOTOR_HOLD_MS) {
      stopMotors();
      Serial.println("[MOTOR] Sound paused -> Inner Circle stopped");
    }
  }
  serviceInnerCircleMotor(now);

  if (connected && !wasConnected) {
    Serial.printf("[WiFi] Connected  IP=%s  gateway=%s  RSSI=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.RSSI());

    if (WiFi.localIP() != SLAVE_IP) {
      Serial.printf("[WiFi] WARNING: got %s, expected 192.168.4.200 — master may not find me\n",
                    WiFi.localIP().toString().c_str());
    }

    startHttpServer();
    pushAllGroups();
    stopMotors();
    Serial.println("[SLAVE] Ready — waiting for master /rgb commands");
  }

  if (!connected) {
    if (wasConnected) {
      Serial.println("[WiFi] Lost connection to Diorama-Park");
      stopMotors();
    }
    // Reconnect every 5s (not disconnect/begin every loop)
    if (now - lastReconnectMs >= 5000) {
      Serial.println("[WiFi] Retry connect...");
      WiFi.disconnect(false, false);
      delay(50);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      lastReconnectMs = now;
    }
  } else if (now - lastStatusMs >= 15000) {
    lastStatusMs = now;
    Serial.printf("[SLAVE] OK ip=%s mode=%s motor=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(),
                  lightMode.c_str(),
                  innerCircleMotorRunning ? "ON" : "OFF",
                  WiFi.RSSI());
  }

  wasConnected = connected;
}
