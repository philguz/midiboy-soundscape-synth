#pragma once
#include "../MozziConfig.h"
#include <MozziHeadersOnly.h>
#include <mozzi_fixmath.h>
#include <avr/pgmspace.h>

// User parameters, in screen order.
enum ParamId : uint8_t {
  PARAM_MOOD,      // scale, 0..SCALE_COUNT-1
  PARAM_DENSITY,   // how often chimes strike, 0 = never
  PARAM_DRIFT,     // how fast everything wanders and the chords change
  PARAM_TONE,      // pad filter brightness and resonance
  PARAM_WIND,      // filtered-noise wind level
  PARAM_ECHO,      // chime echo level and feedback
  PARAM_LEVEL,     // master volume
  PARAM_COUNT
};

// Range of every parameter but MOOD.
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

// Oscillators at the nominal MOZZI_AUDIO_RATE: pass frequencies through
// tuneHz() first. Both are fully inlined; every cycle per sample counts.
static_assert(MOZZI_AUDIO_RATE == 16384, "the oscillators assume 16384 Hz");

// Sawtooth from a 24-bit phase: the top byte is the sample, no table.
// Resolution 1/1024 Hz, fine enough for slow detune on bass notes.
// ~16 cycles.
struct SawOsc {
  __uint24 phase;
  __uint24 step;

  // step = hz * 2^24 / 16384 = hz (Q16n16) >> 6.
  void setFreq_Q16n16(Q16n16 hz) {
    step = (__uint24)(hz >> 6);
  }

  inline int8_t next() __attribute__((always_inline)) {
    phase += step;
    return (int8_t)((uint8_t)(phase >> 16) ^ 0x80);
  }
};

// Sine from a 16-bit phase and a 256-aligned table. Resolution 1/4 Hz:
// under 1 cent from 260 Hz up, so for high notes only (the chimes).
// ~16 cycles.
extern const int8_t SINE_TABLE[256];

struct SineOsc {
  uint16_t phase;
  uint16_t step;

  // step = hz * 2^16 / 16384 = hz (Q16n16) >> 14, rounded.
  void setFreq_Q16n16(Q16n16 hz) {
    step = (uint16_t)((hz + (1UL << 13)) >> 14);
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

// 2-pole resonant lowpass, Mozzi's ResonantFilter topology with
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

    // New root and chord, right away.
    void newScene();

    // From Mozzi's updateControl().
    void updateControl();

    // From Mozzi's updateAudio(): one sample, clipped to +-SOUNDSCAPE_PEAK.
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
    uint8_t _nextChime;
    uint8_t _strikeCount;

    SawOsc _pad[PAD_VOICES];
    Q16n16 _padFreq[PAD_VOICES];
    Q16n16 _padTarget[PAD_VOICES];
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
    uint8_t driftPeriod() const;
};
