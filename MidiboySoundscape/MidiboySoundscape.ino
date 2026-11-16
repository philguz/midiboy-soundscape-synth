// Midiboy Soundscape: random soundscape generator, audio out of DIN MIDI OUT.
//
// Step 3 sketch: the Soundscape engine with each layer switchable, so every
// layer can be heard alone and its CPU cost measured. Starts with the pad only.
//
// Buttons:
//   UP / DOWN     select a row (marked >)
//   LEFT / RIGHT  change the selected row's value:
//                   MOOD  scale          PAD   TONE      WIND  WIND level
//                   CHIME DENSITY        ECHO  ECHO      DRIFT DRIFT
//   A             on a layer row: switch the layer on / off
//   B             new scene (new root and chord)
//
// Screen, meters updated 4 times a second:
//   CYC    CPU cycles per sample making audio (synthesis plus the
//          UartAudioOut encoder, including the UART interrupt firing
//          meanwhile), averaged over the last 250 ms. The budget is 1280.
//   MX     the worst batch (4+ samples) in those 250 ms: chime bursts,
//          control ticks.
//   %      CYC as a share of the budget. What's left is for the UI.
//          (Not timed with micros(): at 20 MHz the Arduino core's micros()
//          runs 6.25% slow, since it multiplies by 64 / 20 = 3.)
//   DRP    dropouts (the ring ran dry) since the last A or B press.
//   PK     highest |sample| since the last update. 511 = the -12 dB ceiling.
//   CL     samples clipped at the ceiling since the last update.
//
// LEVEL is at maximum here, so PK shows the worst case.

#include "src/MozziConfig.h"
#include <Mozzi.h>

#include <MidiboyDisplay.h>
#include <MidiboyInput.h>
#include <MidiboyFonts.h>
#include "src/audio/UartAudioOut.h"
#include "src/audio/Tuning.h"
#include "src/engine/Soundscape.h"
#include "src/engine/Scales.h"

// fillAudio() refills the ring completely, timing it in batches of at most
// AUDIO_BATCH_MAX samples so a batch stays below Timer1's 65536-cycle wrap.
// At high load the ring refills slowly (at 86% load, 32 samples made gain
// only ~4 samples of fill), so topping it up before every UI step is what
// lets a screen row (~10 samples of time) be drawn without a dropout.
#define AUDIO_BATCH_MAX 32
// Batches per fillAudio() call, at most, so the buttons still answer if the
// synthesis ever overloads the CPU.
#define AUDIO_BATCHES_MAX 8

#define STATUS_MS 250
#define CHAR_WIDTH 6

enum Row : uint8_t {
  ROW_MOOD = 0,
  ROW_PAD = 1,
  ROW_WIND = 2,
  ROW_CHIME = 3,
  ROW_ECHO = 4,
  ROW_LOAD = 5,
  ROW_FILL = 6,
  ROW_DRIFT = 7
};

// Rows the cursor visits, top to bottom.
static const uint8_t SELECTABLE[] = { ROW_MOOD, ROW_PAD, ROW_WIND, ROW_CHIME, ROW_ECHO, ROW_DRIFT };
#define SELECTABLE_COUNT sizeof(SELECTABLE)

static const char NOTE_NAMES[12][3] PROGMEM = {
  "C ", "C#", "D ", "D#", "E ", "F ", "F#", "G ", "G#", "A ", "A#", "B "
};

static MidiboyDisplay display;
static MidiboyInput input;
static Soundscape soundscape;

static uint8_t cursor;

static uint16_t dropouts;
static uint16_t peak;
static uint16_t shownPeak;
static uint16_t clips;
static uint16_t shownClips;
static uint32_t busyCycles;
static uint16_t busySamples;
static uint16_t maxCycles;
static uint8_t shownLoad;
static uint16_t shownCycles;
static uint16_t shownMaxCycles;
static unsigned long lastStatusMs;
static uint8_t dirtyRows = 0xFF;
static uint8_t shownRoot = 0xFF;

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
  soundscape.updateControl();
}

AudioOutput updateAudio() {
  return MonoOutput(soundscape.next());
}

// --- Audio -------------------------------------------------------------------

// Timer1 is free (Mozzi claims no timer in custom mode). Free-running at the
// CPU clock, it counts cycles; one batch is well under its 65536 wrap.
static void startCycleCounter() {
  TCCR1A = 0;
  TCCR1B = _BV(CS10);
  TIMSK1 = 0;
}

// One batch: up to AUDIO_BATCH_MAX samples. Returns how many were made.
static uint8_t fillBatch() {
  uint16_t start = TCNT1;
  uint8_t count = 0;
  while(count < AUDIO_BATCH_MAX && UartAudioOut::canWrite()) {
    if(--controlCountdown == 0) {
      controlCountdown = CONTROL_PERIOD;
      updateControl();
    }
    int16_t sample = soundscape.next();
    UartAudioOut::write(sample);
    uint16_t level = sample < 0 ? -sample : sample;
    if(level > peak) {
      peak = level;
    }
    if(level >= SOUNDSCAPE_PEAK) {
      clips++;
    }
    count++;
  }
  if(count != 0) {
    uint16_t cycles = (uint16_t)(TCNT1 - start);
    busyCycles += cycles;
    busySamples += count;
    // Small batches are mostly loop overhead, so they don't count for MX.
    if(count >= 4) {
      uint16_t perSample = cycles / count;
      if(perSample > maxCycles) {
        maxCycles = perSample;
      }
    }
  }
  return count;
}

// Fills the ring up.
static void fillAudio() {
  // An empty ring means the UART interrupt is sending silence: a dropout.
  // (It is never empty otherwise: it starts half full.)
  if(g_uartAudioWriteIndex == UART_AUDIO_READ_INDEX && dropouts < 999) {
    dropouts++;
  }
  for(uint8_t i = 0; i < AUDIO_BATCHES_MAX; i++) {
    if(fillBatch() == 0) {
      break;
    }
  }
}

// Turns the window's counts into the numbers on screen.
// CPU cycles per sample at 15625 Hz.
#define CYCLE_BUDGET (F_CPU / UART_AUDIO_SAMPLE_RATE)

static void updateMeter() {
  shownCycles = busySamples != 0 ? (uint16_t)(busyCycles / busySamples) : 0;
  uint16_t load = (uint32_t)shownCycles * 100 / CYCLE_BUDGET;
  shownLoad = load > 999 ? 999 : load;
  shownMaxCycles = maxCycles;
  maxCycles = 0;
  busyCycles = 0;
  busySamples = 0;
  shownPeak = peak;
  peak = 0;
  shownClips = clips;
  clips = 0;
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

static void drawLayer(const char* name, uint8_t layer, uint8_t param) {
  drawText_P(name);
  drawText_P((soundscape.getLayers() & layer) ? PSTR(" ON  ") : PSTR(" OFF "));
  drawNumber(soundscape.getParam(param), 2);
}

static void drawRow(uint8_t row) {
  display.setDrawPosition(0, row);
  textX = 0;
  drawChar(SELECTABLE[cursor] == row ? '>' : ' ', false);
  switch(row) {
    case ROW_MOOD:
      drawText_P(PSTR("MOOD  "));
      drawText_P(Scales::name(soundscape.getParam(PARAM_MOOD)));
      drawChar(' ', false);
      drawText_P(NOTE_NAMES[soundscape.getRoot()]);
      break;
    case ROW_PAD:
      drawLayer(PSTR("PAD  "), LAYER_PAD, PARAM_TONE);
      drawText_P(PSTR(" TONE"));
      break;
    case ROW_WIND:
      drawLayer(PSTR("WIND "), LAYER_WIND, PARAM_WIND);
      break;
    case ROW_CHIME:
      drawLayer(PSTR("CHIME"), LAYER_CHIMES, PARAM_DENSITY);
      drawText_P(PSTR(" DENS"));
      break;
    case ROW_ECHO:
      drawLayer(PSTR("ECHO "), LAYER_ECHO, PARAM_ECHO);
      break;
    case ROW_LOAD:
      drawText_P(PSTR("CYC"));
      drawNumber(shownCycles, 4);
      drawText_P(PSTR(" MX"));
      drawNumber(shownMaxCycles, 5);
      drawNumber(shownLoad, 4);
      drawChar('%', false);
      break;
    case ROW_FILL:
      drawText_P(PSTR("DRP"));
      drawNumber(dropouts, 4);
      drawText_P(PSTR(" PK"));
      drawNumber(shownPeak, 4);
      drawText_P(PSTR(" CL"));
      drawNumber(shownClips > 999 ? 999 : shownClips, 3);
      break;
    case ROW_DRIFT:
      drawText_P(PSTR("DRIFT       "));
      drawNumber(soundscape.getParam(PARAM_DRIFT), 2);
      if(soundscape.isHeld()) {
        drawText_P(PSTR(" HOLD"));
      }
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

static void changeParam(uint8_t id, int8_t delta) {
  uint8_t value = soundscape.getParam(id);
  if(delta < 0) {
    if(value == 0) {
      return;
    }
    value--;
  } else {
    value++;
  }
  soundscape.setParam(id, value);
}

// The parameter and layer behind a row, if any.
static bool rowTarget(uint8_t row, uint8_t& param, uint8_t& layer) {
  layer = 0;
  switch(row) {
    case ROW_MOOD:  param = PARAM_MOOD; return true;
    case ROW_PAD:   param = PARAM_TONE;    layer = LAYER_PAD;    return true;
    case ROW_WIND:  param = PARAM_WIND;    layer = LAYER_WIND;   return true;
    case ROW_CHIME: param = PARAM_DENSITY; layer = LAYER_CHIMES; return true;
    case ROW_ECHO:  param = PARAM_ECHO;    layer = LAYER_ECHO;   return true;
    case ROW_DRIFT: param = PARAM_DRIFT; return true;
  }
  return false;
}

static void handleButton(MidiboyInput::Button button) {
  uint8_t row = SELECTABLE[cursor];
  uint8_t param;
  uint8_t layer;
  bool hasTarget = rowTarget(row, param, layer);

  switch(button) {
    case MidiboyInput::BUTTON_UP:
      if(cursor > 0) {
        cursor--;
        markRow(row);
        markRow(SELECTABLE[cursor]);
      }
      break;
    case MidiboyInput::BUTTON_DOWN:
      if(cursor < SELECTABLE_COUNT - 1) {
        cursor++;
        markRow(row);
        markRow(SELECTABLE[cursor]);
      }
      break;
    case MidiboyInput::BUTTON_LEFT:
    case MidiboyInput::BUTTON_RIGHT:
      if(hasTarget) {
        changeParam(param, button == MidiboyInput::BUTTON_LEFT ? -1 : 1);
        markRow(row);
      }
      break;
    case MidiboyInput::BUTTON_A:
      if(layer != 0) {
        soundscape.setLayers(soundscape.getLayers() ^ layer);
        markRow(row);
      }
      dropouts = 0;
      break;
    case MidiboyInput::BUTTON_B:
      soundscape.newScene();
      markRow(ROW_MOOD);
      dropouts = 0;
      break;
    default:
      break;
  }
}

// --- Main --------------------------------------------------------------------

void setup() {
  display.begin();
  display.clearScreen();
  input.begin();
  input.setButtonRepeatMs(150);

  soundscape.begin(micros() ^ ((uint32_t)TCNT0 << 16));
  soundscape.setLayers(LAYER_PAD);
  soundscape.setParam(PARAM_LEVEL, PARAM_MAX);

  startCycleCounter();
  // No startMozzi(): in custom output mode it only sets up audioHook()'s
  // control counter, which this sketch doesn't use.

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
  if(now - lastStatusMs >= STATUS_MS) {
    lastStatusMs = now;
    updateMeter();
    markRow(ROW_LOAD);
    markRow(ROW_FILL);
  }
  // The root changes on new scenes only; the screen follows.
  if(soundscape.getRoot() != shownRoot) {
    shownRoot = soundscape.getRoot();
    markRow(ROW_MOOD);
  }

  fillAudio();
  drawNextRow();
}
