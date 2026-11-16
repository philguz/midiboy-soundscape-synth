// Midiboy Soundscape: random soundscape generator, audio out of DIN MIDI OUT.
//
// Two modes, chosen on the title row:
//   SOUNDSCAPE  the generative soundscape. Starts in this mode, with a
//               random scene.
//   MELODY      a melody on a loop (src/melody/; tools/mid2melody.py makes
//               one from a MIDI file). Switching back to SOUNDSCAPE carries
//               on in the melody's key.
//
// Buttons:
//   UP / DOWN     select the mode (title row) or a parameter
//   LEFT / RIGHT  change it (parameters repeat while held)
//   A             soundscape: new scene (new root and chord);
//                 melody: back to the top
//   B             hold: stop chord changes (everything else keeps drifting)
//
// Screen: a title row (mode, root note, HOLD, a * flash on every bell strike),
// then MOOD, DENSITY, DRIFT, TONE, WIND, ECHO and LEVEL.
//
// With SHOW_METER 1 the title row starts with "C1113 D  0" instead of the
// mode, updated 4 times a second:
//   C  CPU cycles per sample making audio (synthesis plus the UartAudioOut
//      encoder, including the UART interrupt meanwhile), averaged over the
//      last 250 ms. The budget is 1280; the rest is for the UI.
//   D  dropouts (the ring ran dry) since start-up or the last A press.

#include <MidiboyDisplay.h>
#include <MidiboyInput.h>
#include "src/audio/UartAudioOut.h"
#include "src/dsp/Dsp.h"
#include "src/engine/Soundscape.h"
#include "src/ui/ParamScreen.h"

#define SHOW_METER 0

// Layers that play (LayerBit mask). For tuning one layer alone, e.g.
// LAYER_CHIMES, or LAYER_CHIMES | LAYER_ECHO.
#define PLAY_LAYERS LAYER_ALL

// fillAudio() refills the ring completely, timing it in batches of at most
// AUDIO_BATCH_MAX samples so a batch stays below Timer1's 65536-cycle wrap.
// At high load the ring refills slowly (at 86% load, 32 samples made gain
// only ~4 samples of fill), so topping it up before every UI step is what
// lets a screen row (~10 samples of time) be drawn without a dropout.
#define AUDIO_BATCH_MAX 32
// Batches per fillAudio() call, at most, so the buttons still answer if the
// synthesis ever overloads the CPU.
#define AUDIO_BATCHES_MAX 8

#define METER_MS 250
#define BUTTON_REPEAT_MS 150

static MidiboyDisplay display;
static MidiboyInput input;
static Soundscape soundscape;
static ParamScreen screen(display, soundscape);

static uint16_t dropouts;
static uint32_t busyCycles;
static uint16_t busySamples;
static unsigned long lastMeterMs;

static uint8_t controlCountdown = 1;

// Not cleared at reset: random-ish after power-up, and a value from the
// last run's generator after a reset, so every start sounds different
// without using the EEPROM.
static uint32_t noinitSeed __attribute__((section(".noinit")));

// --- Audio -------------------------------------------------------------------

// Timer1 is free. Free-running at the CPU clock, it counts cycles; one batch
// is well under its 65536 wrap. It also times button presses for the seed.
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
      soundscape.updateControl();
    }
    UartAudioOut::write(soundscape.next());
    count++;
  }
  if(count != 0) {
    busyCycles += (uint16_t)(TCNT1 - start);
    busySamples += count;
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

static void updateMeter() {
  uint16_t cycles = busySamples != 0 ? (uint16_t)(busyCycles / busySamples) : 0;
  busyCycles = 0;
  busySamples = 0;
  screen.showMeter(cycles, dropouts);
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
  screen.markParam(id);
  if(id == PARAM_MODE) {
    // The melody sets its own key.
    screen.markParam(PARAM_MOOD);
  }
}

static void handleButton(MidiboyInput::Button button) {
  uint8_t selected = screen.getSelected();
  switch(button) {
    // The library's BUTTON_UP / BUTTON_DOWN are swapped relative to the
    // physical buttons on this device (checked on hardware).
    case MidiboyInput::BUTTON_DOWN:
      if(selected > 0) {
        screen.setSelected(selected - 1);
      }
      break;
    case MidiboyInput::BUTTON_UP:
      if(selected < PARAM_COUNT - 1) {
        screen.setSelected(selected + 1);
      }
      break;
    case MidiboyInput::BUTTON_LEFT:
      changeParam(selected, -1);
      break;
    case MidiboyInput::BUTTON_RIGHT:
      changeParam(selected, 1);
      break;
    case MidiboyInput::BUTTON_A:
      soundscape.newScene();
      screen.markParam(PARAM_MOOD);
      dropouts = 0;
      break;
    case MidiboyInput::BUTTON_B:
      soundscape.setHeld(!soundscape.isHeld());
      break;
    default:
      return;
  }
  // Button timing to the CPU cycle is real entropy; keep it across resets.
  Rng::stir(TCNT1 ^ ((uint32_t)micros() << 16));
  noinitSeed = Rng::next();
}

// --- Main --------------------------------------------------------------------

void setup() {
  display.begin();
  input.begin();
  input.setButtonRepeatMs(BUTTON_REPEAT_MS);
  startCycleCounter();

  soundscape.begin(noinitSeed ^ micros() ^ TCNT1);
  noinitSeed = Rng::next();
  soundscape.setLayers(PLAY_LAYERS);

  screen.setMeter(SHOW_METER);
  screen.begin();

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
  if(now - lastMeterMs >= METER_MS) {
    lastMeterMs = now;
    updateMeter();
  }
  screen.poll();

  fillAudio();
  screen.drawNext();
}
