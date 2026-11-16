# Third-party code

This directory holds reusable parts of the two libraries that early versions of
this project used. The parts are kept for later porting.

**Important:** The build doesn't compile anything in this directory. The sketch
builds only the files under `MidiboySoundscape/`.

To use a part, do the following:

1.  Copy the part, or only the code that you need, into
    `MidiboySoundscape/src/`.
1.  Adapt the code to the project. For details, see
    [Port code to the project](#port-code-to-the-project).
1.  Keep the license notice. For details, see [License notes](#license-notes).

## Libraries

The following table lists the libraries in this directory:

| Directory   | Library | Version | Source                                           | License                                                         |
| ----------- | ------- | ------- | ------------------------------------------------ | --------------------------------------------------------------- |
| `mozzi/`    | Mozzi   | 2.0.4   | [Mozzi on GitHub](https://github.com/sensorium/Mozzi)      | LGPL-2.1-or-later (`mozzi/LICENSE.TXT`, `mozzi/LICENCE.LGPL`) |
| `fixmath/`  | FixMath | 1.0.9   | [FixMath on GitHub](https://github.com/tomcombriat/FixMath) | LGPL-3.0 (`fixmath/LICENSE.txt`)                               |

The files are unmodified copies of the originals. The following parts of Mozzi
are left out, because they drive Mozzi's own audio output, which this project
replaces:

*   The core: `Mozzi.h`, `MozziGuts*`, `config/`, `AudioOutput.h`,
    `hardware_defines.h`, and the timer utilities.
*   The hardware I/O: `mozzi_analog.h`, `twi_nonblock.h`, `RCpoll.h`, and
    `CapPoll.h`.

## License notes

*   Code that you port from this directory stays under its original license.
    Mark the ported code in the source file. For an example, see
    `src/dsp/Dsp.*`, which marks the parts already in use: the 16×8 multiply
    and the sine table.
*   The LGPL applies to statically linked firmware as follows: anyone who
    receives the firmware must be able to get the source of the LGPL parts and
    relink the firmware with a changed version of them. For an open source or
    personal project, publishing the source meets this requirement.

## Port code to the project

*   **Rates.** Many headers include `MozziHeadersOnly.h` or
    `hardware_defines.h`, and use `MOZZI_AUDIO_RATE` and `MOZZI_CONTROL_RATE`
    as template defaults. In ported code, use `AUDIO_RATE` and `CONTROL_HZ`
    from `src/dsp/Dsp.h` instead. `AUDIO_RATE` is 15625 Hz, the real output
    rate.
*   **Sample pitch.** The tables and samples were made for Mozzi's 16384 Hz
    rate. Unless you scale its step, a sample plays 4.6% slow, which is
    0.8 semitones flat.
*   **Fixed-point types.** `Oscil`, `Line`, `Smooth`, `MetaOscil`, and
    `mozzi_midi.h` use the FixMath types `UFix` and `SFix`. The rest of the
    code needs only `mozzi_fixmath.h`, which defines types such as `Q16n16`.
*   **Resource budget.** With all layers on and melody mode in place, the
    following resources are free:
    *   About 160 of 1280 CPU cycles per sample.
    *   About 16 KB of flash memory, or more with a short melody table.
    *   About 1 KB of RAM.
*   **Per-sample code.** Mozzi's generic classes are often slower than a
    hand-tuned AVR version. For example, `ResonantFilter` takes about twice the
    cycles of this project's `ResoLowpass`. For code that runs once per sample,
    port the algorithm, not the class. You can use control-rate code as is.

## Contents

The following sections group the parts by how well they fit the CPU budget.

### Low cost per sample

*   **Waveshaping tables.** `tables/waveshape_*` contains `tanh`, `sigmoid`, two
    soft clips, and Chebyshev polynomials of 3rd to 6th order, at 256 bytes
    each. It also contains a 512-to-488 compressor. Each table costs one lookup,
    about 10 cycles. Possible uses: a soft ceiling instead of the hard clamp,
    and extra harmonics on the chimes.
*   **Oscillator tables.** `tables/` contains the following shapes:
    *   Sine and cosine, from 256 bytes to 8 KB.
    *   Saw, square, and triangle.
    *   "Analog" saw, square, and triangle, at 512 bytes each.
    *   Warm, valve, and distorted triangles.
    *   `chum` (vocal-like), `phasor`, `halfsin`, and windows.
    *   Band-limited saw, square, and triangle sets in `BandLimited_*`, for use
        with `MetaOscil`.

    A lookup costs about the same as this project's `SineOsc`.
*   **Bamboo samples.** `samples/bamboo/` contains 11 bamboo chime hits at 2 KB
    each. Six of them also come in 4 KB versions. These samples are a natural
    fit for the chimes layer.
*   **Other samples.** The larger samples, such as `burroughs1` (18 KB),
    `abomb` (16 KB), and `raven_arh`, don't fit in flash memory.
    `thumbpiano_huffman/` and `a-bomb-huffman.h` are Huffman-compressed for
    `SampleHuffman`, which uses less flash memory but more cycles.

### Control rate

You can use the following classes as is, because they run at control rate:

*   Envelopes: `ADSR` and `Ead` (exponential attack and decay).
*   Ramps and smoothing: `Line`, `Smooth`, and `Portamento`.
*   Timing: `EventDelay`, `Metronome`, and `ControlDelay`.
*   Statistics and mapping: `RollingAverage`, `RollingStat`, `IntMap`,
    `AutoMap`, and `AutoRange`.
*   Containers and utilities: `Stack`, `CircularBuffer`, and `primes.h`.

### High cost per sample

The following classes cost too many cycles to run per sample as written. To use
one, port the idea and run it at a reduced rate.

*   **Reverb.** `ReverbTank` is a small Schroeder-style reverb with allpass
    filters, a recirculating delay, and 16-bit delay lines. It's the best
    candidate for the echo layer, at 1/8 of the audio rate.
*   **Filters.** `StateVariable` provides lowpass, highpass, bandpass, and notch
    outputs. Also available: `ResonantFilter` (including a multi-output
    version), `LowPassFilter`, and `DCfilter`.
*   **Delays.** `AudioDelay` and `AudioDelayFeedback`, which supports
    interpolation and an allpass mode.
*   **Voices.** `WavePacket` and `WavePacketSample` (FOF formants, such as
    "aah"), `PDResonant` (phase distortion), `WaveFolder`, `WaveShaper`,
    `MetaOscil`, `Oscil`, `Phasor`, `Sample`, `SampleHuffman`, and
    `OverSample`.

### Support code

*   **Mozzi math and utilities.** `mozzi_fixmath.h` and `mozzi_fixmath.cpp`
    (fixed-point types and `Q16n16_mtof`), `mozzi_midi.h` (`mtof`),
    `mozzi_rand.h` and `internal/mozzi_rand_p.h` (xorshift96), the AVR assembly
    multiplies `mult16x8.h`, `mult16x16.h`, and `mult32x16.h`, plus
    `mozzi_utils.h`, `mozzi_pgmspace.h`, `IntegerType.h`, `meta.h`, and
    `internal/mozzi_macros.h`.
*   **FixMath.** `fixmath/src/` contains `FixMath.h`, which defines the `UFix`
    and `SFix` templates, and `FixMathMapper.h`.
