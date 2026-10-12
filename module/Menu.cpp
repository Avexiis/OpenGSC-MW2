#include "stdafx.h"

#define R_ENDFRAME 0x82351748

// Keep D-pad, A, B, X, LB and RB input out of the game while the menu is open.
#define MENU_BUTTONS (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER)

static const DWORD PAD_COUNT         = 4;
static const int   MAX_FILES         = 64;
static const int   VISIBLE_ROWS      = 10;
static const int   TRIGGER_THRESHOLD = 128;
static const int   LOAD_ATTEMPTS     = 120;    // Maximum load attempts while busy, spaced one second apart.
static const int   INIT_DELAY_TICKS  = 30;     // Wait three seconds (30 ticks of 100 ms) before attempting to install hooks.
static const int   INIT_MAX_TICKS    = 3000;
static const int   SHUTDOWN_WAIT_MS  = 5000;

static const float COLOR_WHITE[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
static const float COLOR_GREY[4]   = { 0.8f, 0.8f, 0.8f, 1.0f };
static const float COLOR_GREEN[4]  = { 0.2f, 1.0f, 0.2f, 1.0f };
static const float COLOR_RED[4]    = { 0.95f, 0.2f, 0.2f, 1.0f };
static const float COLOR_ORANGE[4] = { 1.0f, 0.65f, 0.1f, 1.0f };
static const float COLOR_BODY[4]   = { 0.06f, 0.06f, 0.07f, 0.85f };
static const float COLOR_BLUE[4]   = { 0.25f, 0.28f, 0.85f, 1.0f };
static const float COLOR_BAR[4]    = { 0.45f, 0.48f, 0.95f, 1.0f };

volatile bool bMenuOpen = false;

static Detour<void> R_EndFrame_Detour;
static Detour<DWORD> XamInputGetState_Detour;

// Capture each controller's input before filtering buttons sent to the game.
static volatile WORD gButtons[PAD_COUNT];
static volatile BYTE gLeftTrigger[PAD_COUNT];
static volatile bool gSwallow[PAD_COUNT]; // Keep buttons hidden after closing until all menu buttons are released.

static char gFiles[MAX_FILES][MAX_PATH];
static int gFileCount = 0;
static int gSel = 0;       // Selected row, including the expanded entry's action row.
static int gExpanded = -1; // Entry with its Load/Unload action visible; -1 means none.
static int gScroll = 0;

static volatile BOOL gStop = FALSE;
static volatile BOOL gInitRunning = FALSE;
static volatile BOOL gLoading = FALSE;
static char gLoadName[MAX_PATH];

// Rendering resources and fixed text measurements cached by InitRenderResources.
static void* gFont = NULL;
static void* gMaterial = NULL;
static int gFontHeight = 0;
static int gTitleWidth = 0;
static int gVersionWidth = 0;
static int gSpaceWidth = 0;
static int gOpenWidth = 0;
static int gCloseWidth = 0;

struct MenuRow
{
	char         text[MAX_PATH];
	const float* color;
	int          textWidth;
	const char*  tag;      // Status shown in parentheses after the text; NULL means no status.
	const float* tagColor;
	int          tagWidth;
};

static void SetSwallow(bool swallow)
{
	for (DWORD i = 0; i < PAD_COUNT; i++)
	{
		gSwallow[i] = swallow;
	}
}

static DWORD XamInputGetState_Hook(DWORD dwUserIndex, DWORD dwDeviceContext, XINPUT_STATE* pState)
{
	DWORD result = XamInputGetState_Detour.CallOriginal(dwUserIndex, dwDeviceContext, pState);
	if (dwUserIndex >= PAD_COUNT)
	{
		return result;
	}
	if (result != ERROR_SUCCESS)
	{
		gButtons[dwUserIndex] = 0;
		gLeftTrigger[dwUserIndex] = 0;
		gSwallow[dwUserIndex] = false;
		return result;
	}

	WORD buttons = pState->Gamepad.wButtons;
	gButtons[dwUserIndex] = buttons;
	gLeftTrigger[dwUserIndex] = pState->Gamepad.bLeftTrigger;
	if (gSwallow[dwUserIndex] && (buttons & MENU_BUTTONS) == 0)
	{
		gSwallow[dwUserIndex] = false;
	}
	if (bMenuOpen || gSwallow[dwUserIndex])
	{
		pState->Gamepad.wButtons &= ~MENU_BUTTONS;
		pState->Gamepad.bLeftTrigger = 0;
	}
	return result;
}

// Wait in short intervals so shutdown can interrupt the delay.
static void WaitStep(int milliseconds)
{
	for (int waited = 0; waited < milliseconds && !gStop; waited += 100)
	{
		Sleep(100);
	}
}

// Run loading and busy retries on a worker thread to keep the game thread responsive.
static DWORD WINAPI LoadThread(LPVOID param)
{
	for (int attempt = 0; attempt < LOAD_ATTEMPTS && !gStop; attempt++)
	{
		if (Mw2Gsc_LoadByFileName(gLoadName, (unsigned int)attempt) != GSC_LOAD_BUSY)
		{
			break;
		}
		if (attempt == LOAD_ATTEMPTS - 1)
		{
			Mw2Gsc_NotifyBusyGiveUp(gLoadName);
			break;
		}
		WaitStep(1000);
	}
	gLoading = FALSE;
	return 0;
}

static int RowCount()
{
	return gFileCount + (gExpanded >= 0 ? 1 : 0);
}

// Map a displayed row to its file entry. The expanded entry has an extra action row below it.
static int EntryOfRow(int row, bool* isAction)
{
	*isAction = (gExpanded >= 0 && row == gExpanded + 1);
	return (gExpanded >= 0 && row > gExpanded) ? row - 1 : row;
}

// Request an unload for the installed script; otherwise start a load if one is not already active or queued for this file.
static void RunAction(const char* file)
{
	const char* loaded = Mw2Gsc_GetLoadedName();
	const char* queued = Mw2Gsc_GetQueuedName();
	if (loaded != NULL && _stricmp(loaded, file) == 0)
	{
		Mw2Gsc_RequestUnload();
	}
	else if (!gLoading && (queued == NULL || _stricmp(queued, file) != 0))
	{
		strncpy_s(gLoadName, sizeof(gLoadName), file, _TRUNCATE);
		gLoading = TRUE;
		if (!Utilities::CreateThread((PVOID)LoadThread))
		{
			gLoading = FALSE;
			Utilities::Notify(XNOTIFYUI_TYPE_GENERIC, (const PWCHAR)L"OpenGSC [MW2]: Failed to start load\nthread error");
		}
	}
}

static void OpenMenu()
{
	gFileCount = Mw2Gsc_ListFiles(gFiles, MAX_FILES);
	gSel = 0;
	gExpanded = -1;
	gScroll = 0;
	bMenuOpen = true;
}

static void CloseMenu()
{
	SetSwallow(true);
	bMenuOpen = false;
}

// Combine controller input so the menu can be used from any controller.
static void ReadPads(WORD* buttons, BYTE* leftTrigger)
{
	*buttons = 0;
	*leftTrigger = 0;
	for (DWORD i = 0; i < PAD_COUNT; i++)
	{
		*buttons |= gButtons[i];
		*leftTrigger = max(*leftTrigger, gLeftTrigger[i]);
	}
}

static void HandleInput()
{
	static WORD lastButtons = 0;

	WORD buttons;
	BYTE leftTrigger;
	ReadPads(&buttons, &leftTrigger);
	WORD pressed = buttons & ~lastButtons;
	lastButtons = buttons;
	bool combo = leftTrigger > TRIGGER_THRESHOLD && (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;

	if (!bMenuOpen)
	{
		if (combo)
		{
			OpenMenu();
		}
		return;
	}

	if (combo || ((pressed & XINPUT_GAMEPAD_B) && gExpanded < 0))
	{
		CloseMenu();
		return;
	}

	if (pressed & XINPUT_GAMEPAD_B)
	{
		gSel = gExpanded;
		gExpanded = -1;
	}
	else if (pressed & XINPUT_GAMEPAD_A)
	{
		bool isAction;
		int entry = EntryOfRow(gSel, &isAction);
		if (isAction)
		{
			// Keep the menu open and its buttons hidden from the game; the status tag shows progress.
			RunAction(gFiles[entry]);
			gExpanded = -1;
			gSel = entry;
		}
		else if (entry < gFileCount)
		{
			gExpanded = (gExpanded == entry) ? -1 : entry;
			gSel = entry;
		}
	}
	else if (RowCount() > 0)
	{
		int total = RowCount();
		int delta = ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : 0) - ((pressed & XINPUT_GAMEPAD_DPAD_UP) ? 1 : 0);
		gSel = (gSel + delta + total) % total;
	}

	// Adjust scrolling to keep the selected row visible.
	gScroll = min(min(max(gScroll, gSel - (VISIBLE_ROWS - 1)), gSel), max(RowCount() - VISIBLE_ROWS, 0));
}

// Register the font and material when available, then cache the fixed text measurements.
static bool InitRenderResources()
{
	if (gFont == NULL)
	{
		gFont = R_RegisterFont("fonts/consoleFont", 0);
	}
	if (gMaterial == NULL)
	{
		gMaterial = R_RegisterMaterial("white", 0);
	}
	if (gFont == NULL || gMaterial == NULL)
	{
		return false;
	}

	if (gFontHeight == 0)
	{
		int height = R_TextHeight(gFont);
		gFontHeight = (height >= 1 && height <= 64) ? height : 16;
		gTitleWidth = R_TextWidth("OpenGSC MW2", 0, gFont);
		gVersionWidth = R_TextWidth("v1.0.0", 0, gFont);
		gSpaceWidth = R_TextWidth(" ", 0, gFont);
		gOpenWidth = R_TextWidth("(", 0, gFont);
		gCloseWidth = R_TextWidth(")", 0, gFont);
	}
	return true;
}

static MenuRow* AddRow(MenuRow* rows, int* count, const char* text, const float* color)
{
	MenuRow* row = &rows[(*count)++];
	strcpy_s(row->text, sizeof(row->text), text);
	row->color = color;
	row->textWidth = R_TextWidth(row->text, 0, gFont);
	row->tag = NULL;
	row->tagColor = NULL;
	row->tagWidth = 0;
	return row;
}

static void SetRowTag(MenuRow* row, const char* tag, const float* tagColor)
{
	row->tag = tag;
	row->tagColor = tagColor;
	row->tagWidth = R_TextWidth(tag, 0, gFont);
}

// Build the visible rows and return their count. Row 0 always shows the folder path.
// Set selectedRow to the highlighted row index, or -1 if no row is selected.
static int BuildRows(MenuRow* rows, int* selectedRow)
{
	int count = 0;
	*selectedRow = -1;
	AddRow(rows, &count, "Hdd:/GSC/MW2/...", COLOR_WHITE);
	if (gFileCount == 0)
	{
		AddRow(rows, &count, "No .gscbin files found", COLOR_GREY);
		return count;
	}

	const char* loadedName = Mw2Gsc_GetLoadedName();
	bool pending = Mw2Gsc_IsUnloadPending();
	const char* queuedName = Mw2Gsc_GetQueuedName();
	int end = min(gScroll + VISIBLE_ROWS, RowCount());
	for (int current = gScroll; current < end; current++)
	{
		bool isAction;
		int entry = EntryOfRow(current, &isAction);
		const char* file = gFiles[entry];
		bool loaded = loadedName != NULL && _stricmp(loadedName, file) == 0;
		bool loading = (gLoading && _stricmp(gLoadName, file) == 0) || (queuedName != NULL && _stricmp(queuedName, file) == 0);

		if (current == gSel)
		{
			*selectedRow = count;
		}
		if (isAction)
		{
			bool unload = loaded && !loading;
			AddRow(rows, &count, unload ? "    Unload GSC" : "    Load GSC", unload ? COLOR_RED : COLOR_GREEN);
			continue;
		}

		MenuRow* row = AddRow(rows, &count, file, COLOR_WHITE);
		if (loading)
		{
			SetRowTag(row, "Loading", COLOR_ORANGE);
		}
		else if (loaded)
		{
			SetRowTag(row, pending ? "Unloading" : "Loaded", pending ? COLOR_ORANGE : COLOR_GREEN);
		}
	}
	return count;
}

static void DrawString(const char* text, float x, float y, float scale, const float* color)
{
	R_AddCmdDrawText(text, 0x7FFFFFFF, gFont, x, y, scale, scale, 0.0f, color, 0);
}

static void DrawHeader(float x, float y, float width)
{
	float titleWidth = gTitleWidth * 1.5f;
	DrawString("OpenGSC MW2", x + (width - titleWidth) / 2.0f, y + (50.0f + gFontHeight * 1.5f) / 2.0f, 1.5f, COLOR_WHITE);
	DrawString("v1.0.0", x + width - gVersionWidth * 0.9f - 14.0f, y + (50.0f + gFontHeight * 0.9f) / 2.0f, 0.9f, COLOR_GREY);
}

static void DrawRowText(const MenuRow& row, float x, float y)
{
	DrawString(row.text, x, y, 1.0f, row.color);
	if (row.tag == NULL)
	{
		return;
	}

	x += (float)(row.textWidth + gSpaceWidth);
	DrawString("(", x, y, 1.0f, COLOR_WHITE);
	x += (float)gOpenWidth;
	DrawString(row.tag, x, y, 1.0f, row.tagColor);
	x += (float)row.tagWidth;
	DrawString(")", x, y, 1.0f, COLOR_WHITE);
}

// Fit the menu to the header and widest visible row, capped at 600 pixels.
static float MenuWidth(const MenuRow* rows, int count)
{
	float width = gTitleWidth * 1.5f + 2.0f * (gVersionWidth * 0.9f + 14.0f + 8.0f);
	for (int i = 0; i < count; i++)
	{
		float rowWidth = (float)rows[i].textWidth + 28.0f;
		if (rows[i].tag != NULL)
		{
			rowWidth += (float)(gSpaceWidth + gOpenWidth + rows[i].tagWidth + gCloseWidth);
		}
		width = max(width, rowWidth);
	}
	return min(width, 600.0f);
}

static void Draw()
{
	if (!InitRenderResources())
	{
		return;
	}

	// Build the rows before measuring the menu, since their text determines its width.
	MenuRow rows[VISIBLE_ROWS + 1];
	int selectedRow;
	int count = BuildRows(rows, &selectedRow);

	float width = MenuWidth(rows, count);
	float height = 53.0f + count * 24.0f + 12.0f;
	float x = 20.0f;
	float y = 20.0f;

	DrawRect(x, y, width, height, COLOR_BODY, gMaterial);
	if (selectedRow > 0)
	{
		DrawRect(x + 6.0f, y + 53.0f + selectedRow * 24.0f + 4.0f, width - 12.0f, 24.0f, COLOR_BAR, gMaterial);
	}
	DrawRect(x, y + 50.0f, width, 3.0f, COLOR_BLUE, gMaterial);
	DrawRect(x, y, width, 3.0f, COLOR_BLUE, gMaterial);
	DrawRect(x, y + height - 3.0f, width, 3.0f, COLOR_BLUE, gMaterial);
	DrawRect(x, y, 3.0f, height, COLOR_BLUE, gMaterial);
	DrawRect(x + width - 3.0f, y, 3.0f, height, COLOR_BLUE, gMaterial);

	DrawHeader(x, y, width);
	for (int i = 0; i < count; i++)
	{
		DrawRowText(rows[i], x + 12.0f, y + 53.0f + (i + 1) * 24.0f);
	}
}

static void R_EndFrame_Hook()
{
	Mw2Gsc_Tick();
	HandleInput();
	if (bMenuOpen)
	{
		Draw();
	}

	R_EndFrame_Detour.CallOriginal();
}

static bool RendererReady()
{
	return MmIsAddressValid((PVOID)R_ENDFRAME) && MmIsAddressValid((PVOID)(R_ENDFRAME + 0x10)) && *(volatile DWORD*)R_ENDFRAME != 0;
}

static bool InstallHooks()
{
	DWORD xamInputGetState = (DWORD)Utilities::Resolve((PCHAR)"xam.xex", 401);
	return xamInputGetState != 0
		&& XamInputGetState_Detour.SetupDetour(xamInputGetState, (PVOID)XamInputGetState_Hook)
		&& R_EndFrame_Detour.SetupDetour(R_ENDFRAME, (PVOID)R_EndFrame_Hook);
}

static DWORD WINAPI InitThread(LPVOID param)
{
	for (int tick = 0; tick < INIT_MAX_TICKS && !gStop; tick++)
	{
		Sleep(100);
		if (tick < INIT_DELAY_TICKS || !RendererReady())
		{
			continue;
		}

		if (InstallHooks())
		{
			Utilities::Notify(XNOTIFYUI_TYPE_GENERIC, (const PWCHAR)L"OpenGSC [MW2]: Hold LT + DPAD RIGHT to open");
		}
		break;
	}
	gInitRunning = FALSE;
	return 0;
}

void Menu_Init()
{
	gStop = FALSE;
	gInitRunning = TRUE;
	if (!Utilities::CreateThread((PVOID)InitThread))
	{
		gInitRunning = FALSE;
	}
}

void Menu_Shutdown()
{
	gStop = TRUE;
	for (int waited = 0; (gInitRunning || gLoading) && waited < SHUTDOWN_WAIT_MS; waited += 50)
	{
		Sleep(50);
	}

	bMenuOpen = false;
	SetSwallow(false);
	if (R_EndFrame_Detour.IsDetoured())
	{
		R_EndFrame_Detour.TakeDownDetour();
	}
	if (XamInputGetState_Detour.IsDetoured())
	{
		XamInputGetState_Detour.TakeDownDetour();
	}
	Sleep(100);
}
