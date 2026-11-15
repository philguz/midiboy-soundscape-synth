#pragma once
#include <Arduino.h>

// Audio out of the DIN MIDI OUT port.
//
// MIDI OUT is wired to the UART's TX pin (PD1), which has no PWM. Instead the
// UART runs at 1.25 Mbaud and every 8N1 frame it sends is used as a tiny PWM
// cell: start bit (0) + 8 data bits + stop bit (1). The number of 1s among the
// data bits (0..8) sets the frame's average level, so each frame is a 9-level
// DAC step at 125 kHz. A 2nd-order noise shaper picks the level of each frame,
// pushing quantisation noise up towards 125 kHz where the line input's own
// filtering (or an RC filter in the cable) removes it.
//
// 8 frames make one audio sample: 125000 / 8 = 15625 Hz. The encoder runs in
// the main loop (from Mozzi's audioOutput()) and fills a 256-byte ring; the
// UART "data register empty" interrupt only copies one byte per frame.
//
// The UART's receiver stays off, so MIDI IN is ignored while audio runs.

// UBRR0 with U2X0 set: baud = F_CPU / (8 * (UBRR + 1)).
// 1 -> 1.25 Mbaud (default). 2 -> 833 kbaud: less CPU (frames are 5 per
// sample there) and kinder to slow MIDI OUT drivers, but ~6 dB more noise.
#ifndef UART_AUDIO_UBRR
#define UART_AUDIO_UBRR 1
#endif

#if UART_AUDIO_UBRR == 1
#define UART_AUDIO_FRAMES_PER_SAMPLE 8
#elif UART_AUDIO_UBRR == 2
#define UART_AUDIO_FRAMES_PER_SAMPLE 5
#else
#error "UART_AUDIO_UBRR must be 1 or 2"
#endif

#define UART_AUDIO_SAMPLE_RATE \
  (F_CPU / 8UL / (UART_AUDIO_UBRR + 1) / 10UL / UART_AUDIO_FRAMES_PER_SAMPLE)

// Read index of the ring, owned by the ISR. GPIOR1 is a spare I/O register:
// the ISR reaches it with single-cycle in/out instead of a RAM access.
#define UART_AUDIO_READ_INDEX GPIOR1

extern volatile uint8_t g_uartAudioWriteIndex;

namespace UartAudioOut {
  // Fills the ring with silence and starts the UART. Call once, after any
  // slow setup (display init, EEPROM): from here on the main loop has to keep
  // the ring fed, or it falls back to silence.
  void begin();

  // True when the ring has room for one more sample.
  inline bool canWrite() {
    uint8_t freeBytes = (uint8_t)(UART_AUDIO_READ_INDEX - g_uartAudioWriteIndex - 1);
    return freeBytes >= UART_AUDIO_FRAMES_PER_SAMPLE;
  }

  // Encodes one sample (-2048..2047, clipped) into the ring. Only call it
  // when canWrite() is true.
  void write(int16_t sample);
}
