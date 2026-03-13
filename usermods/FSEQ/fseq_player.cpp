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

FSEQPlayer::PlaybackState FSEQPlayer::realtimeState;
FSEQPlayer::PlaybackState FSEQPlayer::segmentStates[MAX_NUM_SEGMENTS];

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

uint16_t FSEQ_getFileIndexCount() { return gIndexedFseqFileCount; }

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

void FSEQ_clearFppOverride() { gFppOverrideActive = false; }

bool FSEQ_isFppOverrideActive() {
  if (gFppOverrideActive &&
      (millis() - gFppLastControlMs > FSEQ_FPP_OVERRIDE_TIMEOUT_MS)) {
    gFppOverrideActive = false;
  }
  return gFppOverrideActive;
}

inline uint32_t FSEQPlayer::readUInt32(File &file) {
  uint8_t buffer[4];
  if (file.read(buffer, 4) != 4) return 0;
  return (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
         ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
}

inline uint32_t FSEQPlayer::readUInt24(File &file) {
  uint8_t buffer[3];
  if (file.read(buffer, 3) != 3) return 0;
  return (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
         ((uint32_t)buffer[2] << 16);
}

inline uint16_t FSEQPlayer::readUInt16(File &file) {
  uint8_t buffer[2];
  if (file.read(buffer, 2) != 2) return 0;
  return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

inline uint8_t FSEQPlayer::readUInt8(File &file) {
  int c = file.read();
  return (c < 0) ? 0 : (uint8_t)c;
}

bool FSEQPlayer::fileOnSD(const char *filepath) {
  uint8_t cardType = SD_ADAPTER.cardType();
  if (cardType == CARD_NONE) return false;
  return SD_ADAPTER.exists(filepath);
}

bool FSEQPlayer::fileOnFS(const char *filepath) { return false; }

void FSEQPlayer::printHeaderInfo(const PlaybackState &state) {
  DEBUG_PRINTLN("FSEQ file header:");
  DEBUG_PRINTF(" channel_data_offset = %d\n", state.file_header.channel_data_offset);
  DEBUG_PRINTF(" minor_version       = %d\n", state.file_header.minor_version);
  DEBUG_PRINTF(" major_version       = %d\n", state.file_header.major_version);
  DEBUG_PRINTF(" header_length       = %d\n", state.file_header.header_length);
  DEBUG_PRINTF(" channel_count       = %lu\n", (unsigned long)state.file_header.channel_count);
  DEBUG_PRINTF(" frame_count         = %lu\n", (unsigned long)state.file_header.frame_count);
  DEBUG_PRINTF(" step_time           = %d\n", state.file_header.step_time);
  DEBUG_PRINTF(" flags               = %d\n", state.file_header.flags);
}

void FSEQPlayer::processFrameDataForSegment(PlaybackState &state, Segment &segment) {
  uint32_t packetLength = state.file_header.channel_count;
  uint16_t segLen = segment.length();
  uint16_t maxLeds = min((uint32_t)segLen, packetLength / 3U);
  uint8_t frame_data[128];
  uint32_t bytes_remaining = packetLength;
  uint16_t index = 0;

  while (index < maxLeds && bytes_remaining > 0) {
    uint16_t length = (uint16_t)min(bytes_remaining, (uint32_t)sizeof(frame_data));
    size_t bytesRead = state.recordingFile.read(frame_data, length);
    if (bytesRead == 0) break;
    bytes_remaining -= bytesRead;

    for (uint16_t offset = 0; offset + 2 < bytesRead; offset += 3) {
      segment.setPixelColor(index,
          RGBW32(frame_data[offset], frame_data[offset + 1], frame_data[offset + 2], 0));
      if (++index >= maxLeds) break;
    }
  }

  for (uint16_t i = index; i < segLen; i++) {
    segment.setPixelColor(i, BLACK);
  }

  state.next_time = state.now + state.file_header.step_time;
}

void FSEQPlayer::processFrameDataRealtime(PlaybackState &state) {
  uint32_t packetLength = state.file_header.channel_count;
  uint16_t totalLen = strip.getLengthTotal();
  uint16_t maxLeds = min((uint32_t)totalLen, packetLength / 3U);
  uint8_t frame_data[128];
  uint32_t bytes_remaining = packetLength;
  uint16_t index = 0;

  while (index < maxLeds && bytes_remaining > 0) {
    uint16_t length = (uint16_t)min(bytes_remaining, (uint32_t)sizeof(frame_data));
    size_t bytesRead = state.recordingFile.read(frame_data, length);
    if (bytesRead == 0) break;
    bytes_remaining -= bytesRead;

    for (uint16_t offset = 0; offset + 2 < bytesRead; offset += 3) {
      setRealtimePixel(index, frame_data[offset], frame_data[offset + 1], frame_data[offset + 2], 0);
      if (++index >= maxLeds) break;
    }
  }

  for (uint16_t i = index; i < totalLen; i++) {
    setRealtimePixel(i, 0, 0, 0, 0);
  }

  state.next_time = state.now + state.file_header.step_time;
}

bool FSEQPlayer::stopBecauseAtTheEnd(PlaybackState &state) {
  if (state.frame >= state.file_header.frame_count) {
    if (state.recordingRepeats == RECORDING_REPEAT_LOOP) {
      state.frame = 0;
      state.recordingFile.seek(state.file_header.channel_data_offset);
      return false;
    }

    if (state.recordingRepeats > 0) {
      state.recordingRepeats--;
      state.frame = 0;
      state.recordingFile.seek(state.file_header.channel_data_offset);
      DEBUG_PRINTF("Repeat recording again for: %d\n", state.recordingRepeats);
      return false;
    }

    DEBUG_PRINTLN("Finished playing recording");
    clearPlaybackState(state);
    return true;
  }

  return false;
}

void FSEQPlayer::playNextRecordingFrameForSegment(PlaybackState &state, Segment &segment) {
  if (stopBecauseAtTheEnd(state)) return;

  uint32_t offset = state.file_header.channel_data_offset +
                    (state.file_header.channel_count * state.frame++);
  if (!state.recordingFile.seek(offset) && state.recordingFile.position() != offset) {
    DEBUG_PRINTLN("Failed to seek to proper offset for channel data!");
    clearPlaybackState(state);
    return;
  }

  processFrameDataForSegment(state, segment);
}

void FSEQPlayer::playNextRealtimeFrame(PlaybackState &state) {
  if (stopBecauseAtTheEnd(state)) return;

  uint32_t offset = state.file_header.channel_data_offset +
                    (state.file_header.channel_count * state.frame++);
  if (!state.recordingFile.seek(offset) && state.recordingFile.position() != offset) {
    DEBUG_PRINTLN("Failed to seek to proper offset for channel data!");
    clearPlaybackState(state);
    return;
  }

  processFrameDataRealtime(state);
}

void FSEQPlayer::loadRecordingIntoState(PlaybackState &state, const char *filepath,
                                        float secondsElapsed, bool loop) {
  clearPlaybackState(state);

  DEBUG_PRINTF("FSEQ load animation: %s\n", filepath);
  if (fileOnSD(filepath)) {
    DEBUG_PRINTF("Read file from SD: %s\n", filepath);
    state.recordingFile = SD_ADAPTER.open(filepath, "rb");
  } else if (fileOnFS(filepath)) {
    DEBUG_PRINTF("Read file from FS: %s\n", filepath);
    state.recordingFile = WLED_FS.open(filepath, "rb");
  } else {
    DEBUG_PRINTF("File %s not found on SD or FS\n", filepath);
    return;
  }

  if (!state.recordingFile) {
    DEBUG_PRINTF("Failed to open %s\n", filepath);
    return;
  }

  state.currentFileName = String(filepath);
  if (state.currentFileName.startsWith("/")) state.currentFileName = state.currentFileName.substring(1);

  if ((uint64_t)state.recordingFile.available() < sizeof(state.file_header)) {
    DEBUG_PRINTF("Invalid file size: %d\n", state.recordingFile.available());
    clearPlaybackState(state);
    return;
  }

  for (int i = 0; i < 4; i++) state.file_header.identifier[i] = readUInt8(state.recordingFile);
  state.file_header.channel_data_offset = readUInt16(state.recordingFile);
  state.file_header.minor_version = readUInt8(state.recordingFile);
  state.file_header.major_version = readUInt8(state.recordingFile);
  state.file_header.header_length = readUInt16(state.recordingFile);
  state.file_header.channel_count = readUInt32(state.recordingFile);
  state.file_header.frame_count = readUInt32(state.recordingFile);
  state.file_header.step_time = readUInt8(state.recordingFile);
  state.file_header.flags = readUInt8(state.recordingFile);
  printHeaderInfo(state);

  if (state.file_header.identifier[0] != 'P' || state.file_header.identifier[1] != 'S' ||
      state.file_header.identifier[2] != 'E' || state.file_header.identifier[3] != 'Q') {
    DEBUG_PRINTF("Error reading FSEQ file %s header, invalid identifier\n", filepath);
    clearPlaybackState(state);
    return;
  }

  if (state.file_header.frame_count == 0 || state.file_header.channel_count == 0) {
    DEBUG_PRINTF("Error reading FSEQ file %s header, empty data\n", filepath);
    clearPlaybackState(state);
    return;
  }

  if (((uint64_t)state.file_header.channel_count * (uint64_t)state.file_header.frame_count) +
          state.file_header.header_length >
      UINT32_MAX) {
    DEBUG_PRINTF("Error reading FSEQ file %s header, file too long (max 4gb)\n", filepath);
    clearPlaybackState(state);
    return;
  }

  if (state.file_header.step_time < 1) {
    DEBUG_PRINTF("Invalid step time %d, using default %d instead\n",
                 state.file_header.step_time, FSEQ_DEFAULT_STEP_TIME);
    state.file_header.step_time = FSEQ_DEFAULT_STEP_TIME;
  }

  state.recordingRepeats = loop ? RECORDING_REPEAT_LOOP : RECORDING_REPEAT_DEFAULT;
  state.frame = (uint32_t)((secondsElapsed * 1000.0f) / state.file_header.step_time);
  if (state.frame >= state.file_header.frame_count) state.frame = state.file_header.frame_count - 1;
  state.next_time = 0;
  state.secondsElapsed = secondsElapsed;
}

void FSEQPlayer::clearPlaybackState(PlaybackState &state) {
  state.frame = 0;
  state.next_time = 0;
  state.now = 0;
  state.secondsElapsed = 0;
  state.recordingRepeats = RECORDING_REPEAT_DEFAULT;
  state.file_header.frame_count = 0;
  state.file_header.channel_count = 0;
  state.file_header.step_time = 0;
  if (state.recordingFile) state.recordingFile.close();
  state.currentFileName = "";
}

bool FSEQPlayer::isStatePlaying(const PlaybackState &state) {
  return state.recordingFile && state.file_header.frame_count > 0 &&
         state.frame <= state.file_header.frame_count;
}

void FSEQPlayer::setStateLooping(PlaybackState &state, bool loop) {
  state.recordingRepeats = loop ? RECORDING_REPEAT_LOOP : RECORDING_REPEAT_DEFAULT;
}

float FSEQPlayer::getElapsedSeconds(const PlaybackState &state) {
  if (!isStatePlaying(state)) return 0;
  return (float)state.frame * (float)state.file_header.step_time / 1000.0f;
}

void FSEQPlayer::loadRecordingForSegment(uint8_t segmentId, const char *filepath,
                                         float secondsElapsed, bool loop) {
  if (segmentId >= MAX_NUM_SEGMENTS) return;
  loadRecordingIntoState(segmentStates[segmentId], filepath, secondsElapsed, loop);
}

void FSEQPlayer::clearSegmentPlayback(uint8_t segmentId) {
  if (segmentId >= MAX_NUM_SEGMENTS) return;
  clearPlaybackState(segmentStates[segmentId]);
}

void FSEQPlayer::setSegmentLooping(uint8_t segmentId, bool loop) {
  if (segmentId >= MAX_NUM_SEGMENTS) return;
  setStateLooping(segmentStates[segmentId], loop);
}

bool FSEQPlayer::isSegmentPlaying(uint8_t segmentId) {
  if (segmentId >= MAX_NUM_SEGMENTS) return false;
  return isStatePlaying(segmentStates[segmentId]);
}

void FSEQPlayer::renderSegmentFrame(uint8_t segmentId, Segment &segment) {
  if (segmentId >= MAX_NUM_SEGMENTS) return;
  PlaybackState &state = segmentStates[segmentId];
  state.now = millis();
  if (!isStatePlaying(state)) return;
  if (state.now < state.next_time) return;
  playNextRecordingFrameForSegment(state, segment);
}

void FSEQPlayer::loadRecording(const char *filepath, float secondsElapsed, bool loop) {
  loadRecordingIntoState(realtimeState, filepath, secondsElapsed, loop);
}

void FSEQPlayer::clearLastPlayback() { clearPlaybackState(realtimeState); }

bool FSEQPlayer::isPlaying() { return isStatePlaying(realtimeState); }

void FSEQPlayer::setLooping(bool loop) { setStateLooping(realtimeState, loop); }

String FSEQPlayer::getFileName() { return realtimeState.currentFileName; }

float FSEQPlayer::getElapsedSeconds() { return getElapsedSeconds(realtimeState); }

void FSEQPlayer::renderRealtimeFrame() {
  realtimeState.now = millis();
  if (!isStatePlaying(realtimeState)) return;
  if (realtimeState.now < realtimeState.next_time) return;
  playNextRealtimeFrame(realtimeState);
  if (!useMainSegmentOnly) strip.show();
  else strip.trigger();
}

void FSEQPlayer::syncPlayback(float secondsElapsed) {
  PlaybackState &state = realtimeState;
  if (!isStatePlaying(state)) {
    DEBUG_PRINTLN("[FSEQ] Sync: Playback not active, cannot sync.");
    return;
  }

  uint32_t expectedFrame = (uint32_t)((secondsElapsed * 1000.0f) / state.file_header.step_time);
  if (expectedFrame >= state.file_header.frame_count) expectedFrame = state.file_header.frame_count - 1;

  int32_t diff = (int32_t)expectedFrame - (int32_t)state.frame;

  if (abs(diff) > 30) {
    state.frame = expectedFrame;

    uint32_t offset = state.file_header.channel_data_offset +
                      (uint32_t)state.file_header.channel_count * state.frame;

    if (state.recordingFile.seek(offset)) {
      DEBUG_PRINTF("[FSEQ] HARD Sync -> frame=%lu (diff=%ld)\n",
                   (unsigned long)expectedFrame, (long)diff);
      state.next_time = millis() + state.file_header.step_time;
    } else {
      DEBUG_PRINTLN("[FSEQ] HARD Sync failed to seek");
    }

    return;
  }

  if (abs(diff) > 1) {
    float correctionFactor = 0.05f * abs(diff);
    correctionFactor = constrain(correctionFactor, 0.05f, 0.4f);

    int32_t timeAdjustment = (int32_t)(diff * state.file_header.step_time * correctionFactor);
    state.next_time -= timeAdjustment;

    DEBUG_PRINTF("[FSEQ] Soft Sync diff=%ld factor=%.3f adjust=%ldus\n",
                 (long)diff, correctionFactor, (long)timeAdjustment);
  } else {
    DEBUG_PRINTF("[FSEQ] Sync OK (current=%lu expected=%lu)\n",
                 (unsigned long)state.frame, (unsigned long)expectedFrame);
  }
}
