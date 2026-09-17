// Shake detection for a stream of pointer positions.
//
// This header is deliberately free of Windows headers so the algorithm can be
// compiled and unit tested on any platform (see tests/shake_detector_test.cpp)
// and so the hot path stays allocation-free and branch-predictable: it is
// called from inside a WH_MOUSE_LL hook, where every microsecond spent here is
// input latency for the whole desktop.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace stfc {

// One observation of the pointer position. Timestamps are supplied by the
// caller (monotonic microseconds) which keeps the detector deterministic and
// therefore testable.
struct PointerSample {
  int64_t time_us = 0;
  int x = 0;
  int y = 0;
};

// All thresholds that decide what counts as a shake.
struct ShakeTuning {
  // Upper bound on the history retained for one analysis pass. Work per
  // sample is O(kSampleCap), never O(events_per_second), so a 8 kHz gaming
  // mouse costs no more than a 125 Hz one.
  static constexpr int kSampleCap = 128;

  // Sensitivity the tray menu labels "Standard". The field defaults below are
  // exactly ForSensitivity(kDefaultSensitivity); a unit test enforces it.
  static constexpr int kDefaultSensitivity = 55;

  // Everything is judged inside this sliding window.
  int64_t window_us = 345000;
  // A pause longer than this ends the streak (the user stopped moving).
  int64_t stall_us = 76000;
  // A single jump larger than this is treated as a pointer teleport (remote
  // desktop, monitor switch, window snap tools) and resets the streak instead
  // of counting as violent movement.
  int max_step_px = 420;
  // How many direction reversals are required inside the window.
  int min_reversals = 3;
  // One half-stroke must cover at least this much travel along the shake
  // axis. This is what rejects sensor jitter and hand tremor.
  int min_stroke_px = 48;
  // A half-stroke must be completed within this time (rejects slow wobble).
  int64_t max_stroke_us = 198000;
  // Average speed of the counted strokes, in pixels/second.
  double min_speed = 700.0;
  // The shake still has to be *underway*: the newest reversal must be no
  // older than this. Without it, a shake from half a second ago could fire
  // while the pointer is already parked.
  int64_t max_last_reversal_age_us = 202500;
  // After a successful detection, ignore results for this long so one long
  // shake cannot restart the animation dozens of times per second.
  int64_t cooldown_us = 700000;
  // Ignore windows with fewer samples than this (also avoids triggering on the
  // first twitch after logon or after a resume).
  int min_samples = 6;
  // Input decimation: samples arriving faster than this are merged into the
  // pending one. The effective interval is additionally stretched so the
  // window never holds more than kSampleCap samples.
  int64_t min_sample_interval_us = 2000;
  // Retention cap, clamped to kSampleCap by the detector.
  int max_samples = kSampleCap;

  int sample_cap() const {
    return max_samples < kSampleCap ? max_samples : kSampleCap;
  }

  // The window is sampled at most sample_cap() times.
  int64_t effective_sample_interval_us() const {
    const int cap = sample_cap();
    const int64_t spread =
        cap > 1 ? window_us / static_cast<int64_t>(cap - 1) : window_us;
    return std::max(min_sample_interval_us, spread);
  }

  static int64_t RoundUs(double value, int64_t step) {
    return static_cast<int64_t>(value / static_cast<double>(step) + 0.5) * step;
  }

  // Map a user-facing 1..100 sensitivity slider onto the thresholds. Low
  // values demand big, deliberate shakes; high values answer to short
  // flicks. Amplitude and speed carry the discrimination and the reversal
  // count only has to reject one-off sweeps, so the window *grows* at the low
  // end (a slow, wide shake is still a shake) instead of demanding a faster
  // shake rate, which is what made the old build ignore big gentle shakes.
  static ShakeTuning ForSensitivity(int sensitivity) {
    const double t = static_cast<double>(std::clamp(sensitivity, 1, 100) - 1) /
                     99.0;
    ShakeTuning s;
    s.min_stroke_px =
        static_cast<int>(80.0 + (22.0 - 80.0) * t + 0.5);  // px, rounded
    s.min_reversals = t < 0.72 ? 3 : 2;
    const double speed = 1000.0 + (450.0 - 1000.0) * t;
    s.min_speed = static_cast<double>(RoundUs(speed, 10));
    s.window_us = RoundUs(400000.0 + (300000.0 - 400000.0) * t, 5000);
    s.max_stroke_us = RoundUs(220000.0 + (180000.0 - 220000.0) * t, 1000);
    s.stall_us = RoundUs(60000.0 + (90000.0 - 60000.0) * t, 1000);
    s.max_last_reversal_age_us = s.window_us / 2 + 30000;
    return s;
  }
};

// Diagnostics of the last evaluated window; handy for logging and tests.
struct ShakeStats {
  int samples = 0;
  int reversals = 0;
  int good_reversals = 0;
  int64_t travel_px = 0;
  int64_t window_us = 0;
  double speed = 0.0;
  double axis_deg = 0.0;
  int64_t us_since_trigger = -1;
};

// Counts fast, large direction reversals along the *principal axis* of the
// recent movement.
//
// Why not "count X flips plus Y flips" (what v1.3 did): a diagonal shake then
// needs both axes to flip, so real shakes are under-counted, while one smooth
// arc scores several free flips, so ordinary sweeps are over-counted. Worse,
// v1.3 judged a fixed 10-sample history, which at 500-8000 Hz report rates is
// 1-20 ms of motion - far too short to contain five reversals - so shaking
// simply did nothing unless the polling fallback was in use.
//
// Projecting onto the dominant direction of travel and running a hysteresis
// reversal counter on that scalar signal fixes all three problems and is
// cheaper than the per-axis version.
class ShakeDetector {
 public:
  explicit ShakeDetector(ShakeTuning tuning = ShakeTuning{})
      : tuning_(tuning) {}

  void SetTuning(const ShakeTuning& tuning) {
    tuning_ = tuning;
    Reset();
  }
  const ShakeTuning& tuning() const { return tuning_; }
  const ShakeStats& stats() const { return stats_; }

  // Forget the history. The cooldown survives on purpose: changing a setting
  // must not let a shake in progress re-trigger instantly.
  void Reset() {
    head_ = 0;
    count_ = 0;
    have_pending_ = false;
    stats_ = ShakeStats{};
  }

  // Feed one pointer observation. Returns true exactly once per detected
  // shake, at the moment the final reversal is registered.
  bool AddSample(const PointerSample& incoming) {
    if (!have_pending_) {
      have_pending_ = true;
      pending_ = incoming;
      Push(incoming);
      return false;
    }

    const int64_t dt = incoming.time_us - pending_.time_us;
    if (dt <= 0) return false;  // duplicated or non-monotonic stamp

    const int64_t dx = static_cast<int64_t>(incoming.x) - pending_.x;
    const int64_t dy = static_cast<int64_t>(incoming.y) - pending_.y;

    // Stopped moving, or the pointer was warped: start over.
    if (dt > tuning_.stall_us ||
        (tuning_.max_step_px > 0 &&
         ApproxHypot(dx, dy) > tuning_.max_step_px)) {
      StartOver(incoming);
      return false;
    }

    pending_ = incoming;
    // Decimate, but keep the newest position so the next accepted sample
    // carries the full travel in between.
    if (incoming.time_us - Last().time_us <
        tuning_.effective_sample_interval_us()) {
      return false;
    }

    Push(incoming);
    Prune(incoming.time_us);
    return Evaluate(incoming.time_us);
  }

 private:
  static constexpr int kBuf = ShakeTuning::kSampleCap;

  // Integer octagon approximation of the Euclidean length: exact enough for a
  // threshold test and free of sqrt() in the hook path.
  static int64_t ApproxHypot(int64_t dx, int64_t dy) {
    const int64_t ax = dx < 0 ? -dx : dx;
    const int64_t ay = dy < 0 ? -dy : dy;
    const int64_t hi = ax > ay ? ax : ay;
    const int64_t lo = ax > ay ? ay : ax;
    return hi + lo / 2;
  }

  int cap() const { return tuning_.sample_cap(); }

  const PointerSample& At(int i) const {
    return buf_[static_cast<size_t>((head_ + i) % kBuf)];
  }
  const PointerSample& Last() const { return At(count_ - 1); }

  void Push(const PointerSample& s) {
    const int c = cap();
    if (count_ < c) {
      buf_[static_cast<size_t>((head_ + count_) % kBuf)] = s;
      ++count_;
    } else {
      buf_[static_cast<size_t>(head_)] = s;  // overwrite oldest
      head_ = (head_ + 1) % kBuf;
    }
  }

  void StartOver(const PointerSample& s) {
    head_ = 0;
    count_ = 0;
    have_pending_ = true;
    pending_ = s;
    Push(s);
  }

  void Prune(int64_t now_us) {
    const int64_t oldest = now_us - tuning_.window_us;
    while (count_ > 2 && At(0).time_us < oldest) {
      head_ = (head_ + 1) % kBuf;
      --count_;
    }
  }

  bool Evaluate(int64_t now_us) {
    ShakeStats stats;
    stats.samples = count_;
    const int n = count_;
    if (n < 2 || n < tuning_.min_samples) {
      stats_ = stats;
      return false;
    }

    // ---- Pass 1: principal axis + total travel -------------------------
    // Dominant eigenvector of the 2x2 covariance of consecutive deltas is at
    // angle 0.5 * atan2(2*Sxy, Sxx - Syy).
    double sxx = 0.0, syy = 0.0, sxy = 0.0;
    int64_t travel = 0;
    for (int i = 1; i < n; ++i) {
      const PointerSample& a = At(i - 1);
      const PointerSample& b = At(i);
      const double dx = static_cast<double>(b.x - a.x);
      const double dy = static_cast<double>(b.y - a.y);
      sxx += dx * dx;
      syy += dy * dy;
      sxy += dx * dy;
      travel += ApproxHypot(b.x - a.x, b.y - a.y);
    }
    stats.travel_px = travel;
    stats.window_us = Last().time_us - At(0).time_us;

    const double angle =
        (sxx + syy > 0.0) ? 0.5 * std::atan2(2.0 * sxy, sxx - syy) : 0.0;
    const double ux = std::cos(angle);
    const double uy = std::sin(angle);
    stats.axis_deg = angle * 57.29577951308232;  // 180/pi

    // ---- Pass 2: hysteresis reversal counter on the projected signal ----
    double proj[kBuf];
    int64_t stamp[kBuf];
    proj[0] = static_cast<double>(At(0).x) * ux +
              static_cast<double>(At(0).y) * uy;
    stamp[0] = At(0).time_us;
    for (int i = 1; i < n; ++i) {
      const PointerSample& s = At(i);
      proj[i] = static_cast<double>(s.x) * ux + static_cast<double>(s.y) * uy;
      stamp[i] = s.time_us;
    }

    const double amp = static_cast<double>(tuning_.min_stroke_px);
    double anchor = proj[0];
    int64_t anchor_t = stamp[0];
    double high = proj[0], low = proj[0];
    int64_t high_t = stamp[0], low_t = stamp[0];
    int dir = 0;  // +1 along +axis, -1 against it, 0 undecided
    int reversals = 0, good = 0;
    double counted_len = 0.0;
    int64_t counted_us = 0;
    int64_t first_good_t = now_us;
    int64_t last_reversal_t = stamp[0];

    for (int i = 1; i < n; ++i) {
      const double v = proj[i];
      if (dir == 0) {
        if (v - low >= amp) {  // first clear move was positive
          dir = 1;
          anchor = low;
          anchor_t = low_t;
          high = v;
          high_t = stamp[i];
        } else if (high - v >= amp) {  // first clear move was negative
          dir = -1;
          anchor = high;
          anchor_t = high_t;
          low = v;
          low_t = stamp[i];
        } else {  // still inside the hysteresis band
          if (v > high) {
            high = v;
            high_t = stamp[i];
          }
          if (v < low) {
            low = v;
            low_t = stamp[i];
          }
        }
        continue;
      }

      double stroke_len = 0.0;
      int64_t stroke_us = 0;
      int64_t stroke_start_t = stamp[i];
      if (dir > 0) {
        if (v > high) {  // still extending the stroke
          high = v;
          high_t = stamp[i];
          continue;
        }
        if (high - v < amp) continue;  // not a reversal yet
        stroke_len = high - anchor;
        stroke_us = high_t - anchor_t;
        stroke_start_t = anchor_t;
        last_reversal_t = high_t;
        anchor = high;
        anchor_t = high_t;
        low = v;
        low_t = stamp[i];
        dir = -1;
      } else {
        if (v < low) {
          low = v;
          low_t = stamp[i];
          continue;
        }
        if (v - low < amp) continue;
        stroke_len = anchor - low;
        stroke_us = low_t - anchor_t;
        stroke_start_t = anchor_t;
        last_reversal_t = low_t;
        anchor = low;
        anchor_t = low_t;
        high = v;
        high_t = stamp[i];
        dir = 1;
      }

      ++reversals;
      // A stroke only counts if it was long, fast and decisive.
      const double speed_px_s =
          stroke_us > 0
              ? stroke_len * 1000000.0 / static_cast<double>(stroke_us)
              : 0.0;
      if (stroke_us > 0 && stroke_len >= amp &&
          stroke_us <= tuning_.max_stroke_us && speed_px_s >= tuning_.min_speed) {
        if (good == 0) first_good_t = stroke_start_t;
        ++good;
        counted_len += stroke_len;
        counted_us += stroke_us;
      }
    }

    stats.reversals = reversals;
    stats.good_reversals = good;
    const double speed =
        counted_us > 0
            ? counted_len * 1000000.0 / static_cast<double>(counted_us)
            : 0.0;
    stats.speed = speed;

    const bool underway =
        now_us - last_reversal_t <= tuning_.max_last_reversal_age_us;
    bool shake = good >= tuning_.min_reversals && underway &&
                 speed >= tuning_.min_speed &&
                 travel >= std::max(2 * tuning_.min_stroke_px, 120) &&
                 (last_reversal_t - first_good_t) <= tuning_.window_us +
                     tuning_.max_stroke_us;
    if (have_trigger_time_) {
      stats.us_since_trigger = now_us - last_trigger_us_;
      if (shake && now_us - last_trigger_us_ < tuning_.cooldown_us) {
        shake = false;  // still the same shake; stay quiet
      }
    }

    stats_ = stats;
    if (!shake) return false;

    have_trigger_time_ = true;
    last_trigger_us_ = now_us;
    // Consume the streak: a *continued* shake has to build a fresh set of
    // reversals, which is what makes the cooldown above extend the highlight
    // instead of re-firing every frame.
    StartOver(pending_);
    return true;
  }

  ShakeTuning tuning_;
  PointerSample buf_[kBuf];
  int head_ = 0;
  int count_ = 0;
  PointerSample pending_;
  bool have_pending_ = false;
  int64_t last_trigger_us_ = 0;
  bool have_trigger_time_ = false;
  ShakeStats stats_;
};

}  // namespace stfc
