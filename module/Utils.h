#pragma once

namespace Utilities
{
	BOOL CreateSymbolicLink(LPCSTR drive, LPCSTR device);
	PVOID Resolve(PCHAR moduleName, DWORD ordinal);
	BOOL CreateThread(PVOID routine);
	VOID Notify(XNOTIFYQUEUEUI_TYPE type, const PWCHAR format, ...);
}
