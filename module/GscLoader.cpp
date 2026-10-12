#include "stdafx.h"
#include <malloc.h>

#define GSC_DIR_PATH "hdd:\\GSC\\MW2"
#define GSC_BAD      "invalid gscbin: "

// Keep load buffers in the module image to avoid large runtime allocations.
// Only the code holding the operation flag may use them.
#define GSC_FILE_BUFFER_SIZE  0x60000   // 384 KB for raw bytecode (up to GSC_MAXIMUM_SIZE) and inline strings.
#define GSC_CODE_BUFFER_SIZE  0x38000   // Decoded bytecode and the image backup used during a swap.

// Game addresses.
static const DWORD SCRIPT_IMAGE            = 0x82500500;
static const DWORD SCRIPT_POINTER          = 0x823D2DE8;
static const DWORD SCRIPT_LOCK             = 0x838685AC;
static const DWORD SCRIPT_LOAD_HOOK        = 0x82249D78;   // Scr_SetLoading, called at the start and the end of G_LoadScripts
static const DWORD SCRIPT_HOOK_CAVE        = 0x823C76D0;
static const DWORD SCRIPT_HOOK_STATE       = 0x823C7900;
static const DWORD SCRIPT_RESUME_PATCH     = 0x8224FE64;
static const DWORD SCRIPT_REDIRECT_START   = 0x82241AF0;
static const DWORD STOCK_SCRIPT_POINTER    = 0x823A3510;
static const DWORD SCRIPT_VM_ACTIVE        = 0x83593C14;   // thread tree of the script VM, set by the script init and zeroed by the shutdown
static const DWORD CLIENT_CONNECTION_STATE = 0x8267FC48;   // connection state of local client 0: 0 disconnected (main menu), 9 active in a match
static const DWORD CL_INGAME_DVAR          = 0x825E21F0;   // dvar pointer of cl_ingame (1 while the connection state is 9)
static const DWORD SV_RUNNING_DVAR         = 0x831B9014;   // dvar pointer of sv_running (cleared by the server shutdown after the game shutdown)
static const DWORD DVAR_VALUE_OFFSET       = 8;            // current value of a bool dvar
static const DWORD SL_GET_STRING           = 0x82242250;   // SL_GetString(string, user): id of the string, one reference per user bit
static const DWORD SL_SWEEP_USER           = 0x82242B50;   // SL_SweepUser(user): drops the reference of every string that has the user bit

// Script sizes and memory layout.
static const DWORD SCRIPT_IMAGE_SIZE       = 0x38000;      // stock script image
static const DWORD SCRIPT_MARKER_OFFSET    = 0x38004;      // pool pointer that points back into the image
static const DWORD SCRIPT_POOL_SIZE        = 0x38008;
static const DWORD SCRIPT_ALLOCATION_SIZE  = 0x40000;
static const DWORD SCRIPT_REDIRECT_LENGTH  = 0x1164;
static const DWORD HOOK_STATE_SIZE         = 12;
static const DWORD GSC_MAXIMUM_SIZE        = 0x37FFF;
static const DWORD GSC_MAX_STRING_LENGTH   = 0x1FFF;       // Maximum length accepted by the string table.
static const DWORD GSC_MAX_TOKEN_LENGTH    = 255;
static const DWORD GSC_MAX_NAME_LENGTH     = 128;
static const char  GSC_EXTENSION[]         = ".gscbin";

// The game uses user bits 1, 2, 4, 8 and 0x10, leaving our bits untouched by its sweeps.
// Alternate between two bits so the old script keeps its strings until the new one is registered.
static const DWORD GSC_USER_A              = 0x20;
static const DWORD GSC_USER_B              = 0x40;

// Menu stability and shutdown timing.
static const DWORD MENU_SETTLE_FRAMES      = 180;          // frames (about 3 seconds) the main menu has to be stable before a load or unload starts
static const DWORD MENU_SAMPLE_INTERVAL    = 4;            // IsMainMenu reads several game values, so Mw2Gsc_Tick only samples it every 4th frame
static const DWORD MENU_SETTLE_SAMPLES     = MENU_SETTLE_FRAMES / MENU_SAMPLE_INTERVAL;
static const DWORD SHUTDOWN_WAIT_MS        = 5000;         // how long Mw2Gsc_Shutdown waits for a running load

// 16-bit script-pool address operands, with stock and replacement values.
struct U16Patch
{
	DWORD address;
	WORD  stock;
	WORD  patched;
};

#define REDIRECT_HIGH(address)  { address, 0x8250, 0x823D }
#define REDIRECT_LOW(address)   { address, 0xE71C, 0x2DE8 }

static const U16Patch REDIRECTS[] =
{
	REDIRECT_HIGH(0x82241AF2), REDIRECT_HIGH(0x82241B32), REDIRECT_HIGH(0x82241EB6),
	REDIRECT_HIGH(0x8224244E), REDIRECT_HIGH(0x82242762), REDIRECT_HIGH(0x82242B6A),
	REDIRECT_LOW(0x82241AF6),  REDIRECT_LOW(0x82241B42),  REDIRECT_LOW(0x82241BE2), REDIRECT_LOW(0x82241ECA),
	REDIRECT_LOW(0x82241F72),  REDIRECT_LOW(0x8224202A),  REDIRECT_LOW(0x822420FA), REDIRECT_LOW(0x8224212A),
	REDIRECT_LOW(0x82242182),  REDIRECT_LOW(0x8224245E),  REDIRECT_LOW(0x82242486), REDIRECT_LOW(0x8224276E),
	REDIRECT_LOW(0x82242B7E),  REDIRECT_LOW(0x82242C52)
};
static const DWORD REDIRECT_COUNT = sizeof(REDIRECTS) / sizeof(REDIRECTS[0]);

// The hook registers its string table on each script load. Installation already registers
// our strings, so give the hook an empty table: one DWORD with a zero count.
// Store it after the copied script image so it remains valid for the lifetime of the pool.
static const DWORD NO_STRINGS_OFFSET = SCRIPT_POOL_SIZE;

typedef DWORD (*SlGetStringFn)(const char* string, DWORD user);
typedef void  (*SlSweepUserFn)(DWORD user);

// lis r11 / ori r11 / mtctr r11 / bctr to the hook cave
static const DWORD LOAD_HOOK_CODE[4] =
{
	0x3D600000 | (SCRIPT_HOOK_CAVE >> 16), 0x616B0000 | (SCRIPT_HOOK_CAVE & 0xFFFF), 0x7D6903A6, 0x4E800420
};

// 3D60835B    lis   r11, 0x835B
// 394B7C58    addi  r10, r11, 0x7C58
// 906A0010    stw   r3, 0x10(r10)
// 4E800020    blr
static const BYTE LOAD_HOOK_STOCK[] =
{
	0x3D, 0x60, 0x83, 0x5B, 0x39, 0x4B, 0x7C, 0x58, 0x90, 0x6A, 0x00, 0x10, 0x4E, 0x80, 0x00, 0x20
};

// mflr  r12
// stw   r12, -8(r1)
// std   r29, -32(r1)
// std   r30, -24(r1)
// std   r31, -16(r1)
// stwu  r1, -0x90(r1)
// mr    r31, r3
// lis   r11, 0x823C
// cmpwi r31, 0
// beq   0x0A8
//
// li    r10, 1
// stw   r10, 0x7904(r11)
// li    r10, 0
// stw   r10, 0x7908(r11)
// lwz   r30, 0x7900(r11)
// lwz   r29, 0(r30)
// addi  r30, r30, 4
// cmpwi r29, 0
// beq   0x088
//
// lwz   r3, 4(r30)
// li    r4, 0
// lis   r12, 0x8224
// ori   r12, r12, 0x2250
// mtctr r12
// bctrl                         ; call 0x82242250
// cmpwi r3, 0
// beq   0x098
// lwz   r11, 0(r30)
// subf  r10, r11, r3             ; r10 = r3 - r11
// sth   r10, 0(r11)              ; store the low 16 bits
// addi  r30, r30, 8
// addi  r29, r29, -1
// cmpwi r29, 0
// bne   0x04C
//
// lis   r11, 0x823C
// li    r10, 2
// stw   r10, 0x7904(r11)
// b     0x0FC
//
// lis   r11, 0x823C
// li    r10, -1
// stw   r10, 0x7904(r11)
// b     0x0FC
//
// lwz   r10, 0x7904(r11)
// cmpwi r10, 2
// bne   0x0FC
// li    r10, 3
// stw   r10, 0x7904(r11)
// lis   r3, 0x0006
// ori   r3, r3, 0xA280           ; r3 = 0x6A280, the script handle of image offset 0
// li    r4, 0
// lis   r12, 0x8225
// ori   r12, r12, 0x0270
// mtctr r12
// bctrl                         ; Scr_ExecThread(handle, 0) at 0x82250270, runs the entry of the image
// lis   r11, 0x823C
// stw   r3, 0x7908(r11)          ; thread id
// lis   r12, 0x8224
// ori   r12, r12, 0xB210
// mtctr r12
// bctrl                         ; Scr_FreeThread(thread id) at 0x8224B210
// lis   r11, 0x823C
// li    r10, 4
// stw   r10, 0x7904(r11)
//
// lis   r11, 0x835B
// stw   r31, 0x7C68(r11)         ; argument -> 0x835B7C68
// addi  r1, r1, 0x90
// ld    r29, -32(r1)
// ld    r30, -24(r1)
// ld    r31, -16(r1)
// lwz   r12, -8(r1)
// mtlr  r12
// blr
static const BYTE HOOK_HANDLER[] =
{
	0x7D, 0x88, 0x02, 0xA6, 0x91, 0x81, 0xFF, 0xF8, 0xFB, 0xA1, 0xFF, 0xE0, 0xFB, 0xC1, 0xFF, 0xE8,
	0xFB, 0xE1, 0xFF, 0xF0, 0x94, 0x21, 0xFF, 0x70, 0x7C, 0x7F, 0x1B, 0x78, 0x3D, 0x60, 0x82, 0x3C,
	0x2C, 0x1F, 0x00, 0x00, 0x41, 0x82, 0x00, 0x84, 0x39, 0x40, 0x00, 0x01, 0x91, 0x4B, 0x79, 0x04,
	0x39, 0x40, 0x00, 0x00, 0x91, 0x4B, 0x79, 0x08, 0x83, 0xCB, 0x79, 0x00, 0x83, 0xBE, 0x00, 0x00,
	0x3B, 0xDE, 0x00, 0x04, 0x2C, 0x1D, 0x00, 0x00, 0x41, 0x82, 0x00, 0x40, 0x80, 0x7E, 0x00, 0x04,
	0x38, 0x80, 0x00, 0x00, 0x3D, 0x80, 0x82, 0x24, 0x61, 0x8C, 0x22, 0x50, 0x7D, 0x89, 0x03, 0xA6,
	0x4E, 0x80, 0x04, 0x21, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x30, 0x81, 0x7E, 0x00, 0x00,
	0x7D, 0x4B, 0x18, 0x50, 0xB1, 0x4B, 0x00, 0x00, 0x3B, 0xDE, 0x00, 0x08, 0x3B, 0xBD, 0xFF, 0xFF,
	0x2C, 0x1D, 0x00, 0x00, 0x40, 0x82, 0xFF, 0xC8, 0x3D, 0x60, 0x82, 0x3C, 0x39, 0x40, 0x00, 0x02,
	0x91, 0x4B, 0x79, 0x04, 0x48, 0x00, 0x00, 0x68, 0x3D, 0x60, 0x82, 0x3C, 0x39, 0x40, 0xFF, 0xFF,
	0x91, 0x4B, 0x79, 0x04, 0x48, 0x00, 0x00, 0x58, 0x81, 0x4B, 0x79, 0x04, 0x2C, 0x0A, 0x00, 0x02,
	0x40, 0x82, 0x00, 0x4C, 0x39, 0x40, 0x00, 0x03, 0x91, 0x4B, 0x79, 0x04, 0x3C, 0x60, 0x00, 0x06,
	0x60, 0x63, 0xA2, 0x80, 0x38, 0x80, 0x00, 0x00, 0x3D, 0x80, 0x82, 0x25, 0x61, 0x8C, 0x02, 0x70,
	0x7D, 0x89, 0x03, 0xA6, 0x4E, 0x80, 0x04, 0x21, 0x3D, 0x60, 0x82, 0x3C, 0x90, 0x6B, 0x79, 0x08,
	0x3D, 0x80, 0x82, 0x24, 0x61, 0x8C, 0xB2, 0x10, 0x7D, 0x89, 0x03, 0xA6, 0x4E, 0x80, 0x04, 0x21,
	0x3D, 0x60, 0x82, 0x3C, 0x39, 0x40, 0x00, 0x04, 0x91, 0x4B, 0x79, 0x04, 0x3D, 0x60, 0x83, 0x5B,
	0x93, 0xEB, 0x7C, 0x68, 0x38, 0x21, 0x00, 0x90, 0xEB, 0xA1, 0xFF, 0xE0, 0xEB, 0xC1, 0xFF, 0xE8,
	0xEB, 0xE1, 0xFF, 0xF0, 0x81, 0x81, 0xFF, 0xF8, 0x7D, 0x88, 0x03, 0xA6, 0x4E, 0x80, 0x00, 0x20
};

// the cave is all zeros in the stock game
static const BYTE HOOK_CAVE_STOCK[sizeof(HOOK_HANDLER)] = {0};

// 3D60835D    lis   r11, 0x835D
// 396B8D7C    addi  r11, r11, -0x7284
static const BYTE RESUME_STOCK[] =
{
	0x3D, 0x60, 0x83, 0x5D, 0x39, 0x6B, 0x8D, 0x7C
};

// nop
// nop
static const BYTE RESUME_PATCHED[] =
{
	0x60, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00
};

// opcode operand classes, one character per opcode (row = high nibble):
//   . invalid      n none          b 1 byte        s aligned short   t token (or "&name")
//   S string       i aligned int   f aligned float v aligned vector  c 3 bytes
//   C 4 bytes      U byte + aligned short
// 0x69 is stored as 0x76, both take a byte operand.
static const char GSC_OP_CLASS[] =
	"nnnnt.nnnnnnbbnn"   // 0x00
	".bn.nbbssi..fSnn"   // 0x10
	"ccncnCCbbCCbb..S"   // 0x20
	"v..nnnnnnnnnnsnn"   // 0x30
	"nnnnsns.nnnn..ns"   // 0x40
	"nnnninnnnsnsc.tt"   // 0x50
	"tttttt.t.bbnnn.b"   // 0x60
	"tnttn.bssssssUss"   // 0x70
	"ssssUnnn.nnn...."   // 0x80
	"................"   // 0x90
	"................"   // 0xA0
	"................"   // 0xB0
	"................"   // 0xC0
	"................"   // 0xD0
	"................"   // 0xE0
	"................"   // 0xF0
	;
static const BYTE GSC_PAD_BYTE            = 0xFF;
static const BYTE GSC_OP_LOCAL_ASSIGN     = 0x69;
static const BYTE GSC_OP_LOCAL_ASSIGN_OUT = 0x76;

// Failure messages shown in notifications.
static const char* const FAIL_BUSY         = "game busy";
static const char* const FAIL_NO_MEMORY_DECODE  = "out of memory (decode)";
static const char* const FAIL_WRITE       = "memory write/verify failed";
static const char* const FAIL_NOT_READABLE = "game memory not readable";
static const char* const FAIL_MIXED_STATE  = "script state mixed, restart game";
static const char* const FAIL_TOO_LARGE    = "file too large (max 384 KB)";

enum GscPatchState
{
	GSC_STATE_STOCK,
	GSC_STATE_PATCHED,
	GSC_STATE_MIXED
};

// Remember the pool allocator so it can be released with the matching call.
enum GscPoolKind
{
	GSC_POOL_NONE,
	GSC_POOL_VIRTUAL,    // VirtualAlloc
	GSC_POOL_PHYSICAL,   // XPhysicalAlloc
	GSC_POOL_HEAP        // _aligned_malloc
};

static struct GscState
{
	// Installed script pool and the user bit keeping its strings alive.
	DWORD scriptBuffer;
	DWORD userBit;

	// Allocate the pool on the first install and reuse it, even after an unload.
	// Mw2Gsc_Shutdown releases it once the stock game has been restored.
	// Zero means no allocation is owned, including one abandoned after a failed rollback
	// because the game may still reference it. poolKind selects the matching free call.
	DWORD       poolMemory;
	GscPoolKind poolKind;

	// Original hook state captured on the first install for use during unload.
	BYTE  hookStateStock[HOOK_STATE_SIZE];
	bool  hookStateStockValid;

	// Loaded filename (empty if absent or not file-backed) and pending unload request.
	char          loadedName[MAX_PATH];
	char          pendingName[MAX_PATH];
	volatile bool unloadPending;

	// Keep only the queued filename until the game returns to the main menu.
	char          pendingLoadName[MAX_PATH];
	volatile bool loadPending;
	bool          pendingAnnounced;

	// Mw2Gsc_Tick sets this after MENU_SETTLE_FRAMES consecutive menu frames.
	volatile bool menuSettled;

	// Allow only one load or unload at a time, including file reads and decoding.
	// The holder also owns failReason, which records the first failure and is cleared when a load starts.
	volatile LONG operationActive;
	const char*   failReason;

	// Shutdown tells workers to stop and prevents new loads.
	volatile LONG stopping;
} gsc;

// These buffers are shared by LoadFileNow and its helpers under the operation flag.
// Script strings point into gFileBuffer during installation; no pointers survive the load.
__declspec(align(128)) static BYTE gFileBuffer[GSC_FILE_BUFFER_SIZE];     // raw file
__declspec(align(128)) static BYTE gCodeBuffer[GSC_CODE_BUFFER_SIZE];     // decoded bytecode of the script to install
__declspec(align(128)) static BYTE gImageBackup[GSC_CODE_BUFFER_SIZE];    // script image before a swap, for the rollback

static bool BeginOperation()
{
	return InterlockedCompareExchange(&gsc.operationActive, 1, 0) == 0;
}

static void EndOperation()
{
	InterlockedExchange(&gsc.operationActive, 0);
}

// Keep the first failure reason and return false.
static bool SetFail(const char* reason)
{
	if (gsc.failReason == NULL)
	{
		gsc.failReason = reason;
	}
	return false;
}

// The game is busy; the caller can retry later.
static bool SetBusy()
{
	gsc.failReason = FAIL_BUSY;
	return false;
}

// Record the first failure with its error codes. The format may omit the second code.
// The caller must hold the operation flag because the message buffer is shared.
static void SetFailCode(const char* format, DWORD first, DWORD second)
{
	static char text[64];
	if (gsc.failReason == NULL)
	{
		sprintf_s(text, sizeof(text), format, first, second);
		gsc.failReason = text;
	}
}

// Format an ASCII message and widen it for the thread-safe XNotify queue.
static void NotifyText(const char* format, ...)
{
	char msg[256];
	WCHAR wide[256];
	va_list args;
	va_start(args, format);
	_vsnprintf(msg, sizeof(msg) - 1, format, args);
	va_end(args);
	msg[sizeof(msg) - 1] = 0;

	int i = 0;
	do
	{
		wide[i] = (unsigned char)msg[i];
	}
	while (msg[i++] != 0);
	XNotifyQueueUI(XNOTIFYUI_TYPE_GENERIC, XUSER_INDEX_ANY, XNOTIFYUI_PRIORITY_HIGH, wide, 0);
}

static void NotifyFailure(const char* fileName, const char* reason)
{
	NotifyText("OpenGSC [MW2]: Failed to load%s%s\n%s", fileName[0] != 0 ? " " : "", fileName, reason != NULL ? reason : "unknown error");
}

// Check that every 4 KB page in the range is mapped.
static bool RangeValid(DWORD address, DWORD length)
{
	for (DWORD page = address & ~0xFFFu; page <= address + length - 1; page += 0x1000)
	{
		if (!MmIsAddressValid((PVOID)page))
		{
			return false;
		}
	}
	return true;
}

static bool ReadMem(DWORD address, void* dst, DWORD length)
{
	if (!RangeValid(address, length))
	{
		return false;
	}
	memcpy(dst, (const void*)address, length);
	return true;
}

static bool ReadU32(DWORD address, DWORD* value)
{
	return ReadMem(address, value, 4);
}

static void FlushRange(DWORD address, DWORD length)
{
	for (DWORD a = address & ~127u; a < address + length; a += 128)
	{
		__dcbf(0, (void*)a);
	}
	__sync();
	__isync();
}

// Write the bytes, then read them back to verify the result.
static bool WriteVerified(DWORD address, const void* data, DWORD length)
{
	if (RangeValid(address, length))
	{
		memcpy((void*)address, data, length);
		__dcbst(0, (void*)address);
		__sync();
		if (memcmp((const void*)address, data, length) == 0)
		{
			return true;
		}
	}
	return SetFail(FAIL_WRITE);
}

static bool WriteU32(DWORD address, DWORD value)
{
	return WriteVerified(address, &value, 4);
}

static bool WriteRedirects(bool patched)
{
	for (DWORD i = 0; i < REDIRECT_COUNT; i++)
	{
		WORD value = patched ? REDIRECTS[i].patched : REDIRECTS[i].stock;
		if (!WriteVerified(REDIRECTS[i].address, &value, 2))
		{
			return false;
		}
	}
	return true;
}

static GscPatchState ScriptPatchState()
{
	BYTE resume[sizeof(RESUME_STOCK)];
	if (!ReadMem(SCRIPT_RESUME_PATCH, resume, sizeof(resume)))
	{
		return GSC_STATE_MIXED;
	}
	bool stock = (memcmp(resume, RESUME_STOCK, sizeof(resume)) == 0);
	bool patched = (memcmp(resume, RESUME_PATCHED, sizeof(resume)) == 0);
	for (DWORD i = 0; i < REDIRECT_COUNT; i++)
	{
		WORD value = 0;
		if (!ReadMem(REDIRECTS[i].address, &value, 2))
		{
			return GSC_STATE_MIXED;
		}
		stock = stock && (value == REDIRECTS[i].stock);
		patched = patched && (value == REDIRECTS[i].patched);
	}
	return stock ? GSC_STATE_STOCK : (patched ? GSC_STATE_PATCHED : GSC_STATE_MIXED);
}

static GscPatchState ScriptHookState()
{
	BYTE hook[sizeof(LOAD_HOOK_STOCK)];
	BYTE cave[sizeof(HOOK_HANDLER)];
	if (!ReadMem(SCRIPT_LOAD_HOOK, hook, sizeof(hook)) || !ReadMem(SCRIPT_HOOK_CAVE, cave, sizeof(cave)))
	{
		return GSC_STATE_MIXED;
	}
	if (memcmp(hook, LOAD_HOOK_STOCK, sizeof(hook)) == 0 && memcmp(cave, HOOK_CAVE_STOCK, sizeof(cave)) == 0)
	{
		return GSC_STATE_STOCK;
	}
	if (gsc.scriptBuffer != 0 && memcmp(hook, LOAD_HOOK_CODE, sizeof(hook)) == 0 && memcmp(cave, HOOK_HANDLER, sizeof(cave)) == 0)
	{
		return GSC_STATE_PATCHED;
	}
	return GSC_STATE_MIXED;
}

static void FlushScriptCaches()
{
	FlushRange(SCRIPT_HOOK_CAVE, sizeof(HOOK_HANDLER));
	FlushRange(SCRIPT_REDIRECT_START, SCRIPT_REDIRECT_LENGTH);
	FlushRange(SCRIPT_RESUME_PATCH, sizeof(RESUME_PATCHED));
	FlushRange(SCRIPT_LOAD_HOOK, sizeof(LOAD_HOOK_CODE));
}

// Moves the image pointer stored in the pool copy from one base to the other (install and unload).
static void RelocateMarker(BYTE* pool, DWORD fromBase, DWORD toBase)
{
	DWORD* marker = (DWORD*)(pool + SCRIPT_MARKER_OFFSET);
	if (*marker >= fromBase && *marker < fromBase + SCRIPT_IMAGE_SIZE)
	{
		*marker += toBase - fromBase;
	}
}

struct GscDecoded
{
	BYTE*        bytecode;
	DWORD        bytecodeLen;
	DWORD*       strOffsets;
	const char** strValues;
	DWORD        strCount;
	DWORD        strCap;       // allocated entries of strOffsets and strValues
	const char** tokNames;
	DWORD        tokCount;
	DWORD        tokNameCap;   // allocated entries of tokNames
	DWORD*       tokOffsets;   // code offset of every named token operand, which holds the index into tokNames until the install
	DWORD        tokUses;
	DWORD        tokUseCap;    // allocated entries of tokOffsets
	DWORD*       tokTable;     // hash table of the token names (open addressing), holds the index into tokNames + 1, 0 = free
	DWORD        tokTableSize; // power of two, kept at least twice tokCount
	DWORD        entryCap;     // upper limit of strings, names and name uses (the bytecode length allows no more)
};

struct GscDecoder
{
	const BYTE* in;
	DWORD       inSize;
	DWORD       inPos;
	GscDecoded* d;
	DWORD       outPos;
};

// Free only the decoder arrays; bytecode belongs to the shared gCodeBuffer.
static void FreeDecoded(GscDecoded* d)
{
	free(d->strOffsets);
	free(d->strValues);
	free(d->tokNames);
	free(d->tokOffsets);
	free(d->tokTable);
	memset(d, 0, sizeof(*d));
}

static bool DecPut(GscDecoder& c, const BYTE* src, DWORD n)
{
	if (c.d->bytecodeLen - c.outPos < n)
	{
		return SetFail(GSC_BAD "output overflow");
	}
	memcpy(c.d->bytecode + c.outPos, src, n);
	c.outPos += n;
	return true;
}

static bool DecCopy(GscDecoder& c, DWORD n)
{
	if (c.inSize - c.inPos < n)
	{
		return SetFail(GSC_BAD "truncated data");
	}
	c.inPos += n;
	return DecPut(c, c.in + c.inPos - n, n);
}

// Keep operands within a 32-byte boundary by copying the padding already in the file.
// Align floats to four bytes first.
static bool DecAlign(GscDecoder& c, DWORD width, bool isFloat)
{
	for (;;)
	{
		bool floatPad = isFloat && (c.outPos & 3) != 0;
		if (!floatPad && (c.outPos & 31) + width <= 32)
		{
			return true;
		}
		if (c.inPos < c.inSize && c.in[c.inPos] != GSC_PAD_BYTE)
		{
			return SetFail(floatPad ? GSC_BAD "bad float pad" : GSC_BAD "bad align pad");
		}
		if (!DecCopy(c, 1))
		{
			return false;
		}
	}
}

// Read a null-terminated string and advance past it; return NULL if it is unterminated.
static const char* DecCString(GscDecoder& c)
{
	const char* s = (const char*)c.in + c.inPos;
	const void* end = memchr(s, 0, c.inSize - c.inPos);
	if (end == NULL)
	{
		SetFail(GSC_BAD "unterminated string");
		return NULL;
	}
	c.inPos += (DWORD)((const char*)end - s) + 1;
	return s;
}

// Hash the lowercase name with FNV-1a to match case-insensitive token lookup.
static DWORD HashTokenName(const char* name)
{
	DWORD hash = 2166136261u;
	for (; *name != 0; name++)
	{
		char ch = (*name >= 'A' && *name <= 'Z') ? (char)(*name + 32) : *name;
		hash = (hash ^ (BYTE)ch) * 16777619u;
	}
	return hash;
}

// Grow decoder arrays as entries arrive rather than allocating for the largest possible file.
// Resize to the requested element count; return false if allocation fails.
static bool ResizeArray(void** array, DWORD elements, DWORD elementSize)
{
	void* grown = realloc(*array, elements * elementSize);
	if (grown == NULL)
	{
		return SetFail(FAIL_NO_MEMORY_DECODE);
	}
	*array = grown;
	return true;
}

// Double the token hash table and reinsert the existing names.
static bool GrowTokenTable(GscDecoded* d)
{
	DWORD size = d->tokTableSize * 2;
	DWORD* table = (DWORD*)calloc(size, sizeof(DWORD));
	if (table == NULL)
	{
		return SetFail(FAIL_NO_MEMORY_DECODE);
	}
	DWORD mask = size - 1;
	for (DWORD i = 0; i < d->tokCount; i++)
	{
		DWORD slot = HashTokenName(d->tokNames[i]) & mask;
		while (table[slot] != 0)
		{
			slot = (slot + 1) & mask;
		}
		table[slot] = i + 1;
	}
	free(d->tokTable);
	d->tokTable = table;
	d->tokTableSize = size;
	return true;
}

// Find an existing token name or add it to the table.
// Set the failure reason and return false if the name limit or available memory is exhausted.
static bool FindOrAddToken(GscDecoded* d, const char* name, DWORD* index)
{
	DWORD mask = d->tokTableSize - 1;
	DWORD slot = HashTokenName(name) & mask;
	for (; d->tokTable[slot] != 0; slot = (slot + 1) & mask)
	{
		DWORD found = d->tokTable[slot] - 1;
		if (_stricmp(d->tokNames[found], name) == 0)
		{
			*index = found;
			return true;
		}
	}
	if (d->tokCount >= d->entryCap)
	{
		return false;
	}
	if (d->tokCount >= d->tokNameCap)
	{
		DWORD cap = (d->tokNameCap != 0) ? d->tokNameCap * 2 : 64;
		if (!ResizeArray((void**)&d->tokNames, cap, sizeof(const char*)))
		{
			return false;
		}
		d->tokNameCap = cap;
	}
	if ((d->tokCount + 1) * 2 > d->tokTableSize)
	{
		if (!GrowTokenTable(d))
		{
			return false;
		}
		mask = d->tokTableSize - 1;
		slot = HashTokenName(name) & mask;
		while (d->tokTable[slot] != 0)
		{
			slot = (slot + 1) & mask;
		}
	}
	d->tokNames[d->tokCount] = name;
	d->tokTable[slot] = d->tokCount + 1;
	*index = d->tokCount++;
	return true;
}

// Tokens are either raw 16-bit IDs or case-insensitive "&name" references.
// Save named tokens as index placeholders; installation replaces them with game string IDs.
static bool DecToken(GscDecoder& c)
{
	GscDecoded* d = c.d;
	if (!DecAlign(c, 2, false))
	{
		return false;
	}
	if (c.inPos >= c.inSize)
	{
		return SetFail(GSC_BAD "token operand missing");
	}
	if (c.in[c.inPos] != '&')
	{
		return DecCopy(c, 2);
	}
	const char* name = DecCString(c);
	if (name == NULL)
	{
		return false;
	}
	if (*++name == 0)
	{
		return SetFail(GSC_BAD "empty token name");
	}
	DWORD idx = 0;
	if (!FindOrAddToken(d, name, &idx) || idx > 0xFFFF || d->tokUses >= d->entryCap)
	{
		return SetFail(GSC_BAD "too many tokens");
	}
	if (d->tokUses >= d->tokUseCap)
	{
		DWORD cap = (d->tokUseCap != 0) ? d->tokUseCap * 2 : 64;
		if (!ResizeArray((void**)&d->tokOffsets, cap, sizeof(DWORD)))
		{
			return false;
		}
		d->tokUseCap = cap;
	}
	d->tokOffsets[d->tokUses++] = c.outPos;
	BYTE bytes[2] = { (BYTE)(idx >> 8), (BYTE)idx };
	return DecPut(c, bytes, 2);
}

// Record each string and its code offset, leaving a zero placeholder in the bytecode.
static bool DecString(GscDecoder& c)
{
	GscDecoded* d = c.d;
	static const BYTE placeholder[2] = {0, 0};
	if (!DecAlign(c, 2, false))
	{
		return false;
	}
	DWORD strOffset = c.outPos;
	const char* s = DecCString(c);
	if (s == NULL)
	{
		return false;
	}
	if (d->strCount >= d->entryCap)
	{
		return SetFail(GSC_BAD "too many strings");
	}
	if (d->strCount >= d->strCap)
	{
		DWORD cap = (d->strCap != 0) ? d->strCap * 2 : 64;
		if (!ResizeArray((void**)&d->strOffsets, cap, sizeof(DWORD)) || !ResizeArray((void**)&d->strValues, cap, sizeof(const char*)))
		{
			return false;
		}
		d->strCap = cap;
	}
	d->strOffsets[d->strCount] = strOffset;
	d->strValues[d->strCount++] = s;
	return DecPut(c, placeholder, 2);
}

static bool DecOperand(GscDecoder& c, char kind)
{
	switch (kind)
	{
		case 'n': return true;
		case 'b': return DecCopy(c, 1);
		case 's': return DecAlign(c, 2, false) && DecCopy(c, 2);
		case 't': return DecToken(c);
		case 'S': return DecString(c);
		case 'i': return DecAlign(c, 4, false) && DecCopy(c, 4);
		case 'f': return DecAlign(c, 4, true) && DecCopy(c, 4);
		case 'v': return DecAlign(c, 4, true) && DecCopy(c, 12);
		case 'c': return DecCopy(c, 3);
		case 'C': return DecCopy(c, 4);
		default:  return DecCopy(c, 1) && DecAlign(c, 2, false) && DecCopy(c, 2);   // 'U'
	}
}

static bool DecodeOpcodes(GscDecoder& c)
{
	while (c.outPos < c.d->bytecodeLen)
	{
		if (!DecCopy(c, 1))
		{
			return false;
		}
		BYTE op = c.in[c.inPos - 1];
		char kind = GSC_OP_CLASS[op];
		if (kind == '.')
		{
			return SetFail(GSC_BAD "unknown opcode");
		}
		if (op == GSC_OP_LOCAL_ASSIGN)
		{
			c.d->bytecode[c.outPos - 1] = GSC_OP_LOCAL_ASSIGN_OUT;
		}
		if (!DecOperand(c, kind))
		{
			return false;
		}
	}
	return true;
}

// Layout: u32 little endian bytecode length, then the opcode stream. Strings and token names point into data.
static bool DecodeGscbin(const BYTE* data, DWORD size, GscDecoded* d)
{
	memset(d, 0, sizeof(*d));
	if (data == NULL || size < 5)
	{
		return SetFail(GSC_BAD "file too small");
	}

	DWORD len = (DWORD)data[0] | ((DWORD)data[1] << 8) | ((DWORD)data[2] << 16) | ((DWORD)data[3] << 24);
	if (len == 0 || len > GSC_MAXIMUM_SIZE)
	{
		return SetFail(GSC_BAD "bad bytecode length");
	}
	d->bytecodeLen = len;
	d->entryCap = len / 3 + 1;

	// The bytecode fits in the static buffer because len is capped at GSC_MAXIMUM_SIZE.
	// Allocate string and name arrays on demand, then grow them as needed.
	d->bytecode = gCodeBuffer;
	d->tokTableSize = 64;
	d->tokTable = (DWORD*)calloc(d->tokTableSize, sizeof(DWORD));
	if (d->tokTable == NULL)
	{
		FreeDecoded(d);
		return SetFail(FAIL_NO_MEMORY_DECODE);
	}

	GscDecoder c = { data, size, 4, d, 0 };
	bool ok = DecodeOpcodes(c);
	if (ok && c.outPos != len)
	{
		ok = SetFail(GSC_BAD "length mismatch");
	}
	if (ok && c.inPos != c.inSize)
	{
		ok = SetFail(GSC_BAD "trailing data");
	}
	if (!ok)
	{
		FreeDecoded(d);
	}
	return ok;
}

static void PutU16(BYTE* p, DWORD value)
{
	p[0] = (BYTE)(value >> 8);
	p[1] = (BYTE)value;
}

// Release this user bit's string references through the game sweep, which takes the string lock.
static void ReleaseNames(DWORD user)
{
	((SlSweepUserFn)SL_SWEEP_USER)(user);
}

// Register token names and string literals in the active game string table, then patch the bytecode.
// Tokens store the ID directly, including existing game names; strings store an ID relative
// to the operand address. Game field names are lowercase.
// Hold the script lock and use the redirected table. The user bit keeps references alive
// until ReleaseNames releases them.
static bool ResolveNames(GscDecoded* d, DWORD user)
{
	SlGetStringFn getString = (SlGetStringFn)SL_GET_STRING;
	char name[GSC_MAX_TOKEN_LENGTH + 1];

	for (DWORD i = 0; i < d->tokUses; i++)
	{
		DWORD offset = d->tokOffsets[i];
		const char* source = d->tokNames[((DWORD)d->bytecode[offset] << 8) | d->bytecode[offset + 1]];
		if (strlen(source) > GSC_MAX_TOKEN_LENGTH)
		{
			return SetFail(GSC_BAD "token name too long");
		}
		DWORD n = 0;
		do
		{
			name[n] = (source[n] >= 'A' && source[n] <= 'Z') ? (char)(source[n] + 32) : source[n];
		}
		while (source[n++] != 0);

		DWORD id = getString(name, user);
		if (id == 0 || id > 0xFFFF)
		{
			return SetFail("string table refused a name");
		}
		PutU16(d->bytecode + offset, id);
	}

	for (DWORD i = 0; i < d->strCount; i++)
	{
		if (strlen(d->strValues[i]) > GSC_MAX_STRING_LENGTH)
		{
			return SetFail(GSC_BAD "string too long");
		}
		DWORD id = getString(d->strValues[i], user);
		if (id == 0 || id > 0xFFFF)
		{
			return SetFail("string table refused a string");
		}
		PutU16(d->bytecode + d->strOffsets[i], id - (SCRIPT_IMAGE + d->strOffsets[i]));
	}
	return true;
}

// One game-memory backup, at most 16 bytes, saved before installation.
struct BackupEntry
{
	DWORD address;
	DWORD length;
	BYTE  data[16];
};

// Save these entries first, then one entry per redirect.
enum BackupIndex
{
	BACKUP_POINTER,
	BACKUP_HOOK_STATE,
	BACKUP_LOAD_HOOK,
	BACKUP_RESUME_PATCH,
	BACKUP_FIXED_COUNT
};

// Back up everything installation changes except the hook cave, which is stock zeros on
// the first install and unchanged on a swap. The script image is stored separately:
//   First install: rollback uses the stock image copied into the pool.
//   Swap: gImageBackup holds the bytes replaced by the new bytecode.
struct BackupSet
{
	BackupEntry entries[BACKUP_FIXED_COUNT + REDIRECT_COUNT];
	DWORD       count;
	bool        imageFromPool;
	DWORD       poolAddress;
	DWORD       imageLength;   // bytes of the script image that the rollback writes back
};

static BackupSet gBackup;   // used under the operation flag only

static bool AddBackup(BackupSet* b, DWORD address, DWORD length)
{
	BackupEntry& e = b->entries[b->count];
	if (length > sizeof(e.data) || !ReadMem(address, e.data, length))
	{
		return SetFail(FAIL_NOT_READABLE);
	}
	e.address = address;
	e.length = length;
	b->count++;
	return true;
}

// Save the memory that installation will change. On the first install, also copy the stock
// image into the unused pool and relocate its marker pointer to the new base.
static bool BackupGameState(BackupSet* b, bool firstInstall, DWORD poolAddress, DWORD bytecodeLen)
{
	memset(b, 0, sizeof(*b));
	if (firstInstall)
	{
		if (!ReadMem(SCRIPT_IMAGE, (void*)poolAddress, SCRIPT_POOL_SIZE))
		{
			return SetFail(FAIL_NOT_READABLE);
		}
		RelocateMarker((BYTE*)poolAddress, SCRIPT_IMAGE, poolAddress);
		b->imageFromPool = true;
		b->poolAddress = poolAddress;
		b->imageLength = SCRIPT_POOL_SIZE;
	}
	else
	{
		if (bytecodeLen > sizeof(gImageBackup))
		{
			return SetFail(FAIL_TOO_LARGE);
		}
		if (!ReadMem(SCRIPT_IMAGE, gImageBackup, bytecodeLen))
		{
			return SetFail(FAIL_NOT_READABLE);
		}
		b->imageLength = bytecodeLen;
	}

	bool ok = AddBackup(b, SCRIPT_POINTER, 4)
		&& AddBackup(b, SCRIPT_HOOK_STATE, HOOK_STATE_SIZE)
		&& AddBackup(b, SCRIPT_LOAD_HOOK, sizeof(LOAD_HOOK_CODE))
		&& AddBackup(b, SCRIPT_RESUME_PATCH, sizeof(RESUME_STOCK));
	for (DWORD i = 0; ok && i < REDIRECT_COUNT; i++)
	{
		ok = AddBackup(b, REDIRECTS[i].address, 2);
	}
	return ok;
}

// Apply the patches after taking a backup; the caller handles rollback on failure.
// InstallScript fills poolAddress with the stock image on the first install.
// Register d's names under user and replace its bytecode placeholders with real IDs.
static bool WriteScriptPatches(bool firstInstall, DWORD poolAddress, GscDecoded* d, DWORD user)
{
	// state word 0 points to the empty string table in the pool allocation
	DWORD hookState[HOOK_STATE_SIZE / 4] = { poolAddress + NO_STRINGS_OFFSET, 0, 0 };
	if (firstInstall)
	{
		if (!WriteU32(poolAddress + NO_STRINGS_OFFSET, 0))
		{
			return false;
		}
		FlushRange(poolAddress, NO_STRINGS_OFFSET + 4);
	}
	if (!WriteVerified(SCRIPT_HOOK_STATE, hookState, sizeof(hookState)))
	{
		return false;
	}

	// redirects the game to our pool (first install only)
	if (firstInstall)
	{
		DWORD ptr = 0;
		if (!WriteVerified(SCRIPT_HOOK_CAVE, HOOK_HANDLER, sizeof(HOOK_HANDLER))
			|| !WriteU32(SCRIPT_POINTER, poolAddress)
			|| !WriteRedirects(true)
			|| !WriteVerified(SCRIPT_RESUME_PATCH, RESUME_PATCHED, sizeof(RESUME_PATCHED)))
		{
			return false;
		}
		if (ScriptPatchState() != GSC_STATE_PATCHED || !ReadU32(SCRIPT_POINTER, &ptr) || ptr != poolAddress)
		{
			return SetFail(FAIL_WRITE);
		}
	}

	// the game has to use our pool already, the names are registered in the table that stays
	if (!ResolveNames(d, user))
	{
		return false;
	}

	if (!WriteVerified(SCRIPT_IMAGE, d->bytecode, d->bytecodeLen) || !WriteVerified(SCRIPT_LOAD_HOOK, LOAD_HOOK_CODE, sizeof(LOAD_HOOK_CODE)))
	{
		return false;
	}

	FlushScriptCaches();
	FlushRange(SCRIPT_IMAGE, d->bytecodeLen);
	FlushRange(SCRIPT_HOOK_STATE, HOOK_STATE_SIZE);
	if (firstInstall)
	{
		FlushRange(poolAddress, SCRIPT_POOL_SIZE);
		FlushRange(SCRIPT_POINTER, 4);
	}
	return true;
}

// Return true only if every restore succeeded.
static bool RollbackPatches(const BackupSet* b)
{
	// On the first install, the pool holds the stock image and live string table.
	// Relocate its marker back to the game image before restoring. If the write fails,
	// move the marker back to the pool, which the game may still be using.
	const void* image = b->imageFromPool ? (const void*)b->poolAddress : (const void*)gImageBackup;
	if (b->imageFromPool)
	{
		RelocateMarker((BYTE*)b->poolAddress, b->poolAddress, SCRIPT_IMAGE);
	}
	bool restored = WriteVerified(SCRIPT_IMAGE, image, b->imageLength);
	for (DWORD i = 0; restored && i < b->count; i++)
	{
		restored = WriteVerified(b->entries[i].address, b->entries[i].data, b->entries[i].length);
	}
	if (restored && b->imageFromPool)
	{
		restored = WriteVerified(SCRIPT_HOOK_CAVE, HOOK_CAVE_STOCK, sizeof(HOOK_CAVE_STOCK));
	}
	if (!restored && b->imageFromPool)
	{
		RelocateMarker((BYTE*)b->poolAddress, SCRIPT_IMAGE, b->poolAddress);
	}
	// Replace the write failure reason with the rollback result.
	gsc.failReason = restored ? "memory write failed, rollback done" : "write failed, rollback FAILED - restart game";
	if (restored)
	{
		FlushScriptCaches();
		FlushRange(SCRIPT_IMAGE, b->imageLength);
	}
	return restored;
}

// SCRIPT_LOCK is game critical section 17, at stride 0x1C from 0x838683D0.
// String-table calls, including SL_GetString, SL_RemoveRefToString and sweeps,
// take this lock before accessing the hash table. Hold it while copying and redirecting
// that table. Return false if it is busy so the caller can retry.
static bool TakeScriptLock()
{
	if (!RangeValid(SCRIPT_LOCK + 0x14, 8))
	{
		return SetFail("cannot read script lock");
	}
	return TryEnterCriticalSection((LPCRITICAL_SECTION)SCRIPT_LOCK) || SetBusy();
}

static void ReleaseScriptLock()
{
	LeaveCriticalSection((LPCRITICAL_SECTION)SCRIPT_LOCK);
}

// Read the bool dvar referenced by dvarPointer.
// Return false if it is unregistered or its memory cannot be read.
static bool ReadDvarBool(DWORD dvarPointer, bool* value)
{
	DWORD dvar = 0;
	BYTE raw = 0;
	if (!ReadU32(dvarPointer, &dvar) || dvar == 0 || !ReadMem(dvar + DVAR_VALUE_OFFSET, &raw, 1))
	{
		return false;
	}
	*value = (raw != 0);
	return true;
}

// Check that the game is safely back at the main menu: client 0 is disconnected,
// cl_ingame and sv_running are clear, and neither the script VM nor a script load is active.
// The menu shows "main_text" at connection state 0; loading and matches use higher
// states, with 9 meaning active. Server shutdown clears sv_running after game shutdown.
// G_LoadScripts creates the VM and G_ShutdownGame destroys it. Hook state word 1
// stays in the range 1..3 from Scr_SetLoading(1) until the script run finishes.
static bool IsMainMenu()
{
	DWORD connection = 0;
	DWORD vm = 0;
	DWORD hookStep = 0;
	bool inGame = true;
	bool serverRunning = true;
	return ReadU32(CLIENT_CONNECTION_STATE, &connection) && connection == 0
		&& ReadDvarBool(CL_INGAME_DVAR, &inGame) && !inGame
		&& ReadDvarBool(SV_RUNNING_DVAR, &serverRunning) && !serverRunning
		&& ReadU32(SCRIPT_VM_ACTIVE, &vm) && vm == 0
		&& ReadU32(SCRIPT_HOOK_STATE + 4, &hookStep) && hookStep - 1 > 2;
}

// Set wroteMemory when the first game-memory write is attempted; before that, our buffers are private.
static bool InstallScript(bool firstInstall, DWORD poolAddress, GscDecoded* d, bool* wroteMemory)
{
	if (!TakeScriptLock())
	{
		return false;
	}

	BackupSet* backup = &gBackup;
	bool ok = BackupGameState(backup, firstInstall, poolAddress, d->bytecodeLen);
	if (ok && firstInstall)
	{
		// Keep the original hook state for unloading.
		memcpy(gsc.hookStateStock, backup->entries[BACKUP_HOOK_STATE].data, HOOK_STATE_SIZE);
		gsc.hookStateStockValid = true;
	}
	if (ok)
	{
		*wroteMemory = true;

		// Register new names under the user bit not used by the old script.
		DWORD oldUser = firstInstall ? 0 : gsc.userBit;
		DWORD newUser = (oldUser == GSC_USER_A) ? GSC_USER_B : GSC_USER_A;
		ok = WriteScriptPatches(firstInstall, poolAddress, d, newUser);
		if (ok)
		{
			// Release the old script's string references after replacing it.
			if (oldUser != 0)
			{
				ReleaseNames(oldUser);
			}
			gsc.userBit = newUser;
		}
		else
		{
			ReleaseNames(newUser);
			if (RollbackPatches(backup))
			{
				// The game uses the stock script again and no longer references the pool.
				*wroteMemory = false;
			}
		}
	}
	ReleaseScriptLock();
	return ok;
}

// Checks that the game is in a state we can install into.
static bool CheckInstallState(bool* firstInstall)
{
	GscPatchState state = ScriptPatchState();
	if (state == GSC_STATE_MIXED || state != ScriptHookState())
	{
		return SetFail(FAIL_MIXED_STATE);
	}

	DWORD ptr = 0;
	if (!ReadU32(SCRIPT_POINTER, &ptr))
	{
		return SetFail(FAIL_NOT_READABLE);
	}
	*firstInstall = (state == GSC_STATE_STOCK);
	if (*firstInstall && ptr != STOCK_SCRIPT_POINTER)
	{
		return SetFail("another GSC injector active");
	}
	if (!*firstInstall && ptr != gsc.scriptBuffer)
	{
		return SetFail("script state changed, restart game");
	}
	return true;
}

// Prefer system memory because the game may keep using the pool after the module detaches.
// Allocate once and reuse it to avoid repeated allocation/free cycles in a crowded address space.
// Try virtual memory, then physical memory, with the CRT heap as a last resort.
// Return 0 if every attempt fails; error keeps the Win32 error from the first attempt.
static DWORD AcquirePool(DWORD* error)
{
	if (gsc.poolMemory == 0)
	{
		void* memory = VirtualAlloc(NULL, SCRIPT_ALLOCATION_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (memory != NULL)
		{
			gsc.poolKind = GSC_POOL_VIRTUAL;
		}
		else
		{
			*error = GetLastError();
			memory = XPhysicalAlloc(SCRIPT_ALLOCATION_SIZE, MAXULONG_PTR, 4096, PAGE_READWRITE);
			if (memory != NULL)
			{
				gsc.poolKind = GSC_POOL_PHYSICAL;
			}
			else
			{
				memory = _aligned_malloc(SCRIPT_ALLOCATION_SIZE, 128);
				if (memory != NULL)
				{
					gsc.poolKind = GSC_POOL_HEAP;
				}
			}
		}
		gsc.poolMemory = (DWORD)memory;
	}
	return gsc.poolMemory;
}

// Releases the pool with the call that matches its allocator.
static void FreePool()
{
	switch (gsc.poolKind)
	{
		case GSC_POOL_VIRTUAL:
			VirtualFree((LPVOID)gsc.poolMemory, 0, MEM_RELEASE);
			break;
		case GSC_POOL_PHYSICAL:
			XPhysicalFree((LPVOID)gsc.poolMemory);
			break;
		case GSC_POOL_HEAP:
			_aligned_free((void*)gsc.poolMemory);
			break;
		default:
			break;
	}
	gsc.poolMemory = 0;
	gsc.poolKind = GSC_POOL_NONE;
}

static bool InjectDecoded(GscDecoded* d)
{
	bool firstInstall = false;
	if (!CheckInstallState(&firstInstall))
	{
		return false;
	}

	DWORD poolAddress = gsc.scriptBuffer;
	if (firstInstall)
	{
		gsc.scriptBuffer = 0;
		DWORD poolError = 0;
		poolAddress = AcquirePool(&poolError);
		if (poolAddress == 0)
		{
			SetFailCode("out of memory (pool 0x%X)", poolError, 0);
			return false;
		}
	}

	bool wroteMemory = false;
	if (!InstallScript(firstInstall, poolAddress, d, &wroteMemory))
	{
		// After a write, the game may reference the pool. Reuse it only if rollback
		// restored everything. Otherwise abandon it without freeing it, since the game
		// may still hold a pointer to it.
		if (firstInstall && wroteMemory)
		{
			gsc.poolMemory = 0;
			gsc.poolKind = GSC_POOL_NONE;
		}
		return false;
	}

	gsc.scriptBuffer = poolAddress;
	return true;
}

// Install only at the main menu, when script threads are no longer executing the image.
// Check again after decoding to keep the gap between the check and installation short.
static bool LoadBuffer(const unsigned char* gscbin, unsigned int size, const char* name)
{
	GscDecoded decoded;
	if (!DecodeGscbin(gscbin, size, &decoded))
	{
		return false;
	}

	bool ok = IsMainMenu() ? InjectDecoded(&decoded) : SetBusy();
	FreeDecoded(&decoded);
	if (ok)
	{
		// The new script supersedes any pending unload request.
		strncpy_s(gsc.loadedName, sizeof(gsc.loadedName), name, _TRUNCATE);
		gsc.unloadPending = false;
	}
	return ok;
}

// Probes the real target folder and creates the hdd: link only if that fails.
static void EnsureHdd()
{
	if (GetFileAttributes(GSC_DIR_PATH) == INVALID_FILE_ATTRIBUTES)
	{
		Utilities::CreateSymbolicLink("hdd:", "\\Device\\Harddisk0\\Partition1");
	}
}

// FILE_STANDARD_INFORMATION
struct GscStdInfo
{
	LARGE_INTEGER AllocationSize;
	LARGE_INTEGER EndOfFile;
	DWORD         NumberOfLinks;
	BYTE          DeletePending;
	BYTE          Directory;
};

// ReadGscFile fallback: read through the device path and return the open result in openStatus.
// On failure, return a NULL buffer and set the reason unless opening itself failed.
static BYTE* ReadGscFileNt(const char* fileName, DWORD* outSize, NTSTATUS* openStatus)
{
	char path[MAX_PATH];
	sprintf_s(path, MAX_PATH, "\\Device\\Harddisk0\\Partition1\\GSC\\MW2\\%s", fileName);
	STRING name;
	RtlInitAnsiString(&name, path);
	OBJECT_ATTRIBUTES oa;
	InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL);
	IO_STATUS_BLOCK iosb;
	memset(&iosb, 0, sizeof(iosb));
	HANDLE file = NULL;
	*openStatus = NtOpenFile(&file, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE,
		FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
	if (!NT_SUCCESS(*openStatus))
	{
		return NULL;
	}

	BYTE* buffer = NULL;
	DWORD size = 0;
	bool sizeKnown = false;
	GscStdInfo info;
	memset(&info, 0, sizeof(info));
	memset(&iosb, 0, sizeof(iosb));
	NTSTATUS queryStatus = NtQueryInformationFile(file, &iosb, &info, sizeof(info), FileStandardInformation);
	if (!NT_SUCCESS(queryStatus))
	{
		SetFailCode("file size query failed (0x%08X)", (DWORD)queryStatus, 0);
	}
	else if (info.EndOfFile.HighPart != 0)
	{
		SetFail(FAIL_TOO_LARGE);
	}
	else
	{
		size = info.EndOfFile.LowPart;
		sizeKnown = true;
	}

	if (!sizeKnown)
	{
		// The failure reason is already set.
	}
	else if (size < 5)
	{
		SetFailCode("file too small (%u bytes)", size, 0);
	}
	else if (size > GSC_FILE_BUFFER_SIZE)
	{
		SetFail(FAIL_TOO_LARGE);
	}
	else
	{
		memset(&iosb, 0, sizeof(iosb));
		NTSTATUS readStatus = NtReadFile(file, NULL, NULL, NULL, &iosb, gFileBuffer, size, NULL);
		if (!NT_SUCCESS(readStatus))
		{
			SetFailCode("read failed (0x%08X)", (DWORD)readStatus, 0);
		}
		else if (iosb.Information != size)
		{
			SetFailCode("short read (%u bytes)", (DWORD)iosb.Information, 0);
		}
		else
		{
			buffer = gFileBuffer;
		}
	}
	NtClose(file);
	*outSize = size;
	return buffer;
}

// Read an open file using Win32. Return true for success or a definitive failure with a reason.
// Return false to try the NT fallback, leaving the Win32 error in why.
static bool ReadOpenFile(HANDLE file, BYTE** buffer, DWORD* outSize, char* why, size_t whySize)
{
	DWORD size = GetFileSize(file, NULL);
	if (size == INVALID_FILE_SIZE)
	{
		sprintf_s(why, whySize, "file size query failed (0x%08X)", GetLastError());
		return false;
	}
	if (size < 5)
	{
		sprintf_s(why, whySize, "file too small (%u bytes)", size);
		return false;
	}
	if (size > GSC_FILE_BUFFER_SIZE)
	{
		SetFail(FAIL_TOO_LARGE);
		return true;
	}

	DWORD read = 0;
	if (!ReadFile(file, gFileBuffer, size, &read, NULL))
	{
		sprintf_s(why, whySize, "read failed (0x%08X)", GetLastError());
		return false;
	}
	if (read != size)
	{
		sprintf_s(why, whySize, "short read (%u bytes)", read);
		return false;
	}
	*buffer = gFileBuffer;
	*outSize = size;
	return true;
}

// Read the file into gFileBuffer, returning NULL and a reason on failure.
// If the normal open, size query or read fails, try the device path before giving up.
// Set ioError for failures that may be temporary; oversized files are not retryable.
static BYTE* ReadGscFile(const char* fileName, DWORD* outSize, bool* ioError)
{
	static char winReason[64];

	EnsureHdd();

	char path[MAX_PATH];
	sprintf_s(path, MAX_PATH, GSC_DIR_PATH "\\%s", fileName);
	BYTE* buffer = NULL;
	bool opened = false;
	bool conclusive = false;
	DWORD error = 0;
	winReason[0] = 0;
	HANDLE file = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
	{
		error = GetLastError();
	}
	else
	{
		opened = true;
		conclusive = ReadOpenFile(file, &buffer, outSize, winReason, sizeof(winReason));
		CloseHandle(file);
	}

	if (!conclusive)
	{
		NTSTATUS status = 0;
		buffer = ReadGscFileNt(fileName, outSize, &status);
		if (buffer == NULL)
		{
			// Prefer the NT failure reason if the file opened; otherwise use the Win32 error.
			if (!NT_SUCCESS(status))
			{
				if (opened)
				{
					if (gsc.failReason == NULL)
					{
						gsc.failReason = winReason;
					}
				}
				else
				{
					SetFailCode("could not open file (0x%08X 0x%08X)", error, (DWORD)status);
				}
			}
			GSC_LOG("Read failed: win32 open %d err 0x%08X (%s), nt open 0x%08X, reason %s", opened ? 1 : 0, error, winReason, (DWORD)status, gsc.failReason != NULL ? gsc.failReason : "none");
		}
		else
		{
			gsc.failReason = NULL;
		}
	}
	*ioError = (buffer == NULL && gsc.failReason != FAIL_TOO_LARGE);
	return buffer;
}

// Distinguish temporary file errors so queued loads can retry them.
enum LoadStatus
{
	LOAD_OK,
	LOAD_BUSY,
	LOAD_IO_ERROR,
	LOAD_FAILED
};

// Read, decode and install using the shared buffers while holding the operation flag.
// Only the installed script survives this call. reason reports the first failure, or NULL on success.
static LoadStatus LoadFileNow(const char* fileName, const char** reason)
{
	gsc.failReason = NULL;
	bool ok = false;
	bool ioError = false;
	if (gsc.stopping)
	{
		SetFail("loader stopped");
	}
	else if (!IsMainMenu())
	{
		// Skip reading and decoding when installation cannot proceed.
		SetBusy();
	}
	else
	{
		DWORD size = 0;
		BYTE* buffer = ReadGscFile(fileName, &size, &ioError);
		ok = (buffer != NULL) && LoadBuffer(buffer, size, fileName);
	}

	*reason = gsc.failReason;
	if (ok)
	{
		return LOAD_OK;
	}
	if (gsc.failReason == FAIL_BUSY)
	{
		return LOAD_BUSY;
	}
	return ioError ? LOAD_IO_ERROR : LOAD_FAILED;
}

GscLoadResult Mw2Gsc_LoadByFileName(const char* fileName, unsigned int attempt)
{
	// Queue only the name until the menu settles; Mw2Gsc_Tick starts the load later.
	if (!gsc.menuSettled || !IsMainMenu())
	{
		strncpy_s(gsc.pendingLoadName, sizeof(gsc.pendingLoadName), fileName, _TRUNCATE);
		gsc.pendingAnnounced = false;
		gsc.loadPending = true;
		NotifyText("OpenGSC [MW2]: %s queued\nwill load in the main menu", fileName);
		return GSC_LOAD_QUEUED;
	}

	if (attempt == 0)
	{
		//NotifyText("OpenGSC [MW2]: Trying to load %s", fileName);
		GSC_LOG("Started loading %s", fileName);
	}

	// Exclude queued-load workers and unload operations while this load runs.
	LoadStatus status = LOAD_BUSY;
	const char* reason = NULL;
	if (BeginOperation())
	{
		status = LoadFileNow(fileName, &reason);
		EndOperation();
	}

	if (status == LOAD_OK)
	{
		gsc.loadPending = false;
		//NotifyText("OpenGSC [MW2]: %s loaded", fileName);
		GSC_LOG("Loaded %s", fileName);
		return GSC_LOAD_OK;
	}
	if (status != LOAD_BUSY)
	{
		NotifyFailure(fileName, reason);
		return GSC_LOAD_FAILED;
	}
	if (attempt == 0)
	{
		NotifyFailure(fileName, "game busy, will retry");
	}
	return GSC_LOAD_BUSY;
}

struct GscList
{
	char (*names)[MAX_PATH];
	int   max;
	int   count;
};

// Skips directories and other extensions, then inserts the name into the sorted list.
static void AddListEntry(const char* name, DWORD attrs, GscList* list)
{
	size_t n = strlen(name);
	if ((attrs & FILE_ATTRIBUTE_DIRECTORY) || n <= sizeof(GSC_EXTENSION) - 1 || n > GSC_MAX_NAME_LENGTH
		|| _stricmp(name + n - (sizeof(GSC_EXTENSION) - 1), GSC_EXTENSION) != 0 || list->count >= list->max)
	{
		return;
	}
	int i = list->count++;
	for (; i > 0 && _stricmp(name, list->names[i - 1]) < 0; i--)
	{
		strcpy_s(list->names[i], MAX_PATH, list->names[i - 1]);
	}
	strcpy_s(list->names[i], MAX_PATH, name);
}

// FILE_DIRECTORY_INFORMATION with ANSI names
struct GscDirInfo
{
	DWORD         NextEntryOffset;
	DWORD         FileIndex;
	LARGE_INTEGER CreationTime;
	LARGE_INTEGER LastAccessTime;
	LARGE_INTEGER LastWriteTime;
	LARGE_INTEGER ChangeTime;
	LARGE_INTEGER EndOfFile;
	LARGE_INTEGER AllocationSize;
	DWORD         FileAttributes;
	DWORD         FileNameLength;
	CHAR          FileName[1];
};

// Fallback: enumerate the device path with NtQueryDirectoryFile (360 variant takes a file mask).
static void ScanDirectoryNt(GscList* list)
{
	STRING dirName, mask;
	RtlInitAnsiString(&dirName, "\\Device\\Harddisk0\\Partition1\\GSC\\MW2");
	RtlInitAnsiString(&mask, "*");
	OBJECT_ATTRIBUTES oa;
	InitializeObjectAttributes(&oa, &dirName, OBJ_CASE_INSENSITIVE, NULL);
	IO_STATUS_BLOCK iosb;
	memset(&iosb, 0, sizeof(iosb));
	HANDLE dir = NULL;
	NTSTATUS st = NtOpenFile(&dir, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
	if (!NT_SUCCESS(st))
	{
		return;
	}

	static BYTE buf[4096];
	for (bool first = true; ; first = false)
	{
		memset(&iosb, 0, sizeof(iosb));
		st = NtQueryDirectoryFile(dir, NULL, NULL, NULL, &iosb, buf, sizeof(buf), first ? &mask : NULL, 0);
		if (!NT_SUCCESS(st) || iosb.Information == 0)
		{
			break;
		}
		for (DWORD off = 0; ; )
		{
			const GscDirInfo* e = (const GscDirInfo*)(buf + off);
			char name[MAX_PATH];
			DWORD len = min(e->FileNameLength, (DWORD)MAX_PATH - 1);
			memcpy(name, e->FileName, len);
			name[len] = 0;
			AddListEntry(name, e->FileAttributes, list);
			if (e->NextEntryOffset == 0 || off + e->NextEntryOffset >= sizeof(buf))
			{
				break;
			}
			off += e->NextEntryOffset;
		}
	}
	NtClose(dir);
}

int Mw2Gsc_ListFiles(char names[][MAX_PATH], int max)
{
	GscList list = { names, max, 0 };
	EnsureHdd();

	// Use "*" and filter extensions in AddListEntry; partial wildcards are unreliable.
	WIN32_FIND_DATA fd;
	HANDLE h = FindFirstFile(GSC_DIR_PATH "\\*", &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			AddListEntry(fd.cFileName, fd.dwFileAttributes, &list);
		}
		while (FindNextFile(h, &fd));
		FindClose(h);
	}

	if (list.count == 0)
	{
		ScanDirectoryNt(&list);
	}
	return list.count;
}

// Restores the stock state, the caller holds the operation flag. reason stays NULL when the game was just busy.
static bool RestoreStock(const char** reason)
{
	if (gsc.scriptBuffer == 0)
	{
		gsc.loadedName[0] = 0;
		return true;
	}
	DWORD ptr = 0;
	if (ScriptPatchState() != GSC_STATE_PATCHED || ScriptHookState() != GSC_STATE_PATCHED
		|| !ReadU32(SCRIPT_POINTER, &ptr) || ptr != gsc.scriptBuffer || !gsc.hookStateStockValid)
	{
		*reason = FAIL_MIXED_STATE;
		return false;
	}
	if (!TakeScriptLock())
	{
		return false;
	}

	// Restore the stock image directly from the pool after relocating its marker.
	// No extra copy is needed. If restoration fails, move the marker back because
	// the game may still use the pool.
	bool ok = false;
	BYTE* image = (BYTE*)gsc.scriptBuffer;
	if (!RangeValid(gsc.scriptBuffer, SCRIPT_POOL_SIZE))
	{
		*reason = FAIL_NOT_READABLE;
	}
	else
	{
		// Release script strings in the pool before copying it back to the game.
		ReleaseNames(GSC_USER_A);
		ReleaseNames(GSC_USER_B);
		RelocateMarker(image, gsc.scriptBuffer, SCRIPT_IMAGE);

		// Restore the load hook first to stop new cave calls; restore the cave last.
		ok = WriteVerified(SCRIPT_LOAD_HOOK, LOAD_HOOK_STOCK, sizeof(LOAD_HOOK_STOCK))
			&& WriteVerified(SCRIPT_RESUME_PATCH, RESUME_STOCK, sizeof(RESUME_STOCK))
			&& WriteRedirects(false)
			&& WriteU32(SCRIPT_POINTER, STOCK_SCRIPT_POINTER)
			&& WriteVerified(SCRIPT_IMAGE, image, SCRIPT_POOL_SIZE)
			&& WriteVerified(SCRIPT_HOOK_STATE, gsc.hookStateStock, HOOK_STATE_SIZE)
			&& WriteVerified(SCRIPT_HOOK_CAVE, HOOK_CAVE_STOCK, sizeof(HOOK_CAVE_STOCK));
		FlushScriptCaches();
		FlushRange(SCRIPT_IMAGE, SCRIPT_POOL_SIZE);
		FlushRange(SCRIPT_HOOK_STATE, HOOK_STATE_SIZE);
		FlushRange(SCRIPT_POINTER, 4);
		if (!ok)
		{
			RelocateMarker(image, SCRIPT_IMAGE, gsc.scriptBuffer);
			*reason = FAIL_WRITE;
		}
	}
	ReleaseScriptLock();

	if (ok)
	{
		// Keep the pool for the next install; Mw2Gsc_Shutdown releases it.
		gsc.scriptBuffer = 0;
		gsc.userBit = 0;
		gsc.hookStateStockValid = false;
		gsc.loadedName[0] = 0;
	}
	return ok;
}

// Restores the stock state. reason stays NULL when the game was just busy.
static bool UnloadInternal(const char** reason)
{
	*reason = NULL;
	if (!BeginOperation())
	{
		return false;
	}
	bool ok = RestoreStock(reason);
	EndOperation();
	return ok;
}

// Unloads now and reports. Returns false only when the game was busy and a retry makes sense.
static bool FinishUnload(const char* fileName)
{
	const char* reason = NULL;
	if (UnloadInternal(&reason))
	{
		//NotifyText("OpenGSC [MW2]: %s unloaded", fileName);
		GSC_LOG("Unloaded %s", fileName);
	}
	else if (reason == NULL)
	{
		return false;
	}
	else
	{
		NotifyText("OpenGSC [MW2]: Failed to unload %s\n%s", fileName, reason);
	}
	gsc.unloadPending = false;
	return true;
}

bool Mw2Gsc_RequestUnload()
{
	if (gsc.loadedName[0] == 0)
	{
		NotifyText("OpenGSC [MW2]: nothing loaded");
		return false;
	}
	// Keep a filename copy because UnloadInternal clears loadedName.
	char name[MAX_PATH];
	strncpy_s(name, sizeof(name), gsc.loadedName, _TRUNCATE);
	if (gsc.menuSettled && IsMainMenu() && FinishUnload(name))
	{
		return true;
	}
	strncpy_s(gsc.pendingName, sizeof(gsc.pendingName), name, _TRUNCATE);
	gsc.unloadPending = true;
	gsc.loadPending = false;
	NotifyText("OpenGSC [MW2]: %s unload pending\nwill unload in the main menu", name);
	return true;
}

// Sleeps in short steps so that Mw2Gsc_Shutdown does not have to wait for the whole time.
static void WaitStep(DWORD milliseconds)
{
	for (DWORD waited = 0; waited < milliseconds && !gsc.stopping; waited += 50)
	{
		Sleep(50);
	}
}

// Load the queued file on a worker thread to keep file I/O off the game thread.
// Mw2Gsc_Tick acquires the operation flag and hands it to this worker.
// Keep the queue entry until success, a permanent failure or a replacement request,
// so the menu continues to show the load as pending.
static DWORD WINAPI QueuedLoadThread(LPVOID param)
{
	const int maxIoRetries = 5;
	const int maxBusyChecks = 600;
	char name[MAX_PATH];
	strncpy_s(name, sizeof(name), gsc.pendingLoadName, _TRUNCATE);
	if (!gsc.pendingAnnounced)
	{
		gsc.pendingAnnounced = true;
		//NotifyText("OpenGSC [MW2]: Trying to load %s", name);
		GSC_LOG("Started loading %s", name);
	}

	int ioRetries = 0;
	int busyChecks = 0;
	while (!gsc.stopping && gsc.loadPending && _stricmp(name, gsc.pendingLoadName) == 0 && IsMainMenu())
	{
		const char* reason = NULL;
		LoadStatus status = LoadFileNow(name, &reason);
		if (status == LOAD_OK)
		{
			gsc.loadPending = false;
			//NotifyText("OpenGSC [MW2]: %s loaded", name);
			GSC_LOG("Loaded %s", name);
			break;
		}
		if (gsc.stopping)
		{
			break;
		}
		if (status == LOAD_BUSY)
		{
			if (++busyChecks >= maxBusyChecks)
			{
				gsc.loadPending = false;
				NotifyFailure(name, "game busy, gave up");
				break;
			}
			WaitStep(100);
			continue;
		}
		if (status != LOAD_IO_ERROR || ioRetries >= maxIoRetries)
		{
			gsc.loadPending = false;
			NotifyFailure(name, reason);
			break;
		}
		ioRetries++;
		WaitStep(1000);
	}
	EndOperation();
	return 0;
}

// Starts the worker thread for the queued file, which takes over the operation flag.
static void StartQueuedLoad()
{
	if (!BeginOperation())
	{
		return;
	}
	if (!Utilities::CreateThread((PVOID)QueuedLoadThread))
	{
		EndOperation();
		gsc.loadPending = false;
		NotifyText("OpenGSC [MW2]: Failed to start load\nthread error");
	}
}

void Mw2Gsc_Tick()
{
	static DWORD frame = 0;
	static DWORD stableSamples = 0;

	// Sample IsMainMenu periodically and keep menuSettled between samples.
	if (frame++ % MENU_SAMPLE_INTERVAL == 0)
	{
		stableSamples = IsMainMenu() ? min(stableSamples + 1, MENU_SETTLE_SAMPLES) : 0;
		gsc.menuSettled = (stableSamples >= MENU_SETTLE_SAMPLES);
	}

	// Start file I/O on a worker only after the main menu has settled.
	if (gsc.loadPending && gsc.menuSettled && !gsc.stopping)
	{
		StartQueuedLoad();
	}
	// Recheck the menu before unloading because the cached sample may be stale.
	if (gsc.unloadPending && gsc.menuSettled && IsMainMenu())
	{
		FinishUnload(gsc.pendingName);
	}
}

void Mw2Gsc_Shutdown()
{
	InterlockedExchange(&gsc.stopping, 1);
	__sync();
	gsc.loadPending = false;
	gsc.unloadPending = false;

	// Wait for the active operation before attempting restoration and cleanup.
	for (DWORD waited = 0; gsc.operationActive != 0 && waited < SHUTDOWN_WAIT_MS; waited += 50)
	{
		Sleep(50);
	}
	Sleep(100);   // Allow worker routines time to return.

	// During a match or loading screen, the game still uses the installed script and hook.
	// Leave the patches and pool in place to avoid freeing memory that is still in use.
	if (gsc.scriptBuffer != 0 && !IsMainMenu())
	{
		return;
	}

	const char* reason = NULL;
	UnloadInternal(&reason);

	// Free the pool only when the stock game is restored and no operation is running.
	// Keep the allocation if unloading failed or a load is still active.
	if (BeginOperation())
	{
		if (gsc.scriptBuffer == 0 && gsc.poolMemory != 0)
		{
			FreePool();
		}
		EndOperation();
	}
}

void Mw2Gsc_NotifyBusyGiveUp(const char* fileName)
{
	NotifyFailure(fileName, "game busy, gave up");
}

const char* Mw2Gsc_GetLoadedName()
{
	return gsc.loadedName[0] != 0 ? gsc.loadedName : NULL;
}

const char* Mw2Gsc_GetQueuedName()
{
	return gsc.loadPending ? gsc.pendingLoadName : NULL;
}

bool Mw2Gsc_IsUnloadPending()
{
	return gsc.unloadPending;
}