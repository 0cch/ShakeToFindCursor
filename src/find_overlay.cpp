#include "targetver.h"

#include "find_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

#include "app_settings.h"

namespace stfc {
namespace {

constexpr wchar_t kClassName[] = L"ShakeToFindCursorOverlay";
constexpr UINT kMsgRequest = WM_APP + 0x40;  // a request is queued in the owner
constexpr UINT_PTR kAnimTimer = 0xEE1;
constexpr UINT kRippleFrameMs = 15;
constexpr UINT kSpotlightFrameMs = 33;
constexpr int kRingCount = 3;

// White with a faint cool tint: reads as "highlight" on both dark and light
// content, and with a flat colour the premultiplied form is trivial.
constexpr uint8_t kTintR = 235, kTintG = 245, kTintB = 255;

inline uint32_t Premultiplied(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  const uint32_t alpha = a;
  return (alpha << 24) | ((static_cast<uint32_t>(r) * alpha / 255u) << 16) |
         ((static_cast<uint32_t>(g) * alpha / 255u) << 8) |
         (static_cast<uint32_t>(b) * alpha / 255u);
}

inline double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

inline double EaseOutCubic(double t) {
  const double u = 1.0 - t;
  return 1.0 - u * u * u;
}

int ScaleForDpi(int px, int dpi) { return MulDiv(px, dpi, 96); }

// Everything one paint needs.
struct Frame {
  uint32_t* bits = nullptr;
  int width = 0;
  int height = 0;
  POINT center = {};  // client coordinates
  int dpi = 96;
  double progress = 0.0;  // 0..1 through the whole effect
};

// Soft-edged ring. Only the annulus is visited, so a 600 px ripple touches a
// few tens of thousands of pixels per frame instead of the whole surface.
void DrawRing(const Frame& f, double radius, double thickness, uint8_t alpha) {
  if (alpha == 0 || radius <= 0.0 || thickness <= 0.0) return;
  const int cx = static_cast<int>(f.center.x);
  const int cy = static_cast<int>(f.center.y);
  const int reach = static_cast<int>(radius + thickness * 0.5 + 1.0);
  const int y_begin = std::max(0, cy - reach);
  const int y_end = std::min(f.height - 1, cy + reach);
  const int x_begin = std::max(0, cx - reach);
  const int x_end = std::min(f.width - 1, cx + reach);
  const double inv_thickness = 1.0 / thickness;

  for (int y = y_begin; y <= y_end; ++y) {
    const double dy = y - cy;
    uint32_t* row = f.bits + static_cast<size_t>(y) * f.width;
    for (int x = x_begin; x <= x_end; ++x) {
      const double dx = x - cx;
      const double distance = std::sqrt(dx * dx + dy * dy);
      const double off = std::fabs(distance - radius) * inv_thickness;
      if (off >= 0.5) continue;
      const double edge = Clamp01((0.5 - off) * 2.0);
      const uint8_t a = static_cast<uint8_t>(alpha * edge + 0.5);
      if (a == 0) continue;
      uint32_t& pixel = row[x];
      // Overlapping rings take the brighter alpha rather than summing, which
      // keeps crossings from blowing out to opaque.
      if (a <= static_cast<uint8_t>(pixel >> 24)) continue;
      pixel = Premultiplied(kTintR, kTintG, kTintB, a);
    }
  }
}

void DrawDisc(const Frame& f, double radius, uint8_t alpha) {
  if (alpha == 0 || radius <= 0.0) return;
  const int cx = static_cast<int>(f.center.x);
  const int cy = static_cast<int>(f.center.y);
  const int reach = static_cast<int>(radius) + 1;
  for (int y = std::max(0, cy - reach);
       y <= std::min(f.height - 1, cy + reach); ++y) {
    const double dy = y - cy;
    uint32_t* row = f.bits + static_cast<size_t>(y) * f.width;
    for (int x = std::max(0, cx - reach);
         x <= std::min(f.width - 1, cx + reach); ++x) {
      const double dx = x - cx;
      if (dx * dx + dy * dy > radius * radius) continue;
      row[x] = Premultiplied(kTintR, kTintG, kTintB, alpha);
    }
  }
}

void RenderRipple(const Frame& f, int max_radius) {
  for (int k = 0; k < kRingCount; ++k) {
    const double local = f.progress * 1.35 - k * 0.16;
    if (local <= 0.0 || local >= 1.0) continue;
    const double first = 0.18 * max_radius;
    const double radius = first + (max_radius - first) * EaseOutCubic(local);
    const double fade = 1.0 - local;
    const uint8_t alpha = static_cast<uint8_t>(215.0 * fade * fade + 0.5);
    const double thickness =
        static_cast<double>(ScaleForDpi(7, f.dpi)) * (1.0 - 0.4 * local);
    DrawRing(f, radius, thickness, alpha);
  }
  // The exact position stays marked while the rings fade, so the eye can lock
  // onto the pointer instead of onto the halo.
  const uint8_t marker =
      static_cast<uint8_t>(230.0 * (1.0 - f.progress) + 0.5);
  DrawDisc(f, ScaleForDpi(3, f.dpi), marker);
  DrawRing(f, ScaleForDpi(11, f.dpi) * (1.0 - 0.35 * f.progress),
           static_cast<double>(ScaleForDpi(2, f.dpi)), marker);
}

void RenderSpotlight(const Frame& f, int hole_radius, uint8_t veil_alpha) {
  const size_t pixels = static_cast<size_t>(f.width) * f.height;
  const uint32_t veil = static_cast<uint32_t>(veil_alpha) << 24;
  for (size_t i = 0; i < pixels; ++i) f.bits[i] = veil;
  if (veil_alpha == 0) return;

  // A feathered edge is what makes it read as a spotlight instead of a punched
  // rectangle. Smoothstep keeps the ramp cheap and free of seams.
  const int edge = ScaleForDpi(34, f.dpi);
  const int reach = hole_radius + edge + 1;
  const double inner = static_cast<double>(hole_radius);
  const double span = static_cast<double>(edge > 1 ? edge : 1);
  const int cx = static_cast<int>(f.center.x);
  const int cy = static_cast<int>(f.center.y);
  for (int y = std::max(0, cy - reach);
       y <= std::min(f.height - 1, cy + reach); ++y) {
    const double dy = y - cy;
    uint32_t* row = f.bits + static_cast<size_t>(y) * f.width;
    for (int x = std::max(0, cx - reach);
         x <= std::min(f.width - 1, cx + reach); ++x) {
      const double dx = x - cx;
      const double distance = std::sqrt(dx * dx + dy * dy);
      uint32_t& pixel = row[x];
      if (distance < inner) {
        pixel = 0u;  // fully clear: the pointer is plainly visible here
        continue;
      }
      if (distance > inner + span) continue;  // untouched veil
      const double t = Clamp01((distance - inner) / span);
      const double smooth = t * t * (3.0 - 2.0 * t);
      const uint32_t a =
          static_cast<uint32_t>(static_cast<uint8_t>(veil_alpha * smooth + 0.5));
      pixel = a << 24;
    }
  }
  // A thin bright rim keeps the hole readable on dark content.
  DrawRing(f, inner, static_cast<double>(ScaleForDpi(2, f.dpi)), 90);
}

// ---------------------------------------------------------------------------
// Per-window state. Owned by the overlay thread only.
// ---------------------------------------------------------------------------
struct Context {
  FindOverlay* owner = nullptr;
  HDC screen_dc = nullptr;
  HDC mem_dc = nullptr;
  HBITMAP dib = nullptr;
  HBITMAP stock_bitmap = nullptr;  // what the DC held before our DIB
  void* bits = nullptr;
  SIZE bitmap_size = {0, 0};
  RECT window_rect = {0, 0, 0, 0};
  POINT center_screen = {0, 0};
  POINT center_local = {0, 0};
  OverlayStyle style = OverlayStyle::kNone;
  int duration_us = 900000;
  int64_t start_us = 0;
  int dpi = 96;
  int max_radius = 300;
  int last_spot_radius = -1;
  uint8_t last_veil_alpha = 0;
  bool frame_ready = false;
};

Context* FromHwnd(HWND hwnd) {
  return reinterpret_cast<Context*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void ReleaseBitmap(Context* ctx) {
  if (!ctx->dib) return;
  if (ctx->mem_dc && ctx->stock_bitmap) {
    SelectObject(ctx->mem_dc, ctx->stock_bitmap);
  }
  DeleteObject(ctx->dib);
  ctx->dib = nullptr;
  ctx->bits = nullptr;
  ctx->bitmap_size.cx = 0;
  ctx->bitmap_size.cy = 0;
}

bool EnsureBitmap(Context* ctx, int width, int height) {
  if (ctx->dib && ctx->bitmap_size.cx == width &&
      ctx->bitmap_size.cy == height) {
    return true;
  }
  ReleaseBitmap(ctx);
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // negative = top-down rows
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  ctx->dib = CreateDIBSection(ctx->screen_dc, &info, DIB_RGB_COLORS, &ctx->bits,
                              nullptr, 0);
  if (!ctx->dib) {
    LogCritical("overlay: CreateDIBSection(%d x %d) failed", width, height);
    return false;
  }
  const HBITMAP previous =
      static_cast<HBITMAP>(SelectObject(ctx->mem_dc, ctx->dib));
  if (!ctx->stock_bitmap) ctx->stock_bitmap = previous;
  ctx->bitmap_size.cx = width;
  ctx->bitmap_size.cy = height;
  return true;
}

// Ring and hole sizes follow the system DPI. Per-monitor DPI would be nicer on
// a mixed-DPI desktop (GetDpiForWindow exists from Windows 10 1607), but reaching
// it means casting a FARPROC to a typed function pointer, which is a /W4
// warning in a project built with /WX; the effect is only cosmetic here, so the
// universally available system value is used.
int QueryDpi(HDC dc) {
  const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
  return dpi > 0 ? dpi : 96;
}

void Present(Context* ctx, HWND hwnd) {
  BLENDFUNCTION blend = {};
  blend.BlendOp = AC_SRC_OVER;
  blend.BlendFlags = 0;
  blend.SourceConstantAlpha = 255;  // per-pixel alpha carries everything
  blend.AlphaFormat = AC_SRC_ALPHA;
  POINT source = {0, 0};
  SIZE size = ctx->bitmap_size;
  POINT target = {ctx->window_rect.left, ctx->window_rect.top};
  if (!UpdateLayeredWindow(hwnd, ctx->screen_dc, &target, &size, ctx->mem_dc,
                           &source, 0, &blend, ULW_ALPHA)) {
    Logf("overlay: UpdateLayeredWindow failed (%lu)", GetLastError());
  }
}

void ComputeGeometry(Context* ctx) {
  RECT area = {0, 0, 1280, 720};
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  const HMONITOR monitor =
      MonitorFromPoint(ctx->center_screen, MONITOR_DEFAULTTONEAREST);
  if (GetMonitorInfo(monitor, &info)) area = info.rcMonitor;

  ctx->max_radius = ScaleForDpi(300, ctx->dpi);

  if (ctx->style == OverlayStyle::kSpotlight) {
    ctx->window_rect = area;
  } else {
    const int reach = ctx->max_radius + ScaleForDpi(40, ctx->dpi);
    RECT rect = {ctx->center_screen.x - reach, ctx->center_screen.y - reach,
                 ctx->center_screen.x + reach, ctx->center_screen.y + reach};
    // Slide the surface back inside the monitor near screen edges so the rings
    // are not chopped off; the centre is recomputed from the final rect.
    if (rect.left < area.left) {
      rect.right += area.left - rect.left;
      rect.left = area.left;
    }
    if (rect.top < area.top) {
      rect.bottom += area.top - rect.top;
      rect.top = area.top;
    }
    if (rect.right > area.right) {
      rect.left -= rect.right - area.right;
      rect.right = area.right;
    }
    if (rect.bottom > area.bottom) {
      rect.top -= rect.bottom - area.bottom;
      rect.bottom = area.bottom;
    }
    // A surface bigger than the monitor (very high zoom) clamps from the left.
    if (rect.left < area.left) rect.left = area.left;
    if (rect.top < area.top) rect.top = area.top;
    ctx->window_rect = rect;
  }
  ctx->center_local.x = ctx->center_screen.x - ctx->window_rect.left;
  ctx->center_local.y = ctx->center_screen.y - ctx->window_rect.top;
}

void StopAnimation(HWND hwnd, Context* ctx) {
  KillTimer(hwnd, kAnimTimer);
  ShowWindow(hwnd, SW_HIDE);
  ctx->frame_ready = false;
  ctx->last_spot_radius = -1;
  ctx->last_veil_alpha = 0;
}

void RenderOnce(HWND hwnd, Context* ctx) {
  const int width = ctx->window_rect.right - ctx->window_rect.left;
  const int height = ctx->window_rect.bottom - ctx->window_rect.top;
  if (width <= 0 || height <= 0) return;
  if (!EnsureBitmap(ctx, width, height)) return;

  const int64_t elapsed = NowUs() - ctx->start_us;
  const double progress = Clamp01(static_cast<double>(elapsed) /
                                  static_cast<double>(ctx->duration_us));

  if (ctx->style == OverlayStyle::kSpotlight) {
    const double full_hole = static_cast<double>(ScaleForDpi(120, ctx->dpi));
    const int hole = static_cast<int>(
        std::max(8.0, full_hole * EaseOutCubic(Clamp01(progress / 0.30))));
    const double fade = progress < 0.75 ? 1.0 : 1.0 - (progress - 0.75) / 0.25;
    const uint8_t veil = static_cast<uint8_t>(165.0 * fade + 0.5);
    if (hole == ctx->last_spot_radius && veil == ctx->last_veil_alpha &&
        ctx->frame_ready) {
      return;  // hold phase: nothing moved, skip the upload entirely
    }
    ctx->last_spot_radius = hole;
    ctx->last_veil_alpha = veil;

    Frame frame;
    frame.bits = static_cast<uint32_t*>(ctx->bits);
    frame.width = width;
    frame.height = height;
    frame.center = ctx->center_local;
    frame.dpi = ctx->dpi;
    frame.progress = progress;
    RenderSpotlight(frame, hole, veil);
  } else {
    // Clear to fully transparent, then draw. A 640x640 surface is ~0.2 ms.
    std::memset(ctx->bits, 0,
                static_cast<size_t>(width) * height * sizeof(uint32_t));
    Frame frame;
    frame.bits = static_cast<uint32_t*>(ctx->bits);
    frame.width = width;
    frame.height = height;
    frame.center = ctx->center_local;
    frame.dpi = ctx->dpi;
    frame.progress = progress;
    RenderRipple(frame, ctx->max_radius);
  }
  ctx->frame_ready = true;
  Present(ctx, hwnd);
}

void HandleRequest(HWND hwnd) {
  Context* ctx = FromHwnd(hwnd);
  if (!ctx || !ctx->owner) return;

  const FindOverlay::Request request = ctx->owner->TakePending();
  if (request.hide) {
    StopAnimation(hwnd, ctx);
    return;
  }
  if (!request.valid) return;

  ctx->center_screen = request.center;
  ctx->style = request.style;
  ctx->duration_us = std::max(150000, request.duration_ms * 1000);
  ctx->start_us = NowUs();
  ctx->frame_ready = false;
  ctx->last_spot_radius = -1;
  ctx->last_veil_alpha = 0;

  if (!ctx->screen_dc) ctx->screen_dc = GetDC(nullptr);
  ctx->dpi = QueryDpi(ctx->screen_dc);

  ComputeGeometry(ctx);
  SetWindowPos(hwnd, HWND_TOPMOST, ctx->window_rect.left, ctx->window_rect.top,
               ctx->window_rect.right - ctx->window_rect.left,
               ctx->window_rect.bottom - ctx->window_rect.top,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  RenderOnce(hwnd, ctx);
  SetTimer(hwnd, kAnimTimer,
           ctx->style == OverlayStyle::kSpotlight ? kSpotlightFrameMs
                                                   : kRippleFrameMs,
           nullptr);
}

}  // namespace

// --- FindOverlay ------------------------------------------------------------

FindOverlay::~FindOverlay() {
  Shutdown();
  DeleteCriticalSection(&lock_);
}

void FindOverlay::SetPending(const Request& request) {
  EnterCriticalSection(&lock_);
  pending_ = request;
  LeaveCriticalSection(&lock_);
}

FindOverlay::Request FindOverlay::TakePending() {
  Request copy;
  EnterCriticalSection(&lock_);
  copy = pending_;
  pending_ = Request();
  LeaveCriticalSection(&lock_);
  return copy;
}

void FindOverlay::StartThreadIfNeeded() {
  if (thread_) return;
  thread_ = CreateThread(nullptr, 0, &FindOverlay::ThreadEntry, this, 0, nullptr);
  if (!thread_) LogCritical("overlay: could not start the render thread");
}

LRESULT CALLBACK FindOverlay::WndProc(HWND hwnd, UINT message, WPARAM w_param,
                                      LPARAM l_param) {
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return DefWindowProcW(hwnd, message, w_param, l_param);
  }

  Context* ctx = FromHwnd(hwnd);
  switch (message) {
    case kMsgRequest:
      HandleRequest(hwnd);
      return 0;
    case WM_TIMER:
      if (w_param == kAnimTimer && ctx) {
        RenderOnce(hwnd, ctx);
        if (NowUs() - ctx->start_us >= ctx->duration_us) {
          StopAnimation(hwnd, ctx);
          if (ctx->owner) ctx->owner->MarkInactive();
        }
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;  // the layered surface is the only thing that shows
    case WM_DESTROY:
      if (ctx) {
        StopAnimation(hwnd, ctx);
        ReleaseBitmap(ctx);
        if (ctx->mem_dc) DeleteDC(ctx->mem_dc);
        if (ctx->screen_dc) ReleaseDC(nullptr, ctx->screen_dc);
        EnterCriticalSection(ctx->owner->lock());
        ctx->owner->ClearHwndLocked();
        LeaveCriticalSection(ctx->owner->lock());
        delete ctx;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      }
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, w_param, l_param);
}

DWORD WINAPI FindOverlay::ThreadEntry(LPVOID param) {
  auto* self = static_cast<FindOverlay*>(param);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &FindOverlay::WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClassName;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    LogCritical("overlay: RegisterClassExW failed (%lu)", GetLastError());
    return 1;
  }

  Context* ctx = new (std::nothrow) Context();
  if (!ctx) return 1;
  ctx->owner = self;
  ctx->mem_dc = CreateCompatibleDC(nullptr);

  // A 1x1 surface parked off screen: the real geometry is set per show, so no
  // full-screen layered window exists while nothing is happening.
  HWND hwnd = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
          WS_EX_NOACTIVATE,
      kClassName, L"", WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr,
      wc.hInstance, ctx);
  if (!hwnd) {
    LogCritical("overlay: CreateWindowExW failed (%lu)", GetLastError());
    if (ctx->mem_dc) DeleteDC(ctx->mem_dc);
    delete ctx;
    return 1;
  }

  bool has_pending = false;
  EnterCriticalSection(&self->lock_);
  self->hwnd_ = hwnd;
  has_pending = self->pending_.valid || self->pending_.hide;
  LeaveCriticalSection(&self->lock_);
  if (has_pending) PostMessageW(hwnd, kMsgRequest, 0, 0);

  MSG msg = {};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}

void FindOverlay::Show(POINT center, OverlayStyle style, int duration_ms) {
  if (style == OverlayStyle::kNone) return;
  Request request;
  request.center = center;
  request.style = style;
  request.duration_ms = duration_ms;
  request.valid = true;
  SetPending(request);
  active_ = true;
  StartThreadIfNeeded();
  EnterCriticalSection(&lock_);
  HWND target = hwnd_;
  LeaveCriticalSection(&lock_);
  if (target) PostMessageW(target, kMsgRequest, 0, 0);
}

void FindOverlay::Hide() {
  Request request;
  request.hide = true;
  SetPending(request);
  active_ = false;
  EnterCriticalSection(&lock_);
  HWND target = hwnd_;
  LeaveCriticalSection(&lock_);
  if (target) PostMessageW(target, kMsgRequest, 0, 0);
}

void FindOverlay::Shutdown() {
  if (!thread_) return;
  HWND target = nullptr;
  EnterCriticalSection(&lock_);
  target = hwnd_;
  LeaveCriticalSection(&lock_);
  if (target) {
    PostMessageW(target, WM_CLOSE, 0, 0);
  } else {
    PostThreadMessageW(GetThreadId(thread_), WM_QUIT, 0, 0);
  }
  WaitForSingleObject(thread_, 2000);
  CloseHandle(thread_);
  thread_ = nullptr;
  active_ = false;
}

}  // namespace stfc
