# Plan

Midiboy Soundscape is a random soundscape generator for the Blokas Midiboy (ATmega328P at
20 MHz). It sends audio out of the DIN MIDI OUT port, and a DIN-to-jack adapter carries
it to a line input. The project follows the conventions of `../Sequencer`: a sketch folder
with `src/`, `.vscode/arduino.json`, an `arduino-cli` build into `Build/`, and upload over
USBasp.

This document records the design decisions and the roadmap. For the figures behind them,
see [MEASUREMENTS.md](MEASUREMENTS.md).

## Design decisions

| Decision | Reason |
|---|---|
| Audio comes from the UART TX pin (PD1). | MIDI OUT is wired to `Serial` TX, and PD1 has no hardware PWM. |
| The UART runs in asynchronous 8N1 mode at 1.25 Mbaud, not in SPI master (MSPIM) mode. | MSPIM drives XCK (PD4), which is the Midiboy's USB pull-up pin. |
| Each 10-bit frame is a 9-level PWM cell, with 0 to 8 one-bits in the data bits. | The start and stop bits are fixed, so each frame can only set one average level. |
| Second-order noise shaping per frame, 8 frames per sample, for a 15625 Hz sample rate. | It matches the noise of standard 8-bit PWM. Shaping each bit instead does much worse because of the fixed start and stop bits. |
| The encoder is an 8-bit noise shaper in assembly, unrolled for 8 frames, with a 256-byte quantizer table in flash. | The C version took a third of the CPU on its own and starved the main loop. The 8-bit version matches the noise of a 16-bit one. |
| TPDF dither is on by default. | Without dither, the noise shaper whistles on silence. A steady hiss is preferred over occasional whistles. |
| The UART interrupt is naked assembly. | It runs at 125 kHz, so it has to be minimal. |
| The mix peaks at −12 dB of full scale. | Full scale clips the test line input. A cable attenuator would keep more SNR, but isn't built yet (see [Roadmap](#roadmap)). |
| No Mozzi. | Only a few small pieces of Mozzi were still in use, and its power-of-two sample rate needed a pitch correction. Reusable Mozzi and FixMath code is kept, unbuilt, in `third_party/`. |
| The sketch uses `MidiboyDisplay` and `MidiboyInput` directly, without Arduboy2 or the `Midiboy` global. | This avoids the 1 KB frame buffer, and `Serial` is never linked, so the UART interrupt vector stays free. |
| No EEPROM use. | The EEPROM holds the Sequencer's pattern slots on the same device. |
| No USB MIDI while audio plays. | The V-USB interrupt outranks the UART and blocks for tens of microseconds, so frames would be missed. |

### Lessons from development

- **Keep the audio ring full.** The main loop refills the ring completely before each UI
  step. When it made at most 32 samples per call, at high load the ring gained less than
  a screen row draw drained, and audio dropped out in time with the screen.
- **No function calls or 32-bit math in the audio path.** The pad and chimes use
  `SawOsc` and `SineOsc`, which are fully inlined. Chime voices are unrolled, and the wind
  runs at half rate.
- **`micros()` is wrong at 20 MHz.** It runs 6.25% slow. Use Timer1 to count cycles
  instead.
- **The library swaps Up and Down.** `MidiboyInput`'s `BUTTON_UP` and `BUTTON_DOWN` are
  the reverse of the physical buttons, so the sketch swaps them back.
- **The random seed survives resets without EEPROM.** A `.noinit` variable holds the
  generator state across resets, and each button press mixes in its Timer1 timing.

## Roadmap

### Done

- Audio output through the UART, with noise shaping and dither, confirmed on hardware.
- Soundscape engine with four layers: pad, wind, chimes, and echo. All layers run
  together without dropouts.
- Parameter screen: mode, **MOOD**, **DENSITY**, **DRIFT**, **TONE**, **WIND**, **ECHO**,
  and **LEVEL**. **A** starts a new scene, and **B** holds the chord.
- Melody mode: `tools/mid2melody.py` converts a MIDI file into a melody table that plays on
  a loop. Without a converted file, a built-in original phrase plays.
- Optional CPU and dropout meter (`SHOW_METER`).
- [README](README.md) and [measurements](MEASUREMENTS.md).
- Layers tuned by ear on hardware, each alone through `PLAY_LAYERS` in the sketch. The
  echo's wet level now goes up to 1.0 instead of 0.47, because the 1/8-rate line made it
  barely audible. With every parameter at maximum, the mix doesn't clip audibly.

### Later

- Build the cable attenuator and filter, then raise the level in code:
  tip → 3.3 kΩ → node (1 kΩ in parallel with 33 nF to ground) → 4.7 kΩ → 4.7 nF to
  ground → line input.
- Save settings, in an EEPROM area that doesn't collide with the Sequencer's slots.
- Add layers, if the CPU allows.
