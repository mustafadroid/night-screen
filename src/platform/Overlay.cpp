#include "Overlay.hpp"

#include "XThreads.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // keep windows.h min/max macros away from std::min/max
#include <windows.h>
#elif defined(__linux__)
#include <X11/Xatom.h>
#include <X11/Xlib.h>

// Swallow async protocol errors (stale windows, compositor races): the daemon
// must never die because of display chatter. Fatal I/O death (server gone)
// still terminates that Display's owner, as Xlib requires.
int xIgnoreError(Display*, XErrorEvent*) { return 0; }
#endif

namespace nightscreen {
namespace {

// Current local time as "HH:MM:SS".
std::string clockText() {
  using namespace std::chrono;
  std::time_t t = system_clock::to_time_t(system_clock::now());
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", local.tm_hour, local.tm_min,
           local.tm_sec);
  return buf;
}

} // namespace

struct Overlay::Impl {
  std::mutex mu;
  enum class Mode { Hidden, Dim, Break, Oled } mode = Mode::Hidden;
  double alpha = 0.0; // dim/break alpha
  int moveSec = 60;
  std::function<void()> onActivity;
  std::chrono::steady_clock::time_point oledStart{};
#if defined(_WIN32)
  HWND hwnd = nullptr;
  HDC memDC = nullptr;
  HBITMAP bmp = nullptr;
  int bw = 0, bh = 0;
  HBRUSH black = nullptr;
  HFONT font = nullptr;
  int tx = 100, ty = 100; // counter position (worker-thread local use)
  std::chrono::steady_clock::time_point lastMove{};
  std::jthread thread;
  void ensureThread();
  inline static Impl* s_active = nullptr; // overlay instance owning the window
  static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Impl* im = s_active;
    if (im && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_LBUTTONDOWN ||
               m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN || m == WM_MOUSEMOVE)) {
      // Ignore the first moments (the click that summoned the guard).
      auto now = std::chrono::steady_clock::now();
      Mode mode = Mode::Hidden;
      std::function<void()> cb;
      std::chrono::steady_clock::time_point start{};
      {
        std::lock_guard<std::mutex> lk(im->mu);
        mode = im->mode;
        cb = im->onActivity;
        start = im->oledStart;
      }
      auto age =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
      if (mode == Mode::Oled && age > 1500 && cb) {
        std::thread([cb] { cb(); }).detach();
        return 0;
      }
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT; // dim mode stays click-through
    return DefWindowProcA(h, m, w, l);
  }
#elif defined(__linux__)
  Display* dpy = nullptr;
  ::Window win = 0;
  GC gc = nullptr;
  XFontStruct* font = nullptr;
  unsigned long grayPixel = 0;
  std::jthread thread;
  void ensureThread();
#endif
};

#if defined(_WIN32)

void Overlay::Impl::ensureThread() {
  if (thread.joinable()) return;
  s_active = this;
  thread = std::jthread([this](std::stop_token tok) {
    const char* cls = "NightScreenOverlay";
    WNDCLASSA wc{};
    wc.lpfnWndProc = Impl::WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = cls;
    RegisterClassA(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                               WS_EX_TOOLWINDOW,
                           cls, "night-screen overlay", WS_POPUP, 0, 0, sw, sh, nullptr,
                           nullptr, wc.hInstance, nullptr);
    if (!hwnd) return;
    HDC screenDC = GetDC(nullptr);
    memDC = CreateCompatibleDC(screenDC);
    black = CreateSolidBrush(RGB(0, 0, 0));
    font = CreateFontA(56, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, "Segoe UI");
    std::mt19937 rng(std::random_device{}());

    auto ensureBmp = [&](int w, int h) {
      if (bmp && w == bw && h == bh) return;
      if (bmp) DeleteObject(bmp);
      BITMAPINFO bi{};
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = w;
      bi.bmiHeader.biHeight = -h;
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;
      void* bits = nullptr;
      bmp = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
      bw = w;
      bh = h;
      SelectObject(memDC, bmp);
    };

    MSG msg;
    std::string lastText;
    while (!tok.stop_requested()) {
      while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
      }
      int nsw = GetSystemMetrics(SM_CXSCREEN), nsh = GetSystemMetrics(SM_CYSCREEN);
      Mode mode;
      double a;
      int mv;
      {
        std::lock_guard<std::mutex> lk(mu);
        mode = this->mode;
        a = alpha;
        mv = moveSec;
      }
      if (mode == Mode::Hidden) {
        ShowWindow(hwnd, SW_HIDE);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      ensureBmp(nsw, nsh);
      // click-through for dim, capturing for OLED
      LONG_PTR ex = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
      bool wantTransparent = (mode != Mode::Oled);
      bool isTransparent = (ex & WS_EX_TRANSPARENT) != 0;
      if (wantTransparent != isTransparent) {
        if (wantTransparent)
          ex |= WS_EX_TRANSPARENT;
        else
          ex &= ~WS_EX_TRANSPARENT;
        SetWindowLongPtrA(hwnd, GWL_EXSTYLE, ex);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
      }
      RECT rc{0, 0, nsw, nsh};
      FillRect(memDC, &rc, black);
      SIZE sz{nsw, nsh};
      POINT src{0, 0}, dst{0, 0};
      BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
      DWORD flags = ULW_ALPHA;
      if (mode == Mode::Oled) {
        // relocate counter every mv seconds
        auto now = std::chrono::steady_clock::now();
        if (lastText.empty() ||
            std::chrono::duration_cast<std::chrono::seconds>(now - lastMove).count() >=
                mv) {
          lastMove = now;
          std::uniform_int_distribution<int> dx(40, std::max(41, nsw - 320));
          std::uniform_int_distribution<int> dy(60, std::max(61, nsh - 120));
          tx = dx(rng);
          ty = dy(rng);
        }
        std::string text = clockText() + "  burn-in guard";
        SelectObject(memDC, font);
        SetTextColor(memDC, RGB(130, 130, 130));
        SetBkMode(memDC, TRANSPARENT);
        TextOutA(memDC, tx, ty, text.c_str(), (int)text.size());
        flags = ULW_OPAQUE; // alpha channel untouched by GDI: go fully opaque
      } else {
        bf.SourceConstantAlpha = (BYTE)std::lround(std::clamp(a, 0.0, 0.9) * 255);
      }
      UpdateLayeredWindow(hwnd, screenDC, &dst, &sz, memDC, &src, 0, &bf, flags);
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
      SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, nsw, nsh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
      std::this_thread::sleep_for(std::chrono::milliseconds(mode == Mode::Oled ? 500 : 200));
    }
    if (bmp) DeleteObject(bmp);
    if (black) DeleteObject(black);
    if (font) DeleteObject(font);
    if (memDC) DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    DestroyWindow(hwnd);
    hwnd = nullptr;
    s_active = nullptr;
  });
}

#elif defined(__linux__)

void Overlay::Impl::ensureThread() {
  if (thread.joinable()) return;
  thread = std::jthread([this](std::stop_token tok) {
    ensureXThreads();
    dpy = XOpenDisplay(nullptr);
    if (!dpy) return;
    XSetErrorHandler(xIgnoreError);
    int scr = DefaultScreen(dpy);
    int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
    ::Window root = RootWindow(dpy, scr);
    XSetWindowAttributes attrs{};
    attrs.override_redirect = True;
    attrs.background_pixel = BlackPixel(dpy, scr);
    attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | PointerMotionMask;
    win = XCreateWindow(dpy, root, 0, 0, sw, sh, 0, CopyFromParent, InputOutput,
                        CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask,
                        &attrs);
    Atom opacity = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
    Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    Atom fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    Atom props[2] = {above, fullscreen};
    XChangeProperty(dpy, win, state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)props, 2);
    gc = XCreateGC(dpy, win, 0, nullptr);
    font = XLoadQueryFont(dpy, "9x15bold");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (font) XSetFont(dpy, gc, font->fid);
    // dim gray for the counter
    grayPixel = WhitePixel(dpy, scr);
    {
      XColor exact{}, alloc{};
      if (XAllocNamedColor(dpy, DefaultColormap(dpy, scr), "gray50", &alloc, &exact))
        grayPixel = alloc.pixel;
    }
    std::mt19937 rng(std::random_device{}());
    bool mapped = false;
    // Worker-local render state (only this thread touches these).
    int tx = 100, ty = 100;
    auto lastMove = std::chrono::steady_clock::now() - std::chrono::hours(1);
    auto lastTick = std::chrono::steady_clock::now() - std::chrono::hours(1);
    Impl::Mode prevMode = Impl::Mode::Hidden;
    auto oledStartCopy = std::chrono::steady_clock::now();
    auto setOpacity = [&](double a) {
      unsigned long op = (unsigned long)(std::clamp(a, 0.0, 1.0) * 0xffffffffULL);
      XChangeProperty(dpy, win, opacity, XA_CARDINAL, 32, PropModeReplace,
                      (unsigned char*)&op, 1);
    };
    while (!tok.stop_requested()) {
      // drain input -> activity exit
      while (XPending(dpy)) {
        XEvent ev{};
        XNextEvent(dpy, &ev);
        if (ev.type == Expose) {
          // redraw below
        } else if (ev.type == KeyPress || ev.type == ButtonPress ||
                   ev.type == MotionNotify) {
          auto now = std::chrono::steady_clock::now();
          auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - oledStartCopy).count();
          Mode mode;
          std::function<void()> cb;
          {
            std::lock_guard<std::mutex> lk(mu);
            mode = this->mode;
            cb = onActivity;
            oledStartCopy = oledStart;
          }
          if (mode == Mode::Oled && age > 1500 && cb) {
            std::thread([cb] { cb(); }).detach();
          }
          // swallow motion floods
          while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            if (ev.type == KeyPress || ev.type == ButtonPress) break;
          }
        }
      }
      Mode mode;
      double a;
      int mv;
      {
        std::lock_guard<std::mutex> lk(mu);
        mode = this->mode;
        a = alpha;
        mv = moveSec;
      }
      if (mode == Mode::Hidden) {
        if (mapped) {
          XUnmapWindow(dpy, win);
          mapped = false;
        }
        XFlush(dpy);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      if (!mapped) {
        XMapRaised(dpy, win);
        mapped = true;
      }
      if (mode == Mode::Oled) {
        setOpacity(1.0);
        auto now = std::chrono::steady_clock::now();
        bool entered = (prevMode != Mode::Oled);
        bool relocate = entered ||
                        std::chrono::duration_cast<std::chrono::seconds>(now - lastMove).count() >= mv;
        bool tick = entered ||
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick).count() >= 1000;
        if (relocate) {
          lastMove = now;
          std::uniform_int_distribution<int> dx(30, std::max(31, sw - 300));
          std::uniform_int_distribution<int> dy(40, std::max(41, sh - 40));
          tx = dx(rng);
          ty = dy(rng);
        }
        if (tick || relocate) {
          lastTick = now;
          XClearWindow(dpy, win);
          if (font) {
            XSetForeground(dpy, gc, grayPixel);
            std::string text = clockText() + "  burn-in guard";
            XDrawString(dpy, win, gc, tx, ty, text.c_str(), (int)text.size());
          }
        }
        prevMode = mode;
      } else {
        setOpacity(a);
        prevMode = mode;
      }
      XFlush(dpy);
      std::this_thread::sleep_for(std::chrono::milliseconds(mode == Mode::Oled ? 250 : 300));
    }
    if (font) XFreeFont(dpy, font);
    if (gc) XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    dpy = nullptr;
  });
}

#endif

Overlay::Overlay() : impl_(new Impl()) {}
Overlay::~Overlay() {
  hide();
#if defined(_WIN32) || defined(__linux__)
  if (impl_->thread.joinable()) {
    impl_->thread.request_stop();
    impl_->thread.join();
  }
#endif
  delete impl_;
}

void Overlay::setDim(double alpha) {
  if (alpha < 0) alpha = 0;
  if (alpha > 0.9) alpha = 0.9;
#if defined(_WIN32) || defined(__linux__)
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (alpha > 0.005 && impl_->mode != Impl::Mode::Oled) {
      impl_->mode = Impl::Mode::Dim;
      impl_->alpha = alpha;
    } else if (impl_->mode == Impl::Mode::Dim) {
      impl_->mode = Impl::Mode::Hidden;
    }
    // Dim never overrides OLED or break lock.
  }
  if (alpha > 0.005) impl_->ensureThread();
#else
  (void)alpha; // macOS: gamma-only in this release
#endif
}

void Overlay::setBreakMessage(const char* text) {
  if (!text || !*text) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->mode == Impl::Mode::Break) impl_->mode = Impl::Mode::Hidden;
    return;
  }
#if defined(_WIN32) || defined(__linux__)
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->mode != Impl::Mode::Oled) {
      impl_->mode = Impl::Mode::Break;
      impl_->alpha = 0.85;
    }
  }
  impl_->ensureThread();
#else
  (void)text;
#endif
}

void Overlay::setOled(bool on, int moveSeconds) {
#if defined(_WIN32) || defined(__linux__)
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (on) {
      impl_->mode = Impl::Mode::Oled;
      impl_->moveSec = std::clamp(moveSeconds, 10, 600);
      impl_->oledStart = std::chrono::steady_clock::now();
    } else if (impl_->mode == Impl::Mode::Oled) {
      impl_->mode = Impl::Mode::Hidden;
    }
  }
  if (on) impl_->ensureThread();
#else
  (void)on;
  (void)moveSeconds; // macOS: gamma-only in this release
#endif
}

bool Overlay::oledActive() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->mode == Impl::Mode::Oled;
}

void Overlay::setOnActivity(std::function<void()> cb) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->onActivity = std::move(cb);
}

void Overlay::hide() {
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->mode = Impl::Mode::Hidden;
  impl_->alpha = 0.0;
}

} // namespace nightscreen
