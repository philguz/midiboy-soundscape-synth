# Third-party parts

The reusable pieces of the two libraries this project used up to step 3, kept for
later porting. **They are not compiled.** The sketch only builds what is under
`MidiboySoundscape/`. To use a part, copy it (or the part of it you need) into
`MidiboySoundscape/src/`, adapt it, and keep its licence notice (see below).

| Folder | Library | Version | Source | Licence |
|---|---|---|---|---|
| `mozzi/` | Mozzi | 2.0.4 | https://github.com/sensorium/Mozzi | LGPL-2.1-or-later (`mozzi/LICENSE.TXT`, `mozzi/LICENCE.LGPL`) |
| `fixmath/` | FixMath | 1.0.9 | https://github.com/tomcombriat/FixMath | LGPL-3.0 (`fixmath/LICENSE.txt`) |

The files are verbatim copies. Mozzi's core (`Mozzi.h`, `MozziGuts*`, `config/`,
`AudioOutput.h`, `hardware_defines.h`, the timer utilities) and its hardware I/O
(`mozzi_analog.h`, `twi_nonblock.h`, `RCpoll.h`, `CapPoll.h`) are left out: they
drive Mozzi's own audio output, which this project replaces.

## Licence notes

- Code ported from here stays under its licence. Mark it in the source, as
  `src/dsp/Dsp.*` does for the pieces already in use (16×8 multiply, sine table).
- LGPL in a statically linked firmware: whoever receives the firmware must be
  able to get the source of the LGPL parts and relink with a changed version.
  For an open-source or personal project that's met by publishing the source.

## Porting notes

- Many headers include `MozziHeadersOnly.h` or `hardware_defines.h` and use
  `MOZZI_AUDIO_RATE` / `MOZZI_CONTROL_RATE` as template defaults. Ported code
  should use `AUDIO_RATE` (15625, the real rate) and `CONTROL_HZ` from
  `src/dsp/Dsp.h` instead. The tables and samples were made for Mozzi's 16384 Hz:
  a sample plays 4.6 % slow (0.8 semitones flat) unless its step is scaled.
- `Oscil`, `Line`, `Smooth`, `MetaOscil` and `mozzi_midi.h` use FixMath types
  (`UFix`, `SFix`); the rest only need `mozzi_fixmath.h` (`Q16n16` etc.).
- Budget at the end of step 3 (all layers on): ~160 cycles per sample spare of
  1280, ~18 KB flash free, ~1 KB RAM free. Mozzi's generic classes are often
  slower than a hand-tuned AVR version (`ResonantFilter` took about twice the
  cycles of our `ResoLowpass`), so port the algorithm, not the class, for
  anything that runs per sample. Control-rate code can be taken as is.

## What's here, and how well it fits

**Cheap per sample**
- `tables/waveshape_*`: `tanh`, `sigmoid`, two soft clips, Chebyshev 3rd–6th
  (256 B each), plus a 512-to-488 compressor. One lookup, ~10 cycles. Use: a soft
  ceiling instead of the hard clamp, extra harmonics on the chimes.
- `tables/`, oscillator shapes: sine/cos (256 B to 8 KB), saw, square, triangle,
  "analogue" saw/square/triangle (512 B), warm/valve/distorted triangles,
  `chum` (vocal-ish), `phasor`, `halfsin`, windows, and band-limited
  saw/square/triangle sets in `BandLimited_*` (for `MetaOscil`). A lookup costs
  about what our `SineOsc` does.
- `samples/bamboo/`: 11 bamboo-chime hits, 2 KB each (six of them also as 4 KB versions). A
  natural fit for the chimes layer. Larger ones (`burroughs1` 18 KB, `abomb`
  16 KB, `raven_arh`) don't fit the flash. `thumbpiano_huffman/` and
  `a-bomb-huffman.h` are Huffman-compressed for `SampleHuffman` (more cycles,
  less flash).

**Control rate (cheap to run as is)**
- `ADSR`, `Ead` (exponential attack/decay), `Line`, `Smooth`, `Portamento`,
  `EventDelay`, `Metronome`, `ControlDelay`, `RollingAverage`, `RollingStat`,
  `IntMap`, `AutoMap`, `AutoRange`, `Stack`, `CircularBuffer`, `primes.h`.

**Per sample, too costly as written (port the idea at a reduced rate)**
- `ReverbTank`: small Schroeder-style reverb (allpasses plus a recirculating
  delay, 16-bit lines). Best candidate for the echo layer at 1/8 rate.
- `StateVariable`: lowpass / highpass / bandpass / notch.
- `ResonantFilter` (also multi-output), `LowPassFilter`, `DCfilter`.
- `AudioDelay`, `AudioDelayFeedback` (interpolated, allpass mode).
- Voices: `WavePacket` / `WavePacketSample` (FOF formants, "aah"), `PDResonant`
  (phase distortion), `WaveFolder`, `WaveShaper`, `MetaOscil`, `Oscil`,
  `Phasor`, `Sample`, `SampleHuffman`, `OverSample`.

**Support**
- `mozzi_fixmath.h/.cpp` (fixed-point types, `Q16n16_mtof`), `mozzi_midi.h`
  (`mtof`), `mozzi_rand.h` + `internal/mozzi_rand_p.h` (xorshift96),
  `mult16x8.h`, `mult16x16.h`, `mult32x16.h` (AVR asm multiplies), `mozzi_utils.h`,
  `mozzi_pgmspace.h`, `IntegerType.h`, `meta.h`, `internal/mozzi_macros.h`.
- `fixmath/src/`: `FixMath.h` (`UFix` / `SFix` templates), `FixMathMapper.h`.
