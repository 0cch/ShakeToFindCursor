#pragma once

// Pin the APIs we use to Windows 7 so nothing silently requires a newer SDK
// (the project advertises "Windows 7 or later"). Include this header before
// <windows.h> in every translation unit.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// OCR_NORMAL & friends (the system cursor ids) are only declared with
// OEMRESOURCE in effect.
#ifndef OEMRESOURCE
#define OEMRESOURCE
#endif

#include <windows.h>
#include <strsafe.h>
