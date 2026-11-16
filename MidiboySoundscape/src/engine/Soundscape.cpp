#include "Soundscape.h"
#include "Scales.h"
#include "../audio/UartAudioOut.h"
#include "../melody/Melody.h"

#define MELODY_OFF 0xFFFF
// Melody bells: octave partial, decay time constant 2^7 ticks (~1 s).
#define MELODY_RATIO 512
#define MELODY_DECAY_SHIFT 7
// Pad glide while the melody plays: 2^3 ticks (~65 ms) instead of ~0.5 s.
#define MELODY_GLIDE_SHIFT 3
#define SCENE_GLIDE_SHIFT 6

static const uint8_t DEFAULT_PARAMS[PARAM_COUNT] = {
  MODE_SCAPE,
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

// One sample of a chime voice. Silent voices cost nothing.
static inline __attribute__((always_inline)) int16_t chimeSample(ChimeVoice& voice) {
  if(voice.amp == 0) {
    return 0;
  }
  int16_t s = (int16_t)voice.partial1.next() + (voice.partial2.next() >> 1);
  return mulShr8(s, voice.amp);
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
    target = low + Rng::below(high - low + 1);
  }
}

// Mozzi's ResonantFilter formula: feedback = q + q * (1 - cutoff), 8.8
// fixed point, so 0..~2.0. Up to 1.0 is mild; resonance >= 128 at low
// cutoffs goes above.
void ResoLowpass::set(uint8_t newCutoff, uint8_t resonance) {
  cutoff = newCutoff;
  feedback = resonance + (((uint16_t)resonance * (uint8_t)(255 - newCutoff)) >> 8);
}

inline int16_t ResoLowpass::next(int16_t in) {
  int16_t band = buf0 - buf1;
  // band * feedback / 256 with a 16x8 multiply: the high byte of feedback
  // is 0 or 1 (feedback < 512).
  int16_t resonance = mulShr8(band, (uint8_t)feedback);
  if(feedback & 0x100) {
    resonance += band;
  }
  int16_t drive = in - buf0 + resonance;
  buf0 += mulShr8(drive, cutoff);
  buf1 += mulShr8(buf0 - buf1, cutoff);
  return buf1;
}

void Soundscape::begin(uint32_t seed) {
  Rng::seed(seed);
  for(uint8_t i = 0; i < PARAM_COUNT; i++) {
    _params[i] = DEFAULT_PARAMS[i];
  }
  _layers = LAYER_ALL;
  _held = false;
  _strikeCount = 0;
  _melodyRow = MELODY_OFF;
  _melodyWait = 0;
  _padGlideShift = SCENE_GLIDE_SHIFT;
  _chimeTicks = CONTROL_HZ * 2;

  for(uint8_t i = 0; i < PAD_VOICES; i++) {
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
  _windHalf = 0;
  _windOut = 0;
  _windCutoff.value = 80;
  _windCutoff.target = 80;
  _windCutoff.wait = 0;
  _windSwell.value = 0;
  _windSwell.target = 128;
  _windSwell.wait = 0;

  for(uint8_t i = 0; i < CHIME_VOICES; i++) {
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

  newScene();   // plays the melody in melody mode
  // Start every pad voice on its note instead of gliding in from 0 Hz.
  for(uint8_t i = 0; i < PAD_VOICES; i++) {
    _padStep[i] = _padTargetStep[i];
  }
}

void Soundscape::newScene() {
  if(_params[PARAM_MODE] == MODE_MELODY) {
    playMelody();
    return;
  }
  stopMelody();
  _root = 36 + Rng::below(12);
  _chordDegree = Rng::below(Scales::length(_params[PARAM_MOOD]));
  setChordTargets();
  _chordTicks = 1;   // reschedule on the next tick
  nextChord();
}

// Pad notes for the current chord: the chord root in the bass, two more
// chord tones (degree +2 and +4, a triad in 7-note scales) an octave up.
void Soundscape::setChordTargets() {
  uint8_t scale = _params[PARAM_MOOD];
  _padTargetStep[0] = noteStep(Scales::note(scale, _root, _chordDegree));
  _padTargetStep[1] = noteStep(Scales::note(scale, _root + 12, _chordDegree + 2));
  _padTargetStep[2] = noteStep(Scales::note(scale, _root + 12, _chordDegree + 4));
}

void Soundscape::nextChord() {
  if(--_chordTicks != 0) {
    return;
  }
  // Step the chord root by up to 3 degrees, staying near the scene root.
  int8_t step = (int8_t)Rng::below(6) - 3;
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
  _chordTicks = (base >> 1) + (uint16_t)(((uint32_t)base * Rng::byte()) >> 8);
}

void Soundscape::strikeChime() {
  uint8_t scale = _params[PARAM_MOOD];
  uint8_t length = Scales::length(scale);
  // Degrees across two octaves, starting an octave above the pad's top.
  uint8_t note = Scales::note(scale, _root + 12, length + Rng::below(length * 2));
  uint16_t ratio = pgm_read_word(&CHIME_RATIOS[Rng::below(4)]);
  strikeNote(note, 150 + Rng::below(106), ratio, 6 + Rng::below(3));
}

// Strikes the quieter voice, so a ringing note is cut only by a newer one.
void Soundscape::strikeNote(uint8_t note, uint8_t level, uint16_t ratio, uint8_t decayShift) {
  ChimeVoice& voice = _chime[0].envelope <= _chime[1].envelope ? _chime[0] : _chime[1];
  uint32_t step = noteStep(note);
  // The envelope and its level byte change together, at control rate, which
  // never runs inside next(): no torn reads.
  voice.partial1.setPhase(0);
  voice.partial2.setPhase(0);
  voice.partial1.setStep(step);
  voice.partial2.setStep((step >> 8) * ratio);
  voice.envelope = (uint16_t)level << 8;
  voice.amp = level;
  voice.decayShift = decayShift;
  _strikeCount++;
}

// Root, octave and fifth above, bass folded into the pad's range (36..47).
void Soundscape::setPadBass(uint8_t note) {
  while(note < 36) {
    note += 12;
  }
  while(note > 47) {
    note -= 12;
  }
  _padTargetStep[0] = noteStep(note);
  _padTargetStep[1] = noteStep(note + 12);
  _padTargetStep[2] = noteStep(note + 19);
}

void Soundscape::playMelody() {
  // The scene carries on in the melody's key.
  _params[PARAM_MOOD] = MELODY_MOOD < SCALE_COUNT ? MELODY_MOOD : 0;
  _root = 36 + MELODY_ROOT;
  _chordDegree = 0;
  setChordTargets();
  _melodyRow = 0;
  // Counted down before the first row is read, in stepMelody().
  _melodyWait = (uint16_t)pgm_read_byte(&MELODY_DATA[0]) + 1;
  _padGlideShift = MELODY_GLIDE_SHIFT;
}

void Soundscape::stopMelody() {
  if(_melodyRow == MELODY_OFF) {
    return;
  }
  _melodyRow = MELODY_OFF;
  _padGlideShift = SCENE_GLIDE_SHIFT;
  // The scene starts gently: a few seconds before the first random chime
  // and chord change.
  _chimeTicks = CONTROL_HZ * 3;
  _chordTicks = CONTROL_HZ * 6;
}

// Once per control tick while the melody plays: plays every row that is due.
void Soundscape::stepMelody() {
  if(--_melodyWait != 0) {
    return;
  }
  while(true) {
    const uint8_t* row = &MELODY_DATA[_melodyRow * MELODY_ROW_SIZE];
    uint8_t note = pgm_read_byte(&row[1]);
    if(note & MELODY_NOTE_PAD) {
      setPadBass(note & 0x7F);
    } else if(note != MELODY_NOTE_REST) {
      strikeNote(note, pgm_read_byte(&row[2]), MELODY_RATIO, MELODY_DECAY_SHIFT);
    }
    if(++_melodyRow >= MELODY_ROWS) {
      // Again from the top (the table ends with a ring-out rest).
      _melodyRow = 0;
      _melodyWait = pgm_read_byte(&MELODY_DATA[0]);
      if(_melodyWait == 0) {
        _melodyWait = 1;
      }
      return;
    }
    _melodyWait = pgm_read_byte(&row[MELODY_ROW_SIZE]);
    if(_melodyWait != 0) {
      return;
    }
  }
}

// Ticks between Wander steps: 15 at DRIFT 0, 0 at DRIFT 15.
uint8_t Soundscape::driftPeriod() const {
  return PARAM_MAX - _params[PARAM_DRIFT];
}

void Soundscape::updateControl() {
  uint8_t period = driftPeriod();

  bool melody = _melodyRow != MELODY_OFF;
  if(melody) {
    stepMelody();
  } else if(!_held) {
    nextChord();
  }

  // Pad: glide to the chord notes (time constant ~0.5 s), each voice with
  // its own slow detune of up to +-0.3% (~5 cents) for a chorus beat.
  for(uint8_t i = 0; i < PAD_VOICES; i++) {
    int32_t diff = (int32_t)(_padTargetStep[i] - _padStep[i]);
    _padStep[i] += diff >> _padGlideShift;
    _padDetune[i].tick(0, 24, period + 8);
    int8_t wobble = (int8_t)_padDetune[i].value - 12;
    _pad[i].setStep(_padStep[i] + (int32_t)(_padStep[i] >> 12) * wobble);
  }

  uint8_t tone = _params[PARAM_TONE];
  _padCutoff.tick(0, 60, period);
  _padFilter.set(30 + tone * 9 + _padCutoff.value, 60 + tone * 8);
  _padSwell.tick(110, 230, period + 4);

  // Wind: gusts in level and colour.
  _windSwell.tick(30, 255, period >> 1);
  // Coefficients for the half-rate wind: twice the full-rate ones.
  _windCutoff.tick(24, 220, period >> 2);
  // At half rate the same cutoff passes sqrt(2) times more noise: x0.71.
  _windAmp = ((uint32_t)_windSwell.value * _params[PARAM_WIND] * 181) >> 13;

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
  if(melody) {
    // Random chimes wait; stopMelody() sets when they start again.
  } else if(density != 0) {
    if(--_chimeTicks == 0) {
      strikeChime();
      // Next strike after 25..175% of the mean interval.
      uint16_t mean = (uint16_t)pgm_read_byte(&CHIME_INTERVAL_DS[density]) * CONTROL_HZ / 10;
      _chimeTicks = (mean >> 2) + (uint16_t)(((uint32_t)mean * 3 / 2 * Rng::byte()) >> 8) + 1;
    }
  } else {
    _chimeTicks = 1;
  }

  uint8_t echo = _params[PARAM_ECHO];
  _echoFeedback = echo * 11;   // up to ~0.65
  // Up to 1.0: the 1/8-rate line and its smoother already take 10..20 dB
  // off most chime notes.
  _echoWet = echo * 17;

  _master = _params[PARAM_LEVEL] * 17;   // 255 = x1.0
}

// Levels before the master, worst case / typical: pad +-350 / 200 (simulated),
// wind +-420 / 100 (WIND 15), chimes +-380 / 150, echo +-450 / 80 (ECHO 15).
// Those peaks almost never line up, so the sum is clipped, not scaled down.
int16_t Soundscape::next() {
  int16_t mix = 0;

  // Pad: three saws (+-384) -> resonant lowpass -> slow swell (up to 0.9).
  // Simulated peak 120..350 depending on TONE; the filter never overflows.
  if(_layers & LAYER_PAD) {
    int16_t pad = (int16_t)_pad[0].next() + _pad[1].next() + _pad[2].next();
    pad = _padFilter.next(pad);
    mix += mulShr8(pad, _padSwell.value);
  }

  // Wind: xorshift noise -> moving lowpass, minus a fixed ~80 Hz lowpass to
  // drop the rumble. Runs at half rate (its cutoff stays under ~1.3 kHz):
  // every other sample makes a new value and plays the average of the old
  // and new one, the sample between plays the new one.
  if(_layers & LAYER_WIND) {
    _windHalf ^= 1;
    if(_windHalf) {
      uint16_t n = _noise;
      n ^= n << 7;
      n ^= n >> 9;
      n ^= n << 8;
      _noise = n;
      int16_t white = (int16_t)(int8_t)(n >> 8) << 3;
      _windLow += mulShr8(white - _windLow, _windCutoff.value);
      _windRumble += mulShr8(_windLow - _windRumble, 16);   // >> 4, without a shift loop
      int16_t wind = mulShr8(_windLow - _windRumble, _windAmp);
      mix += (_windOut + wind) >> 1;
      _windOut = wind;
    } else {
      mix += _windOut;
    }
  }

  // Chimes.
  int16_t chimes = 0;
  if(_layers & LAYER_CHIMES) {
    // Written out per voice, not looped: with a fixed voice the compiler
    // reaches its fields by address instead of through a pointer
    // (~60 cycles a voice instead of ~100).
    static_assert(CHIME_VOICES == 2, "chime voices are unrolled");
    chimes = chimeSample(_chime[0]) + chimeSample(_chime[1]);
    mix += chimes;
  }

  // Echo of the chimes, running at 1/8 rate: averages 8 samples in, holds
  // the delayed sample out through a one-pole smoother. Stored as chimes / 4
  // in 8 bits.
  if(_layers & LAYER_ECHO) {
    _echoSum += mulShr8(chimes, 32);   // >> 3, without a shift loop
    if(++_echoPhase == ECHO_DECIMATION) {
      _echoPhase = 0;
      int8_t delayed = _echo[_echoIndex];
      int16_t in = (_echoSum >> 2) + mulShr8(delayed, _echoFeedback);
      if(in > 127) {
        in = 127;
      } else if(in < -127) {
        in = -127;
      }
      _echo[_echoIndex] = in;
      _echoIndex = (_echoIndex + 1) & (ECHO_LENGTH - 1);
      _echoSum = 0;
      _echoTarget = (int16_t)delayed << 2;
    }
    _echoOut += mulShr8(_echoTarget - _echoOut, 32);
    mix += mulShr8(_echoOut, _echoWet);
  }

  mix = mulShr8(mix, _master);
  if(mix > SOUNDSCAPE_PEAK) {
    mix = SOUNDSCAPE_PEAK;
  } else if(mix < -SOUNDSCAPE_PEAK) {
    mix = -SOUNDSCAPE_PEAK;
  }
  return mix;
}

uint8_t Soundscape::getLayers() const {
  return _layers;
}

void Soundscape::setLayers(uint8_t layers) {
  _layers = layers & LAYER_ALL;
}

uint8_t Soundscape::getParam(uint8_t id) const {
  return _params[id];
}

uint8_t Soundscape::getParamMax(uint8_t id) const {
  if(id == PARAM_MODE) {
    return MODE_MELODY;
  }
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
  if(id == PARAM_MODE) {
    if(value == MODE_MELODY) {
      playMelody();
    } else {
      stopMelody();
    }
  } else if(id == PARAM_MOOD && _melodyRow == MELODY_OFF) {
    // (While the melody plays, the pad follows its bass instead.)
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
