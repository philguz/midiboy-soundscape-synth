#include "Soundscape.h"
#include "Scales.h"
#include "../audio/UartAudioOut.h"
#include "../audio/Tuning.h"

#include <mozzi_rand.h>
#include <mozzi_midi.h>
#include <mult16x8.h>
#include <tables/sin256_int8.h>
#include <tables/saw256_int8.h>

static const uint8_t DEFAULT_PARAMS[PARAM_COUNT] = {
  0,    // MOOD: penta minor
  6,    // DENSITY
  6,    // DRIFT
  8,    // TONE
  5,    // WIND
  8,    // ECHO
  12,   // LEVEL
};

// Mean time between chime strikes per DENSITY step, in tenths of a second.
static const uint8_t CHIME_INTERVAL_DS[PARAM_MAX + 1] PROGMEM = {
  0, 200, 140, 100, 75, 55, 42, 33, 26, 20, 16, 12, 9, 7, 5, 4
};

// Upper-partial ratios for the chimes, 8.8 fixed point: bell, octave, twelfth.
static const uint16_t CHIME_RATIOS[] PROGMEM = { 707, 512, 768, 645 };

static inline uint8_t randomByte() {
  return (uint8_t)xorshift96();
}

// 0..n-1
static inline uint8_t randomBelow(uint8_t n) {
  return ((uint16_t)randomByte() * n) >> 8;
}

// (a * b) >> 8 for signed 16-bit a and unsigned 8-bit b.
static inline int16_t mulShr8(int16_t a, uint8_t b) {
  int16_t result;
  MultiSU16X8toH16(result, a, b);
  return result;
}

static Q16n16 noteHz(uint8_t note) {
  return tuneHz(Q16n16_mtof((Q16n16)note << 16));
}

void Wander::tick(uint8_t low, uint8_t high, uint8_t period) {
  if(wait != 0) {
    wait--;
    return;
  }
  wait = period;
  if(value < target) {
    value++;
  } else if(value > target) {
    value--;
  } else {
    target = low + randomBelow(high - low + 1);
  }
}

void ResoLowpass::set(uint8_t newCutoff, uint8_t resonance) {
  cutoff = newCutoff;
  feedback = resonance + (((uint16_t)resonance * (255 + (uint16_t)newCutoff)) >> 8);
}

inline int16_t ResoLowpass::next(int16_t in) {
  int16_t band = buf0 - buf1;
  int16_t drive = in - buf0 + (int16_t)(((int32_t)band * (int16_t)feedback) >> 8);
  buf0 += mulShr8(drive, cutoff);
  buf1 += mulShr8(buf0 - buf1, cutoff);
  return buf1;
}

void Soundscape::begin(uint32_t seed) {
  randSeed(seed);
  for(uint8_t i = 0; i < PARAM_COUNT; i++) {
    _params[i] = DEFAULT_PARAMS[i];
  }
  _held = false;
  _strikeCount = 0;
  _nextChime = 0;
  _chimeTicks = CONTROL_HZ * 2;

  for(uint8_t i = 0; i < PAD_VOICES; i++) {
    _pad[i].setTable(SAW256_DATA);
    _padDetune[i].value = 12;
    _padDetune[i].target = 12;
    _padDetune[i].wait = 0;
  }
  _padFilter.buf0 = 0;
  _padFilter.buf1 = 0;
  _padCutoff.value = 30;
  _padCutoff.target = 30;
  _padCutoff.wait = 0;
  // Fade in from silence.
  _padSwell.value = 0;
  _padSwell.target = 180;
  _padSwell.wait = 0;

  _noise = 0xACE1;
  _windLow = 0;
  _windRumble = 0;
  _windAmp = 0;
  _windCutoff.value = 40;
  _windCutoff.target = 40;
  _windCutoff.wait = 0;
  _windSwell.value = 0;
  _windSwell.target = 128;
  _windSwell.wait = 0;

  for(uint8_t i = 0; i < CHIME_VOICES; i++) {
    _chime[i].partial1.setTable(SIN256_DATA);
    _chime[i].partial2.setTable(SIN256_DATA);
    _chime[i].envelope = 0;
    _chime[i].amp = 0;
    _chime[i].decayShift = 7;
  }

  for(uint16_t i = 0; i < ECHO_LENGTH; i++) {
    _echo[i] = 0;
  }
  _echoIndex = 0;
  _echoPhase = 0;
  _echoSum = 0;
  _echoTarget = 0;
  _echoOut = 0;
  _echoFeedback = 0;
  _echoWet = 0;
  _master = 0;

  newScene();
  // Start every pad voice on its note instead of gliding in from 0 Hz.
  for(uint8_t i = 0; i < PAD_VOICES; i++) {
    _padFreq[i] = _padTarget[i];
  }
}

void Soundscape::newScene() {
  _root = 36 + randomBelow(12);
  _chordDegree = randomBelow(Scales::length(_params[PARAM_MOOD]));
  setChordTargets();
  _chordTicks = 1;   // reschedule on the next tick
  nextChord();
}

// Pad notes for the current chord: the chord root in the bass, two more
// chord tones (degree +2 and +4, a triad in 7-note scales) an octave up.
void Soundscape::setChordTargets() {
  uint8_t scale = _params[PARAM_MOOD];
  _padTarget[0] = noteHz(Scales::note(scale, _root, _chordDegree));
  _padTarget[1] = noteHz(Scales::note(scale, _root + 12, _chordDegree + 2));
  _padTarget[2] = noteHz(Scales::note(scale, _root + 12, _chordDegree + 4));
}

void Soundscape::nextChord() {
  if(--_chordTicks != 0) {
    return;
  }
  // Step the chord root by up to 3 degrees, staying near the scene root.
  int8_t step = (int8_t)randomBelow(6) - 3;
  if(step >= 0) {
    step++;   // never 0: -3..-1, 1..3
  }
  int8_t degree = _chordDegree + step;
  int8_t length = Scales::length(_params[PARAM_MOOD]);
  if(degree < -2 || degree > length) {
    degree = _chordDegree - step;
  }
  _chordDegree = degree;
  setChordTargets();

  // 30 s at DRIFT 0 down to ~8 s at DRIFT 15, then 50..150% of that.
  uint16_t seconds = 30 - ((_params[PARAM_DRIFT] * 3) >> 1);
  uint16_t base = seconds * CONTROL_HZ;
  _chordTicks = (base >> 1) + (uint16_t)(((uint32_t)base * randomByte()) >> 8);
}

void Soundscape::strikeChime() {
  ChimeVoice& voice = _chime[_nextChime];
  _nextChime = (_nextChime + 1) % CHIME_VOICES;

  uint8_t scale = _params[PARAM_MOOD];
  uint8_t length = Scales::length(scale);
  // Degrees across two octaves, starting an octave above the pad's top.
  uint8_t note = Scales::note(scale, _root + 12, length + randomBelow(length * 2));
  Q16n16 hz = noteHz(note);
  uint16_t ratio = pgm_read_word(&CHIME_RATIOS[randomBelow(4)]);

  voice.partial1.setPhase(0);
  voice.partial2.setPhase(0);
  voice.partial1.setFreq_Q16n16(hz);
  voice.partial2.setFreq_Q16n16((hz >> 8) * ratio);
  voice.envelope = (uint16_t)(150 + randomBelow(106)) << 8;
  voice.amp = voice.envelope >> 8;
  voice.decayShift = 6 + randomBelow(3);
  _strikeCount++;
}

// Ticks between Wander steps: 15 at DRIFT 0, 0 at DRIFT 15.
uint8_t Soundscape::driftPeriod() const {
  return PARAM_MAX - _params[PARAM_DRIFT];
}

void Soundscape::updateControl() {
  uint8_t period = driftPeriod();

  if(!_held) {
    nextChord();
  }

  // Pad: glide to the chord notes (time constant ~0.5 s), each voice with
  // its own slow detune of up to +-0.3% (~5 cents) for a chorus beat.
  for(uint8_t i = 0; i < PAD_VOICES; i++) {
    int32_t diff = (int32_t)(_padTarget[i] - _padFreq[i]);
    _padFreq[i] += diff >> 6;
    _padDetune[i].tick(0, 24, period + 8);
    int8_t wobble = (int8_t)_padDetune[i].value - 12;
    Q16n16 hz = _padFreq[i] + (int32_t)(_padFreq[i] >> 12) * wobble;
    _pad[i].setFreq_Q16n16(hz);
  }

  uint8_t tone = _params[PARAM_TONE];
  _padCutoff.tick(0, 60, period);
  _padFilter.set(30 + tone * 9 + _padCutoff.value, 60 + tone * 8);
  _padSwell.tick(110, 230, period + 4);

  // Wind: gusts in level and colour.
  _windSwell.tick(30, 255, period >> 1);
  _windCutoff.tick(12, 110, period >> 1);
  _windAmp = ((uint16_t)_windSwell.value * _params[PARAM_WIND]) >> 5;

  // Chimes.
  for(uint8_t i = 0; i < CHIME_VOICES; i++) {
    ChimeVoice& voice = _chime[i];
    voice.envelope -= voice.envelope >> voice.decayShift;
    if(voice.envelope < 256) {
      voice.envelope = 0;
    }
    voice.amp = voice.envelope >> 8;
  }
  uint8_t density = _params[PARAM_DENSITY];
  if(density != 0) {
    if(--_chimeTicks == 0) {
      strikeChime();
      // Next strike after 25..175% of the mean interval.
      uint16_t mean = (uint16_t)pgm_read_byte(&CHIME_INTERVAL_DS[density]) * CONTROL_HZ / 10;
      _chimeTicks = (mean >> 2) + (uint16_t)(((uint32_t)mean * 3 / 2 * randomByte()) >> 8) + 1;
    }
  } else {
    _chimeTicks = 1;
  }

  uint8_t echo = _params[PARAM_ECHO];
  _echoFeedback = echo * 11;   // up to ~0.65
  _echoWet = echo * 16;

  _master = _params[PARAM_LEVEL] * 17;
}

int16_t Soundscape::next() {
  // Pad: three saws -> resonant lowpass -> slow swell. Up to about +-800.
  int16_t pad = (int16_t)_pad[0].next() + _pad[1].next() + _pad[2].next();
  pad = _padFilter.next(pad >> 1);
  pad = mulShr8(pad, _padSwell.value) << 1;

  // Wind: xorshift noise -> moving lowpass, minus a fixed ~80 Hz lowpass to
  // drop the rumble. Up to about +-500.
  uint16_t n = _noise;
  n ^= n << 7;
  n ^= n >> 9;
  n ^= n << 8;
  _noise = n;
  int16_t white = (int16_t)(int8_t)(n >> 8) << 4;
  _windLow += mulShr8(white - _windLow, _windCutoff.value);
  _windRumble += (_windLow - _windRumble) >> 5;
  int16_t wind = mulShr8(_windLow - _windRumble, _windAmp);

  // Chimes. Silent voices cost nothing.
  int16_t chimes = 0;
  for(uint8_t i = 0; i < CHIME_VOICES; i++) {
    ChimeVoice& voice = _chime[i];
    if(voice.amp != 0) {
      int16_t s = (int16_t)voice.partial1.next() + (voice.partial2.next() >> 1);
      chimes += mulShr8(s, voice.amp);
    }
  }
  chimes <<= 1;

  // Echo of the chimes, running at 1/8 rate: averages 8 samples in, holds
  // the delayed sample out through a one-pole smoother.
  _echoSum += chimes >> 3;
  if(++_echoPhase == ECHO_DECIMATION) {
    _echoPhase = 0;
    int8_t delayed = _echo[_echoIndex];
    int16_t in = (_echoSum >> 3) + mulShr8(delayed, _echoFeedback);
    if(in > 127) {
      in = 127;
    } else if(in < -127) {
      in = -127;
    }
    _echo[_echoIndex] = in;
    _echoIndex = (_echoIndex + 1) & (ECHO_LENGTH - 1);
    _echoSum = 0;
    _echoTarget = (int16_t)delayed << 3;
  }
  _echoOut += (_echoTarget - _echoOut) >> 3;
  int16_t echo = mulShr8(_echoOut, _echoWet);

  int16_t mix = pad + wind + chimes + echo;
  return mulShr8(mix, _master);
}

uint8_t Soundscape::getParam(uint8_t id) const {
  return _params[id];
}

uint8_t Soundscape::getParamMax(uint8_t id) const {
  return id == PARAM_MOOD ? SCALE_COUNT - 1 : PARAM_MAX;
}

void Soundscape::setParam(uint8_t id, uint8_t value) {
  uint8_t max = getParamMax(id);
  if(value > max) {
    value = max;
  }
  if(_params[id] == value) {
    return;
  }
  _params[id] = value;
  if(id == PARAM_MOOD) {
    // Keep the chord degree inside the new scale and glide the pad there.
    uint8_t length = Scales::length(value);
    if(_chordDegree > (int8_t)length) {
      _chordDegree = length;
    }
    setChordTargets();
  }
}

bool Soundscape::isHeld() const {
  return _held;
}

void Soundscape::setHeld(bool held) {
  _held = held;
}

uint8_t Soundscape::getRoot() const {
  return _root % 12;
}

uint8_t Soundscape::getStrikeCount() const {
  return _strikeCount;
}
