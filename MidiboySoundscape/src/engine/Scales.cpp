#include "Scales.h"
#include <avr/pgmspace.h>

struct ScaleDef {
  char name[11];
  uint8_t length;
  uint8_t steps[SCALE_MAX_NOTES];   // semitones above the root
};

static const ScaleDef SCALES[SCALE_COUNT] PROGMEM = {
  { "PENTA MIN",  5, { 0, 3, 5, 7, 10 } },
  { "PENTA MAJ",  5, { 0, 2, 4, 7, 9 } },
  { "DORIAN",     7, { 0, 2, 3, 5, 7, 9, 10 } },
  { "LYDIAN",     7, { 0, 2, 4, 6, 7, 9, 11 } },
  { "AEOLIAN",    7, { 0, 2, 3, 5, 7, 8, 10 } },
  { "HIRAJOSHI",  5, { 0, 2, 3, 7, 8 } },
  { "IN SEN",     5, { 0, 1, 5, 7, 10 } },
  { "WHOLE TONE", 6, { 0, 2, 4, 6, 8, 10 } },
};

const char* Scales::name(uint8_t scale) {
  return SCALES[scale % SCALE_COUNT].name;
}

uint8_t Scales::length(uint8_t scale) {
  return pgm_read_byte(&SCALES[scale % SCALE_COUNT].length);
}

uint8_t Scales::note(uint8_t scale, uint8_t root, int8_t degree) {
  const ScaleDef* def = &SCALES[scale % SCALE_COUNT];
  int8_t len = pgm_read_byte(&def->length);
  int8_t octave = 0;
  while(degree < 0) {
    degree += len;
    octave--;
  }
  while(degree >= len) {
    degree -= len;
    octave++;
  }
  int16_t note = root + 12 * octave + pgm_read_byte(&def->steps[degree]);
  if(note < 0) {
    note = 0;
  } else if(note > 127) {
    note = 127;
  }
  return note;
}
