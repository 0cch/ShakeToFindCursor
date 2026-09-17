// Host-side unit tests for the shake detector.
//
// Build and run:
//   g++ -std=c++17 -O2 -Wall -Wextra tests/shake_detector_test.cpp -o tests
//   ./tests
// or through CMake: cmake -S . -B build && cmake --build build && ctest
//
// Everything is deterministic: synthetic pointer traces with explicit
// timestamps, so a failure here is a real algorithmic regression and not a
// timing fluke. The last section replays the same traces through the v1.3
// algorithm (copied below) to show what changed.
#include "../src/shake_detector.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace {

using stfc::PointerSample;
using stfc::ShakeDetector;
using stfc::ShakeTuning;

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL  %s\n", what.c_str());
  }
}

// ---------------------------------------------------------------------------
// Synthetic pointer traces
// ---------------------------------------------------------------------------

struct TraceOptions {
  int64_t duration_us = 1200000;
  int64_t interval_us = 8000;  // ~125 Hz mouse
  double freq_hz = 6.0;        // shake rate
  double amplitude_px = 90.0;  // half peak-to-peak along the shake axis
  double angle_deg = 0.0;      // shake axis
  double drift_px = 0.0;       // slow drift along the axis
  int center_x = 800;
  int center_y = 500;
  int noise_px = 0;            // deterministic sensor noise
  int64_t start_us = 1000000;
};

constexpr double kPi = 3.14159265358979323846;
constexpr double kUsPerSecond = 1e6;

// Deterministic little PRNG, so the noise in a trace is the same everywhere.
int Jitter(uint32_t* state, int amplitude_px) {
  *state = *state * 1103515245u + 12345u;
  if (amplitude_px <= 0) return 0;
  return static_cast<int>(*state % (2 * static_cast<uint32_t>(amplitude_px) + 1)) -
         amplitude_px;
}

// A back-and-forth trace along a line through (center_x, center_y).
// Shorthand for "the default shake, but sampled at this rate / along this
// angle", which keeps the case tables below readable.
TraceOptions WithInterval(int64_t interval_us) {
  TraceOptions o;
  o.interval_us = interval_us;
  return o;
}

TraceOptions WithAngle(double angle_deg) {
  TraceOptions o;
  o.angle_deg = angle_deg;
  return o;
}

std::vector<PointerSample> MakeShake(const TraceOptions& o) {
  std::vector<PointerSample> out;
  const double ca = std::cos(o.angle_deg * kPi / 180.0);
  const double sa = std::sin(o.angle_deg * kPi / 180.0);
  uint32_t rng = 12345u;
  for (int64_t t = 0; t <= o.duration_us; t += o.interval_us) {
    const double seconds = static_cast<double>(t) / kUsPerSecond;
    const double wave = std::sin(2.0 * kPi * o.freq_hz * seconds);
    const double drift = o.drift_px * seconds /
                         (static_cast<double>(o.duration_us) / kUsPerSecond);
    const double along = o.amplitude_px * wave + drift;
    const int dx = static_cast<int>(along * ca) + Jitter(&rng, o.noise_px);
    const int dy = static_cast<int>(along * sa) + Jitter(&rng, o.noise_px);
    out.push_back({o.start_us + t, o.center_x + dx, o.center_y + dy});
  }
  return out;
}

// One long, straight, very fast flick - the classic "reach across the screen".
std::vector<PointerSample> MakeDash(int64_t interval_us, double px_per_s) {
  std::vector<PointerSample> out;
  const int64_t start = 1000000;
  for (int i = 0; i < 60; ++i) {
    const double seconds = static_cast<double>(i * interval_us) / kUsPerSecond;
    out.push_back({start + i * interval_us,
                   100 + static_cast<int>(px_per_s * seconds), 400});
  }
  return out;
}

// A fast curved sweep: high speed, several sign changes per axis, no shake.
std::vector<PointerSample> MakeArc(int64_t interval_us) {
  std::vector<PointerSample> out;
  const int64_t start = 1000000;
  for (int64_t t = 0; t <= 400000; t += interval_us) {
    const double p = static_cast<double>(t) / 400000.0;
    const int x = 200 + static_cast<int>(1400.0 * p);
    const int y = 300 + static_cast<int>(500.0 * std::sin(kPi * p));
    out.push_back({start + t, x, y});
  }
  return out;
}

// Pointer resting on a desk with an optical sensor ticking: sub-pixel jitter,
// enormous sample rate.
std::vector<PointerSample> MakeJitter(int64_t interval_us) {
  std::vector<PointerSample> out;
  uint32_t rng = 7u;
  for (int64_t t = 0; t <= 1200000; t += interval_us) {
    rng = rng * 1103515245u + 12345u;
    const int dx = static_cast<int>(rng % 5u) - 2;
    rng = rng * 1103515245u + 12345u;
    const int dy = static_cast<int>(rng % 5u) - 2;
    out.push_back({1000000 + t, 640 + dx, 360 + dy});
  }
  return out;
}

// Slow, wide wobble while dragging something: big amplitude, low speed.
std::vector<PointerSample> MakeWobble() {
  TraceOptions o;
  o.freq_hz = 1.6;
  o.amplitude_px = 110.0;
  o.duration_us = 2000000;
  return MakeShake(o);
}

struct FeedResult {
  int triggers = 0;
  int64_t first_trigger_us = -1;
  int64_t last_sample_us = 0;
};

FeedResult Feed(ShakeDetector& d, const std::vector<PointerSample>& trace) {
  FeedResult r;
  for (const auto& s : trace) {
    if (d.AddSample(s)) ++r.triggers;
    r.last_sample_us = s.time_us;
  }
  if (r.triggers > 0) {
    // Re-derive the first trigger time by replaying into a fresh detector.
    ShakeDetector fresh(d.tuning());
    for (const auto& s : trace) {
      if (fresh.AddSample(s)) {
        r.first_trigger_us = s.time_us;
        break;
      }
    }
  }
  return r;
}

ShakeDetector DetectorForSensitivity(int sensitivity) {
  return ShakeDetector(ShakeTuning::ForSensitivity(sensitivity));
}

// ---------------------------------------------------------------------------
// The v1.3 algorithm, reproduced so the improvement can be measured instead of
// asserted. Fixed 10-sample history, X/Y sign flips counted independently,
// average speed over the history, 500 ms window.
// ---------------------------------------------------------------------------
class LegacyDetector {
 public:
  bool AddSample(const PointerSample& s) {
    const int64_t now_ms = s.time_us / 1000;
    if (have_last_) {
      const int64_t dt = now_ms - last_ms_;
      if (dt > 0) {
        hist_.push_back({s.x - last_x_, s.y - last_y_, static_cast<int>(dt)});
        if (hist_.size() > kHistorySize) hist_.pop_front();
      }
    }
    last_x_ = s.x;
    last_y_ = s.y;
    last_ms_ = now_ms;
    have_last_ = true;
    return Detect();
  }

 private:
  struct Movement {
    int dx, dy, dt;
  };
  static constexpr size_t kHistorySize = 10;
  static constexpr int kMinDirectionChanges = 5;
  static constexpr double kMinMovementSpeed = 800.0;
  static constexpr int64_t kMaxTimeWindow = 500;

  bool Detect() const {
    if (hist_.size() < kHistorySize) return false;
    int changes = 0;
    double total_speed = 0.0;
    int64_t total_time = 0;
    int last_x_dir = 0, last_y_dir = 0;
    for (const auto& m : hist_) {
      const int cx = m.dx > 0 ? 1 : (m.dx < 0 ? -1 : 0);
      const int cy = m.dy > 0 ? 1 : (m.dy < 0 ? -1 : 0);
      if (last_x_dir != 0 && cx != 0 && last_x_dir != cx) ++changes;
      if (last_y_dir != 0 && cy != 0 && last_y_dir != cy) ++changes;
      last_x_dir = cx;
      last_y_dir = cy;
      const double dd = static_cast<double>(m.dx * m.dx + m.dy * m.dy);
      const double dist = std::sqrt(dd);
      total_speed += m.dt > 0 ? dist / m.dt * 1000.0 : 0.0;
      total_time += m.dt;
    }
    if (total_time > kMaxTimeWindow) return false;
    const double avg = total_speed / static_cast<double>(hist_.size());
    return changes >= kMinDirectionChanges && avg >= kMinMovementSpeed;
  }

  std::deque<Movement> hist_;
  int last_x_ = 0, last_y_ = 0;
  int64_t last_ms_ = 0;
  bool have_last_ = false;
};

int CountTriggers(const std::vector<PointerSample>& trace, int max = 999) {
  LegacyDetector d;
  int n = 0;
  for (const auto& s : trace) {
    if (d.AddSample(s)) ++n;
    if (n >= max) break;
  }
  return n;
}

}  // namespace

int main() {
  std::printf("ShakeToFindCursor detector tests\n");
  std::printf("================================\n");

  // 1. A deliberate shake must trigger, at every mouse report rate.
  {
    std::printf("\n[1] shake -> trigger (report-rate independence)\n");
    const int64_t rates[] = {40000, 16000, 8000, 4000, 1000, 125};  // 25..8000 Hz
    for (int64_t interval : rates) {
      TraceOptions o;
      o.interval_us = interval;
      auto trace = MakeShake(o);
      ShakeDetector d;
      FeedResult r = Feed(d, trace);
      char msg[160];
      std::snprintf(msg, sizeof(msg),
                    "triggers at %lld Hz sampling",
                    static_cast<long long>(1000000 / interval));
      Check(r.triggers >= 1, msg);
      if (r.first_trigger_us >= 0) {
        const int64_t latency_us =
            r.first_trigger_us - trace.front().time_us;
        std::snprintf(msg, sizeof(msg),
                      "latency %.0f ms at %lld Hz stays under 300 ms",
                      static_cast<double>(latency_us) / 1000.0,
                      static_cast<long long>(1000000 / interval));
        Check(latency_us < 300000, msg);
      }
    }
  }

  // 2. Cooldown: a 1.2 s shake must not re-arm more than a couple of times.
  {
    std::printf("\n[2] one long shake does not spam triggers\n");
    TraceOptions o;
    o.duration_us = 1200000;
    ShakeDetector d;
    FeedResult r = Feed(d, MakeShake(o));
    std::printf("  info: %d triggers in 1.2 s of shaking\n", r.triggers);
    Check(r.triggers >= 1 && r.triggers <= 3, "trigger count stays in 1..3");
  }

  // 3. Everyday pointer motion must stay silent.
  {
    std::printf("\n[3] false positives\n");
    struct Case {
      const char* name;
      std::vector<PointerSample> trace;
    };
    std::vector<Case> cases;
    cases.push_back({"fast straight dash", MakeDash(8000, 4000.0)});
    cases.push_back({"fast curved sweep", MakeArc(8000)});
    cases.push_back({"sensor jitter at 1 kHz", MakeJitter(1000)});
    cases.push_back({"sensor jitter at 250 Hz", MakeJitter(4000)});
    cases.push_back({"slow wide wobble", MakeWobble()});
    for (const auto& c : cases) {
      ShakeDetector d;
      FeedResult r = Feed(d, c.trace);
      Check(r.triggers == 0, c.name);
    }
  }

  // 4. A shake along any axis is still a shake (the projection property).
  {
    std::printf("\n[4] diagonal and vertical shakes\n");
    for (double angle : {0.0, 22.0, 35.0, 45.0, 71.0, 90.0, 150.0, -60.0}) {
      TraceOptions o;
      o.angle_deg = angle;
      ShakeDetector d;
      FeedResult r = Feed(d, MakeShake(o));
      char msg[96];
      std::snprintf(msg, sizeof(msg), "triggers at %.0f deg axis", angle);
      Check(r.triggers >= 1, msg);
    }
  }

  // 5. Low amplitude / low speed shakes are rejected unless the user asks for
  //    high sensitivity.
  {
    std::printf("\n[5] sensitivity slider changes behaviour\n");
    TraceOptions small;
    small.amplitude_px = 18.0;
    small.freq_hz = 9.0;  // a short, snappy flick
    {
      ShakeDetector d = DetectorForSensitivity(10);
      Check(Feed(d, MakeShake(small)).triggers == 0,
            "18 px flick ignored at sensitivity 10");
    }
    {
      ShakeDetector d = DetectorForSensitivity(100);
      Check(Feed(d, MakeShake(small)).triggers >= 1,
            "18 px flick honoured at sensitivity 100");
    }
    TraceOptions big;
    big.amplitude_px = 120.0;
    {
      ShakeDetector d = DetectorForSensitivity(1);
      Check(Feed(d, MakeShake(big)).triggers >= 1,
            "big shake honoured even at sensitivity 1");
    }
  }

  // 6. Shake, stop, shake again -> the second shake is still honoured.
  {
    std::printf("\n[6] re-arm after the pointer rests\n");
    auto first = MakeShake(TraceOptions{});
    TraceOptions o;
    o.start_us = first.back().time_us + 700000;  // 700 ms of nothing
    auto second = MakeShake(o);
    ShakeDetector d;
    int triggers = 0;
    for (const auto& s : first) triggers += d.AddSample(s) ? 1 : 0;
    for (const auto& s : second) triggers += d.AddSample(s) ? 1 : 0;
    std::printf("  info: %d triggers for two separated shakes\n", triggers);
    Check(triggers >= 2, "both shakes detected");
  }

  // 7. Pointer teleports (RDP, monitor switch, window snap) are not shakes.
  {
    std::printf("\n[7] warping and session oddities\n");
    {
      std::vector<PointerSample> trace;
      int x = 100;
      for (int i = 0; i < 40; ++i) {
        trace.push_back({1000000 + i * 8000, x, 200});
        x += (i % 2 == 0) ? 3000 : -3000;  // huge alternating jumps
      }
      ShakeDetector d;
      Check(Feed(d, trace).triggers == 0, "alternating 3000 px warps ignored");
    }
    {
      // Non-monotonic / duplicated timestamps must not divide by zero.
      std::vector<PointerSample> trace = {
          {1000000, 0, 0}, {1000000, 90, 0}, {999000, 0, 0},
          {1000000, 90, 0}, {1000000, 0, 0},  {1001000, 90, 0}};
      ShakeDetector d;
      Feed(d, trace);
      Check(true, "duplicate and backwards timestamps do not crash");
    }
  }

  // 7b. The struct defaults must equal the shipped "Standard" preset, so the
  //     defaults the app uses and the preset the user can pick cannot drift.
  {
    std::printf("\n[7b] defaults track the Standard preset\n");
    const auto d = ShakeTuning{};
    const auto p = ShakeTuning::ForSensitivity(ShakeTuning::kDefaultSensitivity);
    Check(d.window_us == p.window_us, "window_us");
    Check(d.min_stroke_px == p.min_stroke_px, "min_stroke_px");
    Check(d.min_reversals == p.min_reversals, "min_reversals");
    Check(d.min_speed == p.min_speed, "min_speed");
    Check(d.max_stroke_us == p.max_stroke_us, "max_stroke_us");
    Check(d.stall_us == p.stall_us, "stall_us");
    Check(d.max_last_reversal_age_us == p.max_last_reversal_age_us,
          "max_last_reversal_age_us");
  }

  // 8. Stats sanity.
  {
    std::printf("\n[8] diagnostics\n");
    ShakeDetector d;
    Feed(d, MakeShake(TraceOptions{}));
    const auto& s = d.stats();
    std::printf(
        "  info: samples=%d reversals=%d good=%d travel=%lld px speed=%.0f px/s "
        "axis=%.1f deg\n",
        s.samples, s.reversals, s.good_reversals,
        static_cast<long long>(s.travel_px), s.speed, s.axis_deg);
    Check(s.samples > 0 && s.samples <= static_cast<int>(ShakeTuning::kSampleCap),
          "sample count stays inside the cap");
    Check(std::fabs(s.axis_deg) < 5.0, "axis of a horizontal shake is ~0 deg");
  }

  // 9. Cost, because this runs inside a WH_MOUSE_LL hook.
  {
    std::printf("\n[9] per-sample cost\n");
    auto trace = MakeShake(TraceOptions{});
    ShakeDetector d;
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kLoops = 4000;
    for (int i = 0; i < kLoops; ++i) {
      d.Reset();
      for (const auto& s : trace) d.AddSample(s);
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    const double per_sample = static_cast<double>(us) /
                              static_cast<double>(kLoops * trace.size());
    std::printf("  info: %.2f us per sample over %zu samples\n", per_sample,
                trace.size());
    Check(per_sample < 5.0, "detector stays far below a mouse report period");
  }

  // 10. What the old algorithm did with the exact same traces.
  {
    std::printf("\n[10] v1.3 algorithm on the same traces (reference only)\n");
    struct Row {
      const char* name;
      std::vector<PointerSample> trace;
    };
    const std::vector<Row> rows = {
        Row{"shake @125Hz", MakeShake(TraceOptions{})},
        Row{"shake @250Hz", MakeShake(WithInterval(4000))},
        Row{"shake @1kHz", MakeShake(WithInterval(1000))},
        Row{"shake @8kHz", MakeShake(WithInterval(125))},
        Row{"diagonal shake @8ms", MakeShake(WithAngle(45.0))},
        Row{"curved sweep", MakeArc(8000)},
        Row{"jitter @1kHz", MakeJitter(1000)},
    };
    std::printf("  %-22s %-16s %-16s\n", "trace", "v1.3 triggers", "new triggers");
    for (const auto& r : rows) {
      ShakeDetector d;
      const int new_n = Feed(d, r.trace).triggers;
      const int old_n = CountTriggers(r.trace);
      std::printf("  %-22s %-16s %-16s\n", r.name,
                  std::to_string(old_n).c_str(), std::to_string(new_n).c_str());
    }
  }

  std::printf("\n================================\n%d checks, %d failures\n",
              g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
