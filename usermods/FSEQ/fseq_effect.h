#pragma once

#include "wled.h"
#include "fseq_player.h"

uint16_t FSEQ_refreshFileIndexCache();
bool FSEQ_getFileNameByIndex(uint16_t index, String &outName);
bool FSEQ_isFppOverrideActive();

static uint16_t _fseq_lastIndex[MAX_NUM_SEGMENTS];
static bool _fseq_lastLoop[MAX_NUM_SEGMENTS];
static uint16_t _fseq_lastFileCount[MAX_NUM_SEGMENTS];
static bool _fseq_stateInit = false;

static void mode_fseq_player(void) {
  if (!_fseq_stateInit) {
    for (uint8_t i = 0; i < MAX_NUM_SEGMENTS; i++) {
      _fseq_lastIndex[i] = 0xFFFF;
      _fseq_lastLoop[i] = false;
      _fseq_lastFileCount[i] = 0xFFFF;
    }
    _fseq_stateInit = true;
  }

  // While FPP is active, local segmented playback must stay out of the way.
  if (FSEQ_isFppOverrideActive()) {
    SEGMENT.fill(BLACK);
    return;
  }

  const uint8_t segId = strip.getCurrSegmentId();
  const uint16_t fileCount = FSEQ_refreshFileIndexCache();
  const uint16_t selectedIndex = SEGMENT.custom1;
  const bool loop = SEGMENT.check1;

  if (fileCount == 0 || selectedIndex >= fileCount) {
    if (FSEQPlayer::isSegmentPlaying(segId)) {
      FSEQPlayer::clearSegmentPlayback(segId);
    }
    SEGMENT.fill(BLACK);
    _fseq_lastIndex[segId] = selectedIndex;
    _fseq_lastLoop[segId] = loop;
    _fseq_lastFileCount[segId] = fileCount;
    return;
  }

  const bool selectionChanged =
      (_fseq_lastIndex[segId] != selectedIndex) ||
      (_fseq_lastLoop[segId] != loop) ||
      (_fseq_lastFileCount[segId] != fileCount);

  if (selectionChanged) {
    String fileName;
    if (FSEQ_getFileNameByIndex(selectedIndex, fileName) && fileName.length() > 0) {
      char path[256];
      fileName.toCharArray(path, sizeof(path));
      FSEQPlayer::loadRecordingForSegment(segId, path, 0.0f, loop);
    }

    _fseq_lastIndex[segId] = selectedIndex;
    _fseq_lastLoop[segId] = loop;
    _fseq_lastFileCount[segId] = fileCount;
  }

  FSEQPlayer::setSegmentLooping(segId, loop);

  if (!FSEQPlayer::isSegmentPlaying(segId)) {
    SEGMENT.fill(BLACK);
    return;
  }

  FSEQPlayer::renderSegmentFrame(segId, SEGMENT);
}

static const char _data_FX_MODE_FSEQ_PLAYER[] PROGMEM =
    "FSEQ Player@,,Index,,,Loop;;;c1=0,o1=1";