// Midiboy Soundscape: random soundscape generator, audio out of DIN MIDI OUT.
//
// Step 1 sketch: a test tone through UartAudioOut, without Mozzi. It proves
// that audio comes out of the MIDI OUT port and lets you check the pitch,
// level and noise floor from the buttons, without reflashing.
//
// Wiring (TRS Type A adapter): tip = DIN pin 5 = audio, sleeve = DIN pin 2 =
// ground. The ring is DIN pin 4, which sits at +5 V through a resistor: DC,
// no audio. Start with a line input at low gain.
//
// Buttons:
//   LEFT / RIGHT  frequency, in octaves (55 Hz .. 7040 Hz)
//   UP / DOWN     level, in 6 dB steps (0 dB = full scale, about 3 Vpp)
//   A             mode: tone -> sweep (55 Hz .. 7 kHz, 10 s) -> silence
//   B             dither on / off (see UartAudioOut.h). Compare in silence:
//                 off can whistle, on is a steady hiss.
//
// The bottom row shows how full the audio ring was at its lowest since the
// last update. 0 means a dropout (the interrupt ran out of data).

#include <MidiboyDisplay.h>
#include <MidiboyInput.h>
#include <MidiboyFonts.h>
#include "src/audio/UartAudioOut.h"

#define SINE_TABLE_BITS 8
#define SINE_TABLE_SIZE (1 << SINE_TABLE_BITS)
#define SINE_AMPLITUDE 2047

#define FREQ_COUNT 8
#define FREQ_DEFAULT 3          // 440 Hz
#define LEVEL_STEPS 9           // 0 .. -48 dB
#define LEVEL_DEFAULT 2         // -12 dB, about 0.75 Vpp: consumer line level

#define SWEEP_LOW_HZ 55.0f
#define SWEEP_HIGH_HZ 7040.0f
#define SWEEP_STEP_MS 10
#define SWEEP_FACTOR 1.0048578f  // (7040 / 55) ^ (1 / 1000): 10 s per sweep

#define STATUS_MS 250
#define CHAR_WIDTH 6

enum Row : uint8_t {
  ROW_TITLE = 0,
  ROW_FREQ = 2,
  ROW_LEVEL = 3,
  ROW_MODE = 4,
  ROW_DITHER = 5,
  ROW_FILL = 6,
  ROW_HELP = 7
};

static const uint16_t FREQUENCIES[FREQ_COUNT] = {55, 110, 220, 440, 880, 1760, 3520, 7040};

static MidiboyDisplay display;
static MidiboyInput input;

// One extra entry, so interpolation can read index + 1 without wrapping.
static int16_t sineTable[SINE_TABLE_SIZE + 1];

static uint32_t phase;
static uint32_t phaseStep;
static float sweepHz;

static uint8_t freqIndex = FREQ_DEFAULT;
static uint8_t levelShift = LEVEL_DEFAULT;
enum Mode : uint8_t {
  MODE_TONE,
  MODE_SWEEP,
  MODE_SILENCE,
  MODE_COUNT
};
static uint8_t mode = MODE_TONE;

static uint8_t minFill = 255;
static uint8_t shownMinFill;
static uint8_t dirtyRows = 0xFF;
static unsigned long lastSweepMs;
static unsigned long lastStatusMs;

// --- Audio -----------------------------------------------------------------

static void buildSineTable() {
  for(uint16_t i = 0; i <= SINE_TABLE_SIZE; i++) {
    sineTable[i] = (int16_t)lroundf(SINE_AMPLITUDE * sinf(i * (2.0f * PI / SINE_TABLE_SIZE)));
  }
}

static void setFrequency(float hz) {
  phaseStep = (uint32_t)(hz * (4294967296.0f / UART_AUDIO_SAMPLE_RATE));
}

// Linear interpolation between table entries: about -80 dB of error, well
// below the output's own noise, so what you hear is the output, not the table.
static int16_t nextSample() {
  phase += phaseStep;
  uint8_t index = phase >> 24;
  uint8_t fraction = phase >> 16;
  int16_t a = sineTable[index];
  int16_t b = sineTable[index + 1];
  int16_t sample = a + (int16_t)(((int32_t)(b - a) * fraction) >> 8);
  return mode == MODE_SILENCE ? 0 : sample >> levelShift;
}

static void fillAudio() {
  uint8_t fill = (uint8_t)(g_uartAudioWriteIndex - UART_AUDIO_READ_INDEX);
  if(fill < minFill) {
    minFill = fill;
  }
  while(UartAudioOut::canWrite()) {
    UartAudioOut::write(nextSample());
  }
}

// --- Screen ----------------------------------------------------------------

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
static void drawNumber(int16_t value, uint8_t width) {
  char digits[7];
  uint8_t count = 0;
  bool negative = value < 0;
  uint16_t magnitude = negative ? -value : value;
  do {
    digits[count++] = '0' + magnitude % 10;
    magnitude /= 10;
  } while(magnitude > 0);
  if(negative) {
    digits[count++] = '-';
  }
  for(uint8_t i = count; i < width; i++) {
    drawChar(' ', false);
  }
  while(count > 0) {
    drawChar(digits[--count], false);
  }
}

static void beginRow(uint8_t row) {
  display.setDrawPosition(0, row);
  textX = 0;
}

static void endRow() {
  display.drawSpace(128 - textX, false);
}

static void drawRow(uint8_t row) {
  beginRow(row);
  switch(row) {
    case ROW_TITLE:
      drawText_P(PSTR("TONE TEST "));
      drawNumber(UART_AUDIO_SAMPLE_RATE, 5);
      drawText_P(PSTR(" HZ"));
      break;
    case ROW_FREQ:
      drawText_P(PSTR("FREQ  "));
      drawNumber(mode == MODE_SWEEP ? (int16_t)sweepHz : FREQUENCIES[freqIndex], 5);
      drawText_P(PSTR(" HZ"));
      break;
    case ROW_LEVEL:
      drawText_P(PSTR("LEVEL "));
      drawNumber(-6 * levelShift, 5);
      drawText_P(PSTR(" DB"));
      break;
    case ROW_MODE:
      drawText_P(PSTR("MODE  "));
      if(mode == MODE_SILENCE) {
        drawText_P(PSTR("SILENCE"));
      } else if(mode == MODE_SWEEP) {
        drawText_P(PSTR("SWEEP"));
      } else {
        drawText_P(PSTR("TONE"));
      }
      break;
    case ROW_DITHER:
      drawText_P(PSTR("DITHER "));
      drawText_P(g_uartAudioDither ? PSTR("ON") : PSTR("OFF"));
      break;
    case ROW_FILL:
      drawText_P(PSTR("MIN FILL "));
      drawNumber(shownMinFill, 3);
      if(shownMinFill == 0) {
        drawText_P(PSTR(" DROPOUT"));
      }
      break;
    case ROW_HELP:
      drawText_P(PSTR("<>FRQ ^vLVL A:MD B:DT"));
      break;
  }
  endRow();
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

// --- Controls --------------------------------------------------------------

static void handleButton(MidiboyInput::Button button) {
  switch(button) {
    case MidiboyInput::BUTTON_LEFT:
      if(freqIndex > 0) {
        freqIndex--;
      }
      if(mode == MODE_SWEEP) {
        mode = MODE_TONE;
      }
      break;
    case MidiboyInput::BUTTON_RIGHT:
      if(freqIndex < FREQ_COUNT - 1) {
        freqIndex++;
      }
      if(mode == MODE_SWEEP) {
        mode = MODE_TONE;
      }
      break;
    case MidiboyInput::BUTTON_UP:
      if(levelShift > 0) {
        levelShift--;
      }
      break;
    case MidiboyInput::BUTTON_DOWN:
      if(levelShift < LEVEL_STEPS - 1) {
        levelShift++;
      }
      break;
    case MidiboyInput::BUTTON_A:
      mode = (mode + 1) % MODE_COUNT;
      sweepHz = SWEEP_LOW_HZ;
      break;
    case MidiboyInput::BUTTON_B:
      g_uartAudioDither = !g_uartAudioDither;
      markRow(ROW_DITHER);
      break;
    default:
      return;
  }
  if(mode == MODE_SWEEP) {
    setFrequency(sweepHz);
  } else {
    setFrequency(FREQUENCIES[freqIndex]);
  }
  markRow(ROW_FREQ);
  markRow(ROW_LEVEL);
  markRow(ROW_MODE);
}

static void updateSweep(unsigned long now) {
  if(mode != MODE_SWEEP || now - lastSweepMs < SWEEP_STEP_MS) {
    return;
  }
  lastSweepMs = now;
  sweepHz *= SWEEP_FACTOR;
  if(sweepHz > SWEEP_HIGH_HZ) {
    sweepHz = SWEEP_LOW_HZ;
  }
  setFrequency(sweepHz);
}

// --- Main ------------------------------------------------------------------

void setup() {
  display.begin();
  display.clearScreen();
  input.begin();
  input.setButtonRepeatMs(150);

  buildSineTable();
  setFrequency(FREQUENCIES[freqIndex]);

  // Last: from here on the loop must keep the ring fed.
  UartAudioOut::begin();
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
  updateSweep(now);
  if(now - lastStatusMs >= STATUS_MS) {
    lastStatusMs = now;
    shownMinFill = minFill;
    minFill = 255;
    markRow(ROW_FILL);
    if(mode == MODE_SWEEP) {
      markRow(ROW_FREQ);
    }
  }

  fillAudio();
  drawNextRow();
}
