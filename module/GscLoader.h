#pragma once

// Installs compiled .gscbin files from hdd:\GSC\MW2\ into the running game.
// Loading can block while the game is busy, so call it from a worker thread.

 // Debug logging. Always supply at least one argument after the format string.
#define GSC_LOG(fmt, ...)  DbgPrint("OpenGSC [MW2]: " fmt "\n", __VA_ARGS__)

// Lists up to max .gscbin filenames from hdd:\GSC\MW2\.
// Names are sorted case-insensitively. Returns the number written.
int Mw2Gsc_ListFiles(char names[][MAX_PATH], int max);

// Possible results from Mw2Gsc_LoadByFileName.
enum GscLoadResult
{
	GSC_LOAD_OK,       // Script installed successfully.
	GSC_LOAD_QUEUED,   // Filename queued until the main menu; Mw2Gsc_Tick starts the load.
	GSC_LOAD_BUSY,     // Game is busy; try again later.
	GSC_LOAD_FAILED    // Load failed and the user has been notified.
};

// Loads hdd:\GSC\MW2\<fileName>. Include the .gscbin extension,
// as returned by Mw2Gsc_ListFiles.
// Use attempt = 0 initially and a value above 0 for busy retries
// to avoid repeating notifications.
// Outside the main menu, only the filename is queued; no file is read
// or memory allocated. Mw2Gsc_Tick starts the load when the menu is ready.
// A later load or unload request replaces the queued request.
GscLoadResult Mw2Gsc_LoadByFileName(const char* fileName, unsigned int attempt);

// Notifies the user that busy retries for fileName have been exhausted.
void Mw2Gsc_NotifyBusyGiveUp(const char* fileName);

// Returns the installed script's filename, or NULL if none was loaded from a file.
const char* Mw2Gsc_GetLoadedName();

// Returns the filename waiting for the main menu, or NULL if no load is queued.
const char* Mw2Gsc_GetQueuedName();

// Returns true if an unload is waiting for the main menu.
// Loading a script cancels the pending unload.
bool Mw2Gsc_IsUnloadPending();

// Requests an unload. At the main menu, attempts it immediately.
// Elsewhere, keeps it pending until Mw2Gsc_Tick can finish it safely,
// since script threads may still be using the image.
// Returns false if no script is loaded.
bool Mw2Gsc_RequestUnload();

// Call once per frame. Handles queued loads and pending unloads
// when the main menu is ready.
void Mw2Gsc_Tick();

// Call during detach to stop workers, wait for an active load,
// and restore the stock game state when safe.
// Mixed or foreign patch states and a busy game are left untouched.
// Outside the main menu, scripts may still be running, so the patches
// and their memory remain in place.
// The loader cannot be used after shutdown.
void Mw2Gsc_Shutdown();