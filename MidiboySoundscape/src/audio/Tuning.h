#pragma once
#include "../MozziConfig.h"
#include "UartAudioOut.h"
#include <mozzi_fixmath.h>

// Mozzi only accepts power-of-two audio rates, so its oscillators assume
// MOZZI_AUDIO_RATE (16384 Hz). The UART really plays UART_AUDIO_SAMPLE_RATE
// (15625 Hz), so every Mozzi oscillator sounds 16384 / 15625 = 1.048576
// times too low (0.82 semitones flat). Pass frequencies through tuneHz()
// before setFreq_Q16n16() to cancel that.

// (ratio - 1) in 1/65536ths: 3183 at 1.25 Mbaud.
#define TUNE_EXTRA \
  ((uint32_t)((65536ULL * MOZZI_AUDIO_RATE + UART_AUDIO_SAMPLE_RATE / 2) / UART_AUDIO_SAMPLE_RATE) - 65536UL)

// Control ticks per real second, for the same reason: Mozzi counts control
// ticks against the nominal rate, so they run ~5% slow.
#define CONTROL_HZ ((uint32_t)MOZZI_CONTROL_RATE * UART_AUDIO_SAMPLE_RATE / MOZZI_AUDIO_RATE)

// hz * 1.048576 without a 32x32 multiply. Error < 1/256 of the correction,
// i.e. under 0.02 cents.
static inline Q16n16 tuneHz(Q16n16 hz) {
  return hz + (((hz >> 8) * TUNE_EXTRA) >> 8);
}
