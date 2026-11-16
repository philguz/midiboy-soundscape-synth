#pragma once
#include <Arduino.h>
#include <MidiboyDisplay.h>
#include "../engine/Soundscape.h"

// The one screen: a title row with the scene status, then one row per
// parameter. Draws straight to the display, without a frame buffer (saves
// 1 KB of RAM for audio), and only one changed row per drawNext() call, so
// the main loop can keep the audio ring fed in between.
//
// Row n draws parameter n:
// Row 0:    SOUNDSCAPE  D#  HOLD *   MODE: SOUNDSCAPE or MELODY, root, hold,
//                                    a flash on every bell strike
// Rows 1-7: NAME     [bar or scale name]  value
//
// With the meter on, the mode name becomes "C1113 D  0": CPU cycles per
// sample and dropouts (see the sketch).
class ParamScreen {
  public:
    ParamScreen(MidiboyDisplay& display, Soundscape& engine);

    // Clears the display and marks every row for drawing.
    void begin();

    uint8_t getSelected() const;
    void setSelected(uint8_t param);

    // Redraw a parameter's row after its value changed.
    void markParam(uint8_t param);
    void markTitle();

    // Show the meter in the title row instead of the name.
    void setMeter(bool on);
    void showMeter(uint16_t cycles, uint16_t dropouts);

    // Watches the engine for title-row changes (root, hold, chime flash).
    void poll();

    // Draws at most one pending row, ~0.3 ms. Returns false when nothing
    // was pending.
    bool drawNext();

  private:
    MidiboyDisplay& _display;
    Soundscape& _engine;
    uint8_t _dirtyRows;   // bit n = row n
    uint8_t _selected;
    uint8_t _x;           // pixels drawn on the current row

    uint8_t _shownRoot;
    bool _shownHeld;
    uint8_t _shownStrikes;
    bool _flashOn;
    unsigned long _flashStartMs;

    bool _meterOn;
    uint16_t _meterCycles;
    uint16_t _meterDropouts;

    void drawTitle();
    void drawParam(uint8_t param);

    void beginRow(uint8_t row);
    void endRow();
    void drawChar(char c, bool inverse);
    void drawText(const char* text, bool inverse);
    void drawText_P(const char* text, bool inverse);
    void drawNumber(uint16_t value, uint8_t width);
    void drawBar(uint8_t value);
};
