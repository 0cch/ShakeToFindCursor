// Shake to Find Cursor - a Windows utility that makes the pointer easy to
// locate. Shake the mouse and the pointer grows (needs admin) and a
// click-through highlight is drawn around it (always, no privileges needed).
//
// Layout:
//   src/shake_detector.h  input-rate independent shake recognition (unit tested)
//   src/pointer_zoomer.*  SetSystemCursor based pointer enlargement
//   src/find_overlay.*    ripple / spotlight highlight on its own thread
//   src/auto_start.*      scheduled task when elevated, HKCU Run key otherwise
//   src/app_settings.*    registry backed settings plus the optional debug log
//
// One threading rule drives most of this file: the low-level mouse hook runs on
// this thread, and every microsecond spent inside it is latency for mouse input
// in *every other application on the desktop*. So the hook only feeds numbers
// to the detector. When a shake is recognised it posts a message, and the
// expensive part (rewriting the whole cursor scheme, painting the overlay)
// happens in the window procedure below.

#include "targetver.h"

#include <shellapi.h>

#include <cstring>

#include "app_settings.h"
#include "auto_start.h"
#include "find_overlay.h"
#include "pointer_zoomer.h"
#include "resource.h"
#include "shake_detector.h"
#include "win_util.h"

namespace stfc {
namespace {

enum class StartupMode { kUseSettings, kForceHook, kForcePolling };

constexpr UINT kMsgTray = WM_APP + 1;
constexpr UINT kMsgTrigger = WM_APP + 2;

constexpr UINT_PTR kAnimTimerId = 1;
constexpr UINT_PTR kTrayRetryTimerId = 2;
constexpr UINT kFastTickMs = 8;    // animation frames
constexpr UINT kHoldTickMs = 100;  // only waiting the hold out
constexpr UINT kPollTickMs = 8;    // polling mode also drives detection

constexpr UINT kTrayIconId = 1;
constexpr UINT kHotkeyId = 0x5CF1;
constexpr UINT kHotkeyModifiers = MOD_CONTROL | MOD_ALT;
constexpr UINT kHotkeyVirtualKey = 'F';

constexpr wchar_t kWindowClass[] = L"ShakeToFindCursor.Window";
constexpr wchar_t kWindowTitle[] = L"Shake to Find Cursor";
constexpr wchar_t kSingleInstanceMutex[] =
    L"Local\\ShakeToFindCursor.SingleInstance";

enum MenuCommand : WORD {
  kCmdFind = 1,
  kCmdPaused = 2,
  kCmdSensGentle = 10,
  kCmdSensStandard = 11,
  kCmdSensSharp = 12,
  kCmdScale2 = 20,
  kCmdScale3 = 21,
  kCmdScale4 = 22,
  kCmdScale5 = 23,
  kCmdHoldShort = 30,
  kCmdHoldNormal = 31,
  kCmdHoldLong = 32,
  kCmdOverlayNone = 40,
  kCmdOverlayRipple = 41,
  kCmdOverlaySpotlight = 42,
  kCmdEnlarge = 50,
  kCmdAutostart = 51,
  kCmdUseHook = 52,
  kCmdIgnoreFullscreen = 53,
  kCmdHotkey = 54,
  kCmdLogging = 55,
  kCmdElevate = 60,
  kCmdExit = 99,
};

int SensitivityForCommand(MenuCommand command) {
  switch (command) {
    case kCmdSensGentle:
      return 20;
    case kCmdSensSharp:
      return 85;
    default:
      return ShakeTuning::kDefaultSensitivity;
  }
}

int ScaleForCommand(MenuCommand command) {
  switch (command) {
    case kCmdScale2:
      return 200;
    case kCmdScale4:
      return 400;
    case kCmdScale5:
      return 500;
    default:
      return 300;
  }
}

int HoldForCommand(MenuCommand command) {
  switch (command) {
    case kCmdHoldShort:
      return 400;
    case kCmdHoldLong:
      return 1800;
    default:
      return 900;
  }
}

OverlayStyle OverlayForCommand(MenuCommand command) {
  switch (command) {
    case kCmdOverlayNone:
      return OverlayStyle::kNone;
    case kCmdOverlaySpotlight:
      return OverlayStyle::kSpotlight;
    default:
      return OverlayStyle::kRipple;
  }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param,
                            LPARAM l_param);
LRESULT CALLBACK MouseHookProc(int code, WPARAM w_param, LPARAM l_param);

// Held for the lifetime of the process: closing it would let a second copy in.
HANDLE g_singleton = nullptr;

// Explorer broadcasts this when (re)creating the taskbar toolbar; the id is
// assigned by the system, so it has to be registered rather than defined.
UINT TaskbarCreatedMessage() {
  static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
  return message;
}

// ---------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------
// All members are public on purpose: the window procedure and the mouse hook
// are plain callbacks in this file and both drive the object directly.
class App {
 public:
  static App& Get() {
    static App instance;
    return instance;
  }

  bool Initialize(StartupMode mode) {
    AppSettings& settings = AppSettings::Get();
    // A previous instance may have died while the pointers were enlarged.
    PointerZoomer::HealIfNeeded();
    detector_.SetTuning(settings.DetectorTuning());

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
      LogCritical("RegisterClassExW failed (%lu)", GetLastError());
      return false;
    }

    // A 0x0 unshown top-level window rather than a message-only one: the tray
    // menu needs a window it can hand activation semantics to, and only
    // top-level windows are told about logoff and shutdown (which is how the
    // pointer scheme gets restored before the session goes away).
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kWindowTitle,
                            WS_POPUP, -32000, -32000, 0, 0, nullptr, nullptr,
                            wc.hInstance, nullptr);
    if (!hwnd_) {
      LogCritical("CreateWindowExW failed (%lu)", GetLastError());
      return false;
    }

    // A command line switch only overrides this session; it must not overwrite
    // what the user chose in the tray menu.
    use_hook_ = mode == StartupMode::kUseSettings
                    ? settings.tracking == TrackingMode::kHook
                    : mode == StartupMode::kForceHook;

    const bool zoomer_ready = zoomer_.Initialize();
    if (!use_hook_ || !InstallHook()) use_hook_ = false;
    RescheduleTimer();

    if (settings.hotkey && !RegisterHotKey(hwnd_, kHotkeyId, kHotkeyModifiers,
                                           kHotkeyVirtualKey)) {
      Logf("hotkey Ctrl+Alt+F is taken by another app (%lu)", GetLastError());
    }

    if (!AddTrayIcon()) {
      // At logon the tray can still be starting up; retry until it appears.
      SetTimer(hwnd_, kTrayRetryTimerId, 2000, nullptr);
    } else {
      AnnounceOnce();
    }
    Logf("started: hook=%d zoomer=%d overlay=%d sensitivity=%d",
         use_hook_ ? 1 : 0, zoomer_ready ? 1 : 0,
         static_cast<int>(settings.overlay), settings.sensitivity);
    return true;
  }

  int Run() {
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
  }

  void Shutdown() {
    if (shut_down_) return;
    shut_down_ = true;
    UninstallHook();
    overlay_.Hide();
    zoomer_.RestoreNow();
    overlay_.Shutdown();
    RemoveTrayIcon();
    if (hwnd_) {
      const AppSettings& settings = AppSettings::Get();
      if (settings.hotkey) UnregisterHotKey(hwnd_, kHotkeyId);
      KillTimer(hwnd_, kAnimTimerId);
      KillTimer(hwnd_, kTrayRetryTimerId);
      DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
  }

  // --- message handling ---------------------------------------------------

  void OnTimer(UINT_PTR id) {
    if (id == kTrayRetryTimerId) {
      if (AddTrayIcon()) {
        KillTimer(hwnd_, kTrayRetryTimerId);
        AnnounceOnce();
      }
      return;
    }
    if (id != kAnimTimerId) return;

    const AppSettings& settings = AppSettings::Get();
    if (!use_hook_ && !paused_) {
      POINT point = {};
      if (GetCursorPos(&point)) FeedSample(point, settings);
    }
    if (zoomer_.Active()) {
      zoomer_.Tick(NowUs());
      RescheduleTimer();
    }
    // If a message could not be queued (a saturated queue) the trigger is
    // still honoured on the next tick.
    if (pending_trigger_) OnTrigger();
  }

  void OnTrayNotification(LPARAM l_param) {
    switch (LOWORD(l_param)) {
      case WM_RBUTTONUP:
      case WM_LBUTTONUP:
        ShowMenu();
        break;
      case WM_LBUTTONDBLCLK:
        TriggerNow();
        break;
      default:
        break;
    }
  }

  void OnSettingChange() {
    // The user picked a new pointer scheme, theme or DPI while we were running.
    zoomer_.RefreshScheme();
  }

  // Explorer owns the notification area, so restarting it takes the icon away.
  // Without this the settings menu becomes unreachable until the app restarts.
  void OnTaskbarCreated() {
    tray_icon_added_ = false;
    if (AddTrayIcon()) Logf("tray icon re-added after an explorer restart");
  }

  void FeedSample(POINT point, const AppSettings& settings) {
    if (paused_) return;
    if (!EffectsEnabled(settings)) return;
    PointerSample sample;
    sample.time_us = NowUs();
    sample.x = point.x;
    sample.y = point.y;
    if (!detector_.AddSample(sample)) return;
    QueueTrigger();
  }

  // The trigger is queued so the work happens on the message-loop side. If the
  // queue is saturated (it takes ~10k pending messages) the flag is still set,
  // so arm a tick to make sure it is picked up.
  void QueueTrigger() {
    pending_trigger_ = true;
    if (hwnd_ && !PostMessageW(hwnd_, kMsgTrigger, 0, 0)) {
      SetTimer(hwnd_, kAnimTimerId, kFastTickMs, nullptr);
      timer_active_ = true;
      timer_interval_ = kFastTickMs;
    }
  }

  // Nothing to detect if every effect is switched off, so do not sample at all.
  static bool EffectsEnabled(const AppSettings& settings) {
    return settings.overlay != OverlayStyle::kNone || settings.enlarge_pointer;
  }

  void OnTrigger() {
    pending_trigger_ = false;
    if (hwnd_) {
      // Any tick armed by QueueTrigger is not needed once we are here; the
      // real reschedule below sets the interval the animation actually wants.
      KillTimer(hwnd_, kAnimTimerId);
      timer_active_ = false;
    }
    if (paused_) return;
    AppSettings& settings = AppSettings::Get();
    if (settings.ignore_fullscreen && IsForegroundFullscreen()) {
      Logf("shake ignored: full screen window in front");
      return;
    }
    POINT point = {};
    if (!GetCursorPos(&point)) return;
    if (settings.enlarge_pointer && zoomer_.available()) zoomer_.Trigger();
    if (settings.overlay != OverlayStyle::kNone) {
      overlay_.Show(point, settings.overlay, settings.OverlayDurationMs());
    }
    const ShakeStats& stats = detector_.stats();
    Logf("shake %d/%d reversals, %lld px, %.0f px/s, axis %.0f deg",
         stats.good_reversals, stats.reversals,
         static_cast<long long>(stats.travel_px), stats.speed, stats.axis_deg);
    RescheduleTimer();
  }

  void TriggerNow() { QueueTrigger(); }

  void RescheduleTimer() {
    if (!hwnd_) return;
    const bool polling = !use_hook_;
    const bool fast = zoomer_.Active() && zoomer_.NeedsFastTick();
    const bool needs_ticks = polling || zoomer_.Active();
    if (!needs_ticks) {
      if (timer_active_) {
        KillTimer(hwnd_, kAnimTimerId);
        timer_active_ = false;
        timer_interval_ = 0;
      }
      return;
    }
    const UINT interval =
        polling ? kPollTickMs : (fast ? kFastTickMs : kHoldTickMs);
    if (timer_active_ && interval == timer_interval_) return;
    SetTimer(hwnd_, kAnimTimerId, interval, nullptr);
    timer_active_ = true;
    timer_interval_ = interval;
  }

  bool InstallHook() {
    mouse_hook_ = SetWindowsHookExW(WH_MOUSE_LL, &MouseHookProc,
                                   GetModuleHandleW(nullptr), 0);
    if (!mouse_hook_) {
      LogCritical("low-level mouse hook refused (%lu); using polling instead",
                  GetLastError());
      return false;
    }
    return true;
  }

  void UninstallHook() {
    if (!mouse_hook_) return;
    UnhookWindowsHookEx(mouse_hook_);
    mouse_hook_ = nullptr;
  }

  bool AddTrayIcon() {
    tray_ = {};
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = hwnd_;
    tray_.uID = kTrayIconId;
    tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray_.uCallbackMessage = kMsgTray;
    tray_.hIcon =
        LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!tray_.hIcon) return false;
    UpdateTip(tray_);
    if (!Shell_NotifyIconW(NIM_ADD, &tray_)) return false;
    tray_icon_added_ = true;
    return true;
  }

  void RemoveTrayIcon() {
    if (!tray_icon_added_) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    tray_icon_added_ = false;
  }

  void UpdateTip(NOTIFYICONDATAW& nid) {
    StringCchCopyW(nid.szTip, ARRAYSIZE(nid.szTip),
                   paused_ ? L"Shake to Find Cursor - paused"
                           : L"Shake to Find Cursor - shake to find it");
  }

  void AnnounceOnce() {
    AppSettings& settings = AppSettings::Get();
    if (settings.seen_intro) return;
    settings.seen_intro = true;
    settings.Save();
    if (zoomer_.available()) {
      ShowBalloon(L"Shake the mouse and the pointer grows. Right-click the tray "
                  L"icon for size, sensitivity and highlight options.",
                  L"Shake to Find Cursor");
    } else {
      ShowBalloon(
          L"The highlight is on. Enlarging the pointer itself needs "
          L"administrator rights: right-click the tray icon and choose "
          L"'Restart as administrator' to enable it.",
          L"Shake to Find Cursor");
    }
  }

  void ShowBalloon(const wchar_t* text, const wchar_t* title) {
    if (!tray_icon_added_) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    StringCchCopyW(nid.szInfo, ARRAYSIZE(nid.szInfo), text);
    StringCchCopyW(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
  }

  // --- tray menu ----------------------------------------------------------

  void BuildMenu(HMENU menu) {
    const AppSettings& settings = AppSettings::Get();
    AppendMenuW(menu, MF_STRING, kCmdFind, L"Find cursor now\t\tCtrl+Alt+F");
    AppendMenuW(menu, paused_ ? MF_CHECKED : MF_STRING, kCmdPaused, L"Paused");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU sub = CreatePopupMenu();
    if (sub) {
      const int level = settings.sensitivity;
      AppendMenuW(sub, level < 40 ? MF_CHECKED : MF_STRING, kCmdSensGentle,
                  L"Gentle - big, deliberate shakes");
      AppendMenuW(sub, (level >= 40 && level < 70) ? MF_CHECKED : MF_STRING,
                  kCmdSensStandard, L"Standard");
      AppendMenuW(sub, level >= 70 ? MF_CHECKED : MF_STRING, kCmdSensSharp,
                  L"Sharp - short flicks too");
      AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub),
                  L"Shake sensitivity");
    }

    sub = CreatePopupMenu();
    if (sub) {
      const int scale = settings.scale_percent;
      AppendMenuW(sub, scale <= 200 ? MF_CHECKED : MF_STRING, kCmdScale2,
                  L"2x");
      AppendMenuW(sub, (scale > 200 && scale <= 300) ? MF_CHECKED : MF_STRING,
                  kCmdScale3, L"3x");
      AppendMenuW(sub, (scale > 300 && scale <= 400) ? MF_CHECKED : MF_STRING,
                  kCmdScale4, L"4x");
      AppendMenuW(sub, scale > 400 ? MF_CHECKED : MF_STRING, kCmdScale5, L"5x");
      AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub),
                  L"Pointer size");
    }

    sub = CreatePopupMenu();
    if (sub) {
      const int hold = settings.hold_ms;
      AppendMenuW(sub, hold <= 600 ? MF_CHECKED : MF_STRING, kCmdHoldShort,
                  L"Briefly (0.4 s)");
      AppendMenuW(sub, (hold > 600 && hold <= 1200) ? MF_CHECKED : MF_STRING,
                  kCmdHoldNormal, L"A moment (0.9 s)");
      AppendMenuW(sub, hold > 1200 ? MF_CHECKED : MF_STRING, kCmdHoldLong,
                  L"A while (1.8 s)");
      AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub),
                  L"Keep it visible");
    }

    sub = CreatePopupMenu();
    if (sub) {
      const OverlayStyle overlay = settings.overlay;
      AppendMenuW(sub, overlay == OverlayStyle::kNone ? MF_CHECKED : MF_STRING,
                  kCmdOverlayNone, L"None (pointer size only)");
      AppendMenuW(sub, overlay == OverlayStyle::kRipple ? MF_CHECKED : MF_STRING,
                  kCmdOverlayRipple, L"Ripple rings");
      AppendMenuW(sub,
                  overlay == OverlayStyle::kSpotlight ? MF_CHECKED : MF_STRING,
                  kCmdOverlaySpotlight, L"Spotlight - dim the screen");
      AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), L"Highlight");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    UINT enlarge_flags =
        MF_STRING | (settings.enlarge_pointer ? MF_CHECKED : 0);
    if (!zoomer_.available()) enlarge_flags |= MF_GRAYED;
    AppendMenuW(menu, enlarge_flags, kCmdEnlarge,
                zoomer_.available()
                    ? L"Enlarge the pointer"
                    : L"Enlarge the pointer (needs administrator)");
    if (!zoomer_.available()) {
      AppendMenuW(menu, MF_STRING, kCmdElevate, L"Restart as administrator...");
    }
    AppendMenuW(menu, AutoStart::IsEnabled() ? MF_CHECKED : MF_STRING,
                kCmdAutostart, L"Start when I sign in");
    AppendMenuW(menu, use_hook_ ? MF_CHECKED : MF_STRING, kCmdUseHook,
                L"Track with a mouse hook (recommended)");
    AppendMenuW(menu, settings.ignore_fullscreen ? MF_CHECKED : MF_STRING,
                kCmdIgnoreFullscreen, L"Skip while a full screen app is in front");
    AppendMenuW(menu, settings.hotkey ? MF_CHECKED : MF_STRING, kCmdHotkey,
                L"Ctrl+Alt+F finds the pointer");
    AppendMenuW(menu, settings.logging ? MF_CHECKED : MF_STRING, kCmdLogging,
                L"Write a debug log (shake.log)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");
  }

  void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    BuildMenu(menu);

    POINT point = {};
    GetCursorPos(&point);
    // Without the foreground dance the menu keeps capture and only closes when
    // clicked, instead of dismissing when the user clicks elsewhere.
    SetForegroundWindow(hwnd_);
    const int command = TrackPopupMenuEx(
        menu, TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD, point.x, point.y,
        hwnd_, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    if (command != 0) HandleCommand(command);
  }

  void HandleCommand(int command_id) {
    AppSettings& settings = AppSettings::Get();
    const MenuCommand command = static_cast<MenuCommand>(command_id);
    bool save = true;

    switch (command) {
      case kCmdFind:
        TriggerNow();
        return;
      case kCmdPaused:
        paused_ = !paused_;
        if (tray_icon_added_) {
          NOTIFYICONDATAW nid = tray_;
          nid.uFlags = NIF_TIP;
          UpdateTip(nid);
          Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
        if (paused_) {
          overlay_.Hide();
        } else {
          detector_.Reset();
        }
        RescheduleTimer();
        return;
      case kCmdSensGentle:
      case kCmdSensStandard:
      case kCmdSensSharp:
        settings.sensitivity = SensitivityForCommand(command);
        detector_.SetTuning(settings.DetectorTuning());
        break;
      case kCmdScale2:
      case kCmdScale3:
      case kCmdScale4:
      case kCmdScale5:
        settings.scale_percent = ScaleForCommand(command);
        zoomer_.Configure();
        break;
      case kCmdHoldShort:
      case kCmdHoldNormal:
      case kCmdHoldLong:
        settings.hold_ms = HoldForCommand(command);
        break;
      case kCmdOverlayNone:
      case kCmdOverlayRipple:
      case kCmdOverlaySpotlight:
        settings.overlay = OverlayForCommand(command);
        if (settings.overlay == OverlayStyle::kNone) overlay_.Hide();
        break;
      case kCmdEnlarge:
        settings.enlarge_pointer = !settings.enlarge_pointer;
        break;
      case kCmdAutostart: {
        if (AutoStart::IsEnabled()) {
          AutoStart::Disable();
        } else if (!AutoStart::Enable()) {
          MessageBoxW(hwnd_,
                      L"Windows would not let this app be added to start-up.",
                      L"Shake to Find Cursor", MB_OK | MB_ICONWARNING);
        } else {
          ShowBalloon(zoomer_.available()
                          ? L"A scheduled task will start this app with "
                            L"administrator rights at sign-in."
                          : L"This app will start automatically at sign-in.",
                      L"Start-up");
        }
        save = false;
        break;
      }
      case kCmdUseHook:
        use_hook_ = !use_hook_;
        settings.tracking =
            use_hook_ ? TrackingMode::kHook : TrackingMode::kPolling;
        if (use_hook_) {
          if (!InstallHook()) {
            use_hook_ = false;
            settings.tracking = TrackingMode::kPolling;
          }
        } else {
          UninstallHook();
        }
        break;
      case kCmdIgnoreFullscreen:
        settings.ignore_fullscreen = !settings.ignore_fullscreen;
        break;
      case kCmdHotkey:
        settings.hotkey = !settings.hotkey;
        if (settings.hotkey) {
          RegisterHotKey(hwnd_, kHotkeyId, kHotkeyModifiers, kHotkeyVirtualKey);
        } else {
          UnregisterHotKey(hwnd_, kHotkeyId);
        }
        break;
      case kCmdLogging:
        settings.logging = !settings.logging;
        break;
      case kCmdElevate:
        // The elevated copy is started before this process exits, so it is
        // told to skip the single-instance check rather than finding our mutex
        // still held and giving up with an "already running" box.
        if (RelaunchElevated(L"--elevated")) {
          PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        } else {
          MessageBoxW(hwnd_,
                      L"The elevation request was declined, so only the "
                      L"highlight overlay is available.",
                      L"Shake to Find Cursor", MB_OK | MB_ICONINFORMATION);
        }
        save = false;
        break;
      case kCmdExit:
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
      default:
        save = false;
        break;
    }

    if (save) {
      settings.Save();
      Logf("settings: sensitivity=%d scale=%d%% hold=%dms overlay=%d "
           "enlarge=%d hook=%d",
           settings.sensitivity, settings.scale_percent, settings.hold_ms,
           static_cast<int>(settings.overlay),
           settings.enlarge_pointer ? 1 : 0, use_hook_ ? 1 : 0);
    }
    RescheduleTimer();
  }

  ~App() { Shutdown(); }

  HWND hwnd_ = nullptr;
  HHOOK mouse_hook_ = nullptr;
  NOTIFYICONDATAW tray_ = {};
  bool use_hook_ = true;
  bool timer_active_ = false;
  bool tray_icon_added_ = false;
  bool shut_down_ = false;
  bool paused_ = false;
  UINT timer_interval_ = 0;
  volatile bool pending_trigger_ = false;
  ShakeDetector detector_;
  PointerZoomer zoomer_;
  FindOverlay overlay_;
};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param,
                            LPARAM l_param) {
  App& app = App::Get();
  if (message == TaskbarCreatedMessage() && TaskbarCreatedMessage() != 0) {
    app.OnTaskbarCreated();
    return 0;
  }
  switch (message) {
    case WM_TIMER:
      app.OnTimer(static_cast<UINT_PTR>(w_param));
      return 0;
    case kMsgTrigger:
      app.OnTrigger();
      return 0;
    case kMsgTray:
      app.OnTrayNotification(l_param);
      return 0;
    case WM_HOTKEY:
      if (w_param == kHotkeyId) {
        app.paused_ = false;
        app.TriggerNow();
      }
      return 0;
    case WM_SETTINGCHANGE:
      app.OnSettingChange();
      return 0;
    case WM_DISPLAYCHANGE:
      // Monitors moved or changed mode: the overlay geometry is stale.
      app.overlay_.Hide();
      return 0;
    case WM_ENDSESSION:
      // Logoff or shutdown: never leave the session with an enlarged cursor.
      if (w_param != 0) app.Shutdown();
      return 0;
    case WM_CLOSE:
      // Shutdown() tears the hooks, the overlay and the cursor scheme down,
      // and destroys this window on the way out; the quit message is posted
      // here as well so the loop ends even when the window is already gone.
      app.Shutdown();
      PostQuitMessage(0);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, w_param, l_param);
}

LRESULT CALLBACK MouseHookProc(int code, WPARAM w_param, LPARAM l_param) {
  if (code == HC_ACTION && w_param == WM_MOUSEMOVE) {
    App& app = App::Get();
    if (!app.paused_) {
      const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(l_param);
      // Injected events come from remote tools and macros: they should not
      // trigger a highlight the user never asked for.
      if ((event->flags & LLMHF_INJECTED) == 0) {
        app.FeedSample(event->pt, AppSettings::Get());
      }
    }
  }
  // hHook may be NULL from Vista on: the system fills in the next one.
  return CallNextHookEx(nullptr, code, w_param, l_param);
}

}  // namespace
}  // namespace stfc

namespace {

bool CommandLineHas(const wchar_t* line, const wchar_t* flag) {
  return line && wcsstr(line, flag) != nullptr;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previous,
                      LPWSTR command_line,
                      int show) {
  UNREFERENCED_PARAMETER(instance);
  UNREFERENCED_PARAMETER(previous);
  UNREFERENCED_PARAMETER(show);

  stfc::AdoptDpiAwareness();

  stfc::StartupMode mode = stfc::StartupMode::kUseSettings;
  const wchar_t* line = command_line;
  const bool relaunch_handoff = CommandLineHas(line, L"--elevated");

  if (!relaunch_handoff) {
    // Two instances would fight over the cursor scheme: the second one would
    // snapshot the first one's enlarged cursor as the "original".
    HANDLE singleton =
        CreateMutexW(nullptr, TRUE, stfc::kSingleInstanceMutex);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      MessageBoxW(nullptr,
                  L"Shake to Find Cursor is already running - look for its icon "
                  L"in the notification area.",
                  L"Shake to Find Cursor", MB_OK | MB_ICONINFORMATION);
      if (singleton) CloseHandle(singleton);
      return 0;
    }
    // The handle is intentionally leaked for the lifetime of the process; it is
    // the mutex's reference.
    stfc::g_singleton = singleton;
  }

  if (CommandLineHas(line, L"--poll")) {
    mode = stfc::StartupMode::kForcePolling;
  } else if (CommandLineHas(line, L"--hook")) {
    mode = stfc::StartupMode::kForceHook;
  } else if (CommandLineHas(line, L"--help") ||
             CommandLineHas(line, L"/?")) {
    MessageBoxW(nullptr,
                L"Shake to Find Cursor\n\n"
                L"  --hook   track the pointer with a low-level mouse hook "
                L"(default, zero idle cost)\n"
                L"  --poll   track the pointer by sampling it on a timer "
                L"(fallback when hooks are blocked)\n\n"
                L"Shake the mouse to find the pointer. Right-click the tray "
                L"icon for settings; Ctrl+Alt+F finds the pointer on demand.",
                L"Shake to Find Cursor", MB_OK | MB_ICONINFORMATION);
    return 0;
  }

  stfc::Logf("=== start (elevated=%d, relaunch=%d) ===",
             stfc::IsElevated() ? 1 : 0, relaunch_handoff ? 1 : 0);

  if (!stfc::App::Get().Initialize(mode)) {
    MessageBoxW(nullptr,
                L"Shake to Find Cursor could not start: it could not create "
                L"its hidden window or tray icon.",
                L"Shake to Find Cursor", MB_OK | MB_ICONERROR);
    stfc::App::Get().Shutdown();
    return 1;
  }

  const int result = stfc::App::Get().Run();
  stfc::App::Get().Shutdown();
  stfc::Logf("=== exit ===");
  return result;
}
