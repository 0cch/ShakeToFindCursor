// Pointer enlargement: snapshots the user's own pointer scheme, pre-scales it
// once, and animates between the cached sizes.
//
// Two deliberate design choices:
//  * The scaled cursors are built *before* the shake happens. Growing a cursor
//    then costs nothing but a SetSystemCursor call per cursor, instead of 13
//    CopyImage() calls per animation frame (what v1.3 did at 30 FPS, on the
//    thread that also serves the low-level mouse hook).
//  * The originals are the system cursors as they are right now, so a user
//    with a custom pointer scheme gets their own scheme enlarged and restored
//    rather than the stock Windows arrow.
#pragma once

#include <vector>

#include "targetver.h"

namespace stfc {

class PointerZoomer {
 public:
  PointerZoomer() = default;
  ~PointerZoomer();
  PointerZoomer(const PointerZoomer&) = delete;
  PointerZoomer& operator=(const PointerZoomer&) = delete;

  // Snapshot the current pointer scheme and pre-build the scaled ladder.
  // Returns false when enlargement is not possible (not elevated), in which
  // case every other method is a cheap no-op and the overlay takes over.
  bool Initialize();

  // A setting changed: re-snapshot and rebuild.
  void Configure();

  // The pointer scheme, theme or DPI changed while we were running. Only
  // re-snapshots while idle, so we can never capture an already enlarged
  // cursor and mistake it for the original.
  void RefreshScheme();

  // Start a highlight, or extend the running one.
  void Trigger();

  // Advance the animation; call at ~8 ms intervals while Active() is true.
  void Tick(int64_t now_us);

  bool Active() const { return phase_ != Phase::kIdle; }
  // Growing and shrinking need ~120 Hz attention; holding only needs to notice
  // a timeout, so the caller can drop to a slow timer in that phase.
  bool NeedsFastTick() const {
    return phase_ == Phase::kGrow || phase_ == Phase::kShrink;
  }
  void RestoreNow();

  bool available() const { return available_; }

  // SetSystemCursor survives our death, so a killed process could leave the
  // whole session with giant pointers. Mark the session while enlarged and
  // heal on the next start-up.
  static bool NeedsHealing();
  static void HealIfNeeded();

  // Number of cursors in the scheme (for logging/tests).
  size_t slot_count() const { return slots_.size(); }

 private:
  enum class Phase { kIdle, kGrow, kHold, kShrink };

  // Animation resolution: step 0 is the native size, step kLadderSteps-1 the
  // configured maximum. 13 steps over ~140 ms reads as a smooth zoom while
  // keeping the pre-built set small.
  static constexpr int kLadderSteps = 13;

  // `scaled` holds kLadderSteps handles; entry 0 stays null because step 0
  // means "the original cursor", which is what `original` already is.
  struct Slot {
    DWORD ocr_id = 0;
    HCURSOR original = nullptr;  // owned by us
    std::vector<HCURSOR> scaled;
  };

  void SnapshotScheme();
  void BuildLadder();
  void FreeScaled();
  void ApplyStep(int step);
  static SIZE OriginalSize(HCURSOR cursor);
  static void MarkDirty(bool dirty);

  std::vector<Slot> slots_;
  Phase phase_ = Phase::kIdle;
  int64_t phase_start_us_ = 0;
  int current_step_ = 0;
  bool available_ = false;
  bool modified_ = false;
  int failed_applies_ = 0;
};

}  // namespace stfc
