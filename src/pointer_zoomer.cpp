#include "targetver.h"

#include "pointer_zoomer.h"

#include <cmath>

#include "app_settings.h"
#include "win_util.h"

// Some SDK versions omit the help/size cursors.
#ifndef OCR_HELP
#define OCR_HELP 32651
#endif
#ifndef OCR_SIZE
#define OCR_SIZE 32640
#endif

namespace stfc {
namespace {

// The system cursor ids an application can ask for. The stock names
// (IDC_ARROW & friends) are exactly MAKEINTRESOURCE of these ids, so listing
// the ids is enough. Everything an ordinary desktop shows is covered here; an
// application that installs its own cursor (a game, a CAD tool) is unaffected
// by us either way.
constexpr DWORD kSystemCursors[] = {
    OCR_NORMAL,    OCR_IBEAM,       OCR_WAIT,     OCR_CROSS,
    OCR_UP,        OCR_SIZE,        OCR_SIZENWSE, OCR_SIZENESW,
    OCR_SIZEWE,    OCR_SIZENS,      OCR_SIZEALL,  OCR_NO,
    OCR_HAND,      OCR_APPSTARTING, OCR_HELP,
};

double EaseOutCubic(double t) {
  const double u = 1.0 - t;
  return 1.0 - u * u * u;
}

double EaseInCubic(double t) {
  return t * t * t;
}

void WriteDirtyFlag(DWORD value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, AppSettings::KeyPath(), 0, nullptr, 0,
                      KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return;
  }
  RegSetValueExW(key, L"CursorsDirty", 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&value), sizeof(value));
  RegCloseKey(key);
}

}  // namespace

PointerZoomer::~PointerZoomer() {
  RestoreNow();
  FreeScaled();
  for (auto& slot : slots_) {
    if (slot.original) DestroyCursor(slot.original);
  }
  slots_.clear();
}

bool PointerZoomer::Initialize() {
  if (!IsElevated()) {
    available_ = false;
    LogCritical(
        "pointer enlargement disabled: process is not elevated "
        "(SetSystemCursor requires admin); the highlight overlay still works");
    return false;
  }
  SnapshotScheme();
  if (slots_.empty()) {
    available_ = false;
    Logf("no system cursors could be snapshotted");
    return false;
  }
  BuildLadder();
  available_ = true;
  Logf("pointer zoomer ready: %d cursors x %d ladder steps",
       static_cast<int>(slots_.size()), kLadderSteps);
  return true;
}

void PointerZoomer::Configure() {
  if (!available_) return;
  FreeScaled();
  if (!modified_) SnapshotScheme();
  BuildLadder();
  if (modified_) {
    current_step_ = -1;  // re-apply: the ladder it was drawn from is gone
    ApplyStep(kLadderSteps - 1);
  }
}

void PointerZoomer::RefreshScheme() {
  if (modified_) return;  // never snapshot a cursor we replaced ourselves
  FreeScaled();
  SnapshotScheme();
  BuildLadder();
}

void PointerZoomer::SnapshotScheme() {
  for (auto& slot : slots_) {
    if (slot.original) DestroyCursor(slot.original);
  }
  slots_.clear();
  slots_.reserve(sizeof(kSystemCursors) / sizeof(kSystemCursors[0]));
  for (const DWORD ocr_id : kSystemCursors) {
    HCURSOR shared = LoadCursorW(nullptr, MAKEINTRESOURCEW(ocr_id));
    if (!shared) continue;
    // The shared system cursor object belongs to user32; our own copy is what
    // we later hand back to SetSystemCursor.
    HCURSOR copy = CopyCursor(shared);
    if (!copy) continue;
    Slot slot;
    slot.ocr_id = ocr_id;
    slot.original = copy;
    slots_.push_back(slot);
  }
}

SIZE PointerZoomer::OriginalSize(HCURSOR cursor) {
  SIZE size = {32, 32};
  ICONINFO icon_info = {};
  if (!GetIconInfo(cursor, &icon_info)) return size;

  HBITMAP query = icon_info.hbmColor ? icon_info.hbmColor : icon_info.hbmMask;
  BITMAP bm = {};
  if (query && GetObject(query, sizeof(BITMAP), &bm)) {
    size.cx = bm.bmWidth;
    // A monochrome cursor stores the AND and XOR planes stacked in one bitmap.
    size.cy = icon_info.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
  }
  if (icon_info.hbmColor) DeleteObject(icon_info.hbmColor);
  if (icon_info.hbmMask) DeleteObject(icon_info.hbmMask);
  if (size.cx <= 0 || size.cy <= 0) {
    size.cx = 32;
    size.cy = 32;
  }
  return size;
}

void PointerZoomer::BuildLadder() {
  const AppSettings& settings = AppSettings::Get();
  const double max_scale = settings.scale_percent / 100.0;
  for (auto& slot : slots_) {
    for (HCURSOR& handle : slot.scaled) {
      if (handle) DestroyCursor(handle);
    }
    slot.scaled.assign(static_cast<size_t>(kLadderSteps), nullptr);

    const SIZE native = OriginalSize(slot.original);
    for (int step = 1; step < kLadderSteps; ++step) {
      const double t = static_cast<double>(step) / (kLadderSteps - 1);
      const double scale = 1.0 + (max_scale - 1.0) * t;
      const int w = static_cast<int>(native.cx * scale + 0.5);
      const int h = static_cast<int>(native.cy * scale + 0.5);
      if (w < 1 || h < 1) continue;
      if (w == native.cx && h == native.cy) continue;  // step 0 already covers it
      HCURSOR scaled = static_cast<HCURSOR>(
          CopyImage(slot.original, IMAGE_CURSOR, w, h, 0));
      // A null or the source itself means "nothing to gain here": fall back to
      // the native cursor for that step instead of risking a double destroy.
      slot.scaled[static_cast<size_t>(step)] =
          (scaled && scaled != slot.original) ? scaled : nullptr;
    }
  }
}

void PointerZoomer::FreeScaled() {
  for (auto& slot : slots_) {
    for (HCURSOR& handle : slot.scaled) {
      if (handle) DestroyCursor(handle);
      handle = nullptr;
    }
    slot.scaled.clear();
  }
}

void PointerZoomer::ApplyStep(int step) {
  if (slots_.empty()) return;
  // Restoring is allowed even once enlargement has been switched off, so a
  // session that lost its privilege halfway cannot strand an enlarged cursor.
  if (!available_ && step != 0) return;
  if (step < 0) step = 0;
  if (step >= kLadderSteps) step = kLadderSteps - 1;
  if (step == current_step_) return;  // nothing visible to change

  int applied = 0;
  for (auto& slot : slots_) {
    HCURSOR source = slot.original;
    if (step > 0 && static_cast<size_t>(step) < slot.scaled.size() &&
        slot.scaled[static_cast<size_t>(step)]) {
      source = slot.scaled[static_cast<size_t>(step)];
    }
    // SetSystemCursor takes ownership of the handle and destroys it, so it
    // always gets a throwaway copy of our cached cursor.
    HCURSOR copy = CopyCursor(source);
    if (!copy) continue;
    if (SetSystemCursor(copy, slot.ocr_id)) ++applied;
  }

  current_step_ = step;
  modified_ = step > 0;

  if (applied == 0) {
    // Two empty passes in a row means the session refuses cursor changes
    // (locked-down policy, another app owns the cursors). Stop trying and let
    // the overlay carry the effect instead of spinning.
    if (++failed_applies_ >= 2) {
      LogCritical("SetSystemCursor failed; pointer enlargement turned off for "
                  "this session");
      available_ = false;
      phase_ = Phase::kIdle;
      modified_ = false;
      current_step_ = 0;
      MarkDirty(false);
    }
  } else {
    failed_applies_ = 0;
  }
}

void PointerZoomer::Trigger() {
  if (!available_) return;
  const int64_t now = NowUs();
  switch (phase_) {
    case Phase::kIdle:
      MarkDirty(true);
      phase_ = Phase::kGrow;
      phase_start_us_ = now;
      if (AppSettings::Get().grow_ms <= 0) {
        ApplyStep(kLadderSteps - 1);
        phase_ = Phase::kHold;
        phase_start_us_ = now;
      }
      break;
    case Phase::kGrow:
    case Phase::kHold:
      phase_start_us_ = now;  // extend the highlight, do not restart it
      break;
    case Phase::kShrink:
      // The user is still shaking mid-shrink: jump back to full size and hold.
      ApplyStep(kLadderSteps - 1);
      phase_ = Phase::kHold;
      phase_start_us_ = now;
      break;
  }
}

void PointerZoomer::Tick(int64_t now_us) {
  if (phase_ == Phase::kIdle) return;
  const AppSettings& settings = AppSettings::Get();
  const int64_t elapsed_us = now_us - phase_start_us_;

  switch (phase_) {
    case Phase::kGrow: {
      if (settings.grow_ms <= 0 || elapsed_us >= settings.grow_ms * 1000LL) {
        phase_ = Phase::kHold;
        phase_start_us_ = now_us;
        ApplyStep(kLadderSteps - 1);
        break;
      }
      const double t =
          static_cast<double>(elapsed_us) / (settings.grow_ms * 1000.0);
      const int step = static_cast<int>(
          EaseOutCubic(t) * (kLadderSteps - 1) + 0.5);
      if (step != current_step_) ApplyStep(step);
      break;
    }
    case Phase::kHold:
      if (elapsed_us >= settings.hold_ms * 1000LL) {
        phase_ = Phase::kShrink;
        phase_start_us_ = now_us;
      }
      break;
    case Phase::kShrink: {
      if (settings.shrink_ms <= 0 ||
          elapsed_us >= settings.shrink_ms * 1000LL) {
        ApplyStep(0);
        phase_ = Phase::kIdle;
        current_step_ = 0;
        MarkDirty(false);
        break;
      }
      const double t =
          static_cast<double>(elapsed_us) / (settings.shrink_ms * 1000.0);
      const double span = kLadderSteps - 1;
      int step = static_cast<int>(span - EaseInCubic(t) * span + 0.5);
      if (step < 0) step = 0;
      if (step != current_step_) ApplyStep(step);
      break;
    }
    default:
      break;
  }
}

void PointerZoomer::RestoreNow() {
  if (modified_) ApplyStep(0);
  phase_ = Phase::kIdle;
  current_step_ = 0;
  MarkDirty(false);
}

void PointerZoomer::MarkDirty(bool dirty) {
  WriteDirtyFlag(dirty ? 1u : 0u);
}

bool PointerZoomer::NeedsHealing() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, AppSettings::KeyPath(), 0, KEY_READ,
                    &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD value = 0;
  DWORD type = 0;
  DWORD size = sizeof(value);
  const BOOL dirty = RegQueryValueExW(key, L"CursorsDirty", nullptr, &type,
                                      reinterpret_cast<BYTE*>(&value), &size) ==
                             ERROR_SUCCESS &&
                     type == REG_DWORD &&
                     size == sizeof(value) && value != 0;
  RegCloseKey(key);
  return dirty != FALSE;
}

void PointerZoomer::HealIfNeeded() {
  if (!NeedsHealing()) return;
  // The previous instance died mid-animation; tell the system to reload the
  // user's pointer scheme from its registry settings.
  SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
  WriteDirtyFlag(0);
  LogCritical("restored the pointer scheme left enlarged by a previous run");
}

}  // namespace stfc
