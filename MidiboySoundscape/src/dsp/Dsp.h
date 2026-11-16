#pragma once
#include <Arduino.h>
#include <avr/pgmspace.h>
#include "../audio/UartAudioOut.h"

// Small DSP helpers in place of Mozzi. Parts derived from Mozzi
// (LGPL-2.1-or-later, see third_party/mozzi) are marked.

// The real sample rate, set by the UART: 15625 Hz at 1.25 Mbaud.
#define AUDIO_RATE UART_AUDIO_SAMPLE_RATE
// Samples per Soundscape::updateControl().
#define CONTROL_PERIOD 128
// Control ticks per second: 122.
#define CONTROL_HZ (AUDIO_RATE / CONTROL_PERIOD)

// (a * b) >> 8 for signed 16-bit a and unsigned 8-bit b, ~8 cycles.
// From Mozzi's mult16x8.h (MultiSU16X8toH16), LGPL-2.1-or-later.
static inline __attribute__((always_inline)) int16_t mulShr8(int16_t a, uint8_t b) {
  int16_t result;
  asm volatile (
    "clr r26        \n\t"
    "mulsu %B1, %A2 \n\t"
    "movw %A0, r0   \n\t"
    "mul %A1, %A2   \n\t"
    "add %A0, r1    \n\t"
    "adc %B0, r26   \n\t"
    "clr r1         \n\t"
    : "=&r" (result)
    : "a" (a), "a" (b)
    : "r26"
  );
  return result;
}

// Marsaglia's xorshift96, period 2^96 - 1, as Mozzi's xorshift96().
namespace Rng {
  void seed(uint32_t seed);
  // Mixes in more entropy (e.g. button timing) without restarting.
  void stir(uint32_t entropy);
  uint32_t next();

  inline uint8_t byte() {
    return (uint8_t)next();
  }

  // 0..n-1
  inline uint8_t below(uint8_t n) {
    return ((uint16_t)byte() * n) >> 8;
  }
}

// Oscillator phase step for a MIDI note (0..119): one full cycle is 2^24,
// so step = hz * 2^24 / AUDIO_RATE. Error under 0.01 cents.
uint32_t noteStep(uint8_t note);

// 256-entry sine, -127..127, 256-byte aligned.
// Mozzi's SIN256_DATA (tables/sin256_int8.h), LGPL-2.1-or-later.
extern const int8_t SINE_TABLE[256];
