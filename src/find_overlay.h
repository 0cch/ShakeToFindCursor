// Click-through highlight drawn around the pointer.
//
// Two effects share one window: "ripple" (expanding rings plus a marker dot)
// and "spotlight" (the monitor dims, a soft hole stays around the pointer).
// The window is layered, topmost and transparent to hit-testing, so it cannot
// steal focus or clicks.
//
// It lives on its own thread with its own message loop. That is the point: a
// full-screen UpdateLayeredWindow costs a few milliseconds, and doing that work
// on the thread that answers the low-level mouse hook would add visible input
// lag for every application on the desktop.
//
// Needs no elevation, which is why the effect survives even when pointer
// enlargement is unavailable.
#pragma once

#include <atomic>

#include "app_settings.h"  // OverlayStyle
#include "targetver.h"

namespace stfc {

class FindOverlay {
 public:
  FindOverlay() { InitializeCriticalSection(&lock_); }
  ~FindOverlay();
  FindOverlay(const FindOverlay&) = delete;
  FindOverlay& operator=(const FindOverlay&) = delete;

  struct Request {
    POINT center = {0, 0};
    OverlayStyle style = OverlayStyle::kNone;
    int duration_ms = 900;
    bool valid = false;
    bool hide = false;
  };

  // Draw the effect centred on `center` (physical screen pixels) for
  // `duration_ms`. Cheap to call from any thread: it queues a request and
  // posts one message.
  void Show(POINT center, OverlayStyle style, int duration_ms);

  // Stop the effect now (used on exit and when the session locks).
  void Hide();

  bool Active() const { return active_.load(); }

  // Stops the worker thread. Safe to call more than once.
  void Shutdown();

  // Internal plumbing, public only so the window procedure and the thread
  // entry point can reach them.
  Request TakePending();
  void MarkInactive() { active_ = false; }
  void ClearHwndLocked() { hwnd_ = nullptr; }
  CRITICAL_SECTION* lock() { return &lock_; }

 private:
  static DWORD WINAPI ThreadEntry(LPVOID param);
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM w, LPARAM l);

  void StartThreadIfNeeded();
  void SetPending(const Request& request);

  // Guards the hand-off queue between the caller thread and the render
  // thread; it exists for the whole lifetime of the object.
  CRITICAL_SECTION lock_;
  HWND hwnd_ = nullptr;  // guarded by lock_; null until the thread is ready
  Request pending_;
  HANDLE thread_ = nullptr;
  std::atomic<bool> active_{false};
};

}  // namespace stfc
