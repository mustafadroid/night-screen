#include "Gamma.hpp"

#include "XThreads.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // keep windows.h min/max macros away from std::min/max
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <vector>
#else
#include <X11/Xlib.h>
#if !defined(NIGHTSCREEN_NO_XRANDR)
#include <X11/extensions/Xrandr.h>
#endif
#include <cstring>
#include <mutex>
#include <vector>
#endif

namespace nightscreen {
namespace {

// Swallow async X11 protocol errors (stale windows, compositor races) so the
// daemon never dies because of display chatter.
#if !defined(_WIN32) && !defined(__APPLE__)
int xIgnoreError(Display*, XErrorEvent*) { return 0; }
#endif

#if defined(_WIN32)

class WinGamma final : public GammaController {
public:
  WinGamma() {
    WORD tmp[3][256];
    HDC dc = GetDC(nullptr);
    if (dc) {
      if (GetDeviceGammaRamp(dc, tmp)) {
        memcpy(saved_, tmp, sizeof(saved_));
        haveSaved_ = true;
      }
      ReleaseDC(nullptr, dc);
    }
  }
  bool apply(const GammaRamps& r) override {
    // Apply to all display DCs.
    bool ok = false;
    DISPLAY_DEVICEA dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
      if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
      HDC dc = CreateDCA(dd.DeviceName, nullptr, nullptr, nullptr);
      if (!dc) continue;
      WORD ramp[3][256];
      for (int k = 0; k < 256; ++k) {
        ramp[0][k] = r.r[k];
        ramp[1][k] = r.g[k];
        ramp[2][k] = r.b[k];
      }
      if (SetDeviceGammaRamp(dc, ramp)) ok = true;
      DeleteDC(dc);
    }
    // Fallback: primary display DC.
    if (!ok) {
      HDC dc = GetDC(nullptr);
      if (dc) {
        WORD ramp[3][256];
        for (int k = 0; k < 256; ++k) {
          ramp[0][k] = r.r[k];
          ramp[1][k] = r.g[k];
          ramp[2][k] = r.b[k];
        }
        ok = SetDeviceGammaRamp(dc, ramp);
        ReleaseDC(nullptr, dc);
      }
    }
    available_ = ok || available_;
    return ok;
  }
  void reset() override {
    if (!haveSaved_) {
      GammaRamps ident;
      for (int i = 0; i < 256; ++i) {
        unsigned short v = static_cast<unsigned short>((i * 65535) / 255);
        ident.r[i] = ident.g[i] = ident.b[i] = v;
      }
      apply(ident);
      return;
    }
    DISPLAY_DEVICEA dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
      if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
      HDC dc = CreateDCA(dd.DeviceName, nullptr, nullptr, nullptr);
      if (!dc) continue;
      SetDeviceGammaRamp(dc, saved_);
      DeleteDC(dc);
    }
  }
  bool available() const override { return true; } // Win32 GDI always present
  std::string backendName() const override { return "windows-gdi-gamma"; }

private:
  WORD saved_[3][256]{};
  bool haveSaved_ = false;
  bool available_ = false;
};

#elif defined(__APPLE__)

class MacGamma final : public GammaController {
public:
  MacGamma() {
    uint32_t count = 0;
    CGGetActiveDisplayList(0, nullptr, &count);
    displays_.resize(count);
    CGGetActiveDisplayList(count, displays_.data(), &count);
    // Save original tables (assume identity if query fails).
    saved_.resize(displays_.size());
    for (size_t i = 0; i < displays_.size(); ++i) {
      saved_[i].fill(0);
      uint32_t n = 256;
      CGGammaValue rr[256], gg[256], bb[256];
      if (CGGetDisplayTransferByTable(displays_[i], 256, rr, gg, bb, &n) == CGDisplayNoErr) {
        for (uint32_t k = 0; k < n && k < 256; ++k) {
          saved_[i][k * 3 + 0] = rr[k];
          saved_[i][k * 3 + 1] = gg[k];
          saved_[i][k * 3 + 2] = bb[k];
        }
        haveSaved_ = true;
      }
    }
  }
  bool apply(const GammaRamps& r) override {
    if (displays_.empty()) return false;
    CGGammaValue rr[256], gg[256], bb[256];
    for (int i = 0; i < 256; ++i) {
      rr[i] = r.r[i] / 65535.0f;
      gg[i] = r.g[i] / 65535.0f;
      bb[i] = r.b[i] / 65535.0f;
    }
    bool ok = false;
    for (auto d : displays_) {
      if (CGSetDisplayTransferByTable(d, 256, rr, gg, bb) == CGDisplayNoErr) ok = true;
    }
    return ok;
  }
  void reset() override {
    if (haveSaved_) {
      for (size_t i = 0; i < displays_.size(); ++i) {
        CGGammaValue rr[256], gg[256], bb[256];
        for (int k = 0; k < 256; ++k) {
          rr[k] = saved_[i][k * 3 + 0];
          gg[k] = saved_[i][k * 3 + 1];
          bb[k] = saved_[i][k * 3 + 2];
        }
        CGSetDisplayTransferByTable(displays_[i], 256, rr, gg, bb);
      }
    } else {
      for (auto d : displays_) CGDisplayRestoreColorSyncSettings();
    }
  }
  bool available() const override { return !displays_.empty(); }
  std::string backendName() const override { return "macos-coregraphics-gamma"; }

private:
  std::vector<CGDirectDisplayID> displays_;
  std::vector<std::array<CGGammaValue, 768>> saved_;
  bool haveSaved_ = false;
};

#else // Linux

class LinuxGamma final : public GammaController {
public:
  LinuxGamma() {
    ensureXThreads();
    const char* d = getenv("DISPLAY");
    if (!d || !*d) return; // headless / Wayland-only session
    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_) return;
    XSetErrorHandler(xIgnoreError); // never die on display chatter
#if !defined(NIGHTSCREEN_NO_XRANDR)
    int ev = 0, err = 0;
    hasRandr_ = XRRQueryExtension(dpy_, &ev, &err);
#endif
  }
  ~LinuxGamma() override {
    if (dpy_) XCloseDisplay(dpy_);
  }
  bool apply(const GammaRamps& r) override {
    std::lock_guard<std::mutex> lk(m_);
    if (!dpy_) return false;
#if !defined(NIGHTSCREEN_NO_XRANDR)
    if (hasRandr_) {
      bool ok = false;
      Window root = DefaultRootWindow(dpy_);
      XRRScreenResources* res = XRRGetScreenResources(dpy_, root);
      if (res) {
        if (orig_.empty()) {
          // Save originals once.
          orig_.resize(res->ncrtc);
          for (int i = 0; i < res->ncrtc; ++i) {
            int sz = XRRGetCrtcGammaSize(dpy_, res->crtcs[i]);
            orig_[i].resize(static_cast<size_t>(sz) * 3, 0);
            if (sz > 0) {
              XRRCrtcGamma* g = XRRGetCrtcGamma(dpy_, res->crtcs[i]);
              if (g) {
                for (int k = 0; k < sz; ++k) {
                  orig_[i][k * 3 + 0] = g->red[k];
                  orig_[i][k * 3 + 1] = g->green[k];
                  orig_[i][k * 3 + 2] = g->blue[k];
                }
                XRRFreeGamma(g);
              }
            }
          }
        }
        for (int i = 0; i < res->ncrtc; ++i) {
          int sz = XRRGetCrtcGammaSize(dpy_, res->crtcs[i]);
          if (sz <= 0) continue;
          XRRCrtcGamma* g = XRRAllocGamma(sz);
          if (!g) continue;
          for (int k = 0; k < sz; ++k) {
            // resample 256-entry ramp to crtc size
            int src = (k * 255) / (sz - 1 > 0 ? sz - 1 : 1);
            g->red[k] = r.r[src];
            g->green[k] = r.g[src];
            g->blue[k] = r.b[src];
          }
          XRRSetCrtcGamma(dpy_, res->crtcs[i], g);
          XRRFreeGamma(g);
          ok = true;
        }
        XRRFreeScreenResources(res);
        XFlush(dpy_);
        return ok;
      }
    }
#endif
    return false;
  }
  void reset() override {
    std::lock_guard<std::mutex> lk(m_);
    if (!dpy_) return;
#if !defined(NIGHTSCREEN_NO_XRANDR)
    if (hasRandr_ && !orig_.empty()) {
      Window root = DefaultRootWindow(dpy_);
      XRRScreenResources* res = XRRGetScreenResources(dpy_, root);
      if (res) {
        for (int i = 0; i < res->ncrtc && i < (int)orig_.size(); ++i) {
          int sz = XRRGetCrtcGammaSize(dpy_, res->crtcs[i]);
          if (sz <= 0) continue;
          XRRCrtcGamma* g = XRRAllocGamma(sz);
          if (!g) continue;
          for (int k = 0; k < sz; ++k) {
            int src = (k * 255) / (sz - 1 > 0 ? sz - 1 : 1);
            // orig_ stored at native crtc size? we stored resampled; restore best-effort
            size_t n = orig_[i].size() / 3;
            size_t s = n > 0 ? (static_cast<size_t>(k) * (n - 1)) / static_cast<size_t>(sz > 1 ? sz - 1 : 1) : 0;
            g->red[k] = orig_[i][s * 3 + 0];
            g->green[k] = orig_[i][s * 3 + 1];
            g->blue[k] = orig_[i][s * 3 + 2];
            (void)src;
          }
          XRRSetCrtcGamma(dpy_, res->crtcs[i], g);
          XRRFreeGamma(g);
        }
        XRRFreeScreenResources(res);
        XFlush(dpy_);
      }
    }
#endif
  }
  bool available() const override { return dpy_ != nullptr; }
  std::string backendName() const override {
#if defined(NIGHTSCREEN_NO_XRANDR)
    return "linux-x11-norandr";
#else
    return "linux-x11-xrandr";
#endif
  }

private:
  Display* dpy_ = nullptr;
  bool hasRandr_ = false;
  std::vector<std::vector<unsigned short>> orig_;
  std::mutex m_;
};

#endif

} // namespace

std::unique_ptr<GammaController> makeGammaController() {
#if defined(_WIN32)
  return std::make_unique<WinGamma>();
#elif defined(__APPLE__)
  return std::make_unique<MacGamma>();
#else
  return std::make_unique<LinuxGamma>();
#endif
}

} // namespace nightscreen
