// header.h : include file for standard system include files,
// or project specific include files
//

#pragma once

#define _CRT_SECURE_NO_WARNINGS

#include "targetver.h"
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files
#include <windows.h>
#include <TraceLoggingProvider.h>

// C RunTime Header Files
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>

#define IMG_WIDTH 858
#define IMG_HEIGHT 263

// {0C14E8D1-51D7-4460-B396-02EB6FFE8EF5}
TRACELOGGING_DECLARE_PROVIDER(hTrace);

LRESULT TvWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

