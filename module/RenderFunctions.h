#pragma once

#define R_ADDCMDDRAWTEXT 0x82350278
#define R_ADDCMDDRAWSTRETCHPIC 0x8234F9B8
#define R_REGISTERFONT 0x8234DCB0
#define R_REGISTERMATERIAL 0x8234E510
#define R_TEXTWIDTH 0x8234DD20
#define R_TEXTHEIGHT 0x8234DE10

void R_AddCmdDrawText(const char* text, int maxChars, void* font, float x, float y, float xScale, float yScale, float rotation, const float* color, int style);
void R_AddCmdDrawStretchPic(float x, float y, float width, float height, float xScale, float yScale, float xay, float yay, const float* color, void* material);
void* R_RegisterFont(const char* name, int imageTrack);
void* R_RegisterMaterial(const char* name, int imageTrack);
int R_TextWidth(const char* text, int maxChars, void* font);
int R_TextHeight(void* font);

void DrawRect(float x, float y, float w, float h, const float* color, void* material);