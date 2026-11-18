# Development

This page covers building and flashing the firmware, debugging it, the melody converter
in detail, and how the code is laid out. For the reasoning behind the design, see
[DESIGN.md](DESIGN.md). For the measured figures, see [MEASUREMENTS.md](MEASUREMENTS.md).

## How it works

MIDI OUT is wired to the UART's TX pin, which has no PWM. The UART runs at 1.25 Mbaud,
and each 10-bit frame carries 0 to 8 one-bits, which makes a 9-level pulse. Eight frames
make one sample, for a 15625 Hz sample rate. Noise shaping and dither move the
quantization noise out of the audio band.

USB MIDI can't run while audio plays, so the Midiboy works as a standalone sound source.

## Set up the development environment

1. Install Arduino IDE 2.x or a standalone
   [`arduino-cli`](https://arduino.github.io/arduino-cli/).
1. Add the Blokas board package URL. In the IDE, it goes under **File > Preferences >
   Additional boards manager URLs**:
   `https://blokas.io/midiboy/package_blokas_labs_index.json`.
1. In the Boards Manager, install **Blokas AVR Boards**. The project was developed
   against version 1.0.6.
1. Select the **Midiboy** board (`Blokas:avr:midiboy`).

The sketch uses only the `MidiboyDisplay` and `MidiboyInput` libraries from the board
package.

The commands below assume `arduino-cli` is on your `PATH`. Arduino IDE 2.x includes one
in `<IDE install folder>/resources/app/node_modules/arduino-ide-extension/build/`.

## Build

From the repository root, run:

```sh
arduino-cli compile -b Blokas:avr:midiboy --warnings all --output-dir Build MidiboySoundscape
```

The project's own code builds without warnings. The few warnings that you see come from
the Blokas libraries `SH1106.cpp` and `usbmidi_vusb.cpp`, and are harmless.

## Flash the Midiboy

1. Put the Midiboy into bootloader mode and connect it over USB. In bootloader mode, it
   appears as a USBasp programmer (VID `16C0`, PID `05DC`). On Windows, the USBasp needs
   a libusb or WinUSB driver, which you can install with [Zadig](https://zadig.akeo.ie/).
1. [Build](#build) the sketch, then run:

   ```sh
   arduino-cli upload -b Blokas:avr:midiboy -p usb -P usbasp --input-dir Build MidiboySoundscape
   ```

   A successful upload prints nothing. If the upload can't find the USBasp, the Midiboy
   has left bootloader mode. Enter it again and repeat the upload.

In the IDE, **Sketch > Upload Using Programmer** does the same, with **USBasp** as the
programmer.

Flashing replaces the sketch on the device, but keeps the bootloader.

## Melody converter

`tools/mid2melody.py` turns a MIDI file into a melody table. It needs only Python 3:

```sh
python tools/mid2melody.py song.mid
```

The script writes `MidiboySoundscape/src/melody/MelodyLocal.h`, which the build uses
instead of the default phrase. To go back to the default phrase, delete `MelodyLocal.h`
and rebuild.

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

## Debug

- To show the CPU meter, set `#define SHOW_METER 1` in `MidiboySoundscape.ino`. The title
  row then starts with a reading like `C1113 D  0`, updated four times a second:
  - `C` is the number of CPU cycles per sample spent on audio, averaged over 250 ms.
    The budget is 1280. Above about 1150, too little time is left for the screen.
  - `D` is the number of dropouts since start-up or since you last pressed **A**. It
    should stay at 0.
- To hear or measure one layer alone, set `PLAY_LAYERS` in `MidiboySoundscape.ino`, for
  example to `LAYER_CHIMES` or `LAYER_CHIMES | LAYER_ECHO`. Set it back to `LAYER_ALL`
  afterwards.
- `Serial` isn't available, because the audio uses the UART. Use the screen for output.

The audio path has a tight CPU budget. Keep function calls and 32-bit math out of
`Soundscape::next()`, and check the meter after any change to it.

## Licensing of derived code

Some parts are derived from [Mozzi](https://github.com/sensorium/Mozzi) and stay under
LGPL-2.1-or-later: the multiply and the sine table in `src/dsp/Dsp.*`, and the resonant
filter in `src/engine/Soundscape.*`. They are marked in the source.

`third_party/` isn't built. It contains Mozzi code under LGPL-2.1-or-later, and FixMath
code under LGPL-3.0. For details, see [third_party/README.md](../third_party/README.md).