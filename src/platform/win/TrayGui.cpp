#include "TrayGui.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // keep windows.h min/max macros away from std::min/max
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace nightscreen {
namespace {

// ---- message ids / control ids ----
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_BALLOON_Q = WM_APP + 2;
constexpr UINT WM_TRAY_SHUTDOWN = WM_APP + 3;

enum : int {
  IDM_OPEN = 100,
  IDM_ENABLE,
  IDM_BREAK_NOW,
  IDM_SNOOZE,
  IDM_AUTOSTART,
  IDM_QUIT,
  IDM_OLED = 107,
  IDM_PRESET_BASE = 200, // + index into builtinPresets()
};

enum : int {
  IDC_TEMP = 1000,
  IDC_BRIGHT,
  IDC_TEMPVAL,
  IDC_BRIGHTVAL,
  IDC_ENABLECHK,
  IDC_TOPMOST,
  IDC_MODE,
  IDC_ON,
  IDC_OFF,
  IDC_WORK,
  IDC_REST,
  IDC_BRKEN,
  IDC_STRICT,
  IDC_BREAKNOW,
  IDC_SNOOZEBTN,
  IDC_WEB,
  IDC_OLED,
  IDC_OLEDMOVE,
  IDC_PRESET_BASE = 1200, // + preset index (max 8)
};

constexpr char kTrayClass[] = "NightScreenTray";
constexpr char kPanelClass[] = "NightScreenPanel";
constexpr UINT kRefreshTimer = 1;

struct Balloon {
  char title[64];
  char body[256];
};

struct UiState {
  HWND trayHwnd = nullptr;
  HWND panelHwnd = nullptr;
  HICON icon = nullptr;
  NOTIFYICONDATAA nid{};
  HWND hTemp = nullptr, hBright = nullptr, hTempVal = nullptr, hBrightVal = nullptr;
  HWND hEnable = nullptr, hTopmost = nullptr, hMode = nullptr;
  HWND hOn = nullptr, hOff = nullptr;
  HWND hBrkEn = nullptr, hStrict = nullptr, hWork = nullptr, hRest = nullptr;
  HWND hOled = nullptr, hOledMove = nullptr;
  HWND hPreset[8]{};
  int presetCount = 0;
};

UiState g_ui;
std::mutex g_mu;
TrayGui* g_owner = nullptr; // set while the UI thread runs

// ---- helpers (forward decls) ----
Settings snapOf();
void updateVia(std::function<void(Settings&)> fn, bool save);
void breakNowVia();
void snoozeVia(int minutes);
void refreshPanel();
void applyFromPanel(bool save);
void applyOled(bool save);
void showPanel(bool show);
void doOpenWeb();
void trayCommand(int cmd);
void popupMenu();

// ---- runtime-generated crescent-moon tray icon (no .ico asset needed) ----
// Prefers the embedded exe icon (resource 1); falls back to a drawn moon so
// the tray always has an icon even if resources were skipped at build time.
HICON makeMoonIcon() {
  if (HICON res = LoadIconA(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(1))) return res;
  const int S = 32;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = S;
  bi.bmiHeader.biHeight = -S; // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP hColor = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!hColor || !bits) return LoadIcon(nullptr, IDI_APPLICATION);
  auto* px = static_cast<unsigned*>(bits);
  unsigned char andMask[32 * 4];
  memset(andMask, 0xFF, sizeof(andMask)); // 1 bit = transparent
  const double cx = 14.0, cy = 15.0, r = 10.5; // main disc
  const double sx = 19.0, sy = 11.0, sr = 8.5; // cut-out -> crescent
  for (int y = 0; y < S; ++y) {
    for (int x = 0; x < S; ++x) {
      double d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
      double s = (x - sx) * (x - sx) + (y - sy) * (y - sy);
      if (d <= r * r && s > sr * sr) {
        double shade = 1.0 - 0.25 * ((x + y) / 64.0);
        unsigned R = (unsigned)(255 * shade), G = (unsigned)(186 * shade),
                 B = (unsigned)(92 * shade);
        px[y * S + x] = (255u << 24) | (R << 16) | (G << 8) | B;
        andMask[y * 4 + x / 8] &= (unsigned char)~(0x80 >> (x % 8));
      }
    }
  }
  HBITMAP hMask = CreateBitmap(S, S, 1, 1, andMask);
  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmColor = hColor;
  ii.hbmMask = hMask;
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(hColor);
  DeleteObject(hMask);
  return icon ? icon : LoadIcon(nullptr, IDI_APPLICATION);
}

void showBalloon(const char* title, const char* body) {
  NOTIFYICONDATAA nid{};
  HWND hwnd = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_owner || !g_ui.trayHwnd) return;
    nid = g_ui.nid;
    hwnd = g_ui.trayHwnd;
  }
  (void)hwnd;
  nid.uFlags = NIF_INFO;
  strncpy(nid.szInfoTitle, title, sizeof(nid.szInfoTitle) - 1);
  strncpy(nid.szInfo, body, sizeof(nid.szInfo) - 1);
  nid.dwInfoFlags = NIIF_INFO | NIIF_LARGE_ICON;
  Shell_NotifyIconA(NIM_MODIFY, &nid);
}

int clampInt(const char* text, int lo, int hi, int dflt) {
  if (!text || !*text) return dflt;
  try {
    return std::clamp(std::stoi(text), lo, hi);
  } catch (...) {
    return dflt;
  }
}
void getEditText(HWND h, char* out, size_t n) {
  out[0] = 0;
  if (h) GetWindowTextA(h, out, (int)n);
}
int parseHm(const char* text) {
  if (!text) return -1;
  std::string t = text;
  auto c = t.find(':');
  if (c == std::string::npos) return -1;
  try {
    int h = std::stoi(t.substr(0, c)), m = std::stoi(t.substr(c + 1));
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
  } catch (...) {
    return -1;
  }
}

// ---------- tray popup menu ----------

void popupMenu() {
  Settings s = snapOf();
  HMENU menu = CreatePopupMenu();
  AppendMenuA(menu, MF_STRING | (s.enabled ? MF_CHECKED : MF_UNCHECKED), IDM_ENABLE,
              "Filter enabled");
  HMENU sub = CreatePopupMenu();
  auto presets = builtinPresets();
  for (size_t i = 0; i < presets.size() && i < 8; ++i) {
    AppendMenuA(sub, MF_STRING | (s.preset == presets[i].name ? MF_CHECKED : MF_UNCHECKED),
                IDM_PRESET_BASE + (int)i, presets[i].name.c_str());
  }
  AppendMenuA(menu, MF_STRING | MF_POPUP, (UINT_PTR)sub, "Preset");
  AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuA(menu, MF_STRING, IDM_OPEN, "Open night-screen");
  AppendMenuA(menu, MF_STRING, IDM_BREAK_NOW, "Take a break now");
  AppendMenuA(menu, MF_STRING, IDM_SNOOZE, "Snooze breaks 10 min");
  AppendMenuA(menu, MF_STRING | (s.oledMode ? MF_CHECKED : MF_UNCHECKED), IDM_OLED,
              "OLED always-on guard");
  AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuA(menu, MF_STRING | (s.startAtLogin ? MF_CHECKED : MF_UNCHECKED),
              IDM_AUTOSTART, "Start at login");
  AppendMenuA(menu, MF_STRING, IDM_QUIT, "Quit");
  SetMenuDefaultItem(menu, IDM_OPEN, FALSE);

  POINT p;
  GetCursorPos(&p);
  HWND hwnd;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    hwnd = g_ui.trayHwnd;
  }
  SetForegroundWindow(hwnd);
  int cmd =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
  PostMessageA(hwnd, WM_NULL, 0, 0);
  DestroyMenu(menu);
  if (cmd != 0) trayCommand(cmd);
}

void trayCommand(int cmd) {
  if (cmd == IDM_QUIT) {
    HWND hwnd;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      hwnd = g_ui.trayHwnd;
    }
    if (hwnd) PostMessageA(hwnd, WM_TRAY_SHUTDOWN, 0, 0);
    return;
  }
  if (cmd == IDM_OPEN) {
    showPanel(true);
    return;
  }
  if (cmd == IDM_ENABLE) {
    bool en = !snapOf().enabled;
    updateVia([en](Settings& s) { s.enabled = en; }, true);
    return;
  }
  if (cmd == IDM_AUTOSTART) {
    bool en = !snapOf().startAtLogin;
    updateVia([en](Settings& s) { s.startAtLogin = en; }, true);
    return;
  }
  if (cmd == IDM_BREAK_NOW) {
    breakNowVia();
    return;
  }
  if (cmd == IDM_SNOOZE) {
    snoozeVia(10);
    return;
  }
  if (cmd == IDM_OLED) {
    bool en = !snapOf().oledMode;
    if (en) {
      int mv = snapOf().oledMoveSec;
      updateVia([mv](Settings& s) {
        s.oledMode = true;
        s.oledMoveSec = mv;
      }, true);
    } else {
      updateVia([](Settings& s) { s.oledMode = false; }, true);
    }
    return;
  }
  if (cmd >= IDM_PRESET_BASE && cmd < IDM_PRESET_BASE + 16) {
    auto presets = builtinPresets();
    size_t i = (size_t)(cmd - IDM_PRESET_BASE);
    if (i < presets.size()) {
      updateVia([p = presets[i]](Settings& s) {
        s.preset = p.name;
        s.temperature = p.temperature;
        s.brightness = p.brightness;
        s.enabled = (p.name != "Off");
      }, true);
    }
  }
}

// ---------- window procedures ----------

LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_TRAY) {
    if (LOWORD(lp) == WM_RBUTTONUP) {
      popupMenu();
    } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
      showPanel(true);
    }
    return 0;
  }
  if (msg == WM_BALLOON_Q) {
    auto* b = reinterpret_cast<Balloon*>(lp);
    if (b) {
      showBalloon(b->title, b->body);
      delete b;
    }
    return 0;
  }
  if (msg == WM_TRAY_SHUTDOWN) {
    TrayGui::VoidFn quit;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      if (g_ui.trayHwnd) Shell_NotifyIconA(NIM_DELETE, &g_ui.nid);
      if (g_ui.panelHwnd) DestroyWindow(g_ui.panelHwnd);
      g_ui = UiState{};
      if (g_owner) {
        quit = g_owner->quit_;
        g_owner = nullptr;
      }
      DestroyWindow(hwnd);
    }
    if (quit) quit(); // ask daemon to stop; then PostQuitMessage below via DESTROY
    return 0;
  }
  if (msg == WM_COMMAND) {
    trayCommand(LOWORD(wp));
    return 0;
  }
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

HWND makeCtl(const char* cls, const char* text, DWORD style, int x, int y, int w, int h,
             HWND parent, int id, HFONT font) {
  HWND hwnd = CreateWindowExA(0, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, w, h,
                              parent, (HMENU)(INT_PTR)id, GetModuleHandleA(nullptr),
                              nullptr);
  if (hwnd && font) SendMessageA(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
  return hwnd;
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
      HWND hT = makeCtl("msctls_trackbar32", "", TBS_AUTOTICKS | TBS_HORZ, 12, 84, 336,
                        30, hwnd, IDC_TEMP, nullptr);
      SendMessageA(hT, TBM_SETRANGE, TRUE, MAKELONG(1000, 6500));
      SendMessageA(hT, TBM_SETTICFREQ, 500, 0);
      SendMessageA(hT, TBM_SETPAGESIZE, 0, 200);
      HWND hB = makeCtl("msctls_trackbar32", "", TBS_AUTOTICKS | TBS_HORZ, 12, 144, 336,
                        30, hwnd, IDC_BRIGHT, nullptr);
      SendMessageA(hB, TBM_SETRANGE, TRUE, MAKELONG(5, 100));
      SendMessageA(hB, TBM_SETTICFREQ, 10, 0);
      SendMessageA(hB, TBM_SETPAGESIZE, 0, 5);
      int gy = 0;
      {
        std::lock_guard<std::mutex> lk(g_mu);
        g_ui.hTemp = hT;
      g_ui.hBright = hB;
      g_ui.hTempVal = makeCtl("STATIC", "", SS_LEFT, 300, 60, 60, 16, hwnd, IDC_TEMPVAL, font);
      makeCtl("STATIC", "Color temperature", SS_LEFT, 12, 60, 200, 16, hwnd, 0, font);
      g_ui.hBrightVal =
          makeCtl("STATIC", "", SS_LEFT, 300, 120, 60, 16, hwnd, IDC_BRIGHTVAL, font);
      makeCtl("STATIC", "Brightness", SS_LEFT, 12, 120, 200, 16, hwnd, 0, font);
      g_ui.hEnable = makeCtl("BUTTON", "Filter enabled", BS_AUTOCHECKBOX, 12, 12, 140,
                             20, hwnd, IDC_ENABLECHK, font);
      g_ui.hTopmost = makeCtl("BUTTON", "Always on top", BS_AUTOCHECKBOX, 200, 12, 140,
                              20, hwnd, IDC_TOPMOST, font);
      SendMessageA(g_ui.hTopmost, BM_SETCHECK, BST_CHECKED, 0);
      makeCtl("STATIC", "Preset", SS_LEFT, 12, 182, 100, 16, hwnd, 0, font);
      auto presets = builtinPresets();
      g_ui.presetCount = 0;
      for (size_t i = 0; i < presets.size() && i < 8; ++i) {
        int col = (int)(i % 3), row = (int)(i / 3);
        g_ui.hPreset[i] =
            makeCtl("BUTTON", presets[i].name.c_str(), BS_PUSHBUTTON, 12 + col * 114,
                    202 + row * 30, 106, 26, hwnd, IDC_PRESET_BASE + (int)i, font);
        g_ui.presetCount++;
      }
      int base = 202 + ((g_ui.presetCount + 2) / 3) * 30 + 12;
      makeCtl("STATIC", "Mode", SS_LEFT, 12, base, 60, 16, hwnd, 0, font);
      g_ui.hMode = makeCtl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 12, base + 18,
                           150, 100, hwnd, IDC_MODE, font);
      SendMessageA(g_ui.hMode, CB_ADDSTRING, 0, (LPARAM) "Manual");
      SendMessageA(g_ui.hMode, CB_ADDSTRING, 0, (LPARAM) "Scheduled");
      SendMessageA(g_ui.hMode, CB_ADDSTRING, 0, (LPARAM) "Sunrise/sunset auto");
      makeCtl("STATIC", "on", SS_LEFT, 172, base + 22, 24, 16, hwnd, 0, font);
      g_ui.hOn = makeCtl("EDIT", "21:00", ES_LEFT | WS_BORDER, 196, base + 18, 60, 22,
                         hwnd, IDC_ON, font);
      makeCtl("STATIC", "off", SS_LEFT, 262, base + 22, 24, 16, hwnd, 0, font);
      g_ui.hOff = makeCtl("EDIT", "07:00", ES_LEFT | WS_BORDER, 288, base + 18, 60, 22,
                          hwnd, IDC_OFF, font);
      int gyLocal = base + 52;
      gy = gyLocal;
      makeCtl("BUTTON", "Eye break (20-20-20)", BS_GROUPBOX, 12, gy, 336, 108, hwnd, 0,
              font);
      g_ui.hBrkEn = makeCtl("BUTTON", "Remind me", BS_AUTOCHECKBOX, 24, gy + 22, 110, 20,
                            hwnd, IDC_BRKEN, font);
      g_ui.hStrict = makeCtl("BUTTON", "Strict lock", BS_AUTOCHECKBOX, 150, gy + 22, 110,
                             20, hwnd, IDC_STRICT, font);
      makeCtl("STATIC", "every", SS_LEFT, 24, gy + 50, 44, 16, hwnd, 0, font);
      g_ui.hWork = makeCtl("EDIT", "20", ES_NUMBER | ES_LEFT | WS_BORDER, 70, gy + 46, 48,
                           22, hwnd, IDC_WORK, font);
      makeCtl("STATIC", "min, rest", SS_LEFT, 124, gy + 50, 60, 16, hwnd, 0, font);
      g_ui.hRest = makeCtl("EDIT", "20", ES_NUMBER | ES_LEFT | WS_BORDER, 188, gy + 46,
                           48, 22, hwnd, IDC_REST, font);
      makeCtl("STATIC", "sec", SS_LEFT, 240, gy + 50, 40, 16, hwnd, 0, font);
      makeCtl("BUTTON", "Break now", BS_PUSHBUTTON, 24, gy + 74, 100, 26, hwnd,
              IDC_BREAKNOW, font);
      makeCtl("BUTTON", "Snooze 10m", BS_PUSHBUTTON, 132, gy + 74, 110, 26, hwnd,
              IDC_SNOOZEBTN, font);
      makeCtl("BUTTON", "Web panel…", BS_PUSHBUTTON, 248, gy + 74, 88, 26, hwnd, IDC_WEB,
              font);
      g_ui.hOled =
          makeCtl("BUTTON", "OLED burn-in guard (dark + roaming clock)", BS_AUTOCHECKBOX,
                  12, gy + 114, 260, 20, hwnd, IDC_OLED, font);
      makeCtl("STATIC", "move every", SS_LEFT, 24, gy + 138, 70, 16, hwnd, 0, font);
      g_ui.hOledMove = makeCtl("EDIT", "60", ES_NUMBER | ES_LEFT | WS_BORDER, 98,
                               gy + 134, 48, 22, hwnd, IDC_OLEDMOVE, font);
      makeCtl("STATIC", "s (any input exits)", SS_LEFT, 152, gy + 138, 150, 16, hwnd,
              0, font);
      // size window to fit content
      } // release g_mu before refresh (it re-locks briefly)
      int wantH = gy + 134 + 22 + 28;
      RECT rc{0, 0, 372, wantH};
      AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                       FALSE);
      SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                   SWP_NOMOVE | SWP_NOACTIVATE);
      SetTimer(hwnd, kRefreshTimer, 1000, nullptr);
      refreshPanel();
      return 0;
    }
    case WM_HSCROLL: {
      HWND src = (HWND)lp;
      bool isSlider = false;
      {
        std::lock_guard<std::mutex> lk(g_mu);
        isSlider = (src == g_ui.hTemp || src == g_ui.hBright);
      }
      if (isSlider) {
        int code = LOWORD(wp);
        if (code == TB_THUMBTRACK || code == TB_ENDTRACK || code == TB_LINEUP ||
            code == TB_LINEDOWN || code == TB_PAGEUP || code == TB_PAGEDOWN ||
            code == TB_TOP || code == TB_BOTTOM) {
          applyFromPanel(code == TB_ENDTRACK);
        }
      }
      return 0;
    }
    case WM_COMMAND: {
      int id = LOWORD(wp);
      int ev = HIWORD(wp);
      if (id == IDC_ENABLECHK && ev == BN_CLICKED) {
        bool en = false;
        {
          std::lock_guard<std::mutex> lk(g_mu);
          en = SendMessageA(g_ui.hEnable, BM_GETCHECK, 0, 0) == BST_CHECKED;
        }
        updateVia([en](Settings& s) { s.enabled = en; }, true);
      } else if (id == IDC_TOPMOST && ev == BN_CLICKED) {
        bool top = false;
        {
          std::lock_guard<std::mutex> lk(g_mu);
          top = SendMessageA(g_ui.hTopmost, BM_GETCHECK, 0, 0) == BST_CHECKED;
        }
        SetWindowPos(hwnd, top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      } else if (id == IDC_MODE && ev == CBN_SELCHANGE) {
        int sel = 0;
        {
          std::lock_guard<std::mutex> lk(g_mu);
          sel = (int)SendMessageA(g_ui.hMode, CB_GETCURSEL, 0, 0);
        }
        updateVia([sel](Settings& s) {
          s.mode = static_cast<FilterMode>(std::clamp(sel, 0, 2));
        }, true);
      } else if ((id == IDC_ON || id == IDC_OFF || id == IDC_WORK || id == IDC_REST) &&
                 (ev == EN_CHANGE || ev == EN_KILLFOCUS)) {
        applyFromPanel(ev == EN_KILLFOCUS);
      } else if ((id == IDC_BRKEN || id == IDC_STRICT) && ev == BN_CLICKED) {
        applyFromPanel(true);
      } else if (id == IDC_OLED && ev == BN_CLICKED) {
        applyOled(true);
      } else if (id == IDC_OLEDMOVE && (ev == EN_CHANGE || ev == EN_KILLFOCUS)) {
        applyOled(ev == EN_KILLFOCUS);
      } else if (id == IDC_BREAKNOW) {
        breakNowVia();
      } else if (id == IDC_SNOOZEBTN) {
        snoozeVia(10);
      } else if (id == IDC_WEB) {
        doOpenWeb();
      } else if (id >= IDC_PRESET_BASE && id < IDC_PRESET_BASE + 16) {
        auto presets = builtinPresets();
        size_t i = (size_t)(id - IDC_PRESET_BASE);
        if (i < presets.size()) {
          updateVia([p = presets[i]](Settings& s) {
            s.preset = p.name;
            s.temperature = p.temperature;
            s.brightness = p.brightness;
            s.enabled = (p.name != "Off");
          }, true);
          refreshPanel();
        }
      }
      return 0;
    }
    case WM_TIMER:
      if (wp == kRefreshTimer) refreshPanel();
      return 0;
    case WM_CLOSE:
      ShowWindow(hwnd, SW_HIDE); // hide, daemon + tray keep running
      return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

// ---------- panel data flow ----------

Settings snapOf() {
  std::function<Settings()> get;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_owner) get = g_owner->get_;
  }
  if (get) return get();
  return Settings{};
}

void updateVia(std::function<void(Settings&)> fn, bool save) {
  TrayGui::UpdateFn set;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_owner) set = g_owner->set_;
  }
  if (set) set(std::move(fn), save);
  refreshPanel();
}

void breakNowVia() {
  TrayGui::VoidFn f;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_owner) f = g_owner->breakNow_;
  }
  if (f) f();
}

void snoozeVia(int minutes) {
  TrayGui::SnoozeFn f;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_owner) f = g_owner->snooze_;
  }
  if (f) f(minutes);
}

void showPanel(bool show) {
  HWND h = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    h = g_ui.panelHwnd;
  }
  if (!h) return;
  refreshPanel();
  ShowWindow(h, show ? SW_SHOW : SW_HIDE);
  if (show) SetForegroundWindow(h);
}

void doOpenWeb() {
  int port = snapOf().uiPort;
  char url[64];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
  ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

void refreshPanel() {
  HWND panel, hTemp, hBright, hTempVal, hBrightVal, hEnable, hMode, hOn, hOff, hBrkEn,
      hStrict, hWork, hRest, hOled, hOledMove;
  HWND presets[8];
  int presetCount = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_ui.panelHwnd || !IsWindowVisible(g_ui.panelHwnd)) return;
    panel = g_ui.panelHwnd;
    hTemp = g_ui.hTemp;
    hBright = g_ui.hBright;
    hTempVal = g_ui.hTempVal;
    hBrightVal = g_ui.hBrightVal;
    hEnable = g_ui.hEnable;
    hMode = g_ui.hMode;
    hOn = g_ui.hOn;
    hOff = g_ui.hOff;
    hBrkEn = g_ui.hBrkEn;
    hStrict = g_ui.hStrict;
    hWork = g_ui.hWork;
    hRest = g_ui.hRest;
    hOled = g_ui.hOled;
    hOledMove = g_ui.hOledMove;
    presetCount = g_ui.presetCount;
    for (int i = 0; i < presetCount; ++i) presets[i] = g_ui.hPreset[i];
  }
  Settings s = snapOf();
  char buf[64];
  if (hTemp && GetCapture() != hTemp)
    SendMessageA(hTemp, TBM_SETPOS, TRUE, s.enabled ? s.temperature : 6500);
  if (hBright && GetCapture() != hBright)
    SendMessageA(hBright, TBM_SETPOS, TRUE, s.enabled ? s.brightness : 100);
  snprintf(buf, sizeof(buf), "%dK", s.temperature);
  if (hTempVal) SetWindowTextA(hTempVal, buf);
  snprintf(buf, sizeof(buf), "%d%%", s.brightness);
  if (hBrightVal) SetWindowTextA(hBrightVal, buf);
  if (hEnable)
    SendMessageA(hEnable, BM_SETCHECK, s.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
  if (hMode) SendMessageA(hMode, CB_SETCURSEL, (WPARAM)(int)s.mode, 0);
  if (hOn && GetFocus() != hOn) SetWindowTextA(hOn, s.scheduleOn.c_str());
  if (hOff && GetFocus() != hOff) SetWindowTextA(hOff, s.scheduleOff.c_str());
  if (hBrkEn)
    SendMessageA(hBrkEn, BM_SETCHECK, s.breakEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
  if (hStrict)
    SendMessageA(hStrict, BM_SETCHECK, s.strictBreak ? BST_CHECKED : BST_UNCHECKED, 0);
  snprintf(buf, sizeof(buf), "%d", s.workMinutes);
  if (hWork && GetFocus() != hWork) SetWindowTextA(hWork, buf);
  snprintf(buf, sizeof(buf), "%d", s.restSeconds);
  if (hRest && GetFocus() != hRest) SetWindowTextA(hRest, buf);
  if (hOled)
    SendMessageA(hOled, BM_SETCHECK, s.oledMode ? BST_CHECKED : BST_UNCHECKED, 0);
  snprintf(buf, sizeof(buf), "%d", s.oledMoveSec);
  if (hOledMove && GetFocus() != hOledMove) SetWindowTextA(hOledMove, buf);
  auto plist = builtinPresets();
  for (int i = 0; i < presetCount && i < (int)plist.size(); ++i) {
    std::string label = (s.preset == plist[i].name ? "● " : "") + plist[i].name;
    char cur[64] = {};
    GetWindowTextA(presets[i], cur, sizeof(cur) - 1);
    if (label != cur) SetWindowTextA(presets[i], label.c_str());
  }
  snprintf(buf, sizeof(buf), "night-screen%s — %dK · %d%%%s",
           s.oledMode ? " [OLED guard]" : "", s.temperature, s.brightness,
           s.enabled ? "" : " (paused)");
  SetWindowTextA(panel, buf);
  {
    std::lock_guard<std::mutex> lk(g_mu);
    strncpy(g_ui.nid.szTip, buf, sizeof(g_ui.nid.szTip) - 1);
    NOTIFYICONDATAA nid = g_ui.nid;
    nid.uFlags = NIF_TIP;
    Shell_NotifyIconA(NIM_MODIFY, &nid);
  }
}

void applyFromPanel(bool save) {
  HWND hTemp, hBright, hTempVal, hBrightVal, hEnable, hMode, hOn, hOff, hBrkEn, hStrict,
      hWork, hRest;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    hTemp = g_ui.hTemp;
    hBright = g_ui.hBright;
    hTempVal = g_ui.hTempVal;
    hBrightVal = g_ui.hBrightVal;
    hEnable = g_ui.hEnable;
    hMode = g_ui.hMode;
    hOn = g_ui.hOn;
    hOff = g_ui.hOff;
    hBrkEn = g_ui.hBrkEn;
    hStrict = g_ui.hStrict;
    hWork = g_ui.hWork;
    hRest = g_ui.hRest;
  }
  if (!hTemp || !hBright) return;
  int temp = std::clamp((int)SendMessageA(hTemp, TBM_GETPOS, 0, 0), 1000, 6500);
  int bright = std::clamp((int)SendMessageA(hBright, TBM_GETPOS, 0, 0), 5, 100);
  char on[32] = "21:00", off[32] = "07:00", work[32] = "20", rest[32] = "20";
  getEditText(hOn, on, sizeof(on));
  getEditText(hOff, off, sizeof(off));
  getEditText(hWork, work, sizeof(work));
  getEditText(hRest, rest, sizeof(rest));
  bool brkEn = SendMessageA(hBrkEn, BM_GETCHECK, 0, 0) == BST_CHECKED;
  bool strict = SendMessageA(hStrict, BM_GETCHECK, 0, 0) == BST_CHECKED;
  int modeSel = (int)SendMessageA(hMode, CB_GETCURSEL, 0, 0);
  updateVia([=](Settings& s) {
    s.temperature = temp;
    s.brightness = bright;
    s.enabled = true;
    s.preset = "Custom";
    if (modeSel >= 0 && modeSel <= 2) s.mode = static_cast<FilterMode>(modeSel);
    if (parseHm(on) >= 0) s.scheduleOn = on;
    if (parseHm(off) >= 0) s.scheduleOff = off;
    s.breakEnabled = brkEn;
    s.strictBreak = strict;
    s.workMinutes = clampInt(work, 1, 180, s.workMinutes);
    s.restSeconds = clampInt(rest, 5, 3600, s.restSeconds);
  }, save);
  // live label update (no full refresh while dragging)
  char buf[32];
  snprintf(buf, sizeof(buf), "%dK", temp);
  if (hTempVal) SetWindowTextA(hTempVal, buf);
  snprintf(buf, sizeof(buf), "%d%%", bright);
  if (hBrightVal) SetWindowTextA(hBrightVal, buf);
  if (hEnable) SendMessageA(hEnable, BM_SETCHECK, BST_CHECKED, 0);
}

void applyOled(bool save) {
  HWND hOled, hOledMove;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    hOled = g_ui.hOled;
    hOledMove = g_ui.hOledMove;
  }
  if (!hOled) return;
  bool on = SendMessageA(hOled, BM_GETCHECK, 0, 0) == BST_CHECKED;
  char mv[32] = "60";
  getEditText(hOledMove, mv, sizeof(mv));
  int moveSec = clampInt(mv, 10, 600, 60);
  updateVia([on, moveSec](Settings& s) {
    s.oledMode = on;
    s.oledMoveSec = moveSec;
  }, save);
}

} // namespace

// ---------- TrayGui members ----------

bool winTryBalloon(const std::string& title, const std::string& body) {
  HWND hwnd = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    hwnd = g_ui.trayHwnd;
  }
  if (!hwnd) return false;
  auto* b = new Balloon();
  strncpy(b->title, title.c_str(), sizeof(b->title) - 1);
  b->title[sizeof(b->title) - 1] = 0;
  strncpy(b->body, body.c_str(), sizeof(b->body) - 1);
  b->body[sizeof(b->body) - 1] = 0;
  if (!PostMessageA(hwnd, WM_BALLOON_Q, 0, (LPARAM)b)) {
    delete b;
    return false;
  }
  return true;
}

// per-instance UI thread, stored in a global map keyed by TrayGui*
namespace {
std::mutex g_threadsMu;
struct ThreadRec {
  std::thread thread;
};
std::map<TrayGui*, ThreadRec> g_threads; // NOLINT: tiny, guarded
} // namespace

void TrayGui::start(SnapFn get, UpdateFn set, VoidFn quit, VoidFn breakNow,
                    SnoozeFn snooze) {
  stop();
  get_ = std::move(get);
  set_ = std::move(set);
  quit_ = std::move(quit);
  breakNow_ = std::move(breakNow);
  snooze_ = std::move(snooze);
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_owner = this;
  }
  std::lock_guard<std::mutex> tlk(g_threadsMu);
  g_threads[this].thread = std::thread([this] {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HINSTANCE hInst = GetModuleHandleA(nullptr);

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInst;
    wc.lpszClassName = kTrayClass;
    wc.lpfnWndProc = TrayProc;
    RegisterClassExA(&wc);
    WNDCLASSEXA pc{};
    pc.cbSize = sizeof(pc);
    pc.hInstance = hInst;
    pc.lpszClassName = kPanelClass;
    pc.lpfnWndProc = PanelProc;
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExA(&pc);

    HWND tray = CreateWindowExA(0, kTrayClass, "night-screen tray", WS_POPUP, 0, 0, 1, 1,
                               nullptr, nullptr, hInst, nullptr);
    HWND panel = CreateWindowExA(
        WS_EX_TOOLWINDOW, kPanelClass, "night-screen",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
        CW_USEDEFAULT, 372, 520, nullptr, nullptr, hInst, (LPVOID)this);
    HICON icon = makeMoonIcon();
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_ui.trayHwnd = tray;
      g_ui.panelHwnd = panel;
      g_ui.icon = icon;
      NOTIFYICONDATAA nid{};
      nid.cbSize = sizeof(nid);
      nid.hWnd = tray;
      nid.uID = 1;
      nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
      nid.uCallbackMessage = WM_TRAY;
      nid.hIcon = icon;
      strncpy(nid.szTip, "night-screen", sizeof(nid.szTip) - 1);
      g_ui.nid = nid;
      Shell_NotifyIconA(NIM_ADD, &g_ui.nid);
    }
    refreshPanel();
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    // thread ending: make sure globals are cleared
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_owner == this) g_owner = nullptr;
    g_ui = UiState{};
  });
}

void TrayGui::stop() {
  HWND hwnd = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    hwnd = g_ui.trayHwnd;
  }
  if (hwnd) {
    PostMessageA(hwnd, WM_TRAY_SHUTDOWN, 0, 0);
  }
  std::lock_guard<std::mutex> tlk(g_threadsMu);
  auto it = g_threads.find(this);
  if (it != g_threads.end()) {
    if (it->second.thread.joinable()) it->second.thread.join();
    g_threads.erase(it);
  }
}

} // namespace nightscreen

#endif // _WIN32
