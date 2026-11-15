# Midiboy Soundscape: plan

A random soundscape generator for the Blokas Midiboy (ATmega328P @ 20 MHz), built on
Mozzi 2. Audio leaves through the **DIN MIDI OUT** port and reaches a line input through
a DIN-to-jack adapter. Project conventions follow `../Sequencer`: sketch folder plus
`src/`, `.vscode/arduino.json`, arduino-cli build into `Build/`, USBasp upload.

Status: ✅ done · 🟡 drafted, not compiled or tested · ⬜ not started

## Key decisions (and why)

| Decision | Why |
|---|---|
| Audio from the UART TX pin (PD1) | MIDI OUT is wired to `Serial` TX. PD1 has no hardware PWM. |
| UART in **async 8N1 mode at 1.25 Mbaud**, not SPI-master (MSPIM) mode | MSPIM drives XCK = PD4, which is the Midiboy's **USB pull-up pin**. |
| Each 10-bit frame is a 9-level PWM cell (0–8 ones in the data bits) | The start and stop bits are fixed, so each frame sets one average level. |
| 2nd-order noise shaping per frame, 8 frames per sample → **15625 Hz** | Simulated about 44 dB SNR in the 0–8 kHz band, about Mozzi's standard 8-bit PWM. Shaping each bit instead managed only 9–30 dB with the forced start/stop bits. |
| Mozzi `MOZZI_OUTPUT_EXTERNAL_CUSTOM` | No Mozzi timer and no 512 B Mozzi buffer. We own a 256 B ring. Timer1 stays free. |
| Pitch correction ×1.048576 in our code | Mozzi only accepts power-of-two rates (nominal 16384), but the real rate is 15625. |
| Encoder in the main loop, naked-asm UART ISR (~40 cycles per frame) | The ISR runs at 125 kHz, so it has to be minimal. |
| `MidiboyDisplay` and `MidiboyInput` directly, no Arduboy2 or `Midiboy` global | No 1 KB frame buffer, and `Serial` is never linked, so the UART vector stays free. |
| No EEPROM use for now | EEPROM is shared with the Sequencer's pattern slots on the same device. |
| Encoder: 8-bit noise shaper in 1/8 level units, asm unrolled for 8 frames, 256 B flash quantiser table, ±½ level dither | ~21 cycles per frame (the C version took ~70 and starved the loop). Simulated: same noise as the 16-bit version up to −6 dB input. Fixes the rate at 1.25 Mbaud. |
| Our own sample loop instead of Mozzi's `audioHook()` | In custom mode the hook only adds a 64-bit sample counter, ~80 cycles a sample. Mozzi still does the synthesis. |

CPU budget per sample (1280 cycles at 20 MHz), measured in step 2: the fixed part (ISR, encoder,
loop) takes ~36 %, so ~620 cycles of wall time per sample remain for synthesis with ~15 % left for UI.

## Steps

### Step 0: Toolchain and scaffold ✅
- [x] Mozzi 2.0.4 and FixMath 1.0.9 vendored in `libraries/`, so there's no
      install into `Documents` and no Controlled Folder Access issue. One local patch:
      a stray debug `Serial.println` in Mozzi's `FrequencyTimer2.cpp` pulled in
      `HardwareSerial0`, whose USART ISR clashes with ours. See `libraries/README.md`.
- [x] `.vscode/arduino.json` (board `Blokas:avr:midiboy`, USBasp, output `../Build`),
      `.gitignore`, and a placeholder `MidiboySoundscape.ino`.
- [x] `git init` (branch `main`, like the Sequencer).
- [x] Build (arduino-cli 0.31, bundled with the VS Code Arduino extension). From `Synth/`:
      `arduino-cli compile -b Blokas:avr:midiboy --libraries libraries --output-dir Build MidiboySoundscape`
      Result: links, 494 B flash, 266 B RAM. All drafts compile with no warnings under
      `--warnings all`, but they aren't called yet, so most of their code is
      optimised out.
- Note: the VS Code extension's Verify/Upload buttons don't know about `libraries/`.
  Use the command line, as in the Sequencer.

### Step 1: Prove audio out of MIDI OUT (no Mozzi) ✅
- [x] Test sketch (the main `.ino` for now). It plays a sine through `UartAudioOut`, with
      frequency, level, mute and sweep on the buttons, and shows ring fill on screen to
      catch dropouts. Builds to 6.4 KB flash and 880 B RAM. The binary has only the Timer0
      and UART interrupts, and the UART ISR is about 45 cycles per frame.
- [x] Adapter: **TRS Type A**. Tip = DIN 5 = audio, sleeve = DIN 2 = ground, ring =
      DIN 4 = +5 V DC through a resistor (no audio).
- [x] First hardware test (no dither): pitch 440 Hz ✓, clean tone ✓, sweep ✓, no dropouts ✓.
      In silence: high-frequency hiss, sometimes very noticeable. The simulation of the exact
      integer code shows the cause: the noise shaper locks into a repeating pattern on
      silence, giving whistles at about 11.7, 15.6 or 19.5 kHz, up to −22 dB. Added switchable
      TPDF dither (on by default). It takes whistles below −45 dB but raises the steady hiss ~5 dB.
- [x] Listen with dither on and off (B in the test sketch) and pick the default.
      Chosen: **dither on** (`g_uartAudioDither = true`). A steady hiss is preferred over
      occasional whistles.
- Note: V-USB (USB MIDI) can't run alongside audio. Its interrupt outranks the UART
  and blocks for tens of µs, so frames would be missed.
- [x] Listen and measure: pitch 440 Hz ✓, no distortion at 1.25 Mbaud, noise floor
      acceptable with dither.
- [x] Level: the user's input clips above −12 to −18 dB (full scale is about 3 Vpp, too hot).
- [x] Decide whether the cable needs an RC filter or attenuator: **skipped for now**, plain
      adapter only. **So the mix peaks at −12 dB in code** (master headroom; LEVEL max
      = −12 dB), at the cost of ~12 dB SNR. For later, the proposed attenuator plus
      2-pole filter: tip → 3.3 kΩ → node (1 kΩ ‖ 33 nF to ground) → 4.7 kΩ → 4.7 nF to
      ground → line in. About −16 dB with a 10 kΩ input, corners about 6 kHz. Attenuate in
      the cable, not in code: scaling down in code keeps the hiss and loses SNR.

### Step 2: Mozzi in custom output mode ✅
- [x] Sketch (the main `.ino` for now): Mozzi `Oscil` sines through `audioHook()`,
      the loop calls `updateControl()`/`updateAudio()` and `UartAudioOut::write()` itself. Peak −12 dB. Builds to 7.4 KB flash and 465 B RAM, with
      only the Timer0 and UART interrupts. The tone test is in git (`21bed91`).
- [x] Pitch correction moved to `src/audio/Tuning.h` (`tuneHz()`, `TUNE_EXTRA`,
      `CONTROL_HZ`), shared with the engine.
- [x] On hardware: B toggles the correction. ON reads 440 Hz, OFF 420 Hz ✓.
- [x] CPU meter: Timer1 (free in custom mode) counts CPU cycles around each
      `audioHook()` batch. The screen shows AUDIO % (share of time spent making
      audio, including the UART interrupt meanwhile) and CYC per sample (budget 1280).
      UP/DOWN sums 1..8 oscillators as a load test.
- [x] First hardware run: AUDIO 94 %, CYC 1133, FILL 0 with a dropout 4 times a second
      (each status redraw). The C encoder took ~70 cycles per frame, Mozzi's `audioHook()`
      another ~80 per sample. Rewrote the encoder (see key decisions) and dropped `audioHook()`.
- [x] On hardware with the new encoder: clean tone, no dropouts up to 6 oscillators.
      1 oscillator: AUDIO 46 %, CYC 555. 8 oscillators: 97 %, 1174 (dropouts at 7–8: overload,
      as expected). Each `Oscil` in the array loop costs ~88 cycles of wall time.
- Budget for step 3: the fixed part (UART interrupt + encoder + loop) is ~36 % of the CPU. Keeping
  ~15 % for the UI leaves **~620 cycles of wall time per sample for synthesis** (~450 of CPU work,
  since the interrupt takes 28 % of all time). That's ~7 of these oscillators, so every layer
  must be lean. Possible savings if needed: dither random numbers in asm (~30 per sample).

### Step 3: Soundscape engine, one layer at a time ⬜ (`Soundscape.*`, `Scales.*` 🟡)
Measure CPU after each layer.
- [ ] 3a: Pad: 3 detuned saws, chord random walk in a scale, glide, resonant lowpass.
- [ ] 3b: Wind: xorshift noise, moving lowpass, gusts.
- [ ] 3c: Chimes: 2 voices × 2 sine partials, random strikes, decay.
- [ ] 3d: Echo: 512 B line at 1/8 rate (~260 ms), feedback.
- [ ] Mix levels and clipping check at maximum settings. Output peak = −12 dB (±512).

### Step 4: UI ⬜ (`ParamScreen.*` 🟡, `.ino` not written)
- [ ] Screen: title row (root, HOLD, chime flash) and 7 parameter rows
      (MOOD, DENSITY, DRIFT, TONE, WIND, ECHO, LEVEL). Draw one row per loop pass.
- [ ] Buttons: ↑↓ select, ←→ change (repeat), A new scene, B hold.
- [ ] Main loop: fill the ring (at most 16 samples per pass), then one UI slice. Check
      that no dropouts happen while drawing.
- [ ] Random seed without EEPROM (`.noinit` RAM plus button-press timing).

### Step 5: Docs and polish ⬜
- [ ] README in the Sequencer's style: hardware, wiring and adapter, controls, build,
      flashing.
- [ ] Later or optional: save settings (in an EEPROM area that doesn't collide with the
      Sequencer, if wanted), MIDI-clock-free features, more layers if CPU allows.

## Open questions
1. Which DIN-jack adapter do you have (TRS Type A, Type B, or DIN to mono jack)?
2. What does it plug into (audio interface line in, mixer, headphones)? Headphones
   need an amp and DC blocking.
3. Version this with git like the Sequencer?
