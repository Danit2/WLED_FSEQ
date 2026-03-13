#pragma once

#include "wled.h"
#include "fseq_player.h"

// Helper functions implemented in fseq_player.cpp
uint16_t FSEQ_refreshFileIndexCache();
bool FSEQ_getFileNameByIndex(uint16_t index, String &outName);
bool FSEQ_isFppOverrideActive();

// Global playback is intentionally single-instance. These values are used to
// detect when the locally selected file/loop setting changed.
static uint16_t _fseq_lastIndex = 0xFFFF;
static bool _fseq_lastLoop = false;
static uint16_t _fseq_lastFileCount = 0xFFFF;

static void mode_fseq_player(void) {
  // FPP has priority while its override is active. Keep rendering the
  // currently loaded sequence, but do not let the local effect selection take
  // over until the override expires or is cleared.
  if (FSEQ_isFppOverrideActive()) {
    if (FSEQPlayer::isPlaying()) {
      FSEQPlayer::renderFrameToSegment();
    } else {
      SEGMENT.fill(SEGCOLOR(0));
    }
    return;
  }

  const uint16_t fileCount = FSEQ_refreshFileIndexCache();
  const uint16_t selectedIndex = SEGMENT.custom1;
  const bool loop = SEGMENT.check2;

  if (fileCount == 0 || selectedIndex >= fileCount) {
    if (FSEQPlayer::isPlaying()) FSEQPlayer::clearLastPlayback();
    SEGMENT.fill(SEGCOLOR(0));
    _fseq_lastIndex = selectedIndex;
    _fseq_lastLoop = loop;
    _fseq_lastFileCount = fileCount;
    return;
  }

  const bool selectionChanged =
      (_fseq_lastIndex != selectedIndex) ||
      (_fseq_lastLoop != loop) ||
      (_fseq_lastFileCount != fileCount);

  if (selectionChanged || !FSEQPlayer::isPlaying()) {
    String fileName;
    if (FSEQ_getFileNameByIndex(selectedIndex, fileName) && fileName.length() > 0) {
      char path[256];
      fileName.toCharArray(path, sizeof(path));
      FSEQPlayer::loadRecording(path, 0.0f, loop);
    }

    _fseq_lastIndex = selectedIndex;
    _fseq_lastLoop = loop;
    _fseq_lastFileCount = fileCount;
  }

  if (!FSEQPlayer::isPlaying()) {
    SEGMENT.fill(SEGCOLOR(0));
    return;
  }

  FSEQPlayer::setLooping(loop);
  FSEQPlayer::renderFrameToSegment();
}

// Use a custom slider as the FSEQ file index and check2 as loop.
static const char _data_FX_MODE_FSEQ_PLAYER[] PROGMEM =
    "FSEQ Player@,,Index,,,,Loop;;c1=0,o2=1";
