// Midiboy Soundscape: random soundscape generator, audio out of DIN MIDI OUT.
//
// Step 2 sketch: Mozzi in MOZZI_OUTPUT_EXTERNAL_CUSTOM mode. Mozzi generates
// the samples (Oscil, updateAudio()); UartAudioOut sends them out of MIDI OUT.
// Checks the pitch correction and measures how much CPU the audio path takes.
//
// Buttons:
//   LEFT / RIGHT  frequency, in octaves (55 Hz .. 7040 Hz)
//   UP / DOWN     oscillators, 1..8 (harmonics 1..N, summed): a CPU load test
//   A             mute on / off
//   B             pitch correction on / off. Off plays 16384/15625 flat:
//                 440 Hz comes out as 420 Hz (0.82 semitones low).
//
// Screen, updated 4 times a second:
//   AUDIO  share of time the main loop spends making audio (Mozzi synthesis
//          plus the UartAudioOut encoder, including the UART interrupt
//          firing meanwhile). What's left is for the UI. 100% = can't keep up.
//   CYC    the same as CPU cycles per sample. The budget is 1280.
//   FILL   lowest ring fill since the last update. 0 = a dropout.
//
// Output peaks at -12 dB (+-512 of +-2048): the line input clips above that.

#include "src/MozziConfig.h"
#include <Mozzi.h>
#include <Oscil.h>
#include <tables/sin2048_int8.h>

#include <MidiboyDisplay.h>
#include <MidiboyInput.h>
#include <MidiboyFonts.h>
#include "src/audio/UartAudioOut.h"
#include "src/audio/Tuning.h"

#define FREQ_COUNT 8
#define FREQ_DEFAULT 3          // 440 Hz
#define OSC_MAX 8

// Samples per audioHook() batch, at most. The ring holds 32 samples (2 ms),
// so this also bounds one batch below 65536 cycles for the Timer1 meter.
#define AUDIO_BATCH_MAX 32

#define STATUS_MS 250
#define CHAR_WIDTH 6

enum Row : uint8_t {
  ROW_TITLE = 0,
  ROW_FREQ = 2,
  ROW_OSC = 3,
  ROW_TUNE = 4,
  ROW_LOAD = 5,
  ROW_FILL = 6,
  ROW_HELP = 7
};

static const uint16_t FREQUENCIES[FREQ_COUNT] = {55, 110, 220, 440, 880, 1760, 3520, 7040};

// Right shift after summing N int8 voices (<< 2), so the peak stays within
// +-512, i.e. -12 dB.
static const uint8_t SUM_SHIFT[OSC_MAX + 1] = {0, 0, 1, 2, 2, 3, 3, 3, 3};

static MidiboyDisplay display;
static MidiboyInput input;

static Oscil<SIN2048_NUM_CELLS, MOZZI_AUDIO_RATE> oscillators[OSC_MAX];

static uint8_t freqIndex = FREQ_DEFAULT;
static uint8_t oscCount = 1;
static bool muted;
static bool tuned = true;

static uint8_t minFill = 255;
static uint8_t shownMinFill;
static uint32_t busyCycles;
static uint16_t busySamples;
static uint8_t shownLoad;
static uint16_t shownCycles;
static unsigned long windowStartUs;
static unsigned long lastStatusMs;
static uint8_t dirtyRows = 0xFF;

// --- Mozzi hooks -------------------------------------------------------------
//
// The loop calls updateControl() and updateAudio() itself instead of Mozzi's
// audioHook(): in custom output mode the hook adds nothing but a 64-bit
// sample counter (audioTicks / mozziMicros, unused here), ~80 cycles a sample.
// Mozzi still needs these two defined, since its audioHook() is compiled in.

// Samples per updateControl(), as Mozzi would: 16384 / 128.
#define CONTROL_PERIOD (MOZZI_AUDIO_RATE / MOZZI_CONTROL_RATE)
static uint8_t controlCountdown = 1;

inline bool canBufferAudioOutput() {
  return UartAudioOut::canWrite();
}

void audioOutput(const AudioOutput f) {
  UartAudioOut::write(f.l());
}

void updateControl() {
}

AudioOutput updateAudio() {
  int16_t sum = 0;
  for(uint8_t i = 0; i < oscCount; i++) {
    sum += oscillators[i].next();
  }
  if(muted) {
    return MonoOutput(0);
  }
  return MonoOutput((sum << 2) >> SUM_SHIFT[oscCount]);
}

// --- Audio -------------------------------------------------------------------

static void setFrequencies() {
  Q16n16 base = (Q16n16)FREQUENCIES[freqIndex] << 16;
  if(tuned) {
    base = tuneHz(base);
  }
  for(uint8_t i = 0; i < OSC_MAX; i++) {
    oscillators[i].setFreq_Q16n16(base * (i + 1));
  }
}

// Timer1 is free (Mozzi claims no timer in custom mode). Free-running at the
// CPU clock, it counts cycles; one batch is well under its 65536 wrap.
static void startCycleCounter() {
  TCCR1A = 0;
  TCCR1B = _BV(CS10);
  TIMSK1 = 0;
}

static void fillAudio() {
  uint8_t fill = (uint8_t)(g_uartAudioWriteIndex - UART_AUDIO_READ_INDEX);
  if(fill < minFill) {
    minFill = fill;
  }
  uint16_t start = TCNT1;
  uint8_t count = 0;
  while(count < AUDIO_BATCH_MAX && UartAudioOut::canWrite()) {
    if(--controlCountdown == 0) {
      controlCountdown = CONTROL_PERIOD;
      updateControl();
    }
    UartAudioOut::write(updateAudio().l());
    count++;
  }
  if(count != 0) {
    busyCycles += (uint16_t)(TCNT1 - start);
    busySamples += count;
  }
}

// Turns the window's counts into the numbers on screen.
static void updateMeter() {
  unsigned long now = micros();
  uint32_t elapsedCycles = (now - windowStartUs) * (F_CPU / 1000000UL);
  windowStartUs = now;
  shownLoad = elapsedCycles != 0 ? (uint8_t)((busyCycles * 100) / elapsedCycles) : 0;
  shownCycles = busySamples != 0 ? (uint16_t)(busyCycles / busySamples) : 0;
  busyCycles = 0;
  busySamples = 0;
  shownMinFill = minFill;
  minFill = 255;
}

// --- Screen ------------------------------------------------------------------

static uint8_t textX;

static void drawChar(char c, bool inverse) {
  if(c < ' ' || c > '~') {
    c = '?';
  }
  const uint8_t* glyph = &MIDIBOY_FONT_5X7::DATA_P[(uint16_t)(c - ' ') * MIDIBOY_FONT_5X7::WIDTH];
  display.drawBitmap_P(glyph, MIDIBOY_FONT_5X7::WIDTH, inverse);
  display.drawSpace(1, inverse);
  textX += CHAR_WIDTH;
}

static void drawText_P(const char* text) {
  char c;
  while((c = pgm_read_byte(text++)) != 0) {
    drawChar(c, false);
  }
}

// Right-aligned in `width` characters.
static void drawNumber(uint16_t value, uint8_t width) {
  char digits[6];
  uint8_t count = 0;
  do {
    digits[count++] = '0' + value % 10;
    value /= 10;
  } while(value > 0);
  for(uint8_t i = count; i < width; i++) {
    drawChar(' ', false);
  }
  while(count > 0) {
    drawChar(digits[--count], false);
  }
}

static void drawRow(uint8_t row) {
  display.setDrawPosition(0, row);
  textX = 0;
  switch(row) {
    case ROW_TITLE:
      drawText_P(PSTR("MOZZI TEST "));
      drawNumber(UART_AUDIO_SAMPLE_RATE, 5);
      drawText_P(PSTR(" HZ"));
      break;
    case ROW_FREQ:
      drawText_P(PSTR("FREQ  "));
      drawNumber(FREQUENCIES[freqIndex], 4);
      drawText_P(PSTR(" HZ"));
      if(muted) {
        drawText_P(PSTR(" MUTE"));
      }
      break;
    case ROW_OSC:
      drawText_P(PSTR("OSC   "));
      drawNumber(oscCount, 4);
      break;
    case ROW_TUNE:
      drawText_P(PSTR("TUNE  "));
      if(tuned) {
        drawText_P(PSTR("  ON"));
      } else {
        drawText_P(PSTR(" OFF -0.82 ST"));
      }
      break;
    case ROW_LOAD:
      drawText_P(PSTR("AUDIO "));
      drawNumber(shownLoad, 3);
      drawText_P(PSTR("% CYC "));
      drawNumber(shownCycles, 4);
      break;
    case ROW_FILL:
      drawText_P(PSTR("FILL  "));
      drawNumber(shownMinFill, 4);
      if(shownMinFill == 0) {
        drawText_P(PSTR(" DROPOUT"));
      }
      break;
    case ROW_HELP:
      drawText_P(PSTR("<>FRQ ^vOSC A:MU B:TN"));
      break;
  }
  display.drawSpace(128 - textX, false);
}

// At most one row per call, so the audio ring gets refilled in between.
static void drawNextRow() {
  if(dirtyRows == 0) {
    return;
  }
  uint8_t row = 0;
  while(!(dirtyRows & (1 << row))) {
    row++;
  }
  dirtyRows &= ~(1 << row);
  drawRow(row);
}

static void markRow(uint8_t row) {
  dirtyRows |= 1 << row;
}

// --- Controls ----------------------------------------------------------------

static void handleButton(MidiboyInput::Button button) {
  switch(button) {
    case MidiboyInput::BUTTON_LEFT:
      if(freqIndex > 0) {
        freqIndex--;
      }
      break;
    case MidiboyInput::BUTTON_RIGHT:
      if(freqIndex < FREQ_COUNT - 1) {
        freqIndex++;
      }
      break;
    case MidiboyInput::BUTTON_UP:
      if(oscCount < OSC_MAX) {
        oscCount++;
      }
      markRow(ROW_OSC);
      return;
    case MidiboyInput::BUTTON_DOWN:
      if(oscCount > 1) {
        oscCount--;
      }
      markRow(ROW_OSC);
      return;
    case MidiboyInput::BUTTON_A:
      muted = !muted;
      break;
    case MidiboyInput::BUTTON_B:
      tuned = !tuned;
      markRow(ROW_TUNE);
      break;
    default:
      return;
  }
  setFrequencies();
  markRow(ROW_FREQ);
}

// --- Main --------------------------------------------------------------------

void setup() {
  display.begin();
  display.clearScreen();
  input.begin();
  input.setButtonRepeatMs(150);

  for(uint8_t i = 0; i < OSC_MAX; i++) {
    oscillators[i].setTable(SIN2048_DATA);
  }
  setFrequencies();
  startCycleCounter();
  // No startMozzi(): in custom output mode it only sets up audioHook()'s
  // control counter, which this sketch doesn't use.

  // Last: from here on the loop must keep the ring fed.
  UartAudioOut::begin();
  windowStartUs = micros();
}

void loop() {
  fillAudio();

  input.think();
  MidiboyInput::Event event;
  while(input.readInputEvent(event)) {
    if(event.m_type == MidiboyInput::EVENT_DOWN) {
      handleButton(event.m_button);
    }
  }

  unsigned long now = millis();
  if(now - lastStatusMs >= STATUS_MS) {
    lastStatusMs = now;
    updateMeter();
    markRow(ROW_LOAD);
    markRow(ROW_FILL);
  }

  fillAudio();
  drawNextRow();
}
