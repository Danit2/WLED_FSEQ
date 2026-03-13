#include "fseq_player.h"
#include "usermod_fseq.h"
#include "wled.h"
#include <Arduino.h>

#ifdef WLED_USE_SD_SPI
#include <SD.h>
#include <SPI.h>
#elif defined(WLED_USE_SD_MMC)
#include "SD_MMC.h"
#endif

// Static member definitions
const char UsermodFseq::_name[] PROGMEM = "FSEQ";
uint8_t UsermodFseq::fseqEffectId = 0;

File FSEQPlayer::recordingFile;
String FSEQPlayer::currentFileName = "";
float FSEQPlayer::secondsElapsed = 0;

uint8_t FSEQPlayer::colorChannels = 3;
int32_t FSEQPlayer::recordingRepeats = RECORDING_REPEAT_DEFAULT;
uint32_t FSEQPlayer::now = 0;
uint32_t FSEQPlayer::next_time = 0;
uint32_t FSEQPlayer::frame = 0;
uint16_t FSEQPlayer::buffer_size = 48;
FSEQPlayer::FileHeader FSEQPlayer::file_header;

namespace {
constexpr uint16_t FSEQ_MAX_INDEXED_FILES = 128;
constexpr uint32_t FSEQ_FPP_OVERRIDE_TIMEOUT_MS = 3000;
String gIndexedFseqFiles[FSEQ_MAX_INDEXED_FILES];
uint16_t gIndexedFseqFileCount = 0;
uint32_t gFppLastControlMs = 0;
bool gFppOverrideActive = false;

bool isFseqFileName(const String &name) {
  return name.endsWith(".fseq") || name.endsWith(".FSEQ");
}

int compareFseqName(const String &a, const String &b) {
  String al = a;
  String bl = b;
  al.toLowerCase();
  bl.toLowerCase();
  return al.compareTo(bl);
}

void insertSortedFseqName(const String &name) {
  if (gIndexedFseqFileCount >= FSEQ_MAX_INDEXED_FILES) return;

  uint16_t insertPos = gIndexedFseqFileCount;
  for (uint16_t i = 0; i < gIndexedFseqFileCount; i++) {
    if (compareFseqName(name, gIndexedFseqFiles[i]) < 0) {
      insertPos = i;
      break;
    }
  }

  for (uint16_t i = gIndexedFseqFileCount; i > insertPos; i--) {
    gIndexedFseqFiles[i] = gIndexedFseqFiles[i - 1];
  }

  gIndexedFseqFiles[insertPos] = name;
  gIndexedFseqFileCount++;
}
}  // namespace

uint16_t FSEQ_refreshFileIndexCache() {
  gIndexedFseqFileCount = 0;

  File root = SD_ADAPTER.open("/");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return 0;
  }

  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String name = file.name();
      if (name.length() > 0 && !name.startsWith("/")) name = "/" + name;
      if (isFseqFileName(name)) insertSortedFseqName(name);
    }

    file.close();
    file = root.openNextFile();
  }

  root.close();
  return gIndexedFseqFileCount;
}

uint16_t FSEQ_getFileIndexCount() {
  return gIndexedFseqFileCount;
}

bool FSEQ_getFileNameByIndex(uint16_t index, String &outName) {
  if (index >= gIndexedFseqFileCount) {
    outName = "";
    return false;
  }
  outName = gIndexedFseqFiles[index];
  return true;
}

int16_t FSEQ_findFileIndexByName(const String &name) {
  String normalized = name;
  if (!normalized.startsWith("/")) normalized = "/" + normalized;

  if (gIndexedFseqFileCount == 0) {
    FSEQ_refreshFileIndexCache();
  }

  for (uint16_t i = 0; i < gIndexedFseqFileCount; i++) {
    if (gIndexedFseqFiles[i].equalsIgnoreCase(normalized)) {
      return (int16_t)i;
    }
  }

  return -1;
}

void FSEQ_markFppControlActivity() {
  gFppLastControlMs = millis();
  gFppOverrideActive = true;
}

void FSEQ_clearFppOverride() {
  gFppOverrideActive = false;
}

bool FSEQ_isFppOverrideActive() {
  if (gFppOverrideActive && (millis() - gFppLastControlMs > FSEQ_FPP_OVERRIDE_TIMEOUT_MS)) {
    gFppOverrideActive = false;
  }
  return gFppOverrideActive;
}

inline uint32_t FSEQPlayer::readUInt32() {
  uint8_t buffer[4];
  if (recordingFile.read(buffer, 4) != 4) return 0;
  return (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
         ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
}

inline uint32_t FSEQPlayer::readUInt24() {
  uint8_t buffer[3];
  if (recordingFile.read(buffer, 3) != 3) return 0;
  return (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
         ((uint32_t)buffer[2] << 16);
}

inline uint16_t FSEQPlayer::readUInt16() {
  uint8_t buffer[2];
  if (recordingFile.read(buffer, 2) != 2) return 0;
  return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

inline uint8_t FSEQPlayer::readUInt8() {
  int c = recordingFile.read();
  return (c < 0) ? 0 : (uint8_t)c;
}

bool FSEQPlayer::fileOnSD(const char *filepath) {
  uint8_t cardType = SD_ADAPTER.cardType();
  if (cardType == CARD_NONE) return false;
  return SD_ADAPTER.exists(filepath);
}

bool FSEQPlayer::fileOnFS(const char *filepath) { return false; }

void FSEQPlayer::printHeaderInfo() {
  DEBUG_PRINTLN("FSEQ file header:");
  DEBUG_PRINTF(" channel_data_offset = %d\n", file_header.channel_data_offset);
  DEBUG_PRINTF(" minor_version       = %d\n", file_header.minor_version);
  DEBUG_PRINTF(" major_version       = %d\n", file_header.major_version);
  DEBUG_PRINTF(" header_length       = %d\n", file_header.header_length);
  DEBUG_PRINTF(" channel_count       = %lu\n", (unsigned long)file_header.channel_count);
  DEBUG_PRINTF(" frame_count         = %lu\n", (unsigned long)file_header.frame_count);
  DEBUG_PRINTF(" step_time           = %d\n", file_header.step_time);
  DEBUG_PRINTF(" flags               = %d\n", file_header.flags);
}

void FSEQPlayer::processFrameData() {
  uint32_t packetLength = file_header.channel_count;
  uint16_t segLen = SEGLEN;
  uint16_t maxLeds = min((uint32_t)segLen, packetLength / 3);
  char frame_data[48];
  CRGB *crgb = reinterpret_cast<CRGB *>(frame_data);
  uint32_t bytes_remaining = packetLength;
  uint16_t index = 0;

  while (index < maxLeds && bytes_remaining > 0) {
    uint16_t length = (uint16_t)min(bytes_remaining, (uint32_t)sizeof(frame_data));
    size_t bytesRead = recordingFile.readBytes(frame_data, length);
    if (bytesRead == 0) break;
    bytes_remaining -= bytesRead;

    for (uint16_t offset = 0; offset < bytesRead / 3; offset++) {
      SEGMENT.setPixelColor(index, RGBW32(crgb[offset].r, crgb[offset].g, crgb[offset].b, 0));
      if (++index >= maxLeds) break;
    }
  }

  for (uint16_t i = index; i < segLen; i++) {
    SEGMENT.setPixelColor(i, BLACK);
  }

  next_time = now + file_header.step_time;
}

bool FSEQPlayer::stopBecauseAtTheEnd() {
  if (frame >= file_header.frame_count) {
    if (recordingRepeats == RECORDING_REPEAT_LOOP) {
      frame = 0;
      recordingFile.seek(file_header.channel_data_offset);
      return false;
    }

    if (recordingRepeats > 0) {
      recordingRepeats--;
      frame = 0;
      recordingFile.seek(file_header.channel_data_offset);
      DEBUG_PRINTF("Repeat recording again for: %d\n", recordingRepeats);
      return false;
    }

    DEBUG_PRINTLN("Finished playing recording");
    clearLastPlayback();
    return true;
  }

  return false;
}

void FSEQPlayer::playNextRecordingFrame() {
  if (stopBecauseAtTheEnd()) return;

  uint32_t offset = file_header.channel_data_offset + (file_header.channel_count * frame++);
  if (!recordingFile.seek(offset) && recordingFile.position() != offset) {
    DEBUG_PRINTLN("Failed to seek to proper offset for channel data!");
    clearLastPlayback();
    return;
  }

  processFrameData();
}

void FSEQPlayer::renderFrameToSegment() {
  now = millis();
  if (!isPlaying()) return;
  if (now < next_time) return;
  playNextRecordingFrame();
}

void FSEQPlayer::loadRecording(const char *filepath, float secondsElapsed, bool loop) {
  clearLastPlayback();

  DEBUG_PRINTF("FSEQ load animation: %s\n", filepath);
  if (fileOnSD(filepath)) {
    DEBUG_PRINTF("Read file from SD: %s\n", filepath);
    recordingFile = SD_ADAPTER.open(filepath, "rb");
  } else if (fileOnFS(filepath)) {
    DEBUG_PRINTF("Read file from FS: %s\n", filepath);
    recordingFile = WLED_FS.open(filepath, "rb");
  } else {
    DEBUG_PRINTF("File %s not found on SD or FS\n", filepath);
    return;
  }

  if (!recordingFile) {
    DEBUG_PRINTF("Failed to open %s\n", filepath);
    return;
  }

  currentFileName = String(filepath);
  if (currentFileName.startsWith("/")) currentFileName = currentFileName.substring(1);

  if ((uint64_t)recordingFile.available() < sizeof(file_header)) {
    DEBUG_PRINTF("Invalid file size: %d\n", recordingFile.available());
    clearLastPlayback();
    return;
  }

  for (int i = 0; i < 4; i++) file_header.identifier[i] = readUInt8();
  file_header.channel_data_offset = readUInt16();
  file_header.minor_version = readUInt8();
  file_header.major_version = readUInt8();
  file_header.header_length = readUInt16();
  file_header.channel_count = readUInt32();
  file_header.frame_count = readUInt32();
  file_header.step_time = readUInt8();
  file_header.flags = readUInt8();
  printHeaderInfo();

  if (file_header.identifier[0] != 'P' || file_header.identifier[1] != 'S' ||
      file_header.identifier[2] != 'E' || file_header.identifier[3] != 'Q') {
    DEBUG_PRINTF("Error reading FSEQ file %s header, invalid identifier\n", filepath);
    clearLastPlayback();
    return;
  }

  if (file_header.frame_count == 0 || file_header.channel_count == 0) {
    DEBUG_PRINTF("Error reading FSEQ file %s header, empty data\n", filepath);
    clearLastPlayback();
    return;
  }

  if (((uint64_t)file_header.channel_count * (uint64_t)file_header.frame_count) +
          file_header.header_length >
      UINT32_MAX) {
    DEBUG_PRINTF("Error reading FSEQ file %s header, file too long (max 4gb)\n", filepath);
    clearLastPlayback();
    return;
  }

  if (file_header.step_time < 1) {
    DEBUG_PRINTF("Invalid step time %d, using default %d instead\n",
                 file_header.step_time, FSEQ_DEFAULT_STEP_TIME);
    file_header.step_time = FSEQ_DEFAULT_STEP_TIME;
  }

  recordingRepeats = loop ? RECORDING_REPEAT_LOOP : RECORDING_REPEAT_DEFAULT;
  frame = (uint32_t)((secondsElapsed * 1000.0f) / file_header.step_time);
  if (frame >= file_header.frame_count) frame = file_header.frame_count - 1;
  next_time = 0;

  playNextRecordingFrame();
}

void FSEQPlayer::clearLastPlayback() {
  frame = 0;
  next_time = 0;
  secondsElapsed = 0;
  if (recordingFile) recordingFile.close();
  currentFileName = "";
}

bool FSEQPlayer::isPlaying() {
  return recordingFile && file_header.frame_count > 0 && frame < file_header.frame_count;
}

void FSEQPlayer::setLooping(bool loop) {
  recordingRepeats = loop ? RECORDING_REPEAT_LOOP : RECORDING_REPEAT_DEFAULT;
}

String FSEQPlayer::getFileName() { return currentFileName; }

float FSEQPlayer::getElapsedSeconds() {
  if (!isPlaying()) return 0;
  return (float)frame * (float)file_header.step_time / 1000.0f;
}

void FSEQPlayer::syncPlayback(float secondsElapsed) {
  if (!isPlaying()) {
    DEBUG_PRINTLN("[FSEQ] Sync: Playback not active, cannot sync.");
    return;
  }

  uint32_t expectedFrame = (uint32_t)((secondsElapsed * 1000.0f) / file_header.step_time);
  if (expectedFrame >= file_header.frame_count) expectedFrame = file_header.frame_count - 1;

  int32_t diff = (int32_t)expectedFrame - (int32_t)frame;

  if (abs(diff) > 30) {
    frame = expectedFrame;

    uint32_t offset = file_header.channel_data_offset +
                      (uint32_t)file_header.channel_count * frame;

    if (recordingFile.seek(offset)) {
      DEBUG_PRINTF("[FSEQ] HARD Sync -> frame=%lu (diff=%ld)\n",
                   (unsigned long)expectedFrame, (long)diff);
      next_time = millis() + file_header.step_time;
    } else {
      DEBUG_PRINTLN("[FSEQ] HARD Sync failed to seek");
    }

    return;
  }

  if (abs(diff) > 1) {
    float correctionFactor = 0.05f * abs(diff);
    correctionFactor = constrain(correctionFactor, 0.05f, 0.4f);

    int32_t timeAdjustment = (int32_t)(diff * file_header.step_time * correctionFactor);
    next_time -= timeAdjustment;

    DEBUG_PRINTF("[FSEQ] Soft Sync diff=%ld factor=%.3f adjust=%ldus\n",
                 (long)diff, correctionFactor, (long)timeAdjustment);
  } else {
    DEBUG_PRINTF("[FSEQ] Sync OK (current=%lu expected=%lu)\n",
                 (unsigned long)frame, (unsigned long)expectedFrame);
  }
}
