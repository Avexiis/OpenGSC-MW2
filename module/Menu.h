#pragma once

// True while the menu is open. The XamInputGetState hook hides the menu buttons from the game while set.
extern volatile bool bMenuOpen;

// Waits on a worker thread until the game is ready, then installs the hooks.
void Menu_Init();

// Removes the hooks and waits briefly for the worker threads.
void Menu_Shutdown();
