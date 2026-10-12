#include "stdafx.h"

BOOL APIENTRY DllMain(HANDLE handle, DWORD reason, PVOID reserved)
{
	switch (reason)
	{
		case DLL_PROCESS_ATTACH:
			Menu_Init();
			break;

		case DLL_PROCESS_DETACH:
			Menu_Shutdown();
			Mw2Gsc_Shutdown();
			break;
	}
	return TRUE;
}
