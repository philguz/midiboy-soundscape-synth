# Midiboy Soundscape

Midiboy Soundscape turns the [Blokas Midiboy](https://blokas.io/midiboy) into a random
ambient soundscape generator. It makes audio, not MIDI: the sound comes out of the DIN
MIDI OUT port, and a DIN-to-jack adapter carries it to a line input.

Features:

- Four layers: a pad of detuned saws with a resonant lowpass filter and wandering chords,
  wind made of filtered noise with gusts, chimes on two bell voices, and an echo on the
  chimes.
- Eight scales, plus controls for density, drift, tone, wind, echo, and level.
- A new random scene at every start-up and every time you press **A**.
- Melody mode, which plays a melody on a loop. You can convert your own MIDI file with
  `tools/mid2melody.py`.
- No EEPROM use, so the Sequencer's saved patterns stay intact.

## How it works

MIDI OUT is wired to the UART's TX pin, which has no PWM. The UART runs at 1.25 Mbaud,
and each 10-bit frame carries 0 to 8 one-bits, which makes a 9-level pulse. Eight frames
make one sample, for a 15625 Hz sample rate. Noise shaping and dither move the
quantization noise out of the audio band.

USB MIDI can't run while audio plays, so the Midiboy works as a standalone sound source.

For the design decisions, see [PLAN.md](PLAN.md). For the figures behind them, see
[MEASUREMENTS.md](MEASUREMENTS.md).

## Connect the Midiboy

You need a TRS MIDI adapter, Type A, from DIN to a 3.5 mm jack:

| DIN pin | Jack | Signal |
|---|---|---|
| 5 | Tip | Audio |
| 2 | Sleeve | Ground |
| 4 | Ring | +5 V DC through the Midiboy's MIDI resistor; no audio |

Plug the jack into a line input, such as an audio interface, mixer, or recorder. The audio
is on the tip, so a stereo input receives it on the left channel.

> **Caution:** Don't connect headphones directly. They need an amplifier and DC blocking.

At full scale, the output is about 3 Vpp, which is too hot for most line inputs. The
firmware keeps the peak at −12 dB of full scale, and **LEVEL** stops there.

## Controls

The screen has a title row, followed by one row for each parameter. The title row shows the
mode, the root note, `HOLD` while the chord is held, and a `*` that flashes on every
bell strike.

| Button | Action |
|---|---|
| **Up**, **Down** | Select the mode (the title row) or a parameter. The selected name appears inverted. |
| **Left**, **Right** | Change the selected item. Parameters repeat while you hold the button. |
| **A** | In SOUNDSCAPE mode, start a new scene with a new root note and chord. In MELODY mode, restart the melody. |
| **B** | Hold the current chord. Everything else keeps drifting. |

| Parameter | Range | Effect |
|---|---|---|
| Mode | SOUNDSCAPE, MELODY | SOUNDSCAPE is the generative scene and the default at start-up. MELODY plays the melody on a loop. |
| **MOOD** | 8 scales | The scale: PENTA MIN, PENTA MAJ, DORIAN, LYDIAN, AEOLIAN, HIRAJOSHI, IN SEN, or WHOLE TONE. The pad glides into the new scale. |
| **DENSITY** | 0 to 15 | How often the chimes strike: never at 0, about every 20 seconds at 1, and about every 0.4 seconds at 15. |
| **DRIFT** | 0 to 15 | How fast the filter, swells, gusts, and detune wander, and how often the chord changes: about every 30 seconds at 0, and every 8 seconds at 15. |
| **TONE** | 0 to 15 | The brightness and resonance of the pad filter. |
| **WIND** | 0 to 15 | The wind level. 0 turns the wind off. |
| **ECHO** | 0 to 15 | The chime echo level and feedback. 0 turns the echo off. |
| **LEVEL** | 0 to 15 | The master volume. 15 is −12 dB of full scale. |

Settings aren't saved. Each start-up begins with the default settings and a new random
scene.

### Melody mode

When you select MELODY, the scene switches to the melody's key, and the melody plays on
the bells while its low notes steer the pad. The melody loops, with a short rest at the
end. Random chimes and chord changes pause while it plays. When you switch back to
SOUNDSCAPE, the scene continues in the melody's key.

Without a converted melody, a short original phrase plays. To play your own melody:

1. Convert a MIDI file:

   ```sh
   python tools/mid2melody.py song.mid
   ```

   The script writes `MidiboySoundscape/src/melody/MelodyLocal.h`, which the build uses
   instead of the default phrase. It needs only Python 3.

1. [Build](#build) and [flash](#flash-the-midiboy) the sketch.

To go back to the default phrase, delete `MelodyLocal.h` and rebuild.

Git ignores `MelodyLocal.h` and `*.mid` files. Only commit melody tables of music that you
have the rights to.

The converter works as follows:

- Notes at or above G3 play on the two bell voices, at most two at a time. To change the
  split point, use `--split` with a MIDI note number.
- On each beat, the lowest note below the split becomes the pad's bass. To play every
  note on the bells, use `--no-pad`.
- Note lengths are ignored, because bells ring out. Velocities set the strike level.
- The converter guesses the key. To set it, use `--root`, for example `--root F#`, and
  add `--minor` for a minor key.
- A melody can have up to 2000 events.

For all options, run `python tools/mid2melody.py -h`.

## Set up the development environment

Set up the toolchain as for the [Sequencer](../Sequencer/README.md#development-setup):

1. Install Arduino IDE 2.x or a standalone `arduino-cli`.
1. Add the Blokas board package URL:
   `https://blokas.io/midiboy/package_blokas_labs_index.json`.
1. Install **Blokas AVR Boards** 1.0.6.
1. Select the **Midiboy** board (`Blokas:avr:midiboy`).

The sketch uses only the `MidiboyDisplay` and `MidiboyInput` libraries from the board
package.

### Build

From the repository root, run:

```sh
arduino-cli compile -b Blokas:avr:midiboy --warnings all --output-dir Build MidiboySoundscape
```

The project's own code builds without warnings. The few warnings that you see come from
the Blokas libraries `SH1106.cpp` and `usbmidi_vusb.cpp`, and are harmless.

### Flash the Midiboy

1. Put the Midiboy into bootloader mode. It appears on USB as a USBasp programmer. For
   details, see the [Sequencer README](../Sequencer/README.md#deployment-flashing-the-midiboy).
1. Run:

   ```sh
   arduino-cli upload -b Blokas:avr:midiboy -p usb --input-dir Build MidiboySoundscape
   ```

Flashing replaces the sketch on the device, such as the Sequencer.

### Debug

- To show the CPU meter, set `#define SHOW_METER 1` in `MidiboySoundscape.ino`. The title
  row then starts with a reading like `C1113 D  0`, updated four times a second:
  - `C` is the number of CPU cycles per sample spent on audio, averaged over 250 ms.
    The budget is 1280. Above about 1150, too little time is left for the screen.
  - `D` is the number of dropouts since start-up or since you last pressed **A**. It
    should stay at 0.
- To hear or measure one layer alone, turn the others off with
  `Soundscape::setLayers()`.
- `Serial` isn't available, because the audio uses the UART. Use the screen for output.

The audio path has a tight CPU budget. Keep function calls and 32-bit math out of
`Soundscape::next()`, and check the meter after any change to it.

## Repository layout

```
MidiboySoundscape/         Arduino sketch folder; the folder name must match the .ino
  MidiboySoundscape.ino    Main loop, buttons, and random seed
  src/audio/UartAudioOut.* Audio output: ring buffer, encoder, and UART interrupt
  src/dsp/Dsp.*            Note phase steps, random generator, multiply, and sine table
  src/engine/Soundscape.*  Synthesis engine, parameters, and melody player
  src/engine/Scales.*      Scales for MOOD
  src/melody/              Melody tables
  src/ui/ParamScreen.*     Parameter screen
tools/mid2melody.py        MIDI to melody table converter
third_party/               Mozzi and FixMath code for reuse; not built
```

The Arduino builder compiles only the sketch folder's top level and its `src/` directory,
so all code other than the `.ino` file is under `src/`.

## License

The project's own code doesn't have a license yet. `third_party/` contains Mozzi code
under LGPL-2.1 or later, and FixMath code under LGPL-3.0. For details, see
[third_party/README.md](third_party/README.md).
