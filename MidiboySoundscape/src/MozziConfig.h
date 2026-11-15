#pragma once

// Mozzi configuration. Every file that includes a Mozzi header must include
// this first, so all translation units agree on the settings.

#include <MozziConfigValues.h>

// Audio leaves through the UART (MIDI OUT), driven by UartAudioOut. Mozzi
// sets up no timer and keeps no output buffer in this mode: it asks
// canBufferAudioOutput() and hands each sample to audioOutput().
#define MOZZI_AUDIO_MODE MOZZI_OUTPUT_EXTERNAL_CUSTOM

// updateAudio() returns 12-bit samples (-2048..2047).
#define MOZZI_AUDIO_BITS 12

// Nominal rate. Mozzi only accepts powers of two here, but the real rate is
// set by the UART: UART_AUDIO_SAMPLE_RATE (15625 Hz). Pitches are corrected
// with tuneHz() in Soundscape.cpp; envelope and control timing just run
// ~5% slower than nominal, which a soundscape doesn't care about.
#define MOZZI_AUDIO_RATE 16384

#define MOZZI_CONTROL_RATE 128

// The Midiboy has no analog inputs; this keeps Mozzi off the ADC.
#define MOZZI_ANALOG_READ MOZZI_ANALOG_READ_NONE
