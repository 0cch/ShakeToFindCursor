#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define OEMRESOURCE

// clang-format off
#include <atlbase.h>
#include <shellapi.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <deque>
#include <vector>
#include <stdexcept>
#include "resource.h"
#include <taskschd.h>
#include <comdef.h>
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsupp.lib")



// Configuration class to manage all configurable parameters
class CursorConfig {
 public:
  static constexpr double kScaleFactor = 3.0;           // Cursor enlargement factor
  static constexpr size_t kHistorySize = 10;            // Keep last 10 movements
  static constexpr int kMinDirectionChanges = 5;        // Minimum direction changes required
  static constexpr double kMinMovementSpeed = 800.0;    // Minimum speed in pixels/second
  static constexpr int kMaxTimeWindow = 500;            // Time window in milliseconds
  static constexpr int kEnlargeDurationMs = 800;       // Cursor enlargement duration (milliseconds)
  static constexpr int kGrowAnimationMs = 150;         // Grow animation duration
  static constexpr int kShrinkAnimationMs = 200;       // Shrink animation duration
  static constexpr UINT_PTR kTimerId = 1;               // Timer ID
  static constexpr UINT kTimerInterval = 16;            // Timer interval for smooth animation (~60 FPS)
  static constexpr UINT kTrayIconId = 1;                // Tray icon ID
  static constexpr UINT kTrayIconMessage = WM_APP + 1;  // Tray message ID
  static constexpr UINT kMenuExitId = 2000;             // Exit menu item ID
  static constexpr UINT kMenuAutoStartId = 2001;        // Enable auto-start menu item ID
  static constexpr UINT kMenuDisableAutoStartId = 2002; // Disable auto-start menu item ID

  enum class MouseTrackingMode {
    kHook,    // Use SetWindowsHookEx
    kPolling  // Use GetCursorPos in WM_TIMER
  };
};

// clang-format on

class Logger {
 public:
  static Logger& GetInstance() {
    static Logger instance;
    return instance;
  }

  void Log(const std::string& message) {
    std::ofstream log_file("ShakeToFindCursor.log", std::ios_base::app);
    if (log_file.is_open()) {
      log_file << GetTimestamp() << " - " << message << std::endl;
    }
  }

 private:
  Logger() = default;
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  std::string GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm;
    localtime_s(&now_tm, &now_time_t);
    std::stringstream ss;
    ss << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
  }
};

#ifdef _DEBUG
#define DEBUG_LOG(msg) Logger::GetInstance().Log(msg)
#else
#define DEBUG_LOG(msg)
#endif

// COM initialization class
class ComInitializer {
 public:
  ComInitializer() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
      throw std::runtime_error("Failed to initialize COM");
    }
  }

  ~ComInitializer() { CoUninitialize(); }
};

// Auto-start manager class to enable/disable auto-start
class AutoStartManager {
 public:
  static bool IsAutoStartEnabled() {
    CComPtr<ITaskService> task_service;
    HRESULT hr = task_service.CoCreateInstance(CLSID_TaskScheduler, nullptr,
                                               CLSCTX_INPROC_SERVER);
    if (FAILED(hr)) return false;

    hr = task_service->Connect(_variant_t(), _variant_t(), _variant_t(),
                               _variant_t());
    if (FAILED(hr)) return false;

    CComPtr<ITaskFolder> root_folder;
    hr = task_service->GetFolder(_bstr_t(L"\\"), &root_folder);
    if (FAILED(hr)) return false;

    CComPtr<IRegisteredTask> task;
    hr = root_folder->GetTask(_bstr_t(L"ShakeToFindCursor"), &task);

    return SUCCEEDED(hr) && task != nullptr;
  }

  static bool EnableAutoStart() {
    WCHAR exe_path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe_path, MAX_PATH)) return false;

    CComPtr<ITaskService> task_service;
    HRESULT hr = task_service.CoCreateInstance(CLSID_TaskScheduler, nullptr,
                                               CLSCTX_INPROC_SERVER);
    if (FAILED(hr)) return false;

    hr = task_service->Connect(_variant_t(), _variant_t(), _variant_t(),
                               _variant_t());
    if (FAILED(hr)) return false;

    CComPtr<ITaskFolder> root_folder;
    hr = task_service->GetFolder(_bstr_t(L"\\"), &root_folder);
    if (FAILED(hr)) return false;

    // Delete existing task if present
    root_folder->DeleteTask(_bstr_t(L"ShakeToFindCursor"), 0);

    CComPtr<ITaskDefinition> task;
    hr = task_service->NewTask(0, &task);
    if (FAILED(hr)) return false;

    // Set general info
    CComPtr<IRegistrationInfo> reg_info;
    hr = task->get_RegistrationInfo(&reg_info);
    if (SUCCEEDED(hr)) {
      reg_info->put_Author(_bstr_t(L"ShakeToFindCursor"));
      reg_info->put_Description(
          _bstr_t(L"Auto-start ShakeToFindCursor with elevated privileges"));
    }

    // Set principal (run with highest privileges)
    CComPtr<IPrincipal> principal;
    hr = task->get_Principal(&principal);
    if (SUCCEEDED(hr)) {
      principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
      principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    }

    // Configure settings
    CComPtr<ITaskSettings> settings;
    hr = task->get_Settings(&settings);
    if (SUCCEEDED(hr)) {
      settings->put_StartWhenAvailable(VARIANT_TRUE);
      settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
      settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
      settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));  // No time limit
      settings->put_Hidden(VARIANT_FALSE);
      settings->put_Priority(5);  // Above normal priority
      settings->put_RunOnlyIfNetworkAvailable(VARIANT_FALSE);
      settings->put_WakeToRun(VARIANT_TRUE);
      settings->put_AllowHardTerminate(VARIANT_TRUE);
      settings->put_Enabled(VARIANT_TRUE);
    }

    // Create triggers
    CComPtr<ITriggerCollection> trigger_collection;
    hr = task->get_Triggers(&trigger_collection);
    if (SUCCEEDED(hr)) {
      // Add logon trigger
      CComPtr<ITrigger> logon_trigger;
      if (SUCCEEDED(
              trigger_collection->Create(TASK_TRIGGER_LOGON, &logon_trigger))) {
        CComQIPtr<ILogonTrigger> logon(logon_trigger);
        if (logon) {
          logon->put_Id(_bstr_t(L"LogonTriggerId"));
          logon->put_Enabled(VARIANT_TRUE);
          // Add a small delay to ensure shell is ready
          logon->put_Delay(_bstr_t(L"PT10S"));
        }
      }

      // Add boot trigger
      CComPtr<ITrigger> boot_trigger;
      if (SUCCEEDED(
              trigger_collection->Create(TASK_TRIGGER_BOOT, &boot_trigger))) {
        CComQIPtr<IBootTrigger> boot(boot_trigger);
        if (boot) {
          boot->put_Id(_bstr_t(L"BootTriggerId"));
          boot->put_Enabled(VARIANT_TRUE);
          // Add a delay after boot
          boot->put_Delay(_bstr_t(L"PT60S"));
        }
      }
    }

    // Create action
    CComPtr<IActionCollection> action_collection;
    hr = task->get_Actions(&action_collection);
    if (SUCCEEDED(hr)) {
      CComPtr<IAction> action;
      hr = action_collection->Create(TASK_ACTION_EXEC, &action);
      if (SUCCEEDED(hr)) {
        CComQIPtr<IExecAction> exec_action(action);
        if (exec_action) {
          exec_action->put_Path(_bstr_t(exe_path));
          // Set working directory
          WCHAR work_dir[MAX_PATH];
          wcscpy_s(work_dir, exe_path);
          PathRemoveFileSpecW(work_dir);
          exec_action->put_WorkingDirectory(_bstr_t(work_dir));
        }
      }
    }

    // Register the task - use current user's credentials
    CComPtr<IRegisteredTask> registered_task;
    hr = root_folder->RegisterTaskDefinition(
        _bstr_t(L"ShakeToFindCursor"), task, TASK_CREATE_OR_UPDATE,
        _variant_t(),                  // Default credentials (current user)
        _variant_t(),                  // Default password
        TASK_LOGON_INTERACTIVE_TOKEN,  // Run only when user is logged on
        _variant_t(L""),               // No sddl
        &registered_task);

    return SUCCEEDED(hr);
  }

  static bool DisableAutoStart() {
    CComPtr<ITaskService> task_service;
    HRESULT hr = task_service.CoCreateInstance(CLSID_TaskScheduler, nullptr,
                                               CLSCTX_INPROC_SERVER);
    if (FAILED(hr)) return false;

    hr = task_service->Connect(_variant_t(), _variant_t(), _variant_t(),
                               _variant_t());
    if (FAILED(hr)) return false;

    CComPtr<ITaskFolder> root_folder;
    hr = task_service->GetFolder(_bstr_t(L"\\"), &root_folder);
    if (FAILED(hr)) return false;

    hr = root_folder->DeleteTask(_bstr_t(L"ShakeToFindCursor"), 0);
    return SUCCEEDED(hr);
  }
};

// Cursor utilities class
class CursorUtils {
 public:
  static std::wstring GetRegistryCursorPath(const wchar_t* value_name) {
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
      return L"";
    }

    wchar_t buffer[MAX_PATH] = {0};
    DWORD buffer_size = sizeof(buffer);
    DWORD type = 0;
    LONG res = RegQueryValueExW(hkey, value_name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &buffer_size);
    RegCloseKey(hkey);

    if (res == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
      wchar_t expanded[MAX_PATH] = {0};
      ExpandEnvironmentStringsW(buffer, expanded, MAX_PATH);
      return std::wstring(expanded);
    }
    return L"";
  }

  static HCURSOR LoadCursorFromFileOrSystem(const std::wstring& file_path, LPCWSTR fallback_id, int target_w = 0, int target_h = 0) {
    if (!file_path.empty()) {
      UINT flags = LR_LOADFROMFILE;
      if (target_w == 0 && target_h == 0) {
        flags |= LR_DEFAULTSIZE;
      }
      HCURSOR h = (HCURSOR)LoadImageW(nullptr, file_path.c_str(), IMAGE_CURSOR, target_w, target_h, flags);
      if (h) return h;
    }
    return (HCURSOR)LoadImageW(nullptr, fallback_id, IMAGE_CURSOR, target_w, target_h, (target_w == 0 && target_h == 0) ? (LR_DEFAULTSIZE | LR_SHARED) : LR_SHARED);
  }

  static HCURSOR ScaleCursor(HCURSOR src_cursor, const std::wstring& file_path, double scale_factor, int base_w, int base_h) {
    if (scale_factor <= 1.0001) {
      return src_cursor;
    }

    int target_w = static_cast<int>(std::round(base_w * scale_factor));
    int target_h = static_cast<int>(std::round(base_h * scale_factor));
    if (target_w <= 0 || target_h <= 0) return nullptr;

    // Automatically detect and load closest native high-resolution frame from the custom cursor file
    if (!file_path.empty()) {
      HCURSOR direct_load = (HCURSOR)LoadImageW(nullptr, file_path.c_str(), IMAGE_CURSOR, target_w, target_h, LR_LOADFROMFILE);
      if (direct_load) {
        return direct_load;
      }
    }

    // Fallback to CopyImage
    if (src_cursor) {
      return (HCURSOR)CopyImage(src_cursor, IMAGE_CURSOR, target_w, target_h, 0);
    }

    return nullptr;
  }
};

HCURSOR GetSystemArrowCursor() {
  CURSORINFO ci = {sizeof(CURSORINFO)};
  if (GetCursorInfo(&ci)) {
    return CopyCursor(ci.hCursor);
  }
  return nullptr;
}

// Large cursor class
class LargeCursor {
 public:
  LargeCursor(const wchar_t* reg_value_name, LPCWSTR fallback_cursor_id, DWORD system_cursor_id)
      : reg_value_name_(reg_value_name), fallback_cursor_id_(fallback_cursor_id), system_cursor_id_(system_cursor_id) {
    ReloadOriginalCursor();
  }

  void ReloadOriginalCursor() {
    if (original_cursor_) {
      DestroyCursor(original_cursor_);
      original_cursor_ = nullptr;
    }

    cursor_path_ = CursorUtils::GetRegistryCursorPath(reg_value_name_.c_str());
    original_cursor_ = CursorUtils::LoadCursorFromFileOrSystem(cursor_path_, fallback_cursor_id_);

    base_width_ = GetSystemMetrics(SM_CXCURSOR);
    base_height_ = GetSystemMetrics(SM_CYCURSOR);
    if (original_cursor_) {
      ICONINFO info = {0};
      if (GetIconInfo(original_cursor_, &info)) {
        BITMAP bm = {0};
        HBITMAP bmp = info.hbmColor ? info.hbmColor : info.hbmMask;
        if (bmp && GetObject(bmp, sizeof(BITMAP), &bm)) {
          base_width_ = bm.bmWidth;
          base_height_ = info.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
        }
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
      }
    }
  }

  void ApplyScale(double scale_factor) {
    if (scale_factor <= 1.0001) {
      Restore();
      return;
    }

    HCURSOR scaled = CursorUtils::ScaleCursor(original_cursor_, cursor_path_, scale_factor, base_width_, base_height_);
    if (scaled) {
      HCURSOR cursor_copy = CopyCursor(scaled);
      DestroyCursor(scaled);
      if (cursor_copy) {
        SetSystemCursor(cursor_copy, system_cursor_id_);
        current_scale_ = scale_factor;
      }
    }
  }

  void Restore() {
    if (original_cursor_) {
      HCURSOR cursor_copy = CopyCursor(original_cursor_);
      if (cursor_copy) {
        SetSystemCursor(cursor_copy, system_cursor_id_);
        current_scale_ = 1.0;
      }
    }
  }

  ~LargeCursor() {
    if (original_cursor_) {
      DestroyCursor(original_cursor_);
    }
  }

 private:
  std::wstring reg_value_name_;
  LPCWSTR fallback_cursor_id_;
  DWORD system_cursor_id_;
  std::wstring cursor_path_;
  HCURSOR original_cursor_ = nullptr;
  int base_width_ = 32;
  int base_height_ = 32;
  double current_scale_ = 1.0;
};

// Large cursor manager class
class LargeCursorManager {
 public:
  LargeCursorManager() {
    large_cursors_.push_back(
        std::make_unique<LargeCursor>(L"Arrow", IDC_ARROW, OCR_NORMAL));
    large_cursors_.push_back(
        std::make_unique<LargeCursor>(L"IBeam", IDC_IBEAM, OCR_IBEAM));
    large_cursors_.push_back(
        std::make_unique<LargeCursor>(L"Wait", IDC_WAIT, OCR_WAIT));
    large_cursors_.push_back(
        std::make_unique<LargeCursor>(L"Crosshair", IDC_CROSS, OCR_CROSS));
    large_cursors_.push_back(
        std::make_unique<LargeCursor>(L"UpArrow", IDC_UPARROW, OCR_UP));
    large_cursors_.push_back(
        std::make_unique<LargeCursor>(L"SizeNWSE", IDC_SIZENWSE, OCR_SIZENWSE));
    large_cursors_.push_back(
        std::make_unique<LargeCursor>(L"SizeNESW", IDC_SIZENESW, OCR_SIZENESW));
    large_cursors_.push_back(
        std::make_unique<LargeCursor>(L"SizeWE", IDC_SIZEWE, OCR_SIZEWE));
    large_cursors_.push_back(
        std::make_unique<LargeCursor>(L"SizeNS", IDC_SIZENS, OCR_SIZENS));
    large_cursors_.push_back(
        std::make_unique<LargeCursor>(L"SizeAll", IDC_SIZEALL, OCR_SIZEALL));
    large_cursors_.push_back(
        std::make_unique<LargeCursor>(L"No", IDC_NO, OCR_NO));
    large_cursors_.push_back(
        std::make_unique<LargeCursor>(L"Hand", IDC_HAND, OCR_HAND));
    large_cursors_.push_back(
        std::make_unique<LargeCursor>(L"AppStarting", IDC_APPSTARTING, OCR_APPSTARTING));
  }

  void ApplyScale(double scale_factor) {
    for (const auto& cursor : large_cursors_) {
      cursor->ApplyScale(scale_factor);
    }
  }

  void RestoreAll() {
    for (const auto& cursor : large_cursors_) {
      cursor->Restore();
    }
  }

  void ReloadAll() {
    for (const auto& cursor : large_cursors_) {
      cursor->ReloadOriginalCursor();
    }
  }

 private:
  std::vector<std::unique_ptr<LargeCursor>> large_cursors_;
};

class CursorState {
 public:
  CursorState() {}

  ~CursorState() {
    DEBUG_LOG("CursorState destroyed");
    if (SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE)) {
      state_ = kIdle;
    }
  }

  void Enlarge() {
    if (state_ == kIdle) {
      large_cursor_manager_.ReloadAll();
      state_ = kGrowing;
      enlarge_start_time_ = std::chrono::high_resolution_clock::now();
    } else if (state_ == kShrinking) {
      // Re-trigger from shrink: jump back to full size and hold
      ApplyScale(CursorConfig::kScaleFactor);
      state_ = kHolding;
      enlarge_start_time_ = std::chrono::high_resolution_clock::now();
      last_apply_time_ = std::chrono::high_resolution_clock::now();
    } else if (state_ == kHolding) {
      // Re-trigger during hold: extend the hold duration
      enlarge_start_time_ = std::chrono::high_resolution_clock::now();
    }
  }

  void ReloadCursors() {
    large_cursor_manager_.ReloadAll();
  }

  void RestoreIfNeeded() {
    if (state_ == kIdle) return;

    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - enlarge_start_time_)
                       .count();

    // Frame throttle: limit cursor updates to ~30 FPS to reduce flicker
    auto since_last_apply =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_apply_time_)
            .count();
    bool should_apply = (since_last_apply >= 33);

    double current_scale = 1.0;

    switch (state_) {
      case kGrowing: {
        if (elapsed >= CursorConfig::kGrowAnimationMs) {
          current_scale = CursorConfig::kScaleFactor;
          state_ = kHolding;
          hold_start_time_ = now;
          ApplyScale(current_scale);
          last_apply_time_ = now;
        } else if (should_apply) {
          double t = static_cast<double>(elapsed) /
                     CursorConfig::kGrowAnimationMs;
          // Ease-out cubic for smooth deceleration
          double eased = 1.0 - std::pow(1.0 - t, 3.0);
          current_scale = 1.0 + (CursorConfig::kScaleFactor - 1.0) * eased;
          ApplyScale(current_scale);
          last_apply_time_ = now;
        }
        break;
      }
      case kHolding: {
        if (elapsed >= CursorConfig::kEnlargeDurationMs) {
          state_ = kShrinking;
          shrink_start_time_ = now;
        }
        break;
      }
      case kShrinking: {
        auto shrink_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - shrink_start_time_)
                .count();
        if (shrink_elapsed >= CursorConfig::kShrinkAnimationMs) {
          RestoreOriginalCursor();
          state_ = kIdle;
        } else if (should_apply) {
          double t = static_cast<double>(shrink_elapsed) /
                     CursorConfig::kShrinkAnimationMs;
          // Ease-in cubic for smooth acceleration
          double eased = t * t * t;
          current_scale =
              CursorConfig::kScaleFactor -
              (CursorConfig::kScaleFactor - 1.0) * eased;
          ApplyScale(current_scale);
          last_apply_time_ = now;
        }
        break;
      }
      default:
        break;
    }
  }
 private:
  void ApplyScale(double scale) {
    large_cursor_manager_.ApplyScale(scale);
  }

  void RestoreOriginalCursor() {
    large_cursor_manager_.RestoreAll();
  }

  enum AnimationState {
    kIdle,
    kGrowing,
    kHolding,
    kShrinking
  };

  AnimationState state_ = kIdle;
  LargeCursorManager large_cursor_manager_;
  std::chrono::high_resolution_clock::time_point enlarge_start_time_;
  std::chrono::high_resolution_clock::time_point hold_start_time_;
  std::chrono::high_resolution_clock::time_point shrink_start_time_;
  std::chrono::high_resolution_clock::time_point last_apply_time_;
};

// Mouse movement detector class with shake pattern recognition
class MouseMoveDetector {
 public:
  MouseMoveDetector() {
    GetCursorPos(&last_pos_);
    last_time_ = std::chrono::high_resolution_clock::now();
  }

  bool ShouldEnlargeCursor(const POINT& current_pos) {
    auto now = std::chrono::high_resolution_clock::now();
    auto delta_time =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time_)
            .count();

    if (delta_time <= 0) return false;

    // Calculate movement vector
    int dx = current_pos.x - last_pos_.x;
    int dy = current_pos.y - last_pos_.y;

    // Update position history
    movement_history_.push_back({dx, dy, delta_time});
    if (movement_history_.size() > CursorConfig::kHistorySize) {
      movement_history_.pop_front();
    }

    last_pos_ = current_pos;
    last_time_ = now;

    return DetectShakePattern();
  }

 private:
  struct Movement {
    int dx;
    int dy;
    long long dt;
  };

  bool DetectShakePattern() {
    if (movement_history_.size() < CursorConfig::kHistorySize) return false;

    int direction_changes = 0;
    double total_speed = 0.0;
    long long total_time = 0;

    // Previous movement direction (-1: negative, 1: positive, 0: neutral)
    int last_x_dir = 0;
    int last_y_dir = 0;

    for (const auto& mov : movement_history_) {
      // Calculate current direction
      int curr_x_dir = (mov.dx > 0) ? 1 : (mov.dx < 0) ? -1 : 0;
      int curr_y_dir = (mov.dy > 0) ? 1 : (mov.dy < 0) ? -1 : 0;

      // Count direction changes
      if (last_x_dir != 0 && curr_x_dir != 0 && last_x_dir != curr_x_dir) {
        direction_changes++;
      }
      if (last_y_dir != 0 && curr_y_dir != 0 && last_y_dir != curr_y_dir) {
        direction_changes++;
      }

      // Update last direction
      last_x_dir = curr_x_dir;
      last_y_dir = curr_y_dir;

      // Calculate speed
      double distance = std::sqrt(mov.dx * mov.dx + mov.dy * mov.dy);
      double speed = (mov.dt > 0) ? (distance / mov.dt) * 1000.0 : 0;
      total_speed += speed;
      total_time += mov.dt;
    }

    // Check if we're within the time window
    if (total_time > CursorConfig::kMaxTimeWindow) return false;

    // Calculate average speed
    double avg_speed = total_speed / movement_history_.size();

    // Return true if we have enough direction changes and sufficient speed
    return direction_changes >= CursorConfig::kMinDirectionChanges &&
           avg_speed >= CursorConfig::kMinMovementSpeed;
  }

  POINT last_pos_;
  std::chrono::high_resolution_clock::time_point last_time_;
  std::deque<Movement> movement_history_;
};

class ShakeToFindCursor {
 public:
  static ShakeToFindCursor& GetInstance() {
    static ShakeToFindCursor instance;
    return instance;
  }

  bool Initialize(CursorConfig::MouseTrackingMode mode) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
      throw std::runtime_error("Failed to initialize COM");
    }
    tracking_mode_ = mode;

    // Register window class
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hIcon = LoadIcon(wc.hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    wc.lpszClassName = L"ShakeToFindCursorClass";

    if (!RegisterClassExW(&wc)) {
      throw std::runtime_error("Failed to register window class");
    }

    // Create hidden window
    hwnd_ = CreateWindowW(L"ShakeToFindCursorClass", L"ShakeToFindCursor",
                          WS_OVERLAPPED, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0,
                          nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!hwnd_) {
      throw std::runtime_error("Failed to create window");
    }

    // Set window instance pointer
    SetWindowLongPtr(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // Create timer with different interval based on mode
    UINT timer_interval =
        (tracking_mode_ == CursorConfig::MouseTrackingMode::kPolling)
            ? 10  // Poll more frequently when using timer
            : CursorConfig::kTimerInterval;

    if (!SetTimer(hwnd_, CursorConfig::kTimerId, timer_interval, nullptr)) {
      DestroyWindow(hwnd_);
      throw std::runtime_error("Failed to create timer");
    }

    // Only install hook if using hook mode
    if (tracking_mode_ == CursorConfig::MouseTrackingMode::kHook) {
      mouse_hook_ =
          SetWindowsHookEx(WH_MOUSE_LL, MouseProc, GetModuleHandle(nullptr), 0);

      if (!mouse_hook_) {
        KillTimer(hwnd_, CursorConfig::kTimerId);
        DestroyWindow(hwnd_);
        throw std::runtime_error("Failed to install mouse hook");
      }
    }

    // Set Ctrl+C handler
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // Create tray icon
    NOTIFYICONDATAW nid = {sizeof(NOTIFYICONDATAW)};
    nid.hWnd = hwnd_;
    nid.uID = CursorConfig::kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = CursorConfig::kTrayIconMessage;
    nid.hIcon =
        LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_APP_ICON));
    wcscpy_s(nid.szTip, L"Shake to Find Cursor");

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
      KillTimer(hwnd_, CursorConfig::kTimerId);
      DestroyWindow(hwnd_);
      throw std::runtime_error("Failed to create tray icon");
    }

    tray_icon_added_ = true;

    return true;
  }

  void Run() {
    MSG msg;
    running_ = true;

    while (running_) {
      // Use PeekMessage instead of GetMessage to handle timers even when there
      // are no messages
      while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
          running_ = false;
          break;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }

      // Yield CPU time slice
      Sleep(1);
    }
  }

  void Stop() {
    running_ = false;
    if (hwnd_) {
      PostMessage(hwnd_, WM_QUIT, 0, 0);
    }
  }

  ~ShakeToFindCursor() {
    RemoveTrayIcon();
    if (mouse_hook_) {
      UnhookWindowsHookEx(mouse_hook_);
    }
    if (hwnd_) {
      KillTimer(hwnd_, CursorConfig::kTimerId);
      DestroyWindow(hwnd_);
    }
    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    CoUninitialize();
  }

  void ProcessMouseMove(const MSLLHOOKSTRUCT* mouse_info) {
    if (move_detector_.ShouldEnlargeCursor(mouse_info->pt)) {
      cursor_state_.Enlarge();
    }
  }

 private:
  ShakeToFindCursor() = default;
  ShakeToFindCursor(const ShakeToFindCursor&) = delete;
  ShakeToFindCursor& operator=(const ShakeToFindCursor&) = delete;

  static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_MOUSEMOVE) {
      auto& instance = GetInstance();
      instance.ProcessMouseMove(reinterpret_cast<MSLLHOOKSTRUCT*>(lParam));
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
  }

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam) {
    auto* instance = reinterpret_cast<ShakeToFindCursor*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
      case WM_SETTINGCHANGE:
        if (lParam && (wcscmp(reinterpret_cast<LPCWSTR>(lParam), L"Cursors") == 0 || msg == WM_SETTINGCHANGE)) {
          if (instance) {
            instance->cursor_state_.ReloadCursors();
          }
        }
        return 0;

      case WM_TIMER:
        if (wParam == CursorConfig::kTimerId && instance) {
          if (instance->tracking_mode_ ==
              CursorConfig::MouseTrackingMode::kPolling) {
            POINT pt;
            GetCursorPos(&pt);
            instance->ProcessMouseMove(reinterpret_cast<MSLLHOOKSTRUCT*>(&pt));
          }
          instance->cursor_state_.RestoreIfNeeded();
        }
        return 0;

      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

      case CursorConfig::kTrayIconMessage:
        if (LOWORD(lParam) == WM_RBUTTONUP) {
          instance->ShowContextMenu(hwnd);
        }
        return 0;

      case WM_COMMAND:
        if (LOWORD(wParam) == CursorConfig::kMenuExitId) {
          instance->Stop();
        } else if (LOWORD(wParam) == CursorConfig::kMenuAutoStartId) {
          if (AutoStartManager::EnableAutoStart()) {
            MessageBoxW(hwnd, L"Auto-start enabled successfully.", L"Success",
                        MB_OK | MB_ICONINFORMATION);
          } else {
            MessageBoxW(hwnd, L"Failed to enable auto-start.", L"Error",
                        MB_OK | MB_ICONERROR);
          }
        } else if (LOWORD(wParam) == CursorConfig::kMenuDisableAutoStartId) {
          if (AutoStartManager::DisableAutoStart()) {
            MessageBoxW(hwnd, L"Auto-start disabled successfully.", L"Success",
                        MB_OK | MB_ICONINFORMATION);
          } else {
            MessageBoxW(hwnd, L"Failed to disable auto-start.", L"Error",
                        MB_OK | MB_ICONERROR);
          }
        }
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT) {
      GetInstance().Stop();
      return TRUE;
    }
    return FALSE;
  }

  void RemoveTrayIcon() {
    if (tray_icon_added_ && hwnd_) {
      NOTIFYICONDATA nid = {sizeof(NOTIFYICONDATA)};
      nid.hWnd = hwnd_;
      nid.uID = CursorConfig::kTrayIconId;
      Shell_NotifyIcon(NIM_DELETE, &nid);
      tray_icon_added_ = false;
    }
  }

  void ShowContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    bool is_auto_start = AutoStartManager::IsAutoStartEnabled();
    if (is_auto_start) {
      AppendMenuW(menu, MF_STRING, CursorConfig::kMenuDisableAutoStartId,
                  L"Disable Auto-start");
    } else {
      AppendMenuW(menu, MF_STRING, CursorConfig::kMenuAutoStartId,
                  L"Enable Auto-start");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CursorConfig::kMenuExitId, L"Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
  }

  HHOOK mouse_hook_ = nullptr;
  HWND hwnd_ = nullptr;
  CursorState cursor_state_;
  MouseMoveDetector move_detector_;
  std::atomic<bool> running_{false};
  bool tray_icon_added_ = false;
  CursorConfig::MouseTrackingMode tracking_mode_;
};

bool IsRunAsAdmin() {
  BOOL is_admin = FALSE;
  PSID admin_group = nullptr;
  SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;

  if (AllocateAndInitializeSid(&nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                               DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                               &admin_group)) {
    if (!CheckTokenMembership(nullptr, admin_group, &is_admin)) {
      is_admin = FALSE;
    }
    FreeSid(admin_group);
  }
  return is_admin != FALSE;
}

#ifdef CONSOLE_MODE
int main(int argc, char* argv[]) {
  if (!IsRunAsAdmin()) {
    std::cerr << "This program requires administrator privileges to run."
              << std::endl;
    return 1;
  }

  ComInitializer com_initializer;

  SetProcessDPIAware();

  CursorConfig::MouseTrackingMode mode =
      CursorConfig::MouseTrackingMode::kPolling;
  if (argc > 1 && std::string(argv[1]) == "--hook") {
    mode = CursorConfig::MouseTrackingMode::kHook;
  }

  try {
    auto& cursor_finder = ShakeToFindCursor::GetInstance();
    if (!cursor_finder.Initialize(mode)) {
      return 1;
    }

    std::cout << "Shake to Find Cursor demo started. Move the mouse quickly to "
                 "trigger zoom."
              << std::endl;
    std::cout << "Press Ctrl + C to exit." << std::endl;

    cursor_finder.Run();
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
    return 1;
  }
  return 0;
}
#else
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                      LPWSTR lpCmdLine, int nCmdShow) {
  UNREFERENCED_PARAMETER(hInstance);
  UNREFERENCED_PARAMETER(hPrevInstance);
  UNREFERENCED_PARAMETER(lpCmdLine);
  UNREFERENCED_PARAMETER(nCmdShow);

  if (!IsRunAsAdmin()) {
    MessageBoxW(nullptr,
                L"This program requires administrator privileges to run.",
                L"Error", MB_OK | MB_ICONERROR);
    return 1;
  }

  ComInitializer com_initializer;

  SetProcessDPIAware();

  CursorConfig::MouseTrackingMode mode =
      CursorConfig::MouseTrackingMode::kPolling;
  if (wcsstr(lpCmdLine, L"--hook")) {
    mode = CursorConfig::MouseTrackingMode::kHook;
  }

  try {
    auto& cursor_finder = ShakeToFindCursor::GetInstance();
    if (!cursor_finder.Initialize(mode)) {
      return 1;
    }

    DEBUG_LOG(
        "Shake to Find Cursor started. Move the mouse quickly to trigger "
        "zoom.");

    cursor_finder.Run();
  } catch (const std::exception& e) {
    std::wstringstream ws;
    ws << L"Error: " << e.what();
    MessageBoxW(nullptr, ws.str().c_str(), L"Error", MB_OK | MB_ICONERROR);
    DEBUG_LOG("Error: " + std::string(e.what()));
    SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
    return 1;
  }
  return 0;
}
#endif
