#include "stdafx.h"

void R_AddCmdDrawText(const char* text, int maxChars, void* font, float x, float y, float xScale, float yScale, float rotation, const float* color, int style)
{
	((void(*)(...))R_ADDCMDDRAWTEXT)(text, maxChars, font, x, y, xScale, yScale, rotation, color, style);
}

void R_AddCmdDrawStretchPic(float x, float y, float width, float height, float xScale, float yScale, float xay, float yay, const float* color, void* material)
{
	((void(*)(...))R_ADDCMDDRAWSTRETCHPIC)(x, y, width, height, xScale, yScale, xay, yay, color, material);
}

void* R_RegisterFont(const char* name, int imageTrack)
{
	return ((void*(*)(...))R_REGISTERFONT)(name, imageTrack);
}

void* R_RegisterMaterial(const char* name, int imageTrack)
{
	return ((void*(*)(...))R_REGISTERMATERIAL)(name, imageTrack);
}

int R_TextWidth(const char* text, int maxChars, void* font)
{
	return ((int(*)(...))R_TEXTWIDTH)(text, maxChars, font);
}

// pixel height of the font (the game returns the int at font + 4)
int R_TextHeight(void* font)
{
	return ((int(*)(...))R_TEXTHEIGHT)(font);
}

void DrawRect(float x, float y, float w, float h, const float* color, void* material)
{
	R_AddCmdDrawStretchPic(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, color, material);
}
