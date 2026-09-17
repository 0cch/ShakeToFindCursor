# Shake to Find Cursor

Lose the pointer on a big or busy screen? Shake the mouse and the pointer grows
and a click-through highlight is drawn around it for a moment, then everything
fades back to normal. Inspired by the macOS "shake to find cursor" behaviour.

```
  shake the mouse  ->  pointer enlarges  +  ripple rings / spotlight around it
                   ->  fades back after ~1 s, no clicks stolen, no focus moved
```

## What changed in 1.4

The detection and the animation were rebuilt around one constraint: a low-level
mouse hook runs on your thread and blocks *everyone's* input while it is inside
the callback.

* **Shake detection actually detects shakes.** The old build judged a fixed
  10-sample history, which at 500-8000 Hz mouse report rates is 1-20 ms of
  movement - far too short to hold the five direction changes it demanded, so
  shaking did nothing. It also counted X and Y sign flips separately, which both
  missed diagonal shakes and mistook high-rate sensor jitter for a shake. The
  new algorithm projects the recent motion onto its principal axis, counts
  hysteresis reversals there with a per-stroke amplitude and speed requirement,
  and ignores pauses and pointer warps. `tests/shake_detector_test.cpp` replays
  recorded synthetic traces through the old and the new algorithm side by side:

  | trace                  | v1.3 | v1.4 |
  | ---------------------- | ---- | ---- |
  | deliberate shake       | 0    | 2    |
  | same, sampled at 1 kHz | 0    | 2    |
  | same, sampled at 8 kHz | 0    | 2    |
  | diagonal shake         | 0    | 2    |
  | fast curved sweep      | 0    | 0    |
  | optical sensor jitter  | 999  | 0    |

* **No input lag, no flicker.** The hook only feeds numbers to the detector and
  posts one message. The pointer enlargement is applied from a pre-built ladder
  of scaled cursors, so a frame is one `SetSystemCursor` per cursor instead of
  thirteen `CopyImage` calls at 60 FPS; growing a cursor that used to be scaled
  on demand is now free while you shake. The overlay renders on its own thread.
* **The highlight overlay** (new): *ripple* rings plus a marker dot, or
  *spotlight* which dims the monitor around the pointer. It is layered,
  topmost and click-through, so it never takes focus or a click - and it works
  without administrator rights, which is what makes the app usable for people
  who will not elevate it.
* **Your pointer scheme is respected.** The cursors that get enlarged are the
  ones the system is using right now, and they are put back afterwards; a custom
  scheme, a theme or DPI change while running is picked up again on the next
  `WM_SETTINGCHANGE`.
* **No more hard requirement for admin.** The manifest is `asInvoker`. Losing
  the dialog at start-up means the pointer enlargement is simply unavailable,
  and the tray menu offers *Restart as administrator* (one click) plus a
  scheduled task for it at sign-in.
* **It cleans up after itself.** A registry marker is written while the pointer
  is enlarged, so an instance that was killed mid-animation heals the session on
  the next start instead of leaving a 5x pointer until you sign out. Logoff and
  shutdown restore the cursors too, and a second copy refuses to start rather
  than fighting over the cursor scheme.
* **Settings, not source edits.** Sensitivity, size, duration, highlight,
  tracking mode, hotkey and logging live in `HKCU\Software\ShakeToFindCursor`
  and are settable from the tray menu. They used to be `constexpr` fields you
  had to recompile to change (and the documented values had drifted from the
  code).
* **Idle cost.** In hook mode there is no periodic timer at all while nothing is
  happening: the old loop woke every 10-16 ms forever (`PeekMessage` +
  `Sleep(1)`). Polling mode still samples at 125 Hz.
* **Small things:** the tray icon is re-added when Explorer restarts
  (`TaskbarCreated`), auto-start works when not elevated (HKCU Run key), the
  debug log goes to `%LOCALAPPDATA%\ShakeToFindCursor\shake.log` instead of a
  file next to the exe, injected mouse events from automation no longer trigger
  the effect, and `--help` works.

## Using it

1. Run `ShakeToFindCursor.exe`. A tray icon appears; the first start shows a
   balloon pointing at the menu.
2. Shake the mouse when you lose the pointer. Clicking the tray icon (either
   button) opens the menu; double-clicking it finds the pointer once.
3. `Ctrl+Alt+F` also finds the pointer - useful in a window where you cannot see
   it but do not want to move the mouse.

### Tray menu

| Item | Effect |
| ---- | ------ |
| Find cursor now | trigger once |
| Paused | stop reacting, keep the tray icon |
| Shake sensitivity | Gentle / Standard / Sharp |
| Pointer size | 2x / 3x / 4x / 5x |
| Keep it visible | 0.4 s / 0.9 s / 1.8 s |
| Highlight | None / Ripple rings / Spotlight |
| Enlarge the pointer | needs administrator; greyed out when not elevated |
| Restart as administrator... | re-launches this exe through UAC |
| Start when I sign in | scheduled task when elevated, Run key otherwise |
| Track with a mouse hook | turn off to fall back to timer polling |
| Skip while a full screen app is in front | avoids interrupting games and video |
| Write a debug log | `%LOCALAPPDATA%\ShakeToFindCursor\shake.log` |

### Command line

```
ShakeToFindCursor.exe            use the saved tracking mode
ShakeToFindCursor.exe --hook     low-level mouse hook (default, zero idle cost)
ShakeToFindCursor.exe --poll     sample the pointer on a timer (if hooks are blocked)
ShakeToFindCursor.exe --help     usage
```

`--hook` and `--poll` apply to that run only; the tray menu choice is what gets
remembered.

## Building

Requires only the Windows SDK - no third-party dependencies, no ATL, and no
Visual C++ redistributable (the CRT is linked statically by both toolchains).

```
cmake -S . -B build
cmake --build build --config Release
```

* Visual Studio: open a *Developer Command Prompt*, run the two lines above, and
  use `build\Release\ShakeToFindCursor.exe`.
* MinGW-w64: `cmake -S . -B build -G "MinGW Makefiles" && cmake --build build`.
* Always-on-administrator build (the pre-1.4 behaviour):
  `cmake -S . -B build -DSTFC_REQUIRE_ADMIN=ON`.

Options: `STFC_BUILD_TESTS` (on), `STFC_STRICT_WARNINGS` (on, adds `/WX`),
`STFC_REQUIRE_ADMIN` (off).

### Tests

The shake detector has no Windows dependency, so it is unit tested on any
platform. CI runs this on Linux and on Windows:

```
cmake -S . -B build -DSTFC_BUILD_TESTS=ON
cmake --build build --target shake_detector_tests
ctest --test-dir build --output-on-failure
```

It covers report-rate independence (25 Hz to 8 kHz), diagonal and vertical
shakes, the false-positive cases (sweeps, wobbles, jitter, warps), the
sensitivity slider, re-arming after a pause, and the cost per sample. To tune
how a shake feels, change `ShakeTuning::ForSensitivity()` in
`src/shake_detector.h` and re-run the tests: the false-positive cases are the
ones to watch.

## How it decides

`src/shake_detector.h` keeps the last `window_us` of pointer samples (decimated
so a 8 kHz mouse costs the same as a 125 Hz one), finds the dominant direction
of that motion from the covariance of its steps, projects the positions onto it,
and counts reversals on that scalar signal with a hysteresis band. A reversal
only counts when the stroke it completed was long enough, fast enough, and short
in duration; the shake must still be underway; and one detection starts a
cooldown so a long shake extends the highlight instead of re-firing sixty times
a second.

## Requirements and limitations

* Windows 7 or later, x86 or x64. Multi-monitor and per-monitor DPI aware.
* Enlarging the *pointer* goes through `SetSystemCursor`, which needs
  administrator rights. Without them the overlay still works, and the effect is
  the part most people actually see.
* Applications that install their own cursor (a game's custom crosshair) are not
  ours to enlarge; the overlay is what will find them.
* A game in exclusive full screen composites its own surface, so no overlay can
  be drawn over it - which is also why the pointer is usually irrelevant there.
* `SetSystemCursor` is session global, so a crash between enlarging and restoring
  would outlive the process; the start-up marker above is what repairs that.

## License

MIT - see [LICENSE](LICENSE).
