#include "app_settings.h"

#include <shlobj.h>
#include <strsafe.h>

#include <cstdio>
#include <cstring>

namespace stfc {
namespace {

constexpr const wchar_t* kValueSensitivity = L"Sensitivity";
constexpr const wchar_t* kValueEnlarge = L"EnlargePointer";
constexpr const wchar_t* kValueScale = L"ScalePercent";
constexpr const wchar_t* kValueGrow = L"GrowMs";
constexpr const wchar_t* kValueHold = L"HoldMs";
constexpr const wchar_t* kValueShrink = L"ShrinkMs";
constexpr const wchar_t* kValueOverlay = L"Overlay";
constexpr const wchar_t* kValueTracking = L"Tracking";
constexpr const wchar_t* kValueHotkey = L"Hotkey";
constexpr const wchar_t* kValueIgnoreFullscreen = L"IgnoreFullscreen";
constexpr const wchar_t* kValueLogging = L"Logging";
constexpr const wchar_t* kValueSeenIntro = L"SeenIntro";

constexpr size_t kLogRotateBytes = 256 * 1024;

bool ReadDword(HKEY key, const wchar_t* name, DWORD* out) {
  DWORD type = 0;
  DWORD size = sizeof(DWORD);
  if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(out),
                       &size) != ERROR_SUCCESS) {
    return false;
  }
  return type == REG_DWORD && size == sizeof(DWORD);
}

void ClampSetting(int& value, int lo, int hi) {
  if (value < lo) value = lo;
  if (value > hi) value = hi;
}

// %LOCALAPPDATA%\ShakeToFindCursor, created if missing. Returns false when no
// writable location could be found, in which case logging is simply off.
bool LogDirectory(wchar_t* out, size_t cch) {
  wchar_t base[MAX_PATH] = L"";
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) {
    if (GetEnvironmentVariableW(L"TEMP", base, MAX_PATH) == 0) return false;
  }
  if (FAILED(StringCchCatW(base, MAX_PATH, L"\\ShakeToFindCursor"))) return false;
  CreateDirectoryW(base, nullptr);
  return SUCCEEDED(StringCchCopyW(out, cch, base));
}

void LogV(const char* fmt, va_list args, bool always) {
  if (!always && !AppSettings::Get().logging) return;

  wchar_t dir[MAX_PATH];
  if (!LogDirectory(dir, MAX_PATH)) return;
  wchar_t path[MAX_PATH];
  if (FAILED(StringCchCopyW(path, MAX_PATH, dir)) ||
      FAILED(StringCchCatW(path, MAX_PATH, L"\\shake.log"))) {
    return;
  }

  // Share read *and* write so the log can be open in an editor while we append,
  // and so a second instance (e.g. the elevated one during a relaunch) does not
  // lock us out of logging entirely.
  const DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
  HANDLE file = CreateFileW(path, FILE_WRITE_DATA | FILE_APPEND_DATA, sharing,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) return;

  // Rotate: move the previous copy aside, then start a fresh file. Bounded,
  // readable in Notepad, and no dependency on a logging framework.
  LARGE_INTEGER size = {};
  if (GetFileSizeEx(file, &size) &&
      size.QuadPart > static_cast<LONGLONG>(kLogRotateBytes)) {
    CloseHandle(file);
    wchar_t backup[MAX_PATH];
    StringCchCopyW(backup, MAX_PATH, dir);
    StringCchCatW(backup, MAX_PATH, L"\\shake.old.log");
    MoveFileExW(path, backup, MOVEFILE_REPLACE_EXISTING);
    file = CreateFileW(path, FILE_WRITE_DATA | FILE_APPEND_DATA, sharing,
                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
  }

  char body[768];
  vsnprintf(body, sizeof(body), fmt, args);

  SYSTEMTIME st = {};
  GetLocalTime(&st);
  char line[900];
  const int n = snprintf(line, sizeof(line),
                         "%04d-%02d-%02d %02d:%02d:%02d.%03d  %s", st.wYear,
                         st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                         st.wMilliseconds, body);
  if (n <= 0) {
    CloseHandle(file);
    return;
  }
  size_t len = static_cast<size_t>(n);
  if (len > sizeof(line) - 3) len = sizeof(line) - 3;
  line[len++] = '\r';
  line[len++] = '\n';

  SetFilePointer(file, 0, nullptr, FILE_END);
  DWORD written = 0;
  WriteFile(file, line, static_cast<DWORD>(len), &written, nullptr);
  CloseHandle(file);
}

}  // namespace

AppSettings& AppSettings::Get() {
  static AppSettings instance;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    instance.Load();
  }
  return instance;
}

int AppSettings::OverlayDurationMs() const {
  const int total = grow_ms + hold_ms + shrink_ms;
  return total < 600 ? 600 : total;
}

void AppSettings::Load() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, AppSettings::KeyPath(), 0, KEY_READ,
                    &key) != ERROR_SUCCESS) {
    return;  // first run: the defaults above stand
  }
  DWORD value = 0;
  if (ReadDword(key, kValueSensitivity, &value))
    sensitivity = static_cast<int>(value);
  if (ReadDword(key, kValueEnlarge, &value)) enlarge_pointer = value != 0;
  if (ReadDword(key, kValueScale, &value)) scale_percent = static_cast<int>(value);
  if (ReadDword(key, kValueGrow, &value)) grow_ms = static_cast<int>(value);
  if (ReadDword(key, kValueHold, &value)) hold_ms = static_cast<int>(value);
  if (ReadDword(key, kValueShrink, &value)) shrink_ms = static_cast<int>(value);
  if (ReadDword(key, kValueOverlay, &value))
    overlay = static_cast<OverlayStyle>(value);
  if (ReadDword(key, kValueTracking, &value))
    tracking = static_cast<TrackingMode>(value);
  if (ReadDword(key, kValueHotkey, &value)) hotkey = value != 0;
  if (ReadDword(key, kValueIgnoreFullscreen, &value))
    ignore_fullscreen = value != 0;
  if (ReadDword(key, kValueLogging, &value)) logging = value != 0;
  if (ReadDword(key, kValueSeenIntro, &value)) seen_intro = value != 0;
  RegCloseKey(key);

  ClampSetting(sensitivity, 1, 100);
  ClampSetting(scale_percent, 150, 600);
  ClampSetting(grow_ms, 0, 1000);
  ClampSetting(hold_ms, 150, 8000);
  ClampSetting(shrink_ms, 0, 1500);
  const int overlay_value = static_cast<int>(overlay);
  if (overlay_value < 0 || overlay_value > 2) overlay = OverlayStyle::kRipple;
  const int tracking_value = static_cast<int>(tracking);
  if (tracking_value < 0 || tracking_value > 1) tracking = TrackingMode::kHook;
}

void AppSettings::Save() const {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, AppSettings::KeyPath(), 0, nullptr, 0,
                      KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return;
  }
  struct Item {
    const wchar_t* name;
    DWORD value;
  };
  const Item items[] = {
      {kValueSensitivity, static_cast<DWORD>(sensitivity)},
      {kValueEnlarge, enlarge_pointer ? 1u : 0u},
      {kValueScale, static_cast<DWORD>(scale_percent)},
      {kValueGrow, static_cast<DWORD>(grow_ms)},
      {kValueHold, static_cast<DWORD>(hold_ms)},
      {kValueShrink, static_cast<DWORD>(shrink_ms)},
      {kValueOverlay, static_cast<DWORD>(static_cast<int>(overlay))},
      {kValueTracking, static_cast<DWORD>(static_cast<int>(tracking))},
      {kValueHotkey, hotkey ? 1u : 0u},
      {kValueIgnoreFullscreen, ignore_fullscreen ? 1u : 0u},
      {kValueLogging, logging ? 1u : 0u},
      {kValueSeenIntro, seen_intro ? 1u : 0u},
  };
  for (const auto& item : items) {
    RegSetValueExW(key, item.name, 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&item.value), sizeof(DWORD));
  }
  RegCloseKey(key);
}

void Logf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  LogV(fmt, args, false);
  va_end(args);
}

void LogCritical(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  LogV(fmt, args, true);
  va_end(args);
}

}  // namespace stfc
