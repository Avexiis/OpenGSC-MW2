#include "stdafx.h"

namespace Utilities
{
	BOOL CreateSymbolicLink(LPCSTR drive, LPCSTR device)
	{
		char destination[MAX_PATH];
		sprintf_s(destination, MAX_PATH, OBJ_SYS_STRING, drive);

		ANSI_STRING linkName, deviceName;
		RtlInitAnsiString(&linkName, destination);
		RtlInitAnsiString(&deviceName, device);

		return (ObCreateSymbolicLink(&linkName, &deviceName) == S_OK ? TRUE : FALSE);
	}

	PVOID Resolve(PCHAR moduleName, DWORD ordinal) {
		HANDLE moduleHandle = 0;
		if (!NT_SUCCESS(XexGetModuleHandle(moduleName, &moduleHandle))) {
			DbgPrint("resolve: failed to get handle for %s.", moduleName);
			return 0;
		}

		PVOID pointer = 0;
		if (!NT_SUCCESS(XexGetProcedureAddress(moduleHandle, ordinal, &pointer))) {
			DbgPrint("resolve: failed to get address from ordinal 0x%X from %s.", ordinal, moduleName);
			return 0;
		}

		return pointer;
	}

	BOOL CreateThread(PVOID routine)
	{
		HANDLE threadHandle = 0;
		DWORD threadId = 0;

		HRESULT result = ExCreateThread(&threadHandle, 0, &threadId, (PVOID)XapiThreadStartup, (LPTHREAD_START_ROUTINE)routine, NULL, 0x2 | CREATE_SUSPENDED);
		if (FAILED(result))
		{
			DbgPrint("createthread: error 0x%X", result);
			return FALSE;
		}

		XSetThreadProcessor(threadHandle, 4);
		SetThreadPriority(threadHandle, THREAD_PRIORITY_ABOVE_NORMAL);

		ResumeThread(threadHandle);
		CloseHandle(threadHandle);
		return TRUE;
	}

	VOID Notify(XNOTIFYQUEUEUI_TYPE type, const PWCHAR format, ...)
	{
		WCHAR buffer[256];

		va_list args;
		va_start(args, format);
		_vsnwprintf(buffer, _countof(buffer) - 1, format, args);
		va_end(args);

		buffer[_countof(buffer) - 1] = L'\0';

		XNotifyQueueUI(type, XUSER_INDEX_ANY, XNOTIFYUI_PRIORITY_HIGH, buffer, 0);
	}
}
