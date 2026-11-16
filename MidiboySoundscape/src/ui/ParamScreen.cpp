#include "ParamScreen.h"
#include <MidiboyFonts.h>
#include "../engine/Scales.h"

#define SCREEN_WIDTH 128
#define CHAR_WIDTH 6      // 5x7 glyph + 1 column of space
#define TITLE_ROW PARAM_MODE
#define FLASH_MS 150

// Pixel columns. The display's page bytes have bit 7 at the top, matching
// the Midiboy fonts.
#define VALUE_X 44
#define NUMBER_X 110

#define BAR_ON_BITS 0x3C
#define BAR_DOT_BITS 0x10

static const char PARAM_NAMES[PARAM_COUNT][8] PROGMEM = {
  "MODE",   // not drawn: the title row shows the mode instead
  "MOOD", "DENSITY", "DRIFT", "TONE", "WIND", "ECHO", "LEVEL"
};

static const char NOTE_NAMES[12][3] PROGMEM = {
  "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

ParamScreen::ParamScreen(MidiboyDisplay& display, Soundscape& engine)
  : _display(display), _engine(engine), _dirtyRows(0), _selected(PARAM_DENSITY), _x(0),
    _shownRoot(0), _shownHeld(false), _shownStrikes(0), _flashOn(false), _flashStartMs(0),
    _meterOn(false), _meterCycles(0), _meterDropouts(0) {
}

void ParamScreen::begin() {
  _display.clearScreen();
  _dirtyRows = 0xFF;
  _shownStrikes = _engine.getStrikeCount();
}

uint8_t ParamScreen::getSelected() const {
  return _selected;
}

void ParamScreen::setSelected(uint8_t param) {
  if(param == _selected) {
    return;
  }
  markParam(_selected);
  _selected = param;
  markParam(_selected);
}

void ParamScreen::markParam(uint8_t param) {
  _dirtyRows |= 1 << param;
}

void ParamScreen::markTitle() {
  _dirtyRows |= 1 << TITLE_ROW;
}

void ParamScreen::setMeter(bool on) {
  _meterOn = on;
  markTitle();
}

void ParamScreen::showMeter(uint16_t cycles, uint16_t dropouts) {
  _meterCycles = cycles > 9999 ? 9999 : cycles;
  _meterDropouts = dropouts > 999 ? 999 : dropouts;
  if(_meterOn) {
    markTitle();
  }
}

void ParamScreen::poll() {
  if(_engine.getRoot() != _shownRoot || _engine.isHeld() != _shownHeld) {
    markTitle();
  }
  uint8_t strikes = _engine.getStrikeCount();
  if(strikes != _shownStrikes) {
    _shownStrikes = strikes;
    _flashOn = true;
    _flashStartMs = millis();
    markTitle();
  } else if(_flashOn && millis() - _flashStartMs >= FLASH_MS) {
    _flashOn = false;
    markTitle();
  }
}

bool ParamScreen::drawNext() {
  if(_dirtyRows == 0) {
    return false;
  }
  uint8_t row = 0;
  while(!(_dirtyRows & (1 << row))) {
    row++;
  }
  _dirtyRows &= ~(1 << row);

  if(row == TITLE_ROW) {
    drawTitle();
  } else {
    drawParam(row);
  }
  return true;
}

void ParamScreen::drawTitle() {
  _shownRoot = _engine.getRoot();
  _shownHeld = _engine.isHeld();

  bool selected = _selected == PARAM_MODE;
  bool melody = _engine.getParam(PARAM_MODE) == MODE_MELODY;

  beginRow(TITLE_ROW);
  if(_meterOn) {
    drawChar('C', selected);
    drawNumber(_meterCycles, 4);
    drawText_P(PSTR(" D"), false);
    drawNumber(_meterDropouts, 3);
  } else {
    // Inverted when selected, like a parameter name.
    drawText_P(melody ? PSTR("MELODY    ") : PSTR("SOUNDSCAPE"), selected);
  }
  _display.drawSpace(2 * CHAR_WIDTH - (_x - 10 * CHAR_WIDTH), false);
  _x = 12 * CHAR_WIDTH;
  const char* note = NOTE_NAMES[_shownRoot];
  drawText_P(note, false);
  if(pgm_read_byte(&note[1]) == 0) {
    drawChar(' ', false);
  }
  drawChar(' ', false);
  if(_shownHeld && !melody) {
    drawText_P(PSTR("HOLD"), true);
  } else {
    drawText_P(PSTR("    "), false);
  }
  drawChar(' ', false);
  drawChar(_flashOn ? '*' : ' ', false);
  endRow();
}

void ParamScreen::drawParam(uint8_t param) {
  bool selected = param == _selected;
  beginRow(param);

  // Name, padded to 7 characters; inverted when selected.
  const char* name = PARAM_NAMES[param];
  uint8_t length = strlen_P(name);
  drawText_P(name, selected);
  for(; length < 7; length++) {
    drawChar(' ', selected);
  }
  _display.drawSpace(VALUE_X - _x, false);
  _x = VALUE_X;

  uint8_t value = _engine.getParam(param);
  if(param == PARAM_MOOD) {
    drawText_P(Scales::name(value), false);
  } else {
    drawBar(value);
    _display.drawSpace(NUMBER_X - _x, false);
    _x = NUMBER_X;
    drawNumber(value, 2);
  }
  endRow();
}

void ParamScreen::beginRow(uint8_t row) {
  _display.setDrawPosition(0, row);
  _x = 0;
}

// Clears whatever the previous contents left to the right.
void ParamScreen::endRow() {
  if(_x < SCREEN_WIDTH) {
    _display.drawSpace(SCREEN_WIDTH - _x, false);
  }
}

void ParamScreen::drawChar(char c, bool inverse) {
  if(c < ' ' || c > '~') {
    c = '?';
  }
  const uint8_t* glyph = &MIDIBOY_FONT_5X7::DATA_P[(uint16_t)(c - ' ') * MIDIBOY_FONT_5X7::WIDTH];
  _display.drawBitmap_P(glyph, MIDIBOY_FONT_5X7::WIDTH, inverse);
  _display.drawSpace(1, inverse);
  _x += CHAR_WIDTH;
}

void ParamScreen::drawText(const char* text, bool inverse) {
  while(*text) {
    drawChar(*text++, inverse);
  }
}

void ParamScreen::drawText_P(const char* text, bool inverse) {
  char c;
  while((c = pgm_read_byte(text++)) != 0) {
    drawChar(c, inverse);
  }
}

// Right-aligned in `width` characters (at most 5).
void ParamScreen::drawNumber(uint16_t value, uint8_t width) {
  char text[6];
  text[width] = 0;
  for(int8_t i = width - 1; i >= 0; i--) {
    text[i] = (value != 0 || i == width - 1) ? '0' + value % 10 : ' ';
    value /= 10;
  }
  drawText(text, false);
}

// PARAM_MAX cells of 4 pixels: a block per step up to value, a dot after.
void ParamScreen::drawBar(uint8_t value) {
  for(uint8_t i = 0; i < PARAM_MAX; i++) {
    if(i < value) {
      _display.drawBits(BAR_ON_BITS, 3, false);
    } else {
      _display.drawSpace(1, false);
      _display.drawBits(BAR_DOT_BITS, 1, false);
      _display.drawSpace(1, false);
    }
    _display.drawSpace(1, false);
    _x += 4;
  }
}
