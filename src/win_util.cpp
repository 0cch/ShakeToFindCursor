#include "targetver.h"

#include "win_util.h"

#include <shellapi.h>

namespace stfc {

void AdoptDpiAwareness() { SetProcessDPIAware(); }

bool IsElevated() {
  BOOL elevated = FALSE;
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    TOKEN_ELEVATION info = {};
    DWORD size = sizeof(info);
    if (GetTokenInformation(token, TokenElevation, &info, sizeof(info), &size)) {
      elevated = info.TokenIsElevated;
    }
    CloseHandle(token);
  }
  return elevated != FALSE;
}

void GetExePath(wchar_t* out, size_t cch) {
  out[0] = L'\0';
  if (GetModuleFileNameW(nullptr, out, static_cast<DWORD>(cch)) == 0) {
    out[0] = L'\0';
  }
}

void RemoveFileName(wchar_t* path) {
  wchar_t* slash = wcsrchr(path, L'\\');
  if (slash) *slash = L'\0';
}

bool RelaunchElevated(const wchar_t* arguments) {
  wchar_t exe[MAX_PATH];
  GetExePath(exe, MAX_PATH);
  if (exe[0] == L'\0') return false;

  SHELLEXECUTEINFOW info = {};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_FLAG_DDEWAIT;
  info.hwnd = nullptr;
  info.lpVerb = L"runas";
  info.lpFile = exe;
  info.lpParameters = (arguments && arguments[0]) ? arguments : nullptr;
  info.lpDirectory = nullptr;
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info)) return false;
  return info.hProcess != nullptr;
}

bool IsForegroundFullscreen() {
  HWND foreground = GetForegroundWindow();
  if (!foreground) return false;

  WINDOWPLACEMENT placement = {};
  placement.length = sizeof(placement);
  if (GetWindowPlacement(foreground, &placement) &&
      placement.showCmd == SW_SHOWMINIMIZED) {
    return false;
  }

  // A layered or tool window covering the screen is an overlay, not a game.
  const LONG_PTR style = GetWindowLongPtrW(foreground, GWL_STYLE);
  const LONG_PTR ex_style = GetWindowLongPtrW(foreground, GWL_EXSTYLE);
  if (ex_style & (WS_EX_LAYERED | WS_EX_TOOLWINDOW)) return false;
  if ((style & (WS_CAPTION | WS_THICKFRAME)) == 0 &&
      (style & WS_POPUP) == 0) {
    return false;
  }

  RECT window = {};
  if (!GetWindowRect(foreground, &window)) return false;
  HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfo(monitor, &info)) return false;

  return window.left <= info.rcMonitor.left &&
         window.top <= info.rcMonitor.top &&
         window.right >= info.rcMonitor.right &&
         window.bottom >= info.rcMonitor.bottom;
}

}  // namespace stfc
