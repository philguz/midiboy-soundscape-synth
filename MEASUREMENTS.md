# Measurements

This document lists the measured, simulated, and calculated figures behind the design of
Midiboy Soundscape. For the design decisions themselves, see [PLAN.md](PLAN.md).

Each figure lists its source:

- **Hardware**: read on the device, from the on-screen meter, a tuner, or by ear.
- **Simulation**: a PC simulation of the exact integer code.
- **Disassembly**: CPU cycles counted from the compiled code.
- **Calculation**: derived on paper.
- **Estimate**: from code comments, not yet measured.

## Overall performance

All layers on, **DENSITY** high, **LEVEL** at maximum:

| Metric | Value | Source |
|---|---|---|
| CPU cycles per sample | 1113 of 1280 (87%), worst batch 1280 | Hardware |
| Dropouts | 0, including while you scroll and hold **Left** or **Right** | Hardware |
| Peak sample | 476 of 511 | Hardware |
| Clipped samples | Up to 4 per 250 ms when layer peaks line up | Hardware |

The engine clamps the mix at ±512 instead of scaling it down, so occasional clipped
samples are expected.

## Audio output quality

| Metric | Value | Source |
|---|---|---|
| SNR in the 0 to 8 kHz band, second-order noise shaping per frame | About 44 dB, similar to Mozzi's standard 8-bit PWM | Simulation |
| SNR with noise shaping per bit (rejected design) | 9 to 30 dB | Simulation |
| 8-bit encoder compared to a 16-bit reference | Same noise for input up to −6 dB | Simulation |
| Idle tones without dither | Whistles at about 11.7, 15.6, or 19.5 kHz, up to −22 dB | Simulation, hardware |
| Idle tones with ±½ level TPDF dither | Below −45 dB; steady hiss about 5 dB higher | Simulation, hardware |

Without dither, the noise shaper locks into a repeating pattern on silence, which you hear
as a whistle. Dither is on by default.

## Output level

| Metric | Value | Source |
|---|---|---|
| Full-scale output on DIN pin 5 | About 3 Vpp | Hardware |
| Clipping point of the test line input | −12 to −18 dB of full scale | Hardware |
| SNR lost to the −12 dB headroom in code | About 12 dB | Calculation |
| Proposed cable attenuator and 2-pole filter (not built) | About −16 dB into 10 kΩ, corners near 6 kHz | Calculation |

Peak level of each layer before the master volume:

| Layer | Worst case | Typical | Source |
|---|---|---|---|
| Pad, depending on **TONE** | ±120 to ±350 | 200 | Simulation |
| Wind at **WIND** 15 | ±420 | 100 | Estimate |
| Chimes | ±380 | 150 | Estimate |
| Echo at **ECHO** 15 | ±450 | 80 | Estimate |

## Pitch accuracy

| Component | Accuracy | Source |
|---|---|---|
| `noteStep()` phase-step table | Within 0.01 cent | Calculation |
| `SineOsc`, 16-bit phase | Within 1 cent from 260 Hz up; within 3 cents from MIDI note 55 up | Calculation |
| `SawOsc`, 24-bit phase | About 0.001 Hz resolution | Calculation |

## CPU budget

The budget is 1280 CPU cycles per sample: 20 MHz divided by 15625 Hz.

Fixed costs:

| Component | Cycles | Source |
|---|---|---|
| UART interrupt | About 45 per frame, about 360 per sample (28% of all CPU time) | Disassembly |
| Noise-shaping encoder, assembly | About 21 per frame, 199 per sample | Disassembly |
| Meter and main loop | About 60 per sample | Disassembly |

Together, these take about 36% of the CPU. The design keeps about 15% for the UI.

Synthesis, in CPU cycles per sample, not counting the UART interrupt:

| Layer | Cycles | Source |
|---|---|---|
| Pad | 164 | Disassembly |
| Chimes, 2 voices ringing | 144 | Disassembly |
| Wind, at half rate | About 60 on average | Disassembly |
| Echo, mix, and dither | 144 | Disassembly |

Rejected approaches, for comparison:

| Approach | Cycles | Source |
|---|---|---|
| Encoder in C | About 70 per frame | Hardware |
| Mozzi `audioHook()` | About 80 extra per sample | Disassembly |
| First versions of the pad and chimes | 229 and 250 per sample | Disassembly |

Drawing a screen row takes about 0.3 ms.

The core's `micros()` runs 6.25% slow at 20 MHz, because it multiplies by the integer
64/20 = 3. The meter calculates its percentage from cycle counts instead.
