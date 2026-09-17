// Windows helpers shared by the app modules.
#pragma once

#include <windows.h>

namespace stfc {

// Called before the first window exists. The manifest is what actually requests
// PerMonitorV2 (with a system-DPI fallback); this is the Win7-era call that
// covers a build whose manifest was stripped. It is spelled without a
// GetProcAddress dance on purpose: casting a FARPROC to a typed function
// pointer is C4191, a /W4 warning, and this project builds with /WX.
void AdoptDpiAwareness();

// True when the process has an elevated token (SetSystemCursor needs it).
bool IsElevated();

// Full path of this executable.
void GetExePath(wchar_t* out, size_t cch);

// Strips the file name in place, i.e. C:\Dir\App.exe -> C:\Dir
void RemoveFileName(wchar_t* path);

// Re-launch this executable with "runas" (one UAC prompt). Returns true when a
// new process was started.
bool RelaunchElevated(const wchar_t* arguments);

// True when the foreground window covers a whole monitor without a title bar
// (a game or a video). Used to avoid interrupting full screen content.
bool IsForegroundFullscreen();

}  // namespace stfc
