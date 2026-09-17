// Persisted user settings and the (optional) debug log.
//
// Settings live in HKCU\Software\ShakeToFindCursor so that nothing needs to be
// written next to the executable (Program Files is not writable, and the old
// build logged into its working directory, which silently failed there).
#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdint>

#include "shake_detector.h"

namespace stfc {

enum class OverlayStyle : int {
  kNone = 0,
  kRipple = 1,     // expanding rings + marker dot at the pointer
  kSpotlight = 2  // dim the monitor, punch a soft hole around the pointer
};

enum class TrackingMode : int {
  kHook = 0,    // WH_MOUSE_LL: per-event samples, zero idle cost
  kPolling = 1  // GetCursorPos on a timer: fallback when hooks are blocked
};

struct AppSettings {
  // Shake detection.
  int sensitivity = ShakeTuning::kDefaultSensitivity;  // 1..100
  // Pointer enlargement.
  bool enlarge_pointer = true;
  int scale_percent = 300;  // 150..600
  int grow_ms = 140;
  int hold_ms = 900;
  int shrink_ms = 240;
  // Highlight overlay.
  OverlayStyle overlay = OverlayStyle::kRipple;
  // Behaviour.
  TrackingMode tracking = TrackingMode::kHook;
  bool hotkey = true;              // Ctrl+Alt+F finds the pointer on demand
  bool ignore_fullscreen = false;  // do not trigger while a fullscreen app is up
  bool logging = false;
  bool seen_intro = false;  // the introductory balloon has been shown

  static AppSettings& Get();
  void Load();
  void Save() const;

  ShakeTuning DetectorTuning() const {
    return ShakeTuning::ForSensitivity(sensitivity);
  }
  // The overlay should stay up at least as long as the pointer is enlarged,
  // otherwise the highlight disappears before the user has looked at it.
  int OverlayDurationMs() const;

  static const wchar_t* KeyPath() { return L"Software\\ShakeToFindCursor"; }
};

// Debug logging. Logf() is a no-op unless the user turned logging on;
// LogCritical() always writes, so a start-up failure is diagnosable without
// asking the user to reproduce it with a debugger.
void Logf(const char* fmt, ...);
void LogCritical(const char* fmt, ...);

// Monotonic clock in microseconds, used for all animation and detection math.
// Split into whole seconds plus remainder so the multiply cannot overflow
// 64-bit math on a machine with a 10 MHz counter that has been up for a week.
inline int64_t NowUs() {
  static const LARGE_INTEGER kFrequency = [] {
    LARGE_INTEGER f = {};
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER now = {};
  QueryPerformanceCounter(&now);
  const long long freq = kFrequency.QuadPart > 0 ? kFrequency.QuadPart : 1;
  const long long seconds = now.QuadPart / freq;
  const long long remainder = now.QuadPart % freq;
  return static_cast<int64_t>(seconds * 1000000 + remainder * 1000000 / freq);
}

}  // namespace stfc
