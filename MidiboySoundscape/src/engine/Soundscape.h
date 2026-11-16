#pragma once
#include <Arduino.h>
#include <avr/pgmspace.h>
#include "../dsp/Dsp.h"

// User parameters, in screen order. MODE is drawn as the title row.
enum ParamId : uint8_t {
  PARAM_MODE,      // MODE_SCAPE or MODE_MELODY
  PARAM_MOOD,      // scale, 0..SCALE_COUNT-1
  PARAM_DENSITY,   // how often chimes strike, 0 = never
  PARAM_DRIFT,     // how fast everything wanders and the chords change
  PARAM_TONE,      // pad filter brightness and resonance
  PARAM_WIND,      // filtered-noise wind level
  PARAM_ECHO,      // chime echo level and feedback
  PARAM_LEVEL,     // master volume
  PARAM_COUNT
};

enum Mode : uint8_t {
  MODE_SCAPE,    // the generative soundscape
  MODE_MELODY    // the melody (src/melody/) on a loop
};

// Range of every parameter but MODE and MOOD.
#define PARAM_MAX 15

// Layers, as bits for setLayers(). All on by default; switching one off
// skips its work entirely (for listening to and measuring each alone).
enum LayerBit : uint8_t {
  LAYER_PAD = 1,
  LAYER_WIND = 2,
  LAYER_CHIMES = 4,
  LAYER_ECHO = 8,
  LAYER_ALL = 15
};

// Peak of next(): +-512, i.e. -12 dB of the 12-bit range, since the line
// input clips above that (see PLAN.md, step 1).
#define SOUNDSCAPE_PEAK 511

// Oscillators. Both take a phase step from noteStep() (a full cycle is
// 2^24) and are fully inlined; every cycle per sample counts.

// Sawtooth from a 24-bit phase: the top byte is the sample, no table.
// Resolution ~1/1000 Hz, fine enough for slow detune on bass notes.
// ~16 cycles.
struct SawOsc {
  __uint24 phase;
  __uint24 step;

  void setStep(uint32_t newStep) {
    step = (__uint24)newStep;
  }

  inline int8_t next() __attribute__((always_inline)) {
    phase += step;
    return (int8_t)((uint8_t)(phase >> 16) ^ 0x80);
  }
};

// Sine from a 16-bit phase and a 256-aligned table. Resolution 1/4 Hz:
// under 1 cent from 260 Hz up, so for high notes only (the chimes).
// ~16 cycles.
struct SineOsc {
  uint16_t phase;
  uint16_t step;

  // Takes the same 2^24-per-cycle step as SawOsc, rounded to 16 bits.
  void setStep(uint32_t newStep) {
    step = (uint16_t)((newStep + 128) >> 8);
  }

  void setPhase(uint16_t newPhase) {
    phase = newPhase;
  }

  inline int8_t next() __attribute__((always_inline)) {
    phase += step;
    return (int8_t)pgm_read_byte(&SINE_TABLE[phase >> 8]);
  }
};

// Slow random walk at control rate: steps by 1 towards a random target,
// then picks the next target.
struct Wander {
  uint8_t value;
  uint8_t target;
  uint8_t wait;

  // One step every `period` + 1 ticks, targets within [low, high].
  void tick(uint8_t low, uint8_t high, uint8_t period);
};

// 2-pole resonant lowpass after Mozzi's ResonantFilter (LGPL-2.1-or-later), with
// 16x8 / 16x16 multiplies instead of 32x32 ones (about half the cycles).
struct ResoLowpass {
  int16_t buf0;
  int16_t buf1;
  uint8_t cutoff;
  uint16_t feedback;

  void set(uint8_t newCutoff, uint8_t resonance);
  inline int16_t next(int16_t in);
};

// A struck bell: two sine partials sharing one decaying envelope.
struct ChimeVoice {
  SineOsc partial1;
  SineOsc partial2;
  uint16_t envelope;   // 8.8 fixed point
  uint8_t amp;         // envelope >> 8, read at audio rate
  uint8_t decayShift;  // envelope -= envelope >> decayShift every tick
};

#define CHIME_VOICES 2
#define PAD_VOICES 3
// Echo line, stored at 1/8 of the audio rate: 512 bytes = ~260 ms.
#define ECHO_LENGTH 512
#define ECHO_DECIMATION 8

// The generative engine: a drifting three-voice pad through a resonant
// lowpass, gusting filtered-noise wind, and randomly struck bell chimes
// feeding a low-fi echo. Everything follows one scale and root.
class Soundscape {
  public:
    void begin(uint32_t seed);

    // Soundscape mode: new root and chord, right away.
    // Melody mode: the melody from the top.
    void newScene();

    // Every CONTROL_PERIOD samples.
    void updateControl();

    // One sample, clipped to +-SOUNDSCAPE_PEAK.
    int16_t next();

    // LayerBit mask.
    uint8_t getLayers() const;
    void setLayers(uint8_t layers);

    uint8_t getParam(uint8_t id) const;
    // Clamped to the parameter's range.
    void setParam(uint8_t id, uint8_t value);
    uint8_t getParamMax(uint8_t id) const;

    // Holding stops chord changes; everything else keeps drifting.
    bool isHeld() const;
    void setHeld(bool held);


    // Root pitch class, 0 = C.
    uint8_t getRoot() const;
    // Goes up by one on every chime strike, for the screen.
    uint8_t getStrikeCount() const;

  private:
    uint8_t _params[PARAM_COUNT];
    uint8_t _layers;
    bool _held;
    uint8_t _root;          // MIDI note of the scene root, 36..47
    int8_t _chordDegree;
    uint16_t _chordTicks;   // until the next chord
    uint16_t _chimeTicks;   // until the next strike
    uint16_t _melodyRow;     // next row; MELODY_OFF when not playing
    uint16_t _melodyWait;    // control ticks until it
    uint8_t _padGlideShift;  // glide time constant, 2^shift ticks
    uint8_t _strikeCount;

    SawOsc _pad[PAD_VOICES];
    uint32_t _padStep[PAD_VOICES];         // gliding, before detune
    uint32_t _padTargetStep[PAD_VOICES];
    Wander _padDetune[PAD_VOICES];
    ResoLowpass _padFilter;
    Wander _padCutoff;
    Wander _padSwell;

    uint16_t _noise;
    int16_t _windLow;
    int16_t _windRumble;
    uint8_t _windAmp;
    uint8_t _windHalf;     // toggles every sample
    int16_t _windOut;      // last half-rate value
    Wander _windCutoff;
    Wander _windSwell;

    ChimeVoice _chime[CHIME_VOICES];

    int8_t _echo[ECHO_LENGTH];
    uint16_t _echoIndex;
    uint8_t _echoPhase;
    int16_t _echoSum;
    int16_t _echoTarget;
    int16_t _echoOut;
    uint8_t _echoFeedback;
    uint8_t _echoWet;

    uint8_t _master;

    void setChordTargets();
    void nextChord();
    void strikeChime();
    void strikeNote(uint8_t note, uint8_t level, uint16_t ratio, uint8_t decayShift);
    void setPadBass(uint8_t note);
    void stepMelody();
    // From the top, on a loop: the scene switches to the melody's key, the
    // pad follows its bass, the bells play its notes, and random chimes and
    // chord changes wait.
    void playMelody();
    // Back to the soundscape, carrying on in the melody's key.
    void stopMelody();
    uint8_t driftPeriod() const;
};
