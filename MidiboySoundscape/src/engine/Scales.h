#pragma once
#include <Arduino.h>

#define SCALE_COUNT 8
#define SCALE_MAX_NOTES 7

namespace Scales {
  // Name for the screen, up to 10 characters. Returns a PROGMEM string.
  const char* name(uint8_t scale);

  // MIDI note of a scale degree counted from root. Any degree works,
  // negative ones included: each wrap of the scale is one octave.
  uint8_t note(uint8_t scale, uint8_t root, int8_t degree);

  // Notes per octave in the scale.
  uint8_t length(uint8_t scale);
}
