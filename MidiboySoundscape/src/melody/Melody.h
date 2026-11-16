#pragma once

// The start-up melody. tools/mid2melody.py writes MelodyLocal.h (git-ignored)
// from a MIDI file; without it, the original phrase in MelodyDefault.h plays.
// Include from one .cpp only: the table is defined here.
#if __has_include("MelodyLocal.h")
#include "MelodyLocal.h"
#else
#include "MelodyDefault.h"
#endif

#define MELODY_ROW_SIZE 3
#define MELODY_ROWS (sizeof(MELODY_DATA) / MELODY_ROW_SIZE)
#define MELODY_NOTE_REST 0
#define MELODY_NOTE_PAD 0x80
