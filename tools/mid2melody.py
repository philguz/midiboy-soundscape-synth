#!/usr/bin/env python3
"""Converts a MIDI file into a melody table for Midiboy Soundscape.

    python tools/mid2melody.py song.mid            # writes src/melody/MelodyLocal.h
    python tools/mid2melody.py --original          # rewrites src/melody/MelodyDefault.h

In MELODY mode the firmware plays the table on a loop; switching back to
SOUNDSCAPE carries on with the generative scene in the melody's key:

- Notes at or above --split are struck on the two chime voices (bells with an
  octave partial, ~1 s decay). At most two notes start together: the highest
  two. They are moved by octaves into the bells' range.
- Notes below --split steer the pad: on every beat, the lowest bass note that
  starts in it becomes the pad's bass (root, octave, fifth above).
- Note lengths are dropped (the bells ring out); velocities set the strike level.

MelodyLocal.h is git-ignored, and the build takes it over MelodyDefault.h when
it exists. Delete it to go back to the default. Only commit tables of music you
have the rights to.

No dependencies beyond Python 3.
"""

import argparse
import os
import struct
import sys

AUDIO_RATE = 15625
CONTROL_PERIOD = 128
CONTROL_HZ = AUDIO_RATE / CONTROL_PERIOD   # 122.07 ticks per second

# Scale indices in src/engine/Scales.cpp.
MOOD_PENTA_MAJ = 1
MOOD_AEOLIAN = 4

# Bell range: the sine oscillator is under ~3 cents off from MIDI 55 up.
BELL_LOW = 55
BELL_HIGH = 100
CHIME_VOICES = 2
MAX_EVENTS = 2000   # 6 KB of flash

NOTE_REST = 0
NOTE_PAD = 0x80

SKETCH_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "MidiboySoundscape", "src", "melody")

NOTE_NAMES = "C C# D D# E F F# G G# A A# B".split()


# --- MIDI reading -------------------------------------------------------------

def read_vlq(data, i):
    value = 0
    while True:
        byte = data[i]
        i += 1
        value = (value << 7) | (byte & 0x7F)
        if byte < 0x80:
            return value, i


def read_midi(path):
    """Returns (notes, ticks_per_beat, tempo_map).

    notes: (start_tick, note, velocity), drum channel 10 left out.
    tempo_map: sorted (tick, microseconds_per_beat).
    """
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"MThd":
        sys.exit(f"{path}: not a standard MIDI file")
    header_len = struct.unpack(">I", data[4:8])[0]
    _, track_count, division = struct.unpack(">HHH", data[8:14])
    if division & 0x8000:
        sys.exit(f"{path}: SMPTE time division is not supported")

    notes = []
    tempos = []
    pos = 8 + header_len
    for _ in range(track_count):
        if data[pos:pos + 4] != b"MTrk":
            break
        length = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        track = data[pos + 8:pos + 8 + length]
        pos += 8 + length

        i = 0
        tick = 0
        status = 0
        while i < len(track):
            delta, i = read_vlq(track, i)
            tick += delta
            byte = track[i]
            if byte == 0xFF:
                kind = track[i + 1]
                size, i = read_vlq(track, i + 2)
                if kind == 0x51 and size == 3:
                    tempos.append((tick, int.from_bytes(track[i:i + 3], "big")))
                i += size
                continue
            if byte in (0xF0, 0xF7):
                size, i = read_vlq(track, i + 1)
                i += size
                continue
            if byte & 0x80:
                status = byte
                i += 1
            kind = status >> 4
            channel = status & 0x0F
            if kind in (0xC, 0xD):
                i += 1
                continue
            a, b = track[i], track[i + 1]
            i += 2
            if kind == 0x9 and b > 0 and channel != 9:
                notes.append((tick, a, b))

    tempos.sort()
    if not tempos or tempos[0][0] != 0:
        tempos.insert(0, (0, 500000))
    notes.sort()
    return notes, division, tempos


def tick_to_seconds(tick, division, tempos):
    seconds = 0.0
    for n, (start, tempo) in enumerate(tempos):
        end = tempos[n + 1][0] if n + 1 < len(tempos) else None
        if end is None or tick < end:
            return seconds + (tick - start) * tempo / 1e6 / division
        seconds += (end - start) * tempo / 1e6 / division
    return seconds


# --- Analysis -----------------------------------------------------------------

# Krumhansl-Kessler key profiles.
MAJOR_PROFILE = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88]
MINOR_PROFILE = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17]


def correlate(a, b):
    ma = sum(a) / 12
    mb = sum(b) / 12
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    den = (sum((x - ma) ** 2 for x in a) * sum((y - mb) ** 2 for y in b)) ** 0.5
    return num / den if den else 0.0


def guess_key(notes, split):
    """Returns (root pitch class, is_minor) by note-count key profiles.

    Bass notes count three times (they settle major against relative minor),
    and the last bass note once more on top.
    """
    counts = [0] * 12
    for _, note, _ in notes:
        counts[note % 12] += 3 if note < split else 1
    bass = [note for _, note, _ in notes if note < split]
    if bass:
        counts[bass[-1] % 12] += max(3, len(notes) // 10)
    best = (-2.0, 0, False)
    for root in range(12):
        rotated = counts[root:] + counts[:root]
        for minor, profile in ((False, MAJOR_PROFILE), (True, MINOR_PROFILE)):
            score = correlate(rotated, profile)
            if score > best[0]:
                best = (score, root, minor)
    return best[1], best[2]


def fold_bell(note):
    while note < BELL_LOW:
        note += 12
    while note > BELL_HIGH:
        note -= 12
    return note


def build_events(notes, division, tempos, split, transpose, max_seconds, pad):
    """Returns [(seconds, order, note_byte, level)], order puts pad changes first."""
    events = []
    notes = [(t, n + transpose, v) for t, n, v in notes if 0 <= n + transpose <= 127]

    # Bells: at most CHIME_VOICES notes per onset, the highest ones.
    by_onset = {}
    for tick, note, velocity in notes:
        if note >= split or not pad:
            by_onset.setdefault(tick, []).append((note, velocity))
    for tick, group in by_onset.items():
        group.sort(reverse=True)
        seen = set()
        for note, velocity in group[:CHIME_VOICES]:
            note = fold_bell(note)
            if note in seen:
                continue
            seen.add(note)
            level = 100 + velocity * 155 // 127
            events.append((tick_to_seconds(tick, division, tempos), 1, note, level))

    # Pad: the lowest bass note starting in each beat, when its pitch class changes.
    if pad:
        lowest = {}
        for tick, note, _ in notes:
            if note < split:
                beat = tick // division
                if beat not in lowest or note < lowest[beat][1]:
                    lowest[beat] = (tick, note)
        current = None
        for beat in sorted(lowest):
            tick, note = lowest[beat]
            if note % 12 != current:
                current = note % 12
                events.append((tick_to_seconds(tick, division, tempos), 0,
                               NOTE_PAD | note, 0))

    events.sort()
    if events:
        first = events[0][0]
        events = [(s - first, o, n, l) for s, o, n, l in events]
    if max_seconds is not None:
        events = [e for e in events if e[0] <= max_seconds]
    return events


def to_table(events, tail_seconds):
    """Converts to (wait, note, level) rows; waits in control ticks, <= 255."""
    rows = []
    last_tick = 0
    for seconds, _, note, level in events:
        tick = round(seconds * CONTROL_HZ)
        wait = tick - last_tick
        while wait > 255:
            rows.append((255, NOTE_REST, 0))
            wait -= 255
        rows.append((wait, note, level))
        last_tick = tick
    # A rest at the end, so the bells ring out before the scene takes over.
    wait = round(tail_seconds * CONTROL_HZ)
    while wait > 0:
        step = min(wait, 255)
        rows.append((step, NOTE_REST, 0))
        wait -= step
    return rows


# --- Output -------------------------------------------------------------------

def write_header(path, rows, root, mood, source):
    seconds = sum(r[0] for r in rows) / CONTROL_HZ
    lines = [
        f"// Generated by tools/mid2melody.py from {source}. Do not edit;",
        "// run the tool again instead.",
        f"// {len(rows)} events, {seconds:.1f} s, key {NOTE_NAMES[root]}"
        f" {'minor' if mood == MOOD_AEOLIAN else 'major'}.",
        "",
        "#pragma once",
        "#include <avr/pgmspace.h>",
        "",
        "// Pitch class of the key (0 = C) and the MOOD the scene continues in.",
        f"#define MELODY_ROOT {root}",
        f"#define MELODY_MOOD {mood}",
        "",
        "// Rows of: control ticks after the previous row (122 per second), note,",
        "// level. Note 0 is a rest, 0x80 | n moves the pad's bass to n, other",
        "// notes strike a bell at that level.",
        "static const uint8_t MELODY_DATA[] PROGMEM = {",
    ]
    for wait, note, level in rows:
        if note == NOTE_REST:
            comment = "rest"
        elif note & NOTE_PAD:
            n = note & 0x7F
            comment = f"pad {NOTE_NAMES[n % 12]}{n // 12 - 1}"
        else:
            comment = f"{NOTE_NAMES[note % 12]}{note // 12 - 1}"
        lines.append(f"  {wait:3d}, 0x{note:02X}, {level:3d},   // {comment}")
    lines += ["};", ""]
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"Wrote {path}: {len(rows)} events ({len(rows) * 3} bytes), "
          f"{seconds:.1f} s, key {NOTE_NAMES[root]} "
          f"{'minor' if mood == MOOD_AEOLIAN else 'major'}")


# --- The built-in original phrase ---------------------------------------------

# A short original phrase in A minor, written for this project: (beat, note,
# velocity) at 84 BPM in 3/4. Notes below 55 steer the pad.
ORIGINAL_BPM = 84
ORIGINAL_PHRASE = [
    # Bass: A, F, C, G, then home to A.
    (0, 45, 80), (6, 41, 80), (12, 48, 80), (18, 43, 80), (24, 45, 80),
    # Bells.
    (0, 76, 90), (1, 72, 70), (2, 71, 70),
    (3, 69, 95), (4.5, 71, 60), (5, 72, 70),
    (6, 77, 90), (7, 76, 70), (8, 72, 65),
    (9, 74, 95), (11, 67, 60),
    (12, 79, 100), (13, 76, 70), (14, 74, 70),
    (15, 72, 90), (16, 74, 65), (17, 76, 70),
    (18, 74, 95), (19.5, 71, 65), (20, 67, 60),
    (21, 71, 80), (22, 72, 70), (23, 74, 70),
    (24, 76, 100), (24, 69, 70), (25.5, 81, 55),
]


def original_notes():
    division = 96
    notes = sorted((int(beat * division), note, velocity)
                   for beat, note, velocity in ORIGINAL_PHRASE)
    tempos = [(0, round(60e6 / ORIGINAL_BPM))]
    return notes, division, tempos


# --- Main ---------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("midi", nargs="?", help="MIDI file to convert")
    parser.add_argument("-o", "--output",
                        help="header to write (default: src/melody/MelodyLocal.h)")
    parser.add_argument("--original", action="store_true",
                        help="write the built-in original phrase to MelodyDefault.h")
    parser.add_argument("--split", type=int, default=55,
                        help="notes below this MIDI note steer the pad (default 55, G3)")
    parser.add_argument("--no-pad", action="store_true",
                        help="play every note on the bells; the pad keeps its chord")
    parser.add_argument("--transpose", type=int, default=0, help="semitones")
    parser.add_argument("--max-seconds", type=float, help="cut the melody here")
    parser.add_argument("--tail", type=float, default=2.0,
                        help="seconds of ring-out before the scene resumes (default 2)")
    parser.add_argument("--root", help="key root (C, F#, Bb, ...), instead of a guess")
    parser.add_argument("--minor", action="store_true", help="with --root: minor key")
    args = parser.parse_args()

    if args.original:
        notes, division, tempos = original_notes()
        source = "the built-in original phrase (--original)"
        output = args.output or os.path.join(SKETCH_DIR, "MelodyDefault.h")
    elif args.midi:
        notes, division, tempos = read_midi(args.midi)
        source = os.path.basename(args.midi)
        output = args.output or os.path.join(SKETCH_DIR, "MelodyLocal.h")
    else:
        parser.error("give a MIDI file, or --original")
    if not notes:
        sys.exit("no notes found")

    if args.root:
        letter, accidental = args.root[:1].upper(), args.root[1:]
        if letter not in NOTE_NAMES or accidental not in ("", "#", "b"):
            sys.exit(f"unknown root {args.root}")
        shift = {"": 0, "#": 1, "b": -1}[accidental]
        root, minor = (NOTE_NAMES.index(letter) + shift) % 12, args.minor
    else:
        root, minor = guess_key([(t, n + args.transpose, v) for t, n, v in notes],
                                args.split)
    mood = MOOD_AEOLIAN if minor else MOOD_PENTA_MAJ

    events = build_events(notes, division, tempos, args.split, args.transpose,
                          args.max_seconds, not args.no_pad)
    rows = to_table(events, args.tail)
    if len(rows) > MAX_EVENTS:
        print(f"warning: {len(rows)} events, cut to {MAX_EVENTS} "
              f"(use --max-seconds to choose where)", file=sys.stderr)
        rows = rows[:MAX_EVENTS]
    write_header(output, rows, root, mood, source)


if __name__ == "__main__":
    main()
