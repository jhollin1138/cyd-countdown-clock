// Drawing helpers shared by the main screen (display.cpp) and touch screens (ui.cpp)
#pragma once
#include "app.h"
#include <TFT_eSPI.h>

extern TFT_eSPI tft;

#define C_ACCENT 0xFD20  // orange
#define C_DIM    0xC618  // light gray
#define C_SKYBL  0x5D1F  // rain blue
#define C_BG     0x0884  // dark navy background
#define C_CARD   0x1947  // slate panel
#define C_BTN    0x320A  // button gray-blue
#define C_GREEN  0x1509
#define C_RED    0xB8E3

void txt(TFT_eSPI& g, const String& s, int x, int y, uint16_t col, uint8_t datum, int sh = 1);
String fit(TFT_eSPI& g, String s, int maxW);
String hhmm(const struct tm& t, bool shortAmPm);
struct tm homeTm(time_t t);
struct tm destTm(time_t t, int32_t off);
void drawIcon(int x, int y, int size, int code, bool day);
// Temperature with a drawn degree ring. align: 0 left, 1 center, 2 right; y is the top.
int tempTxt(float v, int x, int y, int align, uint16_t col, uint8_t numFont, uint8_t unitFont, bool unit);
void uiButton(int x, int y, int w, int h, const String& label, uint16_t bg = C_BTN, uint16_t fg = TFT_WHITE);
