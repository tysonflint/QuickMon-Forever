// QuickMonForever.cpp - QuickMon Forever 2.10, a Performance Monitor screen saver for
// Windows 10 and later (formerly Quick Monitor 11).
// Copyright (C) 2026 Tyson Flint
//
// This program is free software: you can redistribute it and/or modify it under the terms of
// the GNU General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program
// (the LICENSE file). If not, see <https://www.gnu.org/licenses/>.
//
// Additional term under section 7(b) of the GPL (see the NOTICE file): every copy and modified
// version must keep the attribution "Based on QuickMon 1.10 by Tim Hill (1998)" and Tyson
// Flint's copyright notice, in its documentation and in its About box.
//
// Based on QuickMon 1.10, originally written by Tim Hill (1998). This is an
// independent rewrite from the original's manual and observed behavior; it
// contains no code from the original.
//
// A re-creation of QuickMon 1.10: a screen saver that charts
// up to eight Windows performance counters (local or remote), with auto-scaling,
// a legend, schemes, and a window that drifts around the screen.
//
// One source file builds two binaries:
//   x64 -> QuickMonForever.scr     the screen saver itself
//   x86 -> QuickMonForever-32.exe  32-bit helper. On 64-bit Windows, counters that come
//                             from 32-bit-only provider DLLs are visible only to
//                             32-bit processes, so the 64-bit saver browses,
//                             explains and samples those counters through it.
//
// Command line (standard screen saver contract):
//   /s            run full screen
//   /p <hwnd>     draw the preview into the Screen Saver Settings window
//   /c[:hwnd]     show the setup dialog (also the default with no arguments)
// Helper-only switches: /helper32, /browse32 <hwnd> <detail> <path>, /explain32 <hwnd> <path>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <winperf.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <cmath>
#include <cwchar>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <climits>
#include <dxgi.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#include <commdlg.h>
#include <sddl.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#include "resource.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------------------
// Constants and option tables
// ---------------------------------------------------------------------------

static const int     MAX_COUNTERS = 8;
static const int     HISTORY      = 4000;              // samples kept (more than any screen can show)
static const wchar_t REG_ROOT[]   = L"Software\\QuickMonForever";
static const wchar_t REG_OLD_ROOT[] = L"Software\\QuickMon11";      // settings from before the rename
// Two lines; "Revives " is as wide as "Forever ", so the years line up.
static const wchar_t VERSION_STR[] =
    L"QuickMon Forever v2.10 (2026) by Tyson Flint\n"
    L"Revives QuickMon v1.10 (1998) by Tim Hill. Long live QuickMon.";
static const wchar_t ABOUT_TEXT[] =
    L"QuickMon Forever, version 2.10 (build " QM_BUILD_STR ")\n"
    L"Performance Monitor screen saver for Windows 10 and later\n\n"
    L"Revives QuickMon 1.10, the \"QuickMon Performance\n"
    L"Monitor Screen Saver for Windows NT\",\n"
    L"written by Tim Hill in 1998.\n"
    L"Rewritten by Tyson Flint in 2026.\n\n"
    L"This independent rewrite contains no code from the\n"
    L"original QuickMon, which remains Copyright (c) 1998 Tim Hill.\n\n"
    L"QuickMon Forever is Copyright (c) 2026 Tyson Flint.\n"
    L"It is free software, released under the GNU General\n"
    L"Public License, version 3 or later, and comes with\n"
    L"no warranty. Free forever, like the original freeware.\n\n"
    L"Additional term (GPL section 7(b), see the NOTICE file):\n"
    L"every copy and changed version must keep the credit\n"
    L"\"Based on QuickMon 1.10 by Tim Hill (1998)\" and the\n"
    L"copyright notice above.";
static const wchar_t HELPER_EXE[] = L"QuickMonForever-32.exe";

enum { SCALE_AUTO = 0, SCALE_DEFAULT = 1, SCALE_MANUAL = 2 };
enum { DISPLAY_BOTH = 0, DISPLAY_CHART = 1, DISPLAY_LEGEND = 2 };
enum { MODE_CONFIG, MODE_SAVER, MODE_PREVIEW };

struct NamedColor { const wchar_t* name; COLORREF rgb; };
static const NamedColor kColors[] = {
    { L"Red",     RGB(255,  70,  70) }, { L"Green",   RGB( 60, 220,  60) },
    { L"Blue",    RGB( 80, 140, 255) }, { L"Yellow",  RGB(255, 230,  60) },
    { L"Magenta", RGB(255,  80, 255) }, { L"Cyan",    RGB( 60, 230, 230) },
    { L"White",   RGB(240, 240, 240) }, { L"Orange",  RGB(255, 150,  40) },
    { L"Lime",    RGB(170, 255,  60) }, { L"Pink",    RGB(255, 150, 190) },
    { L"Purple",  RGB(170, 110, 255) }, { L"Sky",     RGB(120, 200, 255) },
    { L"Gold",    RGB(220, 180,  60) }, { L"Teal",    RGB(  0, 175, 155) },
    { L"Gray",    RGB(150, 150, 150) }, { L"Brown",   RGB(190, 120,  70) },
};
static const int NCOLORS = (int)(sizeof(kColors) / sizeof(kColors[0]));
// A counter's color index NCOLORS means Custom: its own RGB value (CounterCfg::customRgb), picked
// with the Windows color dialog. Older builds read it as the last named color.
static const int COLOR_CUSTOM = NCOLORS;

// Grid colors are dark shades so the grid stays behind the counter lines; the window border
// is a brighter shade of the same color. Gray is the original's grid (80) and border (192).
// "None" draws no grid (CLR_NONE) and keeps the original gray border.
struct GridColor { const wchar_t* name; COLORREF rgb; COLORREF border; };
static const GridColor kGridColors[] = {
    { L"Gray",       RGB( 80,  80,  80), RGB(192, 192, 192) },
    { L"Dark Gray",  RGB( 48,  48,  48), RGB(128, 128, 128) },
    { L"Light Gray", RGB(120, 120, 120), RGB(220, 220, 220) },
    { L"Green",      RGB( 30,  85,  45), RGB( 60, 170,  90) },
    { L"Blue",       RGB( 35,  55, 120), RGB( 80, 120, 230) },
    { L"Teal",       RGB(  0,  85,  85), RGB(  0, 170, 170) },
    { L"Amber",      RGB(110,  75,   0), RGB(220, 150,   0) },
    { L"Red",        RGB(105,  30,  30), RGB(210,  60,  60) },
    { L"Purple",     RGB( 75,  45, 110), RGB(150,  90, 220) },
    { L"None",       CLR_NONE,           RGB(192, 192, 192) },
};
static const int NGRIDCOLORS = (int)(sizeof(kGridColors) / sizeof(kGridColors[0]));

static const int kRefresh[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600 };
static const int NREFRESH = (int)(sizeof(kRefresh) / sizeof(kRefresh[0]));

// Window sizes. pct > 0: that percentage of the screen each way. w,h > 0: fixed pixels.
// Auto (pct 0, w 0) is one third, as measured on 1.10 (532 x 398 at 1600 x 1200).
// Full Screen is w = -1. The array index is what the registry stores, so new entries are only
// ever appended; kSizeOrder is the order the drop-down lists them in.
struct SizeOpt { const wchar_t* name; int w, h, pct; };
static const SizeOpt kSizes[] = {
    { L"Auto (one third)", 0, 0, 0 },                                                   // 0
    { L"640 x 400", 640, 400, 0 }, { L"960 x 600", 960, 600, 0 },                       // 1, 2
    { L"1280 x 800", 1280, 800, 0 }, { L"1600 x 1000", 1600, 1000, 0 },                 // 3, 4
    { L"1920 x 1200", 1920, 1200, 0 }, { L"Full Screen", -1, -1, 0 },                   // 5, 6
    { L"20% of screen", 0, 0, 20 }, { L"25% of screen", 0, 0, 25 },                     // 7, 8
    { L"30% of screen", 0, 0, 30 }, { L"35% of screen", 0, 0, 35 },                     // 9, 10
    { L"40% of screen", 0, 0, 40 }, { L"50% of screen", 0, 0, 50 },                     // 11, 12
    { L"60% of screen", 0, 0, 60 }, { L"70% of screen", 0, 0, 70 },                     // 13, 14
    { L"80% of screen", 0, 0, 80 }, { L"90% of screen", 0, 0, 90 },                     // 15, 16
    { L"100% of screen", 0, 0, 100 },                                                   // 17
    // The common Windows display resolutions.
    { L"3840 x 2160", 3840, 2160, 0 }, { L"2560 x 1440", 2560, 1440, 0 },               // 18, 19
    { L"1920 x 1440", 1920, 1440, 0 }, { L"1920 x 1080", 1920, 1080, 0 },               // 20, 21
    { L"1680 x 1050", 1680, 1050, 0 }, { L"1600 x 1200", 1600, 1200, 0 },               // 22, 23
    { L"1600 x 1024", 1600, 1024, 0 }, { L"1600 x 900", 1600, 900, 0 },                 // 24, 25
    { L"1440 x 1080", 1440, 1080, 0 }, { L"1440 x 900", 1440, 900, 0 },                 // 26, 27
    { L"1366 x 768", 1366, 768, 0 },   { L"1360 x 768", 1360, 768, 0 },                 // 28, 29
    { L"1280 x 1024", 1280, 1024, 0 }, { L"1280 x 960", 1280, 960, 0 },                 // 30, 31
    { L"1280 x 768", 1280, 768, 0 },   { L"1280 x 720", 1280, 720, 0 },                 // 32, 33
    { L"1176 x 664", 1176, 664, 0 },   { L"1152 x 864", 1152, 864, 0 },                 // 34, 35
    { L"1024 x 768", 1024, 768, 0 },   { L"800 x 600", 800, 600, 0 },                   // 36, 37
};
static const int NSIZES = (int)(sizeof(kSizes) / sizeof(kSizes[0]));
// Auto, the percentages, then every fixed size from largest to smallest, then Full Screen.
static const int kSizeOrder[] = { 0, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
                                  18, 19, 20, 5, 21, 22, 23, 24, 4, 25, 26, 27, 28, 29, 30, 31, 3,
                                  32, 33, 34, 35, 36, 2, 37, 1, 6 };
static_assert(sizeof(kSizeOrder) / sizeof(kSizeOrder[0]) == NSIZES, "list every size once");

// Chart window size for a screen of mw x mh. Returns true for Full Screen.
static bool ChartSize(int sizeIndex, int mw, int mh, int& pw, int& ph) {
    const SizeOpt& so = kSizes[std::clamp(sizeIndex, 0, NSIZES - 1)];
    if (so.w < 0)        { pw = mw; ph = mh; return true; }
    if (so.pct > 0)      { pw = mw * so.pct / 100; ph = mh * so.pct / 100; }
    else if (so.w == 0)  { pw = mw / 3; ph = mh / 3; }
    else                 { pw = std::min(so.w, mw); ph = std::min(so.h, mh); }
    pw = std::max(pw, 1); ph = std::max(ph, 1);
    return false;
}

// speed: glide styles, pixels per 33 ms tick at 1080p.
// classicSec: Classic style, seconds between jumps. Measured on the original 1.10: it always
// jumps CLASSIC_PX across and down, every 5 s at Fast, 15 s at Medium and 30 s at Slow, on its
// own timer (Medium was still 15 s with a 1 s refresh), not tied to chart updates.
struct MoveOpt { const wchar_t* name; double speed; int classicSec; };
static const MoveOpt kMoves[] = { { L"None", 0.0, 0 }, { L"Slow", 0.35, 30 }, { L"Medium", 1.0, 15 }, { L"Fast", 2.5, 5 } };
static const int CLASSIC_PX = 5;   // jump size at 1080 lines, measured on 1.10

// How the window drifts: Smooth glide (every display refresh), Stepped glide (33 ms timer,
// this rewrite's first behavior), or Classic (a jump each time the chart updates, like 1.10).
// Classic scales its jump to the screen height; Classic exact always jumps CLASSIC_PX.
enum { MOVESTYLE_SMOOTH = 0, MOVESTYLE_STEPPED = 1, MOVESTYLE_CLASSIC = 2, MOVESTYLE_CLASSIC_EXACT = 3 };
static const wchar_t* kMoveStyles[] = { L"Smooth", L"Stepped", L"Classic", L"Classic (5 px)" };
static const int NMOVESTYLES = 4;

// Where the window starts on the primary monitor. It heads away from that corner/edge
// (down-right from Top Left, down-left from Top Right, down from Top Center).
enum { START_CENTER = 0, START_TOPLEFT = 1, START_TOPCENTER = 2, START_TOPRIGHT = 3 };
static const wchar_t* kStartPositions[] = { L"Center", L"Top Left", L"Top Center", L"Top Right" };
static const int NSTARTPOS = 4;

// The saved value is the enum; the drop-down lists Raised 3D (the default) first.
enum { FRAME_FLAT = 0, FRAME_RAISED = 1 };
static const wchar_t* kFrameStyles[] = { L"Flat", L"Raised 3D" };
static const int NFRAMESTYLES = 2;
static const int kFrameOrder[] = { FRAME_RAISED, FRAME_FLAT };

// Theme: how the chart window is drawn. Modern QuickMon 2.10 (the default) is this rewrite's
// own look: a title bar, a labelled axis and a legend table with Last, Average, Minimum and
// Maximum. Classic QuickMon 1.10 is the original's look: a framed chart with the legend
// overlaid. The saved value is the enum; the drop-down lists Modern first. The three spaces
// after "Classic" line up "QuickMon" under the Modern entry (Segoe UI: 6 px).
enum { THEME_CLASSIC = 0, THEME_MODERN = 1 };
static const wchar_t* kThemes[] = { L"Classic   QuickMon 1.10", L"Modern QuickMon 2.10" };
static const int NTHEMES = 2;
static const int kThemeOrder[] = { THEME_MODERN, THEME_CLASSIC };

// Which monitor(s) the chart uses. Monitor 1-N is remembered by the monitor's hardware ID
// (MonitorId), with its Windows display number (MonitorNumber) as a fallback.
enum { MONITOR_PRIMARY = 0, MONITOR_ONE = 1, MONITOR_ALL = 2, MONITOR_EACH = 3 };
static const int NMOVES = (int)(sizeof(kMoves) / sizeof(kMoves[0]));

struct DetailOpt { const wchar_t* name; DWORD level; };
static const DetailOpt kDetails[] = {
    { L"Novice", PERF_DETAIL_NOVICE }, { L"Advanced", PERF_DETAIL_ADVANCED },
    { L"Expert", PERF_DETAIL_EXPERT }, { L"Wizard", PERF_DETAIL_WIZARD },
};
static const int NDETAILS = (int)(sizeof(kDetails) / sizeof(kDetails[0]));

static const wchar_t* kDisplays[] = { L"Both", L"Chart", L"Legend" };

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct CounterCfg {
    BOOL         enabled   = FALSE;
    std::wstring path;
    int          color     = 0;
    int          scaleMode = SCALE_AUTO;
    int          manualExp = 0;          // scale factor = 10^manualExp, -7..7
    BOOL         use32     = FALSE;      // read through the 32-bit helper
    COLORREF     customRgb = RGB(255, 255, 255);   // used when color == COLOR_CUSTOM
};

static COLORREF LineRgb(const CounterCfg& c) {
    return c.color == COLOR_CUSTOM ? c.customRgb : kColors[std::clamp(c.color, 0, NCOLORS - 1)].rgb;
}

// "#RRGGBB", the usual way to write a color in hex (HTML, CSS, design tools).
static std::wstring HexColor(COLORREF c) {
    wchar_t b[16];
    swprintf(b, 16, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return b;
}

// Parses "#RRGGBB" or "RRGGBB".
static bool ParseHexColor(const std::wstring& v, COLORREF& out) {
    std::wstring h = (!v.empty() && v[0] == L'#') ? v.substr(1) : v;
    if (h.size() != 6 || h.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) return false;
    unsigned long x = wcstoul(h.c_str(), nullptr, 16);
    out = RGB((x >> 16) & 0xFF, (x >> 8) & 0xFF, x & 0xFF);
    return true;
}

struct Settings {
    CounterCfg   c[MAX_COUNTERS];
    int          displayType  = DISPLAY_BOTH;
    int          refreshSec   = 5;
    int          windowSize   = 0;
    int          moveSpeed    = 2;
    int          gridColor    = 0;       // index into kGridColors
    int          moveStyle    = MOVESTYLE_SMOOTH;
    int          startPos     = START_CENTER;
    int          frameStyle   = FRAME_RAISED;
    int          theme        = THEME_MODERN;
    int          monitorMode  = MONITOR_PRIMARY;
    std::wstring monitorId;              // MONITOR_ONE: the monitor's device interface name
    int          monitorNum   = 1;       // MONITOR_ONE: its \\.\DISPLAYn number, if the ID isn't found
    BOOL         hideTitle    = FALSE;   // Modern: leave "QuickMon Forever" out of the title bar
    BOOL         hideHost     = FALSE;   // Modern: leave the computer name out of the title bar
    BOOL         hideIp       = FALSE;   // Modern: leave the IP address out of the title bar
    BOOL         hideCounterHost = FALSE; // both themes: leave "on \\THISPC" off local counters' names
    std::wstring hostName;               // Modern: name shown; "" = this PC's name, "(none)" = hidden
    std::wstring ipAdapter;              // Modern: network card (adapter GUID); "" = default, "(none)" = hidden
    int          browseDetail = 3;       // index into kDetails
    DWORD        autoScaleup  = 900;     // seconds; 0 = never scale up
    std::wstring scheme;
};

static void DefaultSettings(Settings& s) {
    s = Settings();
    static const wchar_t* defs[] = {
        L"\\Processor(_Total)\\% Processor Time",
        L"\\Memory\\% Committed Bytes In Use",
        L"\\PhysicalDisk(_Total)\\Disk Bytes/sec",
        L"\\System\\Processor Queue Length",
        L"\\Memory\\Pages/sec",
    };
    for (int i = 0; i < MAX_COUNTERS; i++) {
        s.c[i].color = i % NCOLORS;
        if (i < 5) { s.c[i].path = defs[i]; s.c[i].enabled = TRUE; }
    }
    s.c[0].color = 1;  // green, as in the original
    s.c[1].color = 0;
}

static DWORD RegGetDw(HKEY k, const wchar_t* name, DWORD def) {
    DWORD v = 0, sz = sizeof(v), type = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&v, &sz) == ERROR_SUCCESS && type == REG_DWORD) return v;
    return def;
}
static std::wstring RegGetSz(HKEY k, const wchar_t* name, const std::wstring& def) {
    DWORD type = 0, sz = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, nullptr, &sz) != ERROR_SUCCESS || type != REG_SZ) return def;
    std::wstring s(sz / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&s[0], &sz) != ERROR_SUCCESS) return def;
    s.resize(wcslen(s.c_str()));
    return s;
}
static void RegSetDw(HKEY k, const wchar_t* name, DWORD v) {
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
}
static void RegSetSz(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

static void ReadSettings(HKEY k, Settings& s) {
    wchar_t n[64];
    for (int i = 0; i < MAX_COUNTERS; i++) {
        CounterCfg& c = s.c[i];
        swprintf(n, 64, L"Counter%dPath", i + 1);      c.path      = RegGetSz(k, n, c.path);
        swprintf(n, 64, L"Counter%dEnabled", i + 1);   c.enabled   = RegGetDw(k, n, c.enabled) != 0;
        swprintf(n, 64, L"Counter%dColor", i + 1);     c.color     = (int)RegGetDw(k, n, (DWORD)c.color);
        swprintf(n, 64, L"Counter%dScaleMode", i + 1); c.scaleMode = (int)RegGetDw(k, n, (DWORD)c.scaleMode);
        swprintf(n, 64, L"Counter%dScaleExp", i + 1);  c.manualExp = (int)RegGetDw(k, n, (DWORD)c.manualExp);
        swprintf(n, 64, L"Counter%dUse32", i + 1);     c.use32     = RegGetDw(k, n, c.use32) != 0;
        swprintf(n, 64, L"Counter%dRGB", i + 1);       c.customRgb = RegGetDw(k, n, c.customRgb) & 0xFFFFFF;
        c.color     = std::clamp(c.color, 0, COLOR_CUSTOM);
        c.scaleMode = std::clamp(c.scaleMode, 0, 2);
        c.manualExp = std::clamp(c.manualExp, -7, 7);
    }
    s.displayType  = std::clamp((int)RegGetDw(k, L"DisplayType", (DWORD)s.displayType), 0, 2);
    s.refreshSec   = std::clamp((int)RegGetDw(k, L"RefreshSec", (DWORD)s.refreshSec), 1, 3600);
    s.windowSize   = std::clamp((int)RegGetDw(k, L"WindowSize", (DWORD)s.windowSize), 0, NSIZES - 1);
    s.moveSpeed    = std::clamp((int)RegGetDw(k, L"MoveSpeed", (DWORD)s.moveSpeed), 0, NMOVES - 1);
    s.gridColor    = std::clamp((int)RegGetDw(k, L"GridColor", (DWORD)s.gridColor), 0, NGRIDCOLORS - 1);
    s.moveStyle    = std::clamp((int)RegGetDw(k, L"MoveStyle", (DWORD)s.moveStyle), 0, NMOVESTYLES - 1);
    s.startPos     = std::clamp((int)RegGetDw(k, L"StartPosition", (DWORD)s.startPos), 0, NSTARTPOS - 1);
    s.frameStyle   = std::clamp((int)RegGetDw(k, L"FrameStyle", (DWORD)s.frameStyle), 0, NFRAMESTYLES - 1);
    s.theme        = std::clamp((int)RegGetDw(k, L"Theme", (DWORD)s.theme), 0, NTHEMES - 1);
    s.monitorMode  = std::clamp((int)RegGetDw(k, L"MonitorMode", (DWORD)s.monitorMode), 0, 3);
    s.monitorId    = RegGetSz(k, L"MonitorId", s.monitorId);
    s.monitorNum   = (int)RegGetDw(k, L"MonitorNumber", (DWORD)s.monitorNum);
    s.hideTitle    = RegGetDw(k, L"HideTitle", s.hideTitle) != 0;
    s.hideHost     = RegGetDw(k, L"HideHostName", s.hideHost) != 0;
    s.hideIp       = RegGetDw(k, L"HideIpAddress", s.hideIp) != 0;
    s.hideCounterHost = RegGetDw(k, L"HideCounterHost", s.hideCounterHost) != 0;
    s.hostName     = RegGetSz(k, L"HostName", s.hostName);
    s.ipAdapter    = RegGetSz(k, L"IpAdapter", s.ipAdapter);
    // Before the Hide checkboxes, "None" in the Computer name or IPv4 address list was stored as
    // "(none)": it becomes the matching Hide setting.
    if (s.hostName == L"(none)")  { s.hostName.clear();  s.hideHost = TRUE; }
    if (s.ipAdapter == L"(none)") { s.ipAdapter.clear(); s.hideIp = TRUE; }
    s.browseDetail = std::clamp((int)RegGetDw(k, L"BrowseDetail", (DWORD)s.browseDetail), 0, NDETAILS - 1);
    s.autoScaleup  = RegGetDw(k, L"AutoScaleup", s.autoScaleup);
    s.scheme       = RegGetSz(k, L"SchemeName", s.scheme);
}

static void WriteSettings(HKEY k, const Settings& s) {
    wchar_t n[64];
    for (int i = 0; i < MAX_COUNTERS; i++) {
        const CounterCfg& c = s.c[i];
        swprintf(n, 64, L"Counter%dPath", i + 1);      RegSetSz(k, n, c.path);
        swprintf(n, 64, L"Counter%dEnabled", i + 1);   RegSetDw(k, n, c.enabled ? 1 : 0);
        swprintf(n, 64, L"Counter%dColor", i + 1);     RegSetDw(k, n, (DWORD)c.color);
        swprintf(n, 64, L"Counter%dScaleMode", i + 1); RegSetDw(k, n, (DWORD)c.scaleMode);
        swprintf(n, 64, L"Counter%dScaleExp", i + 1);  RegSetDw(k, n, (DWORD)c.manualExp);
        swprintf(n, 64, L"Counter%dUse32", i + 1);     RegSetDw(k, n, c.use32 ? 1 : 0);
        swprintf(n, 64, L"Counter%dRGB", i + 1);       RegSetDw(k, n, (DWORD)c.customRgb);
    }
    RegSetDw(k, L"DisplayType", (DWORD)s.displayType);
    RegSetDw(k, L"RefreshSec", (DWORD)s.refreshSec);
    RegSetDw(k, L"WindowSize", (DWORD)s.windowSize);
    RegSetDw(k, L"MoveSpeed", (DWORD)s.moveSpeed);
    RegSetDw(k, L"GridColor", (DWORD)s.gridColor);
    RegSetDw(k, L"MoveStyle", (DWORD)s.moveStyle);
    RegSetDw(k, L"StartPosition", (DWORD)s.startPos);
    RegSetDw(k, L"FrameStyle", (DWORD)s.frameStyle);
    RegSetDw(k, L"Theme", (DWORD)s.theme);
    RegSetDw(k, L"MonitorMode", (DWORD)s.monitorMode);
    RegSetSz(k, L"MonitorId", s.monitorId);
    RegSetDw(k, L"MonitorNumber", (DWORD)s.monitorNum);
    RegSetDw(k, L"HideTitle", s.hideTitle ? 1 : 0);
    RegSetDw(k, L"HideHostName", s.hideHost ? 1 : 0);
    RegSetDw(k, L"HideIpAddress", s.hideIp ? 1 : 0);
    RegSetDw(k, L"HideCounterHost", s.hideCounterHost ? 1 : 0);
    RegSetSz(k, L"HostName", s.hostName);
    RegSetSz(k, L"IpAdapter", s.ipAdapter);
    RegSetDw(k, L"BrowseDetail", (DWORD)s.browseDetail);
    RegSetDw(k, L"AutoScaleup", s.autoScaleup);
    RegSetSz(k, L"SchemeName", s.scheme);
}

static void LoadSettings(Settings& s) {
    DefaultSettings(s);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_ROOT, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        ReadSettings(k, s);
        RegCloseKey(k);
    }
}

static bool SaveSettingsTo(HKEY root, const std::wstring& sub, const Settings& s) {
    HKEY k;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    WriteSettings(k, s);
    RegCloseKey(k);
    return true;
}
static bool SaveSettings(const Settings& s) { return SaveSettingsTo(HKEY_CURRENT_USER, REG_ROOT, s); }

static std::wstring SchemeKey(const std::wstring& name) { return std::wstring(REG_ROOT) + L"\\Schemes\\" + name; }

static std::vector<std::wstring> EnumSchemes() {
    std::vector<std::wstring> out;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(REG_ROOT) + L"\\Schemes").c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return out;
    wchar_t name[256];
    for (DWORD i = 0;; i++) {
        DWORD len = 256;
        if (RegEnumKeyExW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.push_back(name);
    }
    RegCloseKey(k);
    return out;
}

static bool LoadScheme(const std::wstring& name, Settings& s) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SchemeKey(name).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    ReadSettings(k, s);
    RegCloseKey(k);
    s.scheme = name;
    return true;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static std::wstring ModuleDir() {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p, n);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
}
static std::wstring ModulePath() {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    return std::wstring(p, n);
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// /showname and /showip (one-off runs and /render): stand-ins for this PC's name and IP address,
// so screenshots for documentation don't show the real ones.
static std::wstring g_showName, g_showIp;

static std::wstring RealComputerName() {
    wchar_t comp[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(comp, &n)) return L"";
    return comp;
}

static std::wstring LocalComputerName() {
    if (!g_showName.empty()) return g_showName;
    wchar_t comp[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(comp, &n)) return L"";
    return comp;
}

static std::wstring RefreshLabel(int sec) {
    wchar_t b[64];
    if (sec < 60)        swprintf(b, 64, sec == 1 ? L"%d Second" : L"%d Seconds", sec);
    else if (sec < 3600) swprintf(b, 64, sec == 60 ? L"%d Minute" : L"%d Minutes", sec / 60);
    else                 swprintf(b, 64, L"1 Hour");
    return b;
}

// ---- Computer names and network cards (Modern theme's title bar) ----

static const wchar_t NONE_VALUE[] = L"(none)";   // HostName / IpAdapter: show nothing

// Every name this PC goes by: its NetBIOS name, DNS host name, full DNS name, and any alternate
// names (Tcpip AlternateComputerNames, as set by "netdom computername /add", and LanmanServer
// OptionalNames). Duplicates are dropped, ignoring case.
static std::vector<std::wstring> ComputerNames() {
    std::vector<std::wstring> out;
    auto add = [&](std::wstring n) {
        n = Trim(n);
        if (n.empty()) return;
        for (const auto& x : out) if (_wcsicmp(x.c_str(), n.c_str()) == 0) return;
        out.push_back(n);
    };
    add(LocalComputerName());
    wchar_t b[256];
    for (COMPUTER_NAME_FORMAT f : { ComputerNameDnsHostname, ComputerNameDnsFullyQualified }) {
        DWORD n = 256;
        if (GetComputerNameExW(f, b, &n)) add(b);
    }
    auto multi = [&](const wchar_t* key, const wchar_t* val) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
        DWORD type = 0, sz = 0;
        if (RegQueryValueExW(k, val, nullptr, &type, nullptr, &sz) == ERROR_SUCCESS && sz &&
            (type == REG_MULTI_SZ || type == REG_SZ)) {
            std::vector<wchar_t> buf(sz / sizeof(wchar_t) + 2, L'\0');
            if (RegQueryValueExW(k, val, nullptr, &type, (BYTE*)buf.data(), &sz) == ERROR_SUCCESS)
                for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) add(p);
        }
        RegCloseKey(k);
    };
    multi(L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", L"AlternateComputerNames");
    multi(L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters", L"OptionalNames");
    return out;
}

// The name the Modern title bar shows for a HostName setting.
static std::wstring ShownHostName(const std::wstring& setting) {
    if (setting == NONE_VALUE) return L"";
    return setting.empty() ? LocalComputerName() : setting;
}

// Connected network cards and their addresses: the first IPv4 address, and the IPv6 addresses
// worth showing (global 2000::/3 and unique local fc00::/7), longest-lived first so a card's
// stable address comes before temporary ones. Left out: self-assigned 169.254.x.x and fe80::
// link-local addresses (no real network), and IPv6 addresses not yet usable or being retired.
struct NetCard { std::wstring guid, name, ip; std::vector<std::wstring> ip6; ULONG ifIndex = 0, ifIndex6 = 0; };

// An IPv6 address in the standard short form (RFC 5952): lower-case hex, no leading zeros, and
// the longest run of two or more zero groups written as "::".
static std::wstring Ipv6String(const IN6_ADDR& a) {
    unsigned g[8];
    for (int i = 0; i < 8; i++) g[i] = (a.u.Byte[2 * i] << 8) | a.u.Byte[2 * i + 1];
    int bestAt = -1, bestLen = 0;
    for (int i = 0; i < 8;) {
        if (g[i]) { i++; continue; }
        int j = i;
        while (j < 8 && !g[j]) j++;
        if (j - i > bestLen && j - i >= 2) { bestAt = i; bestLen = j - i; }
        i = j;
    }
    std::wstring s;
    wchar_t h[8];
    for (int i = 0; i < 8; i++) {
        if (i == bestAt) { s += L"::"; i += bestLen - 1; continue; }
        if (!s.empty() && s.back() != L':') s += L':';
        swprintf(h, 8, L"%x", g[i]);
        s += h;
    }
    return s;
}

static std::vector<NetCard> NetCards() {
    std::vector<NetCard> out;
    ULONG sz = 16 * 1024, r;
    std::vector<BYTE> buf;
    do {
        buf.resize(sz);
        r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &sz);
    } while (r == ERROR_BUFFER_OVERFLOW);
    if (r != NO_ERROR) return out;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        NetCard c;
        c.guid = FromUtf8(a->AdapterName);
        c.name = a->FriendlyName ? a->FriendlyName : L"";
        c.ifIndex = a->IfIndex;
        c.ifIndex6 = a->Ipv6IfIndex;
        std::vector<std::pair<ULONG, std::wstring>> v6;   // (valid lifetime, address)
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            int fam = u->Address.lpSockaddr->sa_family;
            if (fam == AF_INET && c.ip.empty()) {
                const BYTE* ip = (const BYTE*)&((sockaddr_in*)u->Address.lpSockaddr)->sin_addr;
                if (ip[0] == 169 && ip[1] == 254) continue;
                wchar_t s[32];
                swprintf(s, 32, L"%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
                c.ip = s;
            } else if (fam == AF_INET6) {
                const IN6_ADDR& in6 = ((sockaddr_in6*)u->Address.lpSockaddr)->sin6_addr;
                BYTE b0 = in6.u.Byte[0];
                bool global = (b0 & 0xE0) == 0x20, uniqueLocal = (b0 & 0xFE) == 0xFC;
                if ((!global && !uniqueLocal) || u->DadState != IpDadStatePreferred) continue;
                v6.push_back({ u->ValidLifetime, Ipv6String(in6) });
            }
        }
        std::stable_sort(v6.begin(), v6.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        for (const auto& p : v6) c.ip6.push_back(p.second);
        if (!c.ip.empty() || !c.ip6.empty()) out.push_back(c);
    }
    return out;
}

// IpAdapter values: "" = Default, "<card GUID>" = that card's IPv4 address,
// "<card GUID>|6|<address>" = that IPv6 address of that card.
static const wchar_t IPV6_MARK[] = L"|6|";

// The default: the card Windows would use to reach the internet (a routing-table lookup, no
// traffic is sent), with its IPv4 address; its IPv6 address only when there is no IPv4 route.
static std::wstring DefaultIp(const std::vector<NetCard>& cards) {
    DWORD idx = 0;
    if (GetBestInterface(0x08080808, &idx) == NO_ERROR)   // 8.8.8.8, any public address will do
        for (const auto& c : cards) if (c.ifIndex == idx && !c.ip.empty()) return c.ip;
    SOCKADDR_IN6 sa = {};
    sa.sin6_family = AF_INET6;
    static const BYTE g6[16] = { 0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0x88, 0x88 };   // 2001:4860:4860::8888
    memcpy(&sa.sin6_addr, g6, 16);
    if (GetBestInterfaceEx((sockaddr*)&sa, &idx) == NO_ERROR)
        for (const auto& c : cards) if (c.ifIndex6 == idx && !c.ip6.empty()) return c.ip6[0];
    for (const auto& c : cards) if (!c.ip.empty()) return c.ip;
    for (const auto& c : cards) if (!c.ip6.empty()) return c.ip6[0];
    return L"";
}

// The address an IpAdapter setting stands for now. A saved IPv6 address that is gone (a new
// prefix from the router) gives that card's current IPv6 address; a card that is gone, the default.
static std::wstring PickIp(const std::vector<NetCard>& cards, const std::wstring& setting) {
    if (!setting.empty()) {
        size_t mark = setting.find(IPV6_MARK);
        std::wstring guid = mark == std::wstring::npos ? setting : setting.substr(0, mark);
        std::wstring addr = mark == std::wstring::npos ? L"" : setting.substr(mark + 3);
        for (const auto& c : cards) {
            if (c.guid != guid) continue;
            if (mark == std::wstring::npos) { if (!c.ip.empty()) return c.ip; }
            else {
                for (const auto& a : c.ip6) if (_wcsicmp(a.c_str(), addr.c_str()) == 0) return a;
                if (!c.ip6.empty()) return c.ip6[0];
            }
        }
    }
    return DefaultIp(cards);
}

// The IP address the Modern title bar shows for an IpAdapter setting. Looked up at most every
// 15 seconds, so an address that changes (DHCP, a new network) is picked up.
static std::wstring ShownIp(const std::wstring& setting) {
    if (setting == NONE_VALUE) return L"";
    if (!g_showIp.empty()) return g_showIp;
    static ULONGLONG last = 0;
    static std::wstring lastSetting, lastIp;
    ULONGLONG now = GetTickCount64();
    if (last && now - last < 15000 && setting == lastSetting) return lastIp;
    lastIp = PickIp(NetCards(), setting);
    last = now; lastSetting = setting;
    return lastIp;
}

static std::wstring GroupThousands(const std::wstring& digits) {
    std::wstring s = digits, sign;
    if (!s.empty() && s[0] == L'-') { sign = L"-"; s.erase(0, 1); }
    std::wstring out;
    int cnt = 0;
    for (size_t i = s.size(); i-- > 0;) {
        out.insert(out.begin(), s[i]);
        if (++cnt % 3 == 0 && i > 0) out.insert(out.begin(), L',');
    }
    return sign + out;
}

static std::wstring FmtValue(double v) {
    if (!std::isfinite(v)) return L"---";
    wchar_t b[64];
    double a = fabs(v);
    if (a >= 1e15)          swprintf(b, 64, L"%.4e", v);
    else if (a >= 100000)   { swprintf(b, 64, L"%.0f", v); return GroupThousands(b); }
    else if (a >= 100)      swprintf(b, 64, L"%.1f", v);
    else if (a >= 0.001 || a == 0) swprintf(b, 64, L"%.3f", v);
    else                    swprintf(b, 64, L"%.3e", v);
    return b;
}

static std::wstring ScaleStr(int exp) {
    wchar_t b[32];
    if (exp >= 0) swprintf(b, 32, L"%.0f", pow(10.0, exp));
    else          swprintf(b, 32, L"%.*f", -exp, pow(10.0, exp));
    return b;
}

// "% Processor Time of Processor(0) on \\MACHINE" - same wording as the original.
// omitLocal (Hide the computer name in counter names): leave " on \\THISPC" off counters read
// from this PC. Counters on other computers keep their computer name.
static std::wstring DescribeCounter(const std::wstring& path, bool omitLocal = false) {
    if (path.empty()) return L"(no counter selected - click Browse...)";
    DWORD sz = 0;
    PDH_STATUS st = PdhParseCounterPathW(path.c_str(), nullptr, &sz, 0);
    if (st == PDH_MORE_DATA && sz > 0) {
        std::vector<BYTE> buf(sz);
        auto* e = (PDH_COUNTER_PATH_ELEMENTS_W*)buf.data();
        if (PdhParseCounterPathW(path.c_str(), e, &sz, 0) == ERROR_SUCCESS && e->szCounterName && e->szObjectName) {
            std::wstring d = e->szCounterName;
            d += L" of ";
            d += e->szObjectName;
            if (e->szInstanceName && *e->szInstanceName) {
                d += L"(";
                if (e->szParentInstance && *e->szParentInstance) { d += e->szParentInstance; d += L"/"; }
                d += e->szInstanceName;
                if (e->dwInstanceIndex != (DWORD)-1 && e->dwInstanceIndex != 0) d += L"#" + std::to_wstring(e->dwInstanceIndex);
                d += L")";
            }
            // PDH fills in this PC's own name for local counters; /showname stands in for it.
            std::wstring mach = (e->szMachineName && *e->szMachineName) ? e->szMachineName : L"";
            std::wstring bare = mach.size() > 2 && mach[0] == L'\\' && mach[1] == L'\\' ? mach.substr(2) : mach;
            bool local = mach.empty() || _wcsicmp(bare.c_str(), RealComputerName().c_str()) == 0;
            if (omitLocal && local) return d;
            d += L" on ";
            if (!g_showName.empty() && !mach.empty() && local)
                d += L"\\\\" + g_showName;
            else if (!mach.empty()) d += mach;
            else d += L"\\\\" + LocalComputerName();
            return d;
        }
    }
    return path;
}

// Title for the Explain box: "% Processor Time of Processor(0)"
static std::wstring CounterTitle(const std::wstring& path) {
    std::wstring d = DescribeCounter(path);
    size_t on = d.rfind(L" on \\\\");
    return on == std::wstring::npos ? d : d.substr(0, on);
}

// Adds a counter by its localized path, falling back to the English path.
static PDH_STATUS AddCounterAnyLang(PDH_HQUERY q, const std::wstring& path, PDH_HCOUNTER* h) {
    PDH_STATUS st = PdhAddCounterW(q, path.c_str(), 0, h);
    if (st != ERROR_SUCCESS) st = PdhAddEnglishCounterW(q, path.c_str(), 0, h);
    return st;
}

static LONG CounterDefaultScale(PDH_HCOUNTER h) {
    DWORD sz = 0;
    if (PdhGetCounterInfoW(h, FALSE, &sz, nullptr) != PDH_MORE_DATA || sz == 0) return 0;
    std::vector<BYTE> buf(sz);
    if (PdhGetCounterInfoW(h, FALSE, &sz, (PPDH_COUNTER_INFO_W)buf.data()) != ERROR_SUCCESS) return 0;
    return ((PPDH_COUNTER_INFO_W)buf.data())->lDefaultScale;
}

// Explain text straight from the counter provider.
static std::wstring ExplainCounterNative(const std::wstring& path) {
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return L"Unable to open a performance query.";
    std::wstring text;
    PDH_HCOUNTER h = nullptr;
    PDH_STATUS st = AddCounterAnyLang(q, path, &h);
    if (st != ERROR_SUCCESS) {
        wchar_t b[128];
        swprintf(b, 128, L"This counter is not available (PDH status 0x%08X).", (unsigned)st);
        text = b;
    } else {
        DWORD sz = 0;
        if (PdhGetCounterInfoW(h, TRUE, &sz, nullptr) == PDH_MORE_DATA && sz) {
            std::vector<BYTE> buf(sz);
            auto* ci = (PPDH_COUNTER_INFO_W)buf.data();
            if (PdhGetCounterInfoW(h, TRUE, &sz, ci) == ERROR_SUCCESS && ci->szExplainText && *ci->szExplainText)
                text = ci->szExplainText;
        }
        if (text.empty()) text = L"No explanation is available for this counter.";
    }
    PdhCloseQuery(q);
    return text;
}

// True when this process (64-bit or 32-bit) can add the counter.
static bool NativeSees(const std::wstring& path) {
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return false;
    PDH_HCOUNTER h = nullptr;
    bool ok = AddCounterAnyLang(q, path, &h) == ERROR_SUCCESS;
    PdhCloseQuery(q);
    return ok;
}

// Markers for counters read through the 32-bit side.
static const wchar_t TAG_32[]     = L"  [32-bit]";    // read through the 32-bit providers
static const wchar_t TAG_32ONLY[] = L"  [32-bit*]";   // and the 64-bit side cannot see it at all

// The marker the 32-bit build puts on everything it reads. On 32-bit Windows there is no
// 64-bit side and every counter is 32-bit, so it shows nothing there; only on 64-bit Windows
// (running under WOW64) does it say [32-bit].
[[maybe_unused]] static const wchar_t* Own32Tag() {
#ifdef _WIN64
    return L"";
#else
    static int wow = -1;
    if (wow < 0) { BOOL w = FALSE; wow = IsWow64Process(GetCurrentProcess(), &w) && w ? 1 : 0; }
    return wow ? TAG_32 : L"";
#endif
}

static PDH_STATUS __stdcall BrowseCallback(DWORD_PTR) { return ERROR_SUCCESS; }

// ---------------------------------------------------------------------------
// Browse dialog enhancements. The dialog belongs to Windows (PDH), so a CBT hook
// catches it as it opens and subclasses it to add: a resizable frame with a size
// grip (the lists and description grow), a hint explaining the counter detail
// level, a tooltip for the instance search box, and a remembered size.
// ---------------------------------------------------------------------------

// Control IDs inside the Windows PDH browse dialog (pdh.dll, Windows 10/11).
enum {
    PB_GROUP = 1019, PB_MACHLABEL = 1000, PB_MACHINE = 1001, PB_MACHINE2 = 1002, PB_MACHBROWSE = 1003,
    PB_DETLABEL = 1004, PB_DETAIL = 1005, PB_COUNTERS = 1006, PB_INSTLABEL = 1010, PB_INSTANCES = 1011,
    PB_SEARCHBOX = 1012, PB_SEARCH = 1013, PB_DESCLABEL = 1007, PB_SHOWDESC = 1008, PB_DESC = 1009,
};
static const UINT WM_BROWSE_SETUP = WM_APP + 77;
static const UINT_PTR BROWSE_TIP_SEARCHBTN = 1;   // tooltip area id (dialog-owned)

struct BrowseCtl { int id; RECT r; };     // base rect, client coordinates
static struct {
    HWND dlg = nullptr, grip = nullptr, hint = nullptr, tip = nullptr;
    bool ready = false;
    int baseW = 0, baseH = 0;             // client size at start, description hidden
    int descBlock = 0;                    // client height the description area adds
    SIZE minWin = {};                     // window size at start, description hidden
    std::vector<BrowseCtl> ctls;
} g_bl;
static HHOOK g_browseHook = nullptr;

static const wchar_t* kDetailHints[] = {
    L"Novice: the basic counters most people need.",
    L"Advanced: adds counters for experienced users.",
    L"Expert: adds counters for in-depth tuning.",
    L"Wizard: every counter, including low-level ones.",
};

static bool BrowseDescShown(HWND d) { return IsWindowVisible(GetDlgItem(d, PB_DESC)) != FALSE; }

// 32-bit Browse only: objects the 64-bit side can see (empty = don't mark anything). Each object
// is a group in the counter list; groups not in this list get TAG_32ONLY added to their header.
static std::vector<std::wstring> g_objs64;
static const UINT_PTR BROWSE_TAG_TIMER = 77;
static bool IsRemoteCounterPath(const std::wstring& path);

static void BrowseTagGroups(HWND d) {
    if (g_objs64.empty()) return;
    // Only this PC's objects can be compared; another computer's list is the same from both sides.
    wchar_t mach[256] = {};
    GetDlgItemTextW(d, PB_MACHINE, mach, 256);
    if (mach[0] && mach[0] != L'<' && IsRemoteCounterPath(std::wstring(L"\\\\") + (mach[0] == L'\\' ? mach + 2 : mach)))
        return;
    HWND lv = GetDlgItem(d, PB_COUNTERS);
    int n = (int)SendMessageW(lv, LVM_GETGROUPCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        wchar_t name[512] = {};
        LVGROUP gi = {};
        gi.cbSize = sizeof(gi);
        gi.mask = LVGF_HEADER | LVGF_GROUPID;
        gi.pszHeader = name;
        gi.cchHeader = 512;
        if (!SendMessageW(lv, LVM_GETGROUPINFOBYINDEX, (WPARAM)i, (LPARAM)&gi)) continue;
        std::wstring h = name;
        if (h.empty() || h.find(L"[32-bit*]") != std::wstring::npos) continue;
        bool seen = false;
        for (const std::wstring& o : g_objs64) if (!_wcsicmp(o.c_str(), h.c_str())) { seen = true; break; }
        if (seen) continue;
        h += TAG_32ONLY;
        LVGROUP si = {};
        si.cbSize = sizeof(si);
        si.mask = LVGF_HEADER;
        si.pszHeader = &h[0];
        SendMessageW(lv, LVM_SETGROUPINFO, (WPARAM)gi.iGroupId, (LPARAM)&si);
    }
}

static void BrowseUpdateHint(HWND d) {
    LRESULT i = SendDlgItemMessageW(d, PB_DETAIL, CB_GETCURSEL, 0, 0);
    if (g_bl.hint) SetWindowTextW(g_bl.hint, (i >= 0 && i < 4) ? kDetailHints[i] : L"");
}

static void BrowseLayoutNow(HWND d) {
    if (!g_bl.ready) return;
    RECT cr; GetClientRect(d, &cr);
    bool desc = BrowseDescShown(d);
    int dx = std::max(0, (int)cr.right - g_bl.baseW);
    int extra = std::max(0, (int)cr.bottom - g_bl.baseH - (desc ? g_bl.descBlock : 0));
    // Extra height goes to the counter list, the instance list and (when shown) the description.
    int addList = desc ? extra * 2 / 5 : extra * 3 / 5;
    int addInst = desc ? extra / 5 : extra - addList;
    int addDesc = desc ? extra - addList - addInst : 0;
    int sh = addList + addInst;

    HDWP dw = BeginDeferWindowPos((int)g_bl.ctls.size() + 2);
    RECT detail = {}, group = {}, searchBtn = {};
    for (const BrowseCtl& c : g_bl.ctls) {
        RECT r = c.r;
        int x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top;
        switch (c.id) {
        case PB_GROUP:      w += dx; h += sh; group = { x, y, x + w, y + h }; break;
        case PB_MACHINE: case PB_MACHINE2: w += dx; h += 200; break;   // combo: include drop-down
        case PB_MACHBROWSE: x += dx; break;
        case PB_DETAIL:     detail = r; h += 200; break;
        case PB_COUNTERS:   w += dx; h += addList; break;
        case PB_INSTLABEL:  y += addList; break;
        case PB_INSTANCES:  y += addList; w += dx; h += addInst; break;
        case PB_SEARCHBOX:  y += sh; w += dx; h += 200; break;
        case PB_SEARCH:     x += dx; y += sh; searchBtn = { x, y, x + w, y + h }; break;
        case PB_SHOWDESC: case PB_DESCLABEL: y += sh; break;
        case IDOK: case IDCANCEL: x += dx; y += sh; break;
        case PB_DESC:       // keep the bottom-right corner clear for the size grip
            y += sh; w += dx; h += addDesc;
            h = std::min(h, (int)cr.bottom - GetSystemMetrics(SM_CYHSCROLL) - y);
            break;
        default: break;
        }
        HWND hc = GetDlgItem(d, c.id);
        if (hc && dw) dw = DeferWindowPos(dw, hc, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_bl.hint && dw) {
        RECT lab = {};
        for (const BrowseCtl& c : g_bl.ctls) if (c.id == PB_DETLABEL) lab = c.r;
        int x0 = detail.right + 10, x1 = group.right - 10;
        dw = DeferWindowPos(dw, g_bl.hint, nullptr, x0, lab.top, std::max(10, x1 - x0), detail.bottom - lab.top,
                            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_bl.grip && dw) {
        int gx = GetSystemMetrics(SM_CXVSCROLL), gy = GetSystemMetrics(SM_CYHSCROLL);
        dw = DeferWindowPos(dw, g_bl.grip, HWND_TOP, cr.right - gx, cr.bottom - gy, gx, gy, SWP_NOACTIVATE);
    }
    if (dw) EndDeferWindowPos(dw);
    if (g_bl.tip) {
        // Area tool on the dialog itself: a disabled control gets no mouse messages (they go
        // to the dialog), so this is what shows the tip when the Search button is grayed.
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.hwnd = d;
        ti.uId = BROWSE_TIP_SEARCHBTN;
        ti.rect = searchBtn;
        SendMessageW(g_bl.tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
    RedrawWindow(d, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void BrowseAddTip(HWND tip, HWND owner, HWND ctl, const wchar_t* text) {
    if (!ctl) return;
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = owner;
    ti.uId = (UINT_PTR)ctl;
    ti.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

// A tip over a control's area of the dialog itself. Needed where the control can be disabled
// or ignores the mouse (plain statics): the mouse messages then go to the dialog, not the control.
static void AddAreaTip(HWND tip, HWND dlg, HWND ctl, UINT_PTR id, const wchar_t* text) {
    if (!ctl) return;
    RECT r;
    GetWindowRect(ctl, &r);
    MapWindowPoints(nullptr, dlg, (POINT*)&r, 2);
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = dlg;
    ti.uId = id;
    ti.rect = r;
    ti.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

// A tip on a control that also works while it is disabled.
static void AddTipAlways(HWND tip, HWND dlg, int ctlId, const wchar_t* text) {
    HWND ctl = GetDlgItem(dlg, ctlId);
    BrowseAddTip(tip, dlg, ctl, text);
    AddAreaTip(tip, dlg, ctl, 0x10000 + ctlId, text);
}

static void BrowseSetup(HWND d) {
    g_bl.ctls.clear();
    const int ids[] = { PB_GROUP, PB_MACHLABEL, PB_MACHINE, PB_MACHINE2, PB_MACHBROWSE, PB_DETLABEL, PB_DETAIL,
                        PB_COUNTERS, PB_INSTLABEL, PB_INSTANCES, PB_SEARCHBOX, PB_SEARCH, PB_SHOWDESC,
                        PB_DESCLABEL, PB_DESC, IDOK, IDCANCEL };
    RECT ok = {}, desc = {};
    for (int id : ids) {
        HWND c = GetDlgItem(d, id);
        if (!c) continue;
        RECT r; GetWindowRect(c, &r);
        MapWindowPoints(nullptr, d, (POINT*)&r, 2);    // combos report their closed height
        if (id == IDOK) ok = r;
        if (id == PB_DESC) desc = r;
        g_bl.ctls.push_back({ id, r });
    }
    if (!ok.bottom || !desc.bottom) return;        // unexpected layout: leave the dialog alone
    g_bl.descBlock = desc.bottom - ok.bottom;
    bool descNow = BrowseDescShown(d);

    // Add a sizing frame, keeping the client area the same size.
    RECT c0; GetClientRect(d, &c0);
    SetWindowLongPtrW(d, GWL_STYLE, GetWindowLongPtrW(d, GWL_STYLE) | WS_THICKFRAME);
    SetWindowPos(d, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    // A fixed dialog's system menu has no Size command, and without it Windows ignores
    // sizing drags on the border and the grip. Add it back (after Move).
    if (HMENU sm = GetSystemMenu(d, FALSE)) {
        if (GetMenuState(sm, SC_SIZE, MF_BYCOMMAND) == (UINT)-1) {
            int pos = 0, n = GetMenuItemCount(sm);
            for (int i = 0; i < n; i++) if (GetMenuItemID(sm, i) == SC_MOVE) { pos = i + 1; break; }
            InsertMenuW(sm, pos, MF_BYPOSITION | MF_STRING, SC_SIZE, L"&Size");
        } else {
            EnableMenuItem(sm, SC_SIZE, MF_BYCOMMAND | MF_ENABLED);
        }
    }
    RECT c1, w1; GetClientRect(d, &c1); GetWindowRect(d, &w1);
    int ww = (w1.right - w1.left) + (c0.right - c1.right), wh = (w1.bottom - w1.top) + (c0.bottom - c1.bottom);
    SetWindowPos(d, nullptr, 0, 0, ww, wh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    g_bl.baseW = c0.right;
    g_bl.baseH = c0.bottom - (descNow ? g_bl.descBlock : 0);
    g_bl.minWin = { ww, wh - (descNow ? g_bl.descBlock : 0) };

    HFONT font = (HFONT)SendMessageW(d, WM_GETFONT, 0, 0);
    g_bl.hint = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_NOTIFY,
                                0, 0, 10, 10, d, nullptr, nullptr, nullptr);
    SendMessageW(g_bl.hint, WM_SETFONT, (WPARAM)font, FALSE);
    g_bl.grip = CreateWindowExW(0, L"SCROLLBAR", nullptr, WS_CHILD | WS_VISIBLE | SBS_SIZEGRIP | SBS_SIZEBOXBOTTOMRIGHTALIGN,
                                0, 0, 10, 10, d, nullptr, nullptr, nullptr);

    g_bl.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX | TTS_BALLOON,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, d, nullptr, nullptr, nullptr);
    if (g_bl.tip) {
        SendMessageW(g_bl.tip, TTM_SETMAXTIPWIDTH, 0, 360);
        SendMessageW(g_bl.tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
        static const wchar_t kDetailTip[] =
            L"Counter detail level filters which counters are listed.\n"
            L"Novice: basic counters.  Advanced: adds counters for experienced users.\n"
            L"Expert: adds in-depth tuning counters.  Wizard: lists every counter.\n"
            L"Each level includes all the levels below it.";
        static const wchar_t kSearchTip[] =
            L"Instance search: type part of an instance name (wildcards * and ? work), then click Search. "
            L"Matching instances are added to the list above, with <All searched instances> to pick every match.\n\n"
            L"QuickMon Forever charts one line per counter, so choose a single instance (such as _Total) "
            L"rather than <All instances> or <All searched instances>.";
        HWND det = GetDlgItem(d, PB_DETAIL);
        BrowseAddTip(g_bl.tip, d, det, kDetailTip);
        BrowseAddTip(g_bl.tip, d, GetWindow(det, GW_CHILD), kDetailTip);
        BrowseAddTip(g_bl.tip, d, g_bl.hint, kDetailTip);
        // Search button only: its own tool while enabled, plus an area on the dialog over the
        // button for while it is grayed out.
        BrowseAddTip(g_bl.tip, d, GetDlgItem(d, PB_SEARCH), kSearchTip);
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = d;
        ti.uId = BROWSE_TIP_SEARCHBTN;
        ti.lpszText = const_cast<LPWSTR>(kSearchTip);
        SendMessageW(g_bl.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);   // rect is set by BrowseLayoutNow
    }
    BrowseUpdateHint(d);
    g_bl.ready = true;
#ifndef _WIN64
    // The 32-bit Browse dialog (run by QuickMonForever-32.exe) says so in its title, on 64-bit Windows.
    SetWindowTextW(d, (std::wstring(L"Browse Performance Counters") + Own32Tag()).c_str());
#endif
    // The list is refilled when the detail level or computer changes, so re-mark it on a timer.
    if (!g_objs64.empty()) { BrowseTagGroups(d); SetTimer(d, BROWSE_TAG_TIMER, 300, nullptr); }

    // Restore the size the user last chose.
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_ROOT, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        int sw = (int)RegGetDw(k, L"BrowseWidth", 0), sh = (int)RegGetDw(k, L"BrowseHeight", 0);
        RegCloseKey(k);
        if (sw > g_bl.minWin.cx || sh > g_bl.minWin.cy) {
            sw = std::max(sw, (int)g_bl.minWin.cx);
            sh = std::max(sh, (int)g_bl.minWin.cy) + (descNow ? g_bl.descBlock : 0);
            RECT wr; GetWindowRect(d, &wr);
            HMONITOR mon = MonitorFromWindow(d, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfoW(mon, &mi);
            sw = std::min(sw, (int)(mi.rcWork.right - mi.rcWork.left));
            sh = std::min(sh, (int)(mi.rcWork.bottom - mi.rcWork.top));
            int x = std::clamp((int)wr.left, (int)mi.rcWork.left, (int)mi.rcWork.right - sw);
            int y = std::clamp((int)wr.top, (int)mi.rcWork.top, (int)mi.rcWork.bottom - sh);
            SetWindowPos(d, nullptr, x, y, sw, sh, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    BrowseLayoutNow(d);
}

static void BrowseSaveSize(HWND d) {
    if (!g_bl.ready) return;
    RECT wr; GetWindowRect(d, &wr);
    int h = (wr.bottom - wr.top) - (BrowseDescShown(d) ? g_bl.descBlock : 0);
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_ROOT, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetDw(k, L"BrowseWidth", (DWORD)(wr.right - wr.left));
        RegSetDw(k, L"BrowseHeight", (DWORD)std::max(0, h));
        RegCloseKey(k);
    }
}

static LRESULT CALLBACK BrowseSubclassProc(HWND d, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    switch (m) {
    case WM_BROWSE_SETUP:
        BrowseSetup(d);
        return 0;
    case WM_GETMINMAXINFO:
        if (g_bl.ready) {
            LRESULT r = DefSubclassProc(d, m, w, l);
            MINMAXINFO* mm = (MINMAXINFO*)l;
            mm->ptMinTrackSize.x = g_bl.minWin.cx;
            mm->ptMinTrackSize.y = g_bl.minWin.cy + (BrowseDescShown(d) ? g_bl.descBlock : 0);
            return r;
        }
        break;
    case WM_SIZE: {
        LRESULT r = DefSubclassProc(d, m, w, l);
        BrowseLayoutNow(d);
        return r;
    }
    case WM_COMMAND:
        if (g_bl.ready && LOWORD(w) == PB_SHOWDESC && HIWORD(w) == BN_CLICKED) {
            // The dialog grows or shrinks itself for the description; keep the user's size otherwise.
            RECT before; GetWindowRect(d, &before);
            bool was = BrowseDescShown(d);
            LRESULT r = DefSubclassProc(d, m, w, l);
            bool now = BrowseDescShown(d);
            int H = before.bottom - before.top;
            if (now != was) H += now ? g_bl.descBlock : -g_bl.descBlock;
            SetWindowPos(d, nullptr, 0, 0, before.right - before.left, H, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            BrowseLayoutNow(d);
            return r;
        }
        if (g_bl.ready && LOWORD(w) == PB_DETAIL && HIWORD(w) == CBN_SELCHANGE) {
            LRESULT r = DefSubclassProc(d, m, w, l);
            BrowseUpdateHint(d);
            return r;
        }
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) BrowseSaveSize(d);
        break;
    case WM_TIMER:
        if (w == BROWSE_TAG_TIMER) { BrowseTagGroups(d); return 0; }
        break;
    case WM_NCDESTROY:
        KillTimer(d, BROWSE_TAG_TIMER);
        g_bl.ready = false;
        RemoveWindowSubclass(d, BrowseSubclassProc, 1);
        break;
    }
    return DefSubclassProc(d, m, w, l);
}

static LRESULT CALLBACK BrowseCbtProc(int code, WPARAM w, LPARAM l) {
    if (code == HCBT_ACTIVATE && !g_bl.dlg) {
        HWND h = (HWND)w;
        wchar_t cls[64] = {}, lv[64] = {}, lb[64] = {};
        GetClassNameW(h, cls, 64);
        HWND hl = GetDlgItem(h, PB_COUNTERS), hb = GetDlgItem(h, PB_INSTANCES);
        if (hl) GetClassNameW(hl, lv, 64);
        if (hb) GetClassNameW(hb, lb, 64);
        if (!wcscmp(cls, L"#32770") && !wcscmp(lv, WC_LISTVIEWW) && !_wcsicmp(lb, L"ListBox")) {
            g_bl.dlg = h;
            SetWindowSubclass(h, BrowseSubclassProc, 1, 0);
            PostMessageW(h, WM_BROWSE_SETUP, 0, 0);
        }
    }
    return CallNextHookEx(g_browseHook, code, w, l);
}

// True when a counter path names another computer: \\MACHINE\Object\Counter where MACHINE is
// not this PC (by NetBIOS or DNS name), "localhost" or ".".
static bool IsRemoteCounterPath(const std::wstring& path) {
    if (path.size() < 3 || path[0] != L'\\' || path[1] != L'\\') return false;
    size_t end = path.find(L'\\', 2);
    std::wstring m = path.substr(2, end == std::wstring::npos ? std::wstring::npos : end - 2);
    if (m.empty() || !_wcsicmp(m.c_str(), L"localhost") || m == L".") return false;
    wchar_t name[256]; DWORD n = 256;
    if (GetComputerNameExW(ComputerNameNetBIOS, name, &n) && !_wcsicmp(m.c_str(), name)) return false;
    n = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, name, &n) && !_wcsicmp(m.c_str(), name)) return false;
    n = 256;
    if (GetComputerNameExW(ComputerNameDnsFullyQualified, name, &n) && !_wcsicmp(m.c_str(), name)) return false;
    return true;
}

// The standard Windows "Browse Performance Counters" dialog (the same one QuickMon used).
static bool BrowseCounterNative(HWND owner, DWORD detail, std::wstring& path) {
    for (int attempt = 0; attempt < 2; attempt++) {
        std::vector<wchar_t> buf(PDH_MAX_COUNTER_PATH * 4, L'\0');
        // Pre-select the current counter only when it is on another computer (\\MACHINE\...).
        // Pre-selecting a local counter makes the dialog switch to this PC by name, and in that
        // mode it fetches instances the remote way, which fails when Remote Registry is off:
        // clicking a counter then leaves the instance list empty and disabled.
        bool init = attempt == 0 && IsRemoteCounterPath(path);
        if (init) wcsncpy_s(buf.data(), buf.size(), path.c_str(), _TRUNCATE);
        PDH_BROWSE_DLG_CONFIG_W cfg = {};
        cfg.bIncludeInstanceIndex    = FALSE;
        cfg.bSingleCounterPerAdd     = TRUE;
        cfg.bSingleCounterPerDialog  = TRUE;
        cfg.bLocalCountersOnly       = FALSE;
        cfg.bWildCardInstances       = FALSE;
        cfg.bHideDetailBox           = FALSE;
        cfg.bInitializePath          = init ? TRUE : FALSE;
        cfg.bDisableMachineSelection = FALSE;
        cfg.bIncludeCostlyObjects    = FALSE;
        cfg.bShowObjectBrowser       = FALSE;
        cfg.hWndOwner                = owner;
        cfg.szDataSource             = nullptr;
        cfg.szReturnPathBuffer       = buf.data();
        cfg.cchReturnPathLength      = (DWORD)buf.size();
        cfg.pCallBack                = BrowseCallback;
        cfg.dwCallBackArg            = 0;
        cfg.CallBackStatus           = ERROR_SUCCESS;
        cfg.dwDefaultDetailLevel     = detail;
        cfg.szDialogBoxCaption       = const_cast<LPWSTR>(L"Browse Performance Counters");
        g_bl.dlg = nullptr;
        g_bl.ready = false;
        g_bl.hint = g_bl.grip = g_bl.tip = nullptr;
        g_browseHook = SetWindowsHookExW(WH_CBT, BrowseCbtProc, nullptr, GetCurrentThreadId());
        PDH_STATUS st = PdhBrowseCountersW(&cfg);
        if (g_browseHook) { UnhookWindowsHookEx(g_browseHook); g_browseHook = nullptr; }
        if (st == ERROR_SUCCESS) {
            if (!buf[0]) return false;      // OK with nothing selected
            path = buf.data();
            // In case the dialog builds the path from a marked group header, drop the marker.
            for (size_t at; (at = path.find(TAG_32ONLY)) != std::wstring::npos; ) path.erase(at, wcslen(TAG_32ONLY));
            return true;
        }
        if (st == PDH_DIALOG_CANCELLED) return false;
        if (!init) return false;            // retry once without the initial path
    }
    return false;
}

// ---------------------------------------------------------------------------
// 32-bit helper process (used only by the 64-bit build)
// ---------------------------------------------------------------------------

static bool HelperAvailable() {
#ifdef _WIN64
    return GetFileAttributesW((ModuleDir() + HELPER_EXE).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    return false;
#endif
}

// Runs the helper with arguments and captures its stdout. If input is given, it is handed to
// the helper on its stdin through a private pipe (nothing is stored anywhere). Returns the exit
// code, or -1.
static int RunHelperCapture(const std::wstring& args, std::wstring& out, const std::string* input = nullptr) {
    std::wstring exe = ModuleDir() + HELPER_EXE;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE inRd = nullptr, inWr = nullptr;
    if (input) {
        // Big enough for the whole input, so writing it never waits on the helper.
        if (!CreatePipe(&inRd, &inWr, &sa, (DWORD)input->size() + 4096)) { CloseHandle(rd); CloseHandle(wr); return -1; }
        SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
    }
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRd; si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (inRd) CloseHandle(inRd);
    if (inWr) {
        if (ok && !input->empty()) { DWORD n = 0; WriteFile(inWr, input->data(), (DWORD)input->size(), &n, nullptr); }
        CloseHandle(inWr);                     // end of input: the helper reads up to here
    }
    if (!ok) { CloseHandle(rd); return -1; }
    std::string data;
    char chunk[4096];
    DWORD n;
    for (;;) {
        // Keep the owner dialog painting while the helper's modal dialog is open.
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;   // helper closed the pipe
        if (avail) {
            if (!ReadFile(rd, chunk, std::min<DWORD>(avail, sizeof(chunk)), &n, nullptr) || n == 0) break;
            data.append(chunk, n);
            continue;
        }
        if (MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 50, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        }
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    out = Trim(FromUtf8(data));
    return (int)code;
}

// Long-lived helper used while the screen saver runs: line protocol over pipes.
//   -> "A <idx> <path>"     add counter       <- "A <idx> <status> <defaultScale>"
//   -> "S"                  collect sample    <- "V <idx> <ok> <value>" ... "E"
//   -> "Q"                  quit
struct Helper32 {
    HANDLE proc = nullptr, toChild = nullptr, fromChild = nullptr;
    std::string buf;
    bool failed = false;

    bool Start() {
        if (proc) return true;
        if (failed || !HelperAvailable()) return false;
        std::wstring exe = ModuleDir() + HELPER_EXE;
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        HANDLE inRd, inWr, outRd, outWr;
        if (!CreatePipe(&inRd, &inWr, &sa, 0)) { failed = true; return false; }
        if (!CreatePipe(&outRd, &outWr, &sa, 0)) { CloseHandle(inRd); CloseHandle(inWr); failed = true; return false; }
        SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si = { sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inRd; si.hStdOutput = outWr; si.hStdError = outWr;
        PROCESS_INFORMATION pi = {};
        std::wstring cmd = L"\"" + exe + L"\" /helper32";
        BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(inRd); CloseHandle(outWr);
        if (!ok) { CloseHandle(inWr); CloseHandle(outRd); failed = true; return false; }
        CloseHandle(pi.hThread);
        proc = pi.hProcess; toChild = inWr; fromChild = outRd;
        return true;
    }
    bool Send(const std::wstring& line) {
        if (!proc) return false;
        std::string s = ToUtf8(line) + "\n";
        DWORD n;
        if (!WriteFile(toChild, s.data(), (DWORD)s.size(), &n, nullptr)) { Stop(); failed = true; return false; }
        return true;
    }
    bool ReadLine(std::string& line, DWORD timeoutMs) {
        ULONGLONG until = GetTickCount64() + timeoutMs;
        for (;;) {
            size_t nl = buf.find('\n');
            if (nl != std::string::npos) {
                line = buf.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buf.erase(0, nl + 1);
                return true;
            }
            DWORD avail = 0;
            if (!PeekNamedPipe(fromChild, nullptr, 0, nullptr, &avail, nullptr)) { Stop(); failed = true; return false; }
            if (avail) {
                char chunk[4096]; DWORD n;
                if (!ReadFile(fromChild, chunk, std::min<DWORD>(avail, sizeof(chunk)), &n, nullptr)) { Stop(); failed = true; return false; }
                buf.append(chunk, n);
                continue;
            }
            if (GetTickCount64() > until) return false;
            Sleep(2);
        }
    }
    void Stop() {
        if (!proc) return;
        DWORD n;
        WriteFile(toChild, "Q\n", 2, &n, nullptr);
        CloseHandle(toChild);
        if (WaitForSingleObject(proc, 1000) != WAIT_OBJECT_0) TerminateProcess(proc, 0);
        CloseHandle(fromChild); CloseHandle(proc);
        proc = toChild = fromChild = nullptr;
        buf.clear();
    }
};

// Helper side of the protocol (runs in the 32-bit process).
static int RunHelperServer() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return 1;
    PDH_HCOUNTER h[MAX_COUNTERS] = {};
    std::string buf;
    auto reply = [&](const std::string& s) { DWORD n; std::string t = s + "\n"; WriteFile(out, t.data(), (DWORD)t.size(), &n, nullptr); };
    for (;;) {
        size_t nl;
        while ((nl = buf.find('\n')) == std::string::npos) {
            char chunk[4096]; DWORD n = 0;
            if (!ReadFile(in, chunk, sizeof(chunk), &n, nullptr) || n == 0) { PdhCloseQuery(q); return 0; }
            buf.append(chunk, n);
        }
        std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        char cmd = line[0];
        if (cmd == 'Q') break;
        if (cmd == 'A') {
            int idx = -1, consumed = 0;
            if (sscanf_s(line.c_str(), "A %d %n", &idx, &consumed) < 1 || idx < 0 || idx >= MAX_COUNTERS) continue;
            std::wstring path = FromUtf8(line.substr(consumed));
            PDH_STATUS st = AddCounterAnyLang(q, path, &h[idx]);
            LONG def = st == ERROR_SUCCESS ? CounterDefaultScale(h[idx]) : 0;
            if (st != ERROR_SUCCESS) h[idx] = nullptr;
            char b[96];
            sprintf_s(b, "A %d %u %ld", idx, (unsigned)st, def);
            reply(b);
            PdhCollectQueryData(q);
        } else if (cmd == 'S') {
            PdhCollectQueryData(q);
            for (int i = 0; i < MAX_COUNTERS; i++) {
                if (!h[i]) continue;
                PDH_FMT_COUNTERVALUE v = {};
                PDH_STATUS st = PdhGetFormattedCounterValue(h[i], PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v);
                bool ok = st == ERROR_SUCCESS && (v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA);
                char b[96];
                sprintf_s(b, "V %d %d %.17g", i, ok ? 1 : 0, ok ? v.doubleValue : 0.0);
                reply(b);
            }
            reply("E");
        }
    }
    PdhCloseQuery(q);
    return 0;
}

// ---------------------------------------------------------------------------
// Data collection and auto-scaling
// ---------------------------------------------------------------------------

struct CounterState {
    bool               active   = false;   // enabled and has a path
    bool               ok       = false;   // counter was added successfully
    bool               via32    = false;   // sampled by the 32-bit helper
    bool               only32   = false;   // ...because the 64-bit side cannot see it
    PDH_HCOUNTER       h        = nullptr;
    PDH_STATUS         err      = 0;
    LONG               defScale = 0;
    int                exp      = 0;       // current scale factor = 10^exp
    ULONGLONG          lastHigh = 0;
    std::deque<float>  ys;                 // plotted value 0..100 (NaN = no data)
    std::deque<double> raw;                // unscaled value (NaN = no data)
};

struct Monitor {
    Settings                    s;
    PDH_HQUERY                  q = nullptr;
    CounterState                cs[MAX_COUNTERS];
    std::deque<unsigned short>  marks;     // bit i set: counter i rescaled at this sample
    Helper32                    helper;

    void Open() {
        PdhOpenQueryW(nullptr, 0, &q);
        ULONGLONG now = GetTickCount64();
        for (int i = 0; i < MAX_COUNTERS; i++) {
            const CounterCfg& c = s.c[i];
            CounterState& st = cs[i];
            st.active = c.enabled && !c.path.empty();
            if (!st.active) continue;
            bool tryNative = true;
#ifdef _WIN64
            if (c.use32 && HelperAvailable()) tryNative = false;
#endif
            if (tryNative && q) {
                st.err = AddCounterAnyLang(q, c.path, &st.h);
                st.ok = st.err == ERROR_SUCCESS;
                if (st.ok) st.defScale = CounterDefaultScale(st.h);
            }
#ifdef _WIN64
            if (!st.ok && helper.Start()) {          // 32-bit-only counter (or user asked for it)
                helper.Send(L"A " + std::to_wstring(i) + L" " + c.path);
                std::string line;
                if (helper.ReadLine(line, 5000)) {
                    int idx = -1; unsigned code = 1; long def = 0;
                    if (sscanf_s(line.c_str(), "A %d %u %ld", &idx, &code, &def) == 3 && idx == i) {
                        st.err = (PDH_STATUS)code;
                        st.ok = code == ERROR_SUCCESS;
                        st.via32 = st.ok;
                        st.defScale = def;
                        // 32-bit only when the 64-bit side failed (or, when the box is ticked, would fail).
                        st.only32 = st.ok && !IsRemoteCounterPath(c.path) && (tryNative || !NativeSees(c.path));
                    }
                }
            }
#endif
            switch (c.scaleMode) {
                case SCALE_AUTO:    st.exp = std::clamp((int)st.defScale + 1, -7, 7); break;
                case SCALE_DEFAULT: st.exp = std::clamp((int)st.defScale, -7, 7); break;
                default:            st.exp = c.manualExp; break;
            }
            st.lastHigh = now;
        }
        if (q) PdhCollectQueryData(q);              // prime rate counters
    }

    void Close() {
        helper.Stop();
        if (q) { PdhCloseQuery(q); q = nullptr; }
    }

    void Sample() {
        double vals[MAX_COUNTERS];
        bool   have[MAX_COUNTERS] = {};
        if (q) PdhCollectQueryData(q);
        for (int i = 0; i < MAX_COUNTERS; i++) {
            CounterState& st = cs[i];
            if (!st.active || !st.ok || st.via32) continue;
            PDH_FMT_COUNTERVALUE v = {};
            // PDH_FMT_DOUBLE reads 32-bit and 64-bit (LARGE) counter types without overflow.
            PDH_STATUS r = PdhGetFormattedCounterValue(st.h, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v);
            if (r == ERROR_SUCCESS && (v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA)) {
                vals[i] = v.doubleValue; have[i] = true;
            }
        }
        if (helper.proc && helper.Send(L"S")) {
            std::string line;
            while (helper.ReadLine(line, 3000)) {
                if (line == "E") break;
                int idx = -1, ok = 0; double val = 0;
                if (sscanf_s(line.c_str(), "V %d %d %lf", &idx, &ok, &val) == 3 && idx >= 0 && idx < MAX_COUNTERS && ok && cs[idx].via32) {
                    vals[idx] = val; have[idx] = true;
                }
            }
        }

        ULONGLONG now = GetTickCount64();
        unsigned short mark = 0;
        for (int i = 0; i < MAX_COUNTERS; i++) {
            CounterState& st = cs[i];
            float y = NAN;
            double rv = NAN;
            if (st.active && st.ok && have[i]) {
                rv = vals[i];
                if (s.c[i].scaleMode == SCALE_AUTO) {
                    // QuickMon rules: scale down when a value exceeds 110% of the chart;
                    // scale up 10x after AutoScaleup seconds below 8% of the chart.
                    double plotted = rv * pow(10.0, st.exp);
                    if (plotted > 110.0 && st.exp > -7) {
                        while (st.exp > -7 && rv * pow(10.0, st.exp) > 100.0) st.exp--;
                        mark |= (unsigned short)(1u << i);
                        st.lastHigh = now;
                    } else if (plotted >= 8.0) {
                        st.lastHigh = now;
                    } else if (s.autoScaleup && st.exp < 7 && now - st.lastHigh >= (ULONGLONG)s.autoScaleup * 1000) {
                        st.exp++;
                        mark |= (unsigned short)(1u << i);
                        st.lastHigh = now;
                    }
                }
                y = (float)std::clamp(rv * pow(10.0, st.exp), 0.0, 100.0);
            }
            st.ys.push_back(y);
            st.raw.push_back(rv);
            while ((int)st.ys.size() > HISTORY) st.ys.pop_front();
            while ((int)st.raw.size() > HISTORY) st.raw.pop_front();
        }
        marks.push_back(mark);
        while ((int)marks.size() > HISTORY) marks.pop_front();
    }
};

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static void FillRectColor(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void Line(HDC dc, int x1, int y1, int x2, int y2) { MoveToEx(dc, x1, y1, nullptr); LineTo(dc, x2, y2); }

// Layout follows the original QuickMon window: a framed black chart with a square
// gray grid, the legend overlaid in the top-left corner ("value [] scale:1 counter"),
// a "Seconds/horizontal division" line, and thick rescale markers.

static const int SAMPLES_PER_DIV = 10;    // samples per grid square

struct ChartGeom { RECT rc; int cell; double pxPerSample; };

static ChartGeom MakeGeom(const RECT& rc) {
    ChartGeom gm;
    gm.rc = rc;
    gm.cell = std::max(4, (int)(rc.bottom - rc.top) / 10);   // 10 horizontal divisions, square cells
    gm.pxPerSample = (double)gm.cell / SAMPLES_PER_DIV;
    return gm;
}

static void DrawChart(HDC dc, const ChartGeom& gm, Monitor& m, bool preview) {
    const Settings& s = m.s;
    const RECT& rc = gm.rc;
    FillRectColor(dc, rc, RGB(0, 0, 0));
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw < 4 || ch < 4) return;

    // Grid: horizontal lines at every 10%, vertical lines one cell apart, anchored at the right
    // edge so the grid lines up with the newest sample.
    COLORREF gridRgb = kGridColors[std::clamp(s.gridColor, 0, NGRIDCOLORS - 1)].rgb;
    HPEN grid = CreatePen(PS_SOLID, 1, gridRgb == CLR_NONE ? RGB(0, 0, 0) : gridRgb);
    HGDIOBJ oldPen = SelectObject(dc, grid);
    if (gridRgb != CLR_NONE) {
        for (int i = 1; i < 10; i++) {
            int y = rc.bottom - 1 - (ch - 1) * i / 10;
            Line(dc, rc.left, y, rc.right, y);
        }
        for (int x = rc.right - 1 - gm.cell; x > rc.left; x -= gm.cell) Line(dc, x, rc.top, x, rc.bottom);
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    size_t n = m.marks.size();
    size_t visible = (size_t)(cw / gm.pxPerSample) + 2;
    size_t first = n > visible ? n - visible : 0;
    auto X = [&](size_t j) { return rc.right - 1 - (int)((double)(n - 1 - j) * gm.pxPerSample + 0.5); };
    auto Y = [&](float y) { return rc.bottom - 1 - (int)(y * (ch - 1) / 100.0f + 0.5f); };

    // Rescale markers: thick vertical line in the counter's color, white when several rescaled.
    int markW = preview ? 1 : std::max(2, gm.cell / 30);
    for (size_t j = first; j < n; j++) {
        unsigned short mk = m.marks[j];
        if (!mk) continue;
        int bits = 0, which = 0;
        for (int i = 0; i < MAX_COUNTERS; i++) if (mk & (1u << i)) { bits++; which = i; }
        COLORREF col = bits == 1 ? LineRgb(s.c[which]) : RGB(255, 255, 255);
        HPEN p = CreatePen(PS_SOLID, markW, col);
        SelectObject(dc, p);
        Line(dc, X(j), rc.top, X(j), rc.bottom);
        SelectObject(dc, grid);
        DeleteObject(p);
    }

    int lw = std::max(1, gm.cell / 40);
    for (int i = 0; i < MAX_COUNTERS; i++) {
        const CounterState& st = m.cs[i];
        if (!st.active || !st.ok || st.ys.size() != n) continue;
        COLORREF col = LineRgb(s.c[i]);
        HPEN p = CreatePen(PS_SOLID, lw, col);
        SelectObject(dc, p);
        std::vector<POINT> run;
        auto flush = [&]() {
            if (run.size() >= 2) Polyline(dc, run.data(), (int)run.size());
            else if (run.size() == 1) SetPixel(dc, run[0].x, run[0].y, col);
            run.clear();
        };
        for (size_t j = first; j < n; j++) {
            float y = st.ys[j];
            if (std::isnan(y)) { flush(); continue; }
            run.push_back({ X(j), Y(y) });
        }
        flush();
        SelectObject(dc, grid);
        DeleteObject(p);
    }
    RestoreDC(dc, saved);
    SelectObject(dc, oldPen);
    DeleteObject(grid);
}

// Original legend format: "4.36 [green] 10:1 % Processor Time of Processor(0) on \\MACHINE"
static void DrawLegend(HDC dc, const RECT& rc, Monitor& m, int base) {
    const Settings& s = m.s;
    int rowH = base + base / 4;
    SIZE vsz = {}, ssz = {};
    GetTextExtentPoint32W(dc, L"00000000.00", 11, &vsz);
    GetTextExtentPoint32W(dc, L"0.0001:1", 8, &ssz);
    int xVal = rc.left + base / 2, xValR = xVal + vsz.cx;
    int xSw = xValR + base / 3, swW = base * 2 / 3;
    int xScale = xSw + swW + base / 3, xScaleR = xScale + ssz.cx;
    int xName = xScale;   // scale and name share one text run, like the original
    (void)xScaleR;

    int y = rc.top + base / 4;
    auto text = [&](int x0, int x1, const std::wstring& t, UINT align) {
        RECT r = { x0, y, x1, y + rowH };
        DrawTextW(dc, t.c_str(), -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    };
    SetTextColor(dc, RGB(225, 225, 225));
    for (int i = 0; i < MAX_COUNTERS; i++) {
        const CounterState& st = m.cs[i];
        if (!st.active) continue;
        if (y + rowH > rc.bottom) break;
        double last = st.raw.empty() ? NAN : st.raw.back();
        wchar_t v[64];
        if (std::isfinite(last)) swprintf(v, 64, L"%.2f", last); else wcscpy_s(v, L"----");
        text(xVal - base * 4, xValR, v, DT_RIGHT);
        RECT sw = { xSw, y + (rowH - swW) / 2, xSw + swW, y + (rowH - swW) / 2 + swW };
        FillRectColor(dc, sw, LineRgb(s.c[i]));
        std::wstring label = (st.ok ? ScaleStr(st.exp) + L":1 " : std::wstring(L"----  ")) + DescribeCounter(s.c[i].path, s.hideCounterHost);
        if (st.via32) label += st.only32 ? TAG_32ONLY : TAG_32;
#ifndef _WIN64
        else label += Own32Tag();             // the 32-bit build reads everything itself
#endif
        if (!st.ok) label += L"  (not available)";
        text(xName, rc.right - base / 2, label, DT_LEFT);
        y += rowH;
    }
    if (y + rowH <= rc.bottom) {
        wchar_t v[64];
        swprintf(v, 64, L"%.2f", (double)s.refreshSec * SAMPLES_PER_DIV);
        text(xVal - base * 4, xValR, v, DT_RIGHT);
        text(xName, rc.right - base / 2, L"Seconds/horizontal division", DT_LEFT);
    }
}

// ---- Modern QuickMon 2.10 theme: this rewrite's own look -------------------------------------
// A title bar (name, scheme, computer, time, refresh rate), a 10 x 10 grid with a 0-100 axis,
// and a legend table under the chart. Time runs at the same rate as the Classic theme:
// each vertical division is SAMPLES_PER_DIV samples.

static HFONT MkFont(int px, int weight) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static void DrawChartModern(HDC dc, const RECT& rc, Monitor& m, bool preview, int base) {
    const Settings& s = m.s;
    FillRectColor(dc, rc, RGB(0, 0, 0));
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw < 4 || ch < 4) return;

    HPEN grid  = CreatePen(PS_SOLID, 1, RGB(36, 36, 44));
    HPEN grid2 = CreatePen(PS_SOLID, 1, RGB(64, 64, 76));
    HGDIOBJ oldPen = SelectObject(dc, grid);
    for (int i = 1; i < 10; i++) {
        int y = rc.bottom - ch * i / 10;
        SelectObject(dc, i == 5 ? grid2 : grid);
        Line(dc, rc.left, y, rc.right, y);
        int x = rc.left + cw * i / 10;
        SelectObject(dc, grid);
        Line(dc, x, rc.top, x, rc.bottom);
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    size_t n = m.marks.size();
    double pxPerSample = (double)cw / (10.0 * SAMPLES_PER_DIV);
    size_t visible = (size_t)(cw / pxPerSample) + 2;
    size_t first = n > visible ? n - visible : 0;
    auto X = [&](size_t j) { return rc.right - 1 - (int)((double)(n - 1 - j) * pxPerSample + 0.5); };
    auto Y = [&](float y) { return rc.bottom - 1 - (int)(y * (ch - 1) / 100.0f + 0.5f); };

    // Rescale markers: a thick vertical line in the rescaled counter's color (white if several).
    int markW = preview ? 2 : std::max(3, base / 4);
    for (size_t j = first; j < n; j++) {
        unsigned short mk = m.marks[j];
        if (!mk) continue;
        int bits = 0, which = 0;
        for (int i = 0; i < MAX_COUNTERS; i++) if (mk & (1u << i)) { bits++; which = i; }
        COLORREF col = bits == 1 ? LineRgb(s.c[which]) : RGB(255, 255, 255);
        HPEN p = CreatePen(PS_SOLID, markW, col);
        SelectObject(dc, p);
        Line(dc, X(j), rc.top, X(j), rc.bottom);
        SelectObject(dc, grid);
        DeleteObject(p);
    }

    int lw = preview ? 1 : std::max(2, base / 9);
    for (int i = 0; i < MAX_COUNTERS; i++) {
        const CounterState& st = m.cs[i];
        if (!st.active || !st.ok || st.ys.size() != n) continue;
        COLORREF col = LineRgb(s.c[i]);
        HPEN p = CreatePen(PS_SOLID, lw, col);
        SelectObject(dc, p);
        std::vector<POINT> run;
        auto flush = [&]() {
            if (run.size() >= 2) Polyline(dc, run.data(), (int)run.size());
            else if (run.size() == 1) { RECT d = { run[0].x - lw, run[0].y - lw, run[0].x + lw, run[0].y + lw }; FillRectColor(dc, d, col); }
            run.clear();
        };
        for (size_t j = first; j < n; j++) {
            float y = st.ys[j];
            if (std::isnan(y)) { flush(); continue; }
            run.push_back({ X(j), Y(y) });
        }
        flush();
        SelectObject(dc, grid);
        DeleteObject(p);
    }
    RestoreDC(dc, saved);

    SelectObject(dc, grid2);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(grid);
    DeleteObject(grid2);
}

static void DrawLegendModern(HDC dc, const RECT& rc, Monitor& m, const std::vector<int>& rows, int rowH, int base,
                             HFONT fText, HFONT fSmall) {
    const Settings& s = m.s;
    int W = rc.right - rc.left;
    int sw = base * 2;
    int colW = base * 6;
    if (sw + 5 * colW > W * 62 / 100) colW = std::max(base * 3, (W * 62 / 100 - sw) / 5);
    int xScale = rc.left + sw, xLast = xScale + colW, xAvg = xLast + colW, xMin = xAvg + colW, xMax = xMin + colW,
        xCtr = xMax + colW + base / 2;

    auto cell = [&](int x0, int x1, int y, const std::wstring& t, UINT align) {
        RECT r = { x0, y, x1 - base / 3, y + rowH };
        DrawTextW(dc, t.c_str(), -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    };

    int y = rc.top;
    SelectObject(dc, fSmall);
    SetTextColor(dc, RGB(140, 140, 155));
    cell(xScale, xLast, y, L"Scale", DT_RIGHT);
    cell(xLast, xAvg, y, L"Last", DT_RIGHT);
    cell(xAvg, xMin, y, L"Average", DT_RIGHT);
    cell(xMin, xMax, y, L"Minimum", DT_RIGHT);
    cell(xMax, xMax + colW, y, L"Maximum", DT_RIGHT);
    cell(xCtr, rc.right, y, L"Counter", DT_LEFT);
    y += rowH;

    SelectObject(dc, fText);
    for (int i : rows) {
        if (y + rowH > rc.bottom + 2) break;
        const CounterState& st = m.cs[i];
        COLORREF col = LineRgb(s.c[i]);
        RECT swr = { rc.left + base / 4, y + rowH / 4, rc.left + sw - base / 2, y + rowH * 3 / 4 };
        FillRectColor(dc, swr, col);

        double last = NAN, sum = 0, mn = INFINITY, mx = -INFINITY;
        int cnt = 0;
        for (double v : st.raw) {
            if (std::isnan(v)) continue;
            last = v; sum += v; cnt++;
            mn = std::min(mn, v); mx = std::max(mx, v);
        }
        if (!st.raw.empty() && std::isnan(st.raw.back())) last = NAN;
        bool any = cnt > 0;

        SetTextColor(dc, st.ok ? RGB(215, 215, 222) : RGB(110, 110, 120));
        cell(xScale, xLast, y, st.ok ? ScaleStr(st.exp) : L"---", DT_RIGHT);
        cell(xLast, xAvg, y, FmtValue(last), DT_RIGHT);
        cell(xAvg, xMin, y, any ? FmtValue(sum / cnt) : L"---", DT_RIGHT);
        cell(xMin, xMax, y, any ? FmtValue(mn) : L"---", DT_RIGHT);
        cell(xMax, xMax + colW, y, any ? FmtValue(mx) : L"---", DT_RIGHT);
        std::wstring name = DescribeCounter(s.c[i].path, s.hideCounterHost);
        if (st.via32) name += st.only32 ? TAG_32ONLY : TAG_32;
#ifndef _WIN64
        else name += Own32Tag();
#endif
        if (!st.ok) name += L"  (not available)";
        SetTextColor(dc, st.ok ? col : RGB(110, 110, 120));
        cell(xCtr, rc.right, y, name, DT_LEFT);
        y += rowH;
    }
}

static void RenderModern(HDC dc, int W, int H, Monitor& m, bool preview) {
    const Settings& s = m.s;
    RECT all = { 0, 0, W, H };
    FillRectColor(dc, all, RGB(10, 10, 14));
    if (W < 8 || H < 8) return;

    int base = preview ? std::max(7, H / 12) : std::clamp(H / 36, 11, 30);
    int pad = std::max(2, base / 2);
    bool showChart  = preview || s.displayType != DISPLAY_LEGEND;
    bool showLegend = !preview && s.displayType != DISPLAY_CHART;
    bool showTitle  = !preview;

    HFONT fText  = MkFont(base, FW_NORMAL);
    HFONT fTitle = MkFont(base * 115 / 100, FW_SEMIBOLD);
    HFONT fSmall = MkFont(std::max(8, base * 85 / 100), FW_NORMAL);
    HGDIOBJ oldFont = SelectObject(dc, fText);
    SetBkMode(dc, TRANSPARENT);

    if (!preview) {
        HPEN border = CreatePen(PS_SOLID, 1, RGB(72, 72, 88));
        HGDIOBJ op = SelectObject(dc, border);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, W, H);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(border);
    }

    int top = pad, bottom = H - pad;
    if (showTitle) {
        int th = base * 2;
        RECT r = { pad, top, W - pad, top + th };
        SelectObject(dc, fSmall);
        SetTextColor(dc, RGB(150, 150, 165));
        wchar_t tm[64] = L"";
        // With seconds. It is redrawn on each refresh (and at least every 15 seconds, T_CLOCK).
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, nullptr, tm, 64);
        // Computer name, IP address, time, refresh rate (the name and IP can each be turned off).
        std::wstring right;
        for (const std::wstring& part : { s.hideHost ? std::wstring() : ShownHostName(s.hostName), s.hideIp ? std::wstring() : ShownIp(s.ipAdapter), std::wstring(tm),
                                          L"every " + RefreshLabel(s.refreshSec) })
            if (!part.empty()) right += (right.empty() ? L"" : L"     ") + part;
        SIZE rsz = {};
        GetTextExtentPoint32W(dc, right.c_str(), (int)right.size(), &rsz);
        DrawTextW(dc, right.c_str(), -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, fTitle);
        SetTextColor(dc, RGB(232, 232, 238));
        std::wstring t = s.hideTitle ? L"" : L"QuickMon Forever";
        if (!s.scheme.empty()) t += (t.empty() ? L"" : L"   \x2014   ") + s.scheme;
        RECT lr = r;
        lr.right = std::max(lr.left, (LONG)(r.right - rsz.cx - base));
        DrawTextW(dc, t.c_str(), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        top += th + pad / 2;
    }

    std::vector<int> rows;
    for (int i = 0; i < MAX_COUNTERS; i++) if (m.cs[i].active) rows.push_back(i);
    int rowH = base * 3 / 2;
    int legendH = showLegend ? rowH * (int)(rows.size() + 1) : 0;

    RECT chart = {};
    if (showChart) {
        SelectObject(dc, fSmall);
        SIZE ax = {};
        GetTextExtentPoint32W(dc, L"100", 3, &ax);
        int axisW = preview ? 0 : ax.cx + pad;
        chart = { pad + axisW, top + (preview ? 0 : base / 2), W - pad, bottom - legendH - (showLegend ? pad : 0) };
        if (chart.bottom - chart.top < base * 3) chart.bottom = chart.top + base * 3;
        DrawChartModern(dc, chart, m, preview, base);
        if (!preview) {
            SetTextColor(dc, RGB(120, 120, 135));
            int ch = chart.bottom - chart.top;
            for (int i = 0; i <= 10; i += 2) {
                int y = chart.bottom - ch * i / 10;
                RECT r = { pad, y - base, chart.left - pad / 2, y + base };
                std::wstring lab = std::to_wstring(i * 10);
                DrawTextW(dc, lab.c_str(), -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
        }
    }
    if (showLegend) {
        RECT lr = { pad, showChart ? chart.bottom + pad : top, W - pad, bottom };
        DrawLegendModern(dc, lr, m, rows, rowH, base, fText, fSmall);
    }

    SelectObject(dc, oldFont);
    DeleteObject(fText); DeleteObject(fTitle); DeleteObject(fSmall);
}

// ---- Classic QuickMon 1.10 theme ----------------------------------------------------------

static void Render(HDC dc, int W, int H, Monitor& m, bool preview) {
    const Settings& s = m.s;
    if (s.theme == THEME_MODERN) { RenderModern(dc, W, H, m, preview); return; }
    RECT all = { 0, 0, W, H };
    FillRectColor(dc, all, RGB(0, 0, 0));
    if (W < 8 || H < 8) return;

    // Frame. Flat: a 2-pixel border in a bright shade of the grid color. Raised 3D: like 1.10,
    // 3 pixels on the top and left (face, highlight, face) and 2 on the right and bottom
    // (face, shadow). With the Gray grid it uses the colors measured on 1.10; other grid colors
    // get matching shades of their border color.
    RECT inner = { 1, 1, W - 1, H - 1 };
    if (!preview) {
        const GridColor& gc = kGridColors[std::clamp(s.gridColor, 0, NGRIDCOLORS - 1)];
        if (s.frameStyle == FRAME_RAISED) {
            COLORREF face, light, dark;
            if (s.gridColor == 0 || gc.rgb == CLR_NONE) {
                face = RGB(168, 152, 144); light = RGB(216, 208, 200); dark = RGB(120, 96, 88);
            } else {
                int r = GetRValue(gc.border), gg = GetGValue(gc.border), b = GetBValue(gc.border);
                face  = RGB(r * 85 / 100, gg * 85 / 100, b * 85 / 100);
                light = RGB(r + (255 - r) * 45 / 100, gg + (255 - gg) * 45 / 100, b + (255 - b) * 45 / 100);
                dark  = RGB(r * 55 / 100, gg * 55 / 100, b * 55 / 100);
            }
            FillRectColor(dc, all, face);
            RECT rr = { 1, 1, W - 1, 2 };     FillRectColor(dc, rr, light);   // highlight, top
            rr = { 1, 1, 2, H - 2 };          FillRectColor(dc, rr, light);   // highlight, left
            rr = { 0, H - 1, W, H };          FillRectColor(dc, rr, dark);    // shadow, bottom
            rr = { W - 1, 0, W, H };          FillRectColor(dc, rr, dark);    // shadow, right
            inner = { 3, 3, W - 2, H - 2 };
        } else {
            FillRectColor(dc, all, gc.border);
            inner = { 2, 2, W - 2, H - 2 };
        }
    }

    int base = std::clamp(H / 30, 11, 26);
    HFONT font = CreateFontW(-base, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Tahoma");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    bool showChart  = preview || s.displayType != DISPLAY_LEGEND;
    bool showLegend = !preview && s.displayType != DISPLAY_CHART;
    if (showChart) DrawChart(dc, MakeGeom(inner), m, preview);
    else           FillRectColor(dc, inner, RGB(0, 0, 0));
    if (showLegend) DrawLegend(dc, inner, m, base);

    SelectObject(dc, oldFont);
    DeleteObject(font);
}

// ---- Monitors ----

// One display monitor: its screen rectangle, Windows display number (\\.\DISPLAYn) and
// hardware ID (device interface name, stable when display numbers change).
struct MonInfo { HMONITOR h = nullptr; RECT rc = {}; bool primary = false; int number = 0; std::wstring device, id; };

static BOOL CALLBACK MonEnumProc(HMONITOR h, HDC, LPRECT, LPARAM lp) {
    auto* v = (std::vector<MonInfo>*)lp;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(h, &mi)) return TRUE;
    MonInfo m;
    m.h = h;
    m.rc = mi.rcMonitor;
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    m.device = mi.szDevice;
    size_t k = m.device.find_last_not_of(L"0123456789");
    m.number = (k != std::wstring::npos && k + 1 < m.device.size()) ? _wtoi(m.device.c_str() + k + 1) : (int)v->size() + 1;
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME)) m.id = dd.DeviceID;
    v->push_back(m);
    return TRUE;
}

// Every monitor, in display-number order.
static std::vector<MonInfo> Monitors() {
    std::vector<MonInfo> v;
    EnumDisplayMonitors(nullptr, nullptr, MonEnumProc, (LPARAM)&v);
    std::sort(v.begin(), v.end(), [](const MonInfo& a, const MonInfo& b) { return a.number < b.number; });
    return v;
}

static const MonInfo* PrimaryMonitor(const std::vector<MonInfo>& ms) {
    for (const auto& m : ms) if (m.primary) return &m;
    return ms.empty() ? nullptr : &ms[0];
}

// The monitor a Monitor N setting means: by hardware ID, then by display number, else the primary.
static const MonInfo* ChosenMonitor(const std::vector<MonInfo>& ms, const Settings& s) {
    if (!s.monitorId.empty())
        for (const auto& m : ms) if (_wcsicmp(m.id.c_str(), s.monitorId.c_str()) == 0) return &m;
    for (const auto& m : ms) if (m.number == s.monitorNum) return &m;
    return PrimaryMonitor(ms);
}

// "2: 2560 x 1440 (right)": number, size, and where it sits relative to the primary monitor.
static std::wstring MonitorLabel(const MonInfo& m, const MonInfo* primary) {
    wchar_t b[96];
    swprintf(b, 96, L"%d: %d x %d", m.number, (int)(m.rc.right - m.rc.left), (int)(m.rc.bottom - m.rc.top));
    std::wstring t = b;
    if (m.primary || !primary) return t + L" (primary)";
    long dx = (m.rc.left + m.rc.right) / 2 - (primary->rc.left + primary->rc.right) / 2;
    long dy = (m.rc.top + m.rc.bottom) / 2 - (primary->rc.top + primary->rc.bottom) / 2;
    if (labs(dx) >= labs(dy)) t += dx < 0 ? L" (left)" : L" (right)";
    else                      t += dy < 0 ? L" (above)" : L" (below)";
    return t;
}

// All monitors as one screen: the largest rectangle that the monitors fully cover, so the chart
// never drifts into a gap no monitor shows (screens of different sizes or offsets). For a
// layout that is a perfect rectangle, that is the whole desktop.
static RECT LargestCoveredRect(const std::vector<MonInfo>& ms) {
    std::vector<LONG> xs, ys;
    for (const auto& m : ms) { xs.push_back(m.rc.left); xs.push_back(m.rc.right); ys.push_back(m.rc.top); ys.push_back(m.rc.bottom); }
    std::sort(xs.begin(), xs.end()); xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end()); ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    int nx = (int)xs.size() - 1, ny = (int)ys.size() - 1;
    if (nx < 1 || ny < 1) return ms.empty() ? RECT{} : ms[0].rc;
    // covered[i][j]: the grid cell between xs[i..i+1] and ys[j..j+1] lies on some monitor.
    std::vector<std::vector<int>> gap(nx + 1, std::vector<int>(ny + 1, 0));   // prefix sums of uncovered cells
    for (int i = 0; i < nx; i++)
        for (int j = 0; j < ny; j++) {
            POINT c = { (xs[i] + xs[i + 1]) / 2, (ys[j] + ys[j + 1]) / 2 };
            bool cov = false;
            for (const auto& m : ms) if (PtInRect(&m.rc, c)) cov = true;
            gap[i + 1][j + 1] = (cov ? 0 : 1) + gap[i][j + 1] + gap[i + 1][j] - gap[i][j];
        }
    RECT best = ms[0].rc;
    long long bestArea = 0;
    for (int a = 0; a < nx; a++) for (int b = a + 1; b <= nx; b++)
        for (int c = 0; c < ny; c++) for (int d = c + 1; d <= ny; d++) {
            int holes = gap[b][d] - gap[a][d] - gap[b][c] + gap[a][c];
            long long area = (long long)(xs[b] - xs[a]) * (ys[d] - ys[c]);
            // Largest area; on a tie, the wider one (the chart is wider than tall).
            if (!holes && (area > bestArea || (area == bestArea && xs[b] - xs[a] > best.right - best.left))) {
                bestArea = area; best = { xs[a], ys[c], xs[b], ys[d] };
            }
        }
    return best;
}

// The screen area(s) the chart window(s) move in, and the monitor smooth movement is timed to.
static std::vector<RECT> ChartAreas(const Settings& s, HMONITOR& syncTo) {
    std::vector<MonInfo> ms = Monitors();
    std::vector<RECT> out;
    const MonInfo* prim = PrimaryMonitor(ms);
    syncTo = prim ? prim->h : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    if (ms.empty()) {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(syncTo, &mi);
        out.push_back(mi.rcMonitor);
        return out;
    }
    switch (s.monitorMode) {
    case MONITOR_ONE: { const MonInfo* m = ChosenMonitor(ms, s); out.push_back(m->rc); syncTo = m->h; break; }
    case MONITOR_ALL:  out.push_back(LargestCoveredRect(ms)); break;
    case MONITOR_EACH: for (const auto& m : ms) out.push_back(m.rc); break;
    default:           out.push_back(prim->rc); break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Screen saver and preview windows
// ---------------------------------------------------------------------------

enum { T_FIRST = 1, T_SAMPLE, T_CLOCK, T_MOVE };
static const UINT WM_APP_FRAME = WM_APP + 1;   // posted once per display refresh (smooth movement)
static const double MOVE_TICK_MS = 33.0;       // kMoves speeds are pixels per 33 ms tick

// One chart window: one for Primary, Monitor N and All monitors, one per monitor for Each monitor.
struct Panel {
    HWND    hwnd = nullptr;
    RECT    bounds = {};                       // the area it moves in (main window coordinates)
    int     pw = 0, ph = 0;
    double  px = 0, py = 0, vx = 0, vy = 0;
    // Smooth movement: position is a function of time (bouncing inside bounds), evaluated
    // once per display refresh, so every frame advances by the same amount.
    double  x0 = 0, y0 = 0;                    // starting offset inside bounds
    int     lastX = INT_MIN, lastY = INT_MIN;
    HDC     memdc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
};

struct App {
    HINSTANCE hi = nullptr;
    int       mode = MODE_CONFIG;
    HWND      hMain = nullptr, hTimer = nullptr;
    Monitor   mon;
    std::vector<Panel> panels;
    HMONITOR  syncMonitor = nullptr;           // smooth movement waits for this display's refresh
    POINT     mouse = {};
    bool      haveMouse = false;
    ULONGLONG start = 0;
    bool      classic = false;                 // Classic movement: T_MOVE jumps every classicSec
    LARGE_INTEGER t0 = {}, qpf = {};
    HANDLE    frameThread = nullptr;
    std::atomic<bool> running{ false }, framePending{ false };
} g;

static void RenderToCache() {
    for (auto& p : g.panels) {
        if (!p.hwnd || p.pw <= 0 || p.ph <= 0) continue;
        if (!p.memdc) {
            HDC wdc = GetDC(p.hwnd);
            p.memdc = CreateCompatibleDC(wdc);
            p.bmp = CreateCompatibleBitmap(wdc, p.pw, p.ph);
            p.oldBmp = SelectObject(p.memdc, p.bmp);
            ReleaseDC(p.hwnd, wdc);
        }
        Render(p.memdc, p.pw, p.ph, g.mon, g.mode == MODE_PREVIEW);
        InvalidateRect(p.hwnd, nullptr, FALSE);
    }
}

static void FreeCache() {
    for (auto& p : g.panels) {
        if (p.memdc) { SelectObject(p.memdc, p.oldBmp); DeleteObject(p.bmp); DeleteDC(p.memdc); }
        p.memdc = nullptr; p.bmp = nullptr;
    }
}

// Moves each window one step of (vx, vy), bouncing off the edges of its bounds.
static void StepMove() {
    for (auto& p : g.panels) {
        p.px += p.vx; p.py += p.vy;
        if (p.px < p.bounds.left)          { p.px = p.bounds.left; p.vx = fabs(p.vx); }
        if (p.px + p.pw > p.bounds.right)  { p.px = p.bounds.right - p.pw; p.vx = -fabs(p.vx); }
        if (p.py < p.bounds.top)           { p.py = p.bounds.top; p.vy = fabs(p.vy); }
        if (p.py + p.ph > p.bounds.bottom) { p.py = p.bounds.bottom - p.ph; p.vy = -fabs(p.vy); }
        SetWindowPos(p.hwnd, nullptr, (int)p.px, (int)p.py, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void OnTimer(UINT_PTR id) {
    switch (id) {
    case T_FIRST:  KillTimer(g.hTimer, T_FIRST); g.mon.Sample(); RenderToCache(); break;
    case T_SAMPLE:
        g.mon.Sample(); RenderToCache();
        break;
    case T_CLOCK:  RenderToCache(); break;
    case T_MOVE:   StepMove(); break;
    }
}

// Reflects p into [0, range], like a ball bouncing between two walls.
static double Bounce(double p, double range) {
    if (range <= 0) return 0;
    double m = fmod(p, 2 * range);
    if (m < 0) m += 2 * range;
    return m <= range ? m : 2 * range - m;
}

static void OnFrame() {
    g.framePending = false;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double ticks = (double)(now.QuadPart - g.t0.QuadPart) * 1000.0 / (double)g.qpf.QuadPart / MOVE_TICK_MS;
    for (auto& p : g.panels) {
        int x = p.bounds.left + (int)floor(Bounce(p.x0 + p.vx * ticks, (double)(p.bounds.right - p.bounds.left - p.pw)) + 0.5);
        int y = p.bounds.top  + (int)floor(Bounce(p.y0 + p.vy * ticks, (double)(p.bounds.bottom - p.bounds.top - p.ph)) + 0.5);
        if (x == p.lastX && y == p.lastY) continue;
        p.lastX = x; p.lastY = y;
        SetWindowPos(p.hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// The DXGI output (display) showing a monitor, for WaitForVBlank.
static IDXGIOutput* FindOutput(HMONITOR want) {
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return nullptr;
    IDXGIOutput* found = nullptr;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; !found && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        IDXGIOutput* o = nullptr;
        for (UINT j = 0; !found && a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC d;
            if (SUCCEEDED(o->GetDesc(&d)) && d.Monitor == want) found = o;
            else o->Release();
        }
        a->Release();
    }
    f->Release();
    return found;
}

// Wakes once per display refresh and asks the UI thread to move the windows. Waits on the
// vertical blank of the monitor the chart is on (the primary one for All and Each monitors);
// if that is unavailable (e.g. Remote Desktop), on the desktop compositor; failing both, about
// 100 times a second.
static DWORD WINAPI FrameThread(void*) {
    IDXGIOutput* out = FindOutput(g.syncMonitor ? g.syncMonitor : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    while (g.running) {
        bool waited = false;
        if (out) {
            if (SUCCEEDED(out->WaitForVBlank())) waited = true;
            else { out->Release(); out = nullptr; }
        }
        if (!waited) {
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            if (SUCCEEDED(DwmFlush())) {
                QueryPerformanceCounter(&b);
                waited = (b.QuadPart - a.QuadPart) * 1000 >= 2 * g.qpf.QuadPart;   // really blocked >= 2 ms
            }
            if (!waited) Sleep(10);
        }
        if (g.running && !g.framePending.exchange(true)) PostMessageW(g.hMain, WM_APP_FRAME, 0, 0);
    }
    if (out) out->Release();
    return 0;
}

static void CloseSaver() { if (g.hMain) PostMessageW(g.hMain, WM_CLOSE, 0, 0); }

// Shared exit rules for the full-screen saver.
static bool SaverInput(UINT msg) {
    if (g.mode != MODE_SAVER) return false;
    switch (msg) {
    case WM_MOUSEMOVE: {
        POINT p; GetCursorPos(&p);
        if (!g.haveMouse || GetTickCount64() - g.start < 500) { g.mouse = p; g.haveMouse = true; return true; }
        if (abs(p.x - g.mouse.x) > 6 || abs(p.y - g.mouse.y) > 6) CloseSaver();
        return true;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        CloseSaver();
        return true;
    }
    return false;
}

static LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        for (const auto& p : g.panels)
            if (p.hwnd == h && p.memdc) BitBlt(dc, 0, 0, p.pw, p.ph, p.memdc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER: OnTimer(wp); return 0;
    case WM_SETCURSOR:
        if (g.mode == MODE_SAVER) { SetCursor(nullptr); return TRUE; }
        break;
    case WM_DESTROY:
        if (g.mode == MODE_PREVIEW) PostQuitMessage(0);
        return 0;
    default:
        if (SaverInput(msg)) return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK SaverProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER: OnTimer(wp); return 0;
    case WM_APP_FRAME: OnFrame(); return 0;
    case WM_SETCURSOR: SetCursor(nullptr); return TRUE;
    case WM_ACTIVATEAPP:
        if (!wp && GetTickCount64() - g.start > 1000) CloseSaver();
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_SCREENSAVE || (wp & 0xFFF0) == SC_MONITORPOWER) return 0;
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default:
        if (SaverInput(msg)) return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void RegisterClasses() {
    WNDCLASSW wc = {};
    wc.hInstance = g.hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(g.hi, MAKEINTRESOURCEW(IDI_APP));
    wc.lpfnWndProc = PanelProc;
    wc.lpszClassName = L"QMFPanel";
    RegisterClassW(&wc);
    wc.lpfnWndProc = SaverProc;
    wc.lpszClassName = L"QMFSaver";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    wc.lpfnWndProc = DefWindowProcW;           // plain black backdrop behind the preview chart
    wc.lpszClassName = L"QMFBack";
    RegisterClassW(&wc);
}

static int MessageLoop() {
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return (int)m.wParam;
}

// One-off settings from the command line (/theme, /scheme, /counter... without /save): the
// saver and /render use these instead of the saved settings, and nothing is written.
static bool     g_useOverride = false;
static Settings g_override;
static void LoadRunSettings(Settings& s) { if (g_useOverride) s = g_override; else LoadSettings(s); }

static int RunSaver() {
    HANDLE mx = CreateMutexW(nullptr, TRUE, L"Local\\QuickMonForeverScreenSaver");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { if (mx) CloseHandle(mx); return 0; }

    LoadRunSettings(g.mon.s);
    g.mon.Open();
    RegisterClasses();

    // A black window over the whole desktop (every monitor); the chart window(s) go on top of it.
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    g.hMain = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"QMFSaver", L"QuickMon Forever",
                              WS_POPUP | WS_CLIPCHILDREN, vx, vy, vw, vh, nullptr, nullptr, g.hi, nullptr);
    if (!g.hMain) return 1;

    // One chart window per area: the primary monitor, the chosen monitor, the area all monitors
    // cover, or each monitor. Each wanders inside its own area.
    std::vector<RECT> areas = ChartAreas(g.mon.s, g.syncMonitor);
    const MoveOpt& mo = kMoves[g.mon.s.moveSpeed];
    bool exact = g.mon.s.moveStyle == MOVESTYLE_CLASSIC_EXACT;
    bool moving = false;
    ULONGLONG seed = GetTickCount64();
    for (size_t i = 0; i < areas.size(); i++) {
        Panel p;
        RECT mr = areas[i];
        OffsetRect(&mr, -vx, -vy);
        p.bounds = mr;
        int mw = mr.right - mr.left, mh = mr.bottom - mr.top;
        bool full = ChartSize(g.mon.s.windowSize, mw, mh, p.pw, p.ph);
        // Start Position, and the direction that heads away from it (random where it doesn't
        // matter, and different for each monitor).
        double sx = ((seed >> (2 * i)) & 1) ? 1 : -1, sy = ((seed >> (2 * i + 1)) & 1) ? 1 : -1;
        switch (g.mon.s.startPos) {
        case START_TOPLEFT:   p.px = mr.left;                    p.py = mr.top; sx = 1;  sy = 1; break;
        case START_TOPCENTER: p.px = mr.left + (mw - p.pw) / 2.0; p.py = mr.top;          sy = 1; break;
        case START_TOPRIGHT:  p.px = mr.right - p.pw;            p.py = mr.top; sx = -1; sy = 1; break;
        default:              p.px = mr.left + (mw - p.pw) / 2.0; p.py = mr.top + (mh - p.ph) / 2.0; break;
        }
        p.px = floor(p.px); p.py = floor(p.py);
        double speed = full ? 0.0 : mo.speed * mh / 1080.0;
        g.classic = !full && mo.classicSec > 0 && (g.mon.s.moveStyle == MOVESTYLE_CLASSIC || exact);
        if (g.classic) {
            // Whole-pixel jumps, equal across and down (a 45-degree path), as measured on 1.10.
            double step = exact ? CLASSIC_PX : std::max(1.0, floor(CLASSIC_PX * mh / 1080.0 + 0.5));
            p.vx = step * sx; p.vy = step * sy;
        } else {
            p.vx = speed * 0.8 * sx;
            p.vy = speed * 0.6 * sy;
            if (speed > 0) moving = true;
        }
        p.x0 = p.px - p.bounds.left;
        p.y0 = p.py - p.bounds.top;
        p.hwnd = CreateWindowExW(0, L"QMFPanel", L"", WS_CHILD | WS_VISIBLE, (int)p.px, (int)p.py, p.pw, p.ph,
                                 g.hMain, nullptr, g.hi, nullptr);
        g.panels.push_back(p);
    }
    g.hTimer = g.hMain;
    RenderToCache();
    g.start = GetTickCount64();
    ShowWindow(g.hMain, SW_SHOW);
    SetForegroundWindow(g.hMain);
    SetFocus(g.hMain);

    SetTimer(g.hTimer, T_FIRST, 1000, nullptr);
    SetTimer(g.hTimer, T_SAMPLE, (UINT)g.mon.s.refreshSec * 1000, nullptr);
    SetTimer(g.hTimer, T_CLOCK, 15000, nullptr);
    if (moving && g.mon.s.moveStyle == MOVESTYLE_SMOOTH) {
        QueryPerformanceFrequency(&g.qpf);
        QueryPerformanceCounter(&g.t0);
        g.running = true;
        g.frameThread = CreateThread(nullptr, 0, FrameThread, nullptr, 0, nullptr);
        if (!g.frameThread) g.running = false;
    }
    if (moving && !g.running) SetTimer(g.hTimer, T_MOVE, (UINT)MOVE_TICK_MS, nullptr);
    if (g.classic) SetTimer(g.hTimer, T_MOVE, (UINT)mo.classicSec * 1000, nullptr);   // one jump per tick

    int rc = MessageLoop();
    g.running = false;
    if (g.frameThread) { WaitForSingleObject(g.frameThread, 2000); CloseHandle(g.frameThread); g.frameThread = nullptr; }
    g.mon.Close();
    FreeCache();
    if (mx) { ReleaseMutex(mx); CloseHandle(mx); }
    return rc;
}

// Test aid: /render <file.bmp> <width> <height> <samples>
// Samples the configured counters once a second and writes the rendered window to a BMP,
// so the look can be checked without taking over the screen.
static int RunRenderTest(const std::wstring& file, int w, int h, int samples);

static int RunPreview(HWND parent) {
    if (!IsWindow(parent)) return 1;
    LoadSettings(g.mon.s);
    g.mon.Open();
    RegisterClasses();
    RECT r;
    GetClientRect(parent, &r);
    // Show the chart at the size and start position it has on the real screen, scaled down to
    // the preview, on a black background (the default Auto is one third, as on 1.10). The size
    // is measured against the area it uses: the chosen monitor, or the area all monitors cover;
    // the primary monitor for Primary and Each monitor.
    HMONITOR sync = nullptr;
    std::vector<RECT> areas = ChartAreas(g.mon.s, sync);
    RECT ref = areas.front();
    if (g.mon.s.monitorMode == MONITOR_EACH) {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
        ref = mi.rcMonitor;
    }
    int mw = std::max(1L, ref.right - ref.left), mh = std::max(1L, ref.bottom - ref.top);
    int rw = 0, rh = 0;
    ChartSize(g.mon.s.windowSize, mw, mh, rw, rh);
    Panel p;
    p.pw = std::max(8, (int)((double)r.right * rw / mw + 0.5));
    p.ph = std::max(8, (int)((double)r.bottom * rh / mh + 0.5));
    p.pw = std::min(p.pw, (int)r.right); p.ph = std::min(p.ph, (int)r.bottom);
    int x = (r.right - p.pw) / 2, y = (r.bottom - p.ph) / 2;
    switch (g.mon.s.startPos) {
    case START_TOPLEFT:   x = 0; y = 0; break;
    case START_TOPCENTER: y = 0; break;
    case START_TOPRIGHT:  x = r.right - p.pw; y = 0; break;
    }
    HWND back = CreateWindowExW(0, L"QMFBack", L"", WS_CHILD | WS_VISIBLE | WS_DISABLED, 0, 0, r.right, r.bottom,
                                parent, nullptr, g.hi, nullptr);
    p.hwnd = CreateWindowExW(0, L"QMFPanel", L"", WS_CHILD | WS_VISIBLE | WS_DISABLED, x, y, p.pw, p.ph,
                             back ? back : parent, nullptr, g.hi, nullptr);
    if (!p.hwnd) return 1;
    g.panels.push_back(p);
    g.hTimer = p.hwnd;
    RenderToCache();
    SetTimer(g.hTimer, T_FIRST, 1000, nullptr);
    SetTimer(g.hTimer, T_SAMPLE, (UINT)std::min(g.mon.s.refreshSec, 5) * 1000, nullptr);
    int rc = MessageLoop();
    g.mon.Close();
    FreeCache();
    return rc;
}

// ---------------------------------------------------------------------------
// Setup dialog
// ---------------------------------------------------------------------------

struct ConfigState {
    Settings orig, work;
    int      cur = 0;
    bool     previewed = false;
    HBRUSH   swatch = nullptr;
    HBRUSH   gridSwatch = nullptr;
} cfg;

static void CbAdd(HWND d, int id, const std::wstring& t) { SendDlgItemMessageW(d, id, CB_ADDSTRING, 0, (LPARAM)t.c_str()); }
static void CbSel(HWND d, int id, int i) { SendDlgItemMessageW(d, id, CB_SETCURSEL, (WPARAM)i, 0); }
static int  CbCur(HWND d, int id) { return (int)SendDlgItemMessageW(d, id, CB_GETCURSEL, 0, 0); }
static std::wstring DlgText(HWND d, int id) {
    wchar_t b[1024] = L"";
    GetDlgItemTextW(d, id, b, 1024);
    return b;
}

static bool Use32Effective(const CounterCfg& c) {
#ifdef _WIN64
    return c.use32 && HelperAvailable();
#else
    (void)c;
    return false;
#endif
}

// True when the 64-bit side cannot see this counter on this PC but the 32-bit side can.
// Asks the helper (a quick run of QuickMonForever-32.exe /check32), so results are cached.
static bool Only32(const std::wstring& path) {
#ifdef _WIN64
    if (path.empty() || IsRemoteCounterPath(path)) return false;
    static std::map<std::wstring, bool> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    bool r = false;
    if (!NativeSees(path) && HelperAvailable()) {
        std::wstring out;
        r = RunHelperCapture(L"/check32 " + path, out) == 0;
    }
    cache[path] = r;
    return r;
#else
    (void)path;
    return false;
#endif
}

// The marker for a counter slot: [32-bit*] for a 32-bit-only counter (read through the helper
// whether or not the box is ticked), [32-bit] for any other counter read through the 32-bit side.
static const wchar_t* BitTag(const CounterCfg& c) {
    if (c.path.empty()) return L"";
#ifdef _WIN64
    if (Only32(c.path)) return TAG_32ONLY;
    return Use32Effective(c) ? TAG_32 : L"";
#else
    return Own32Tag();                   // the 32-bit build reads everything itself
#endif
}

// Sets a control's text to desc + tag, shortening desc (with "...") so the tag stays visible.
static void SetTextKeepTag(HWND ctl, const std::wstring& desc, const std::wstring& tag, int reserve) {
    std::wstring t = desc + tag;
    RECT r; GetClientRect(ctl, &r);
    int avail = (r.right - r.left) - reserve;
    HDC dc = GetDC(ctl);
    HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(ctl, WM_GETFONT, 0, 0));
    auto width = [&](const std::wstring& s) { SIZE z = {}; GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &z); return (int)z.cx; };
    if (!tag.empty() && width(t) > avail) {
        std::wstring dd = desc;
        while (!dd.empty() && width(dd + L"..." + tag) > avail) dd.pop_back();
        t = dd + L"..." + tag;
    }
    SelectObject(dc, old);
    ReleaseDC(ctl, dc);
    SetWindowTextW(ctl, t.c_str());
}

// The last Line Color entry: "Custom..." until this counter has a custom color, then its
// hex value ("#FF8800"). Choosing it (or clicking the swatch) opens the Windows color dialog.
static void SetCustomColorItem(HWND d, const CounterCfg& c) {
    HWND cb = GetDlgItem(d, IDC_COLOR);
    std::wstring t = c.color == COLOR_CUSTOM ? HexColor(c.customRgb) : std::wstring(L"Custom...");
    SendMessageW(cb, CB_DELETESTRING, COLOR_CUSTOM, 0);
    SendMessageW(cb, CB_INSERTSTRING, COLOR_CUSTOM, (LPARAM)t.c_str());
}

static void ShowCounter(HWND d) {
    const CounterCfg& c = cfg.work.c[cfg.cur];
    CbSel(d, IDC_COUNTER, cfg.cur);
    SetCustomColorItem(d, c);
    SetTextKeepTag(GetDlgItem(d, IDC_ENABLED), DescribeCounter(c.path), BitTag(c),
                   GetSystemMetrics(SM_CXMENUCHECK) + 2 * GetSystemMetrics(SM_CXEDGE) + 4);
    CheckDlgButton(d, IDC_ENABLED, c.enabled ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(GetDlgItem(d, IDC_ENABLED), !c.path.empty());
    CbSel(d, IDC_COLOR, c.color);
    CbSel(d, IDC_SCALE, c.scaleMode == SCALE_AUTO ? 0 : c.scaleMode == SCALE_DEFAULT ? 1 : 2 + (7 - c.manualExp));
    CheckDlgButton(d, IDC_USE32, c.use32 ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(GetDlgItem(d, IDC_EXPLAIN), !c.path.empty());
    InvalidateRect(GetDlgItem(d, IDC_SWATCH), nullptr, TRUE);
}

static void HarvestCounter(HWND d) {
    CounterCfg& c = cfg.work.c[cfg.cur];
    c.enabled = IsDlgButtonChecked(d, IDC_ENABLED) == BST_CHECKED;
    int col = CbCur(d, IDC_COLOR);
    if (col >= 0 && col < NCOLORS) c.color = col;   // Custom is set only by the color dialog
    int sc = CbCur(d, IDC_SCALE);
    if (sc <= 0)       c.scaleMode = SCALE_AUTO;
    else if (sc == 1)  c.scaleMode = SCALE_DEFAULT;
    else             { c.scaleMode = SCALE_MANUAL; c.manualExp = 7 - (sc - 2); }
    c.use32 = IsDlgButtonChecked(d, IDC_USE32) == BST_CHECKED;
}

// The Windows color dialog, opened full (spectrum, Hue/Sat/Lum and Red/Green/Blue) on the
// counter's current color. Its 16 custom-color slots are kept in the registry (CustomColors).
static const UINT WM_APP_PICKCOLOR = WM_APP + 20;
static bool g_pickPending = false;

// The color dialog gets one extra row, "Hex: #RRGGBB", under Add to Custom Colors (a hook adds
// it). It follows the dialog: clicking the spectrum or a color, or typing in Red/Green/Blue,
// updates it. Typing a full hex value (# optional) sets Red/Green/Blue, and the dialog follows.
// Control IDs in the Windows color dialog (dlgs.h): Hue/Sat/Lum 703-705, Red/Green/Blue
// 706-708, Add to Custom Colors 712.
static const int CC_LUMLABEL = 725, CC_LUMEDIT = 705, CC_RED = 706, CC_GREEN = 707, CC_BLUE = 708, CC_ADD = 712;
static const int IDC_HEXLABEL = 0x3F0, IDC_HEXEDIT = 0x3F1;
static bool g_hexSync = false;   // set while one side is updating the other

static RECT ChildRect(HWND dlg, int id) {
    RECT r = {};
    GetWindowRect(GetDlgItem(dlg, id), &r);
    MapWindowPoints(nullptr, dlg, (POINT*)&r, 2);
    return r;
}

static void ShowHexFromRgbEdits(HWND dlg) {
    BOOL a = FALSE, b = FALSE, c = FALSE;
    UINT r = GetDlgItemInt(dlg, CC_RED, &a, FALSE), g2 = GetDlgItemInt(dlg, CC_GREEN, &b, FALSE),
         bl = GetDlgItemInt(dlg, CC_BLUE, &c, FALSE);
    if (!a || !b || !c || r > 255 || g2 > 255 || bl > 255) return;
    g_hexSync = true;
    SetDlgItemTextW(dlg, IDC_HEXEDIT, HexColor(RGB(r, g2, bl)).c_str());
    g_hexSync = false;
}

static UINT_PTR CALLBACK ColorDialogHook(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        const CHOOSECOLORW* cc = (const CHOOSECOLORW*)lp;
        HFONT font = (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0);
        RECT add = ChildRect(dlg, CC_ADD), lumL = ChildRect(dlg, CC_LUMLABEL), lumE = ChildRect(dlg, CC_LUMEDIT);
        int eh = lumE.bottom - lumE.top, lh = lumL.bottom - lumL.top, gap = std::max(4, eh / 4);
        int y = add.bottom + gap;
        HDC dc = GetDC(dlg);
        HGDIOBJ of = SelectObject(dc, font);
        SIZE sz = {};
        GetTextExtentPoint32W(dc, L"#DDDDDD", 7, &sz);
        SelectObject(dc, of);
        ReleaseDC(dlg, dc);
        // Flush left under Add to Custom Colors, with the same label-to-box spacing as Lum.
        int x = add.left, lw = lumL.right - lumL.left, toBox = lumE.left - lumL.left;
        HWND lab = CreateWindowExW(0, L"STATIC", L"He&x:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   x, y + (eh - lh) / 2, lw, lh,
                                   dlg, (HMENU)(INT_PTR)IDC_HEXLABEL, nullptr, nullptr);
        HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_UPPERCASE | ES_AUTOHSCROLL,
                                  x + toBox, y, sz.cx + eh, eh, dlg, (HMENU)(INT_PTR)IDC_HEXEDIT, nullptr, nullptr);
        SendMessageW(lab, WM_SETFONT, (WPARAM)font, FALSE);
        SendMessageW(ed, WM_SETFONT, (WPARAM)font, FALSE);
        SendMessageW(ed, EM_LIMITTEXT, 7, 0);
        SetWindowPos(ed, GetDlgItem(dlg, CC_ADD), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // tab order
        RECT wr;
        GetWindowRect(dlg, &wr);
        SetWindowPos(dlg, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top + eh + gap,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        g_hexSync = true;
        SetWindowTextW(ed, HexColor(cc->rgbResult).c_str());
        g_hexSync = false;
        return 0;
    }
    if (msg == WM_COMMAND && HIWORD(wp) == EN_CHANGE && !g_hexSync) {
        int id = LOWORD(wp);
        if (id == CC_RED || id == CC_GREEN || id == CC_BLUE) ShowHexFromRgbEdits(dlg);
        else if (id == IDC_HEXEDIT) {
            wchar_t t[16] = L"";
            GetDlgItemTextW(dlg, IDC_HEXEDIT, t, 16);
            COLORREF c;
            if (ParseHexColor(t, c)) {
                g_hexSync = true;   // the dialog then updates Hue/Sat/Lum and the preview itself
                SetDlgItemInt(dlg, CC_RED, GetRValue(c), FALSE);
                SetDlgItemInt(dlg, CC_GREEN, GetGValue(c), FALSE);
                SetDlgItemInt(dlg, CC_BLUE, GetBValue(c), FALSE);
                g_hexSync = false;
            }
        }
    }
    return 0;   // let the dialog do its own processing
}

static void PickCustomColor(HWND d) {
    HarvestCounter(d);
    CounterCfg& c = cfg.work.c[cfg.cur];
    static COLORREF cust[16];
    for (auto& x : cust) x = RGB(255, 255, 255);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_ROOT, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD type = 0, sz = sizeof(cust);
        RegQueryValueExW(k, L"CustomColors", nullptr, &type, (BYTE*)cust, &sz);
        RegCloseKey(k);
    }
    CHOOSECOLORW cc = { sizeof(cc) };
    cc.hwndOwner = d;
    cc.rgbResult = LineRgb(c);
    cc.lpCustColors = cust;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR | CC_ENABLEHOOK;
    cc.lpfnHook = ColorDialogHook;
    if (ChooseColorW(&cc)) {
        c.color = COLOR_CUSTOM;
        c.customRgb = cc.rgbResult & 0xFFFFFF;
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_ROOT, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"CustomColors", 0, REG_BINARY, (const BYTE*)cust, sizeof(cust));
        RegCloseKey(k);
    }
    ShowCounter(d);   // Cancel puts the list back on the counter's color
}

// Browse Detail: which counters the Browse dialog lists (and its starting detail level).
static const wchar_t* kDetailInfo[] = {
    L"Browse lists only basic counters.",
    L"Browse adds counters for experienced users.",
    L"Browse adds in-depth tuning counters.",
    L"Browse lists every counter.",
};
static const wchar_t kDetailTooltip[] =
    L"Browse Detail sets which counters the Browse... dialog lists, to keep the list short "
    L"for less experienced users. It is the starting value of that dialog's "
    L"\"Counter detail level\" box.\n\n"
    L"Novice: basic counters only.\n"
    L"Advanced: adds counters for experienced users.\n"
    L"Expert: adds counters for in-depth tuning.\n"
    L"Wizard: every counter.\n\n"
    L"Each level includes the levels below it. It does not change which counters are charted.";

static void ShowDetailInfo(HWND d) {
    int i = CbCur(d, IDC_DETAIL);
    SetDlgItemTextW(d, IDC_DETAILINFO, (i >= 0 && i < NDETAILS) ? kDetailInfo[i] : L"");
}

// Grid Color and Frame apply only to the Classic theme; Modern draws its own grid and edge.
// The title bar settings (hide the name, computer name, IP address) apply only to Modern; the
// Computer name and IPv4 address lists are also greyed out while their Hide box is ticked.
// Hiding the computer name in counter names applies to both themes.
static void EnableThemeControls(HWND d, int theme) {
    BOOL classic = theme == THEME_CLASSIC;
    EnableWindow(GetDlgItem(d, IDC_GRIDCOLOR), classic);
    EnableWindow(GetDlgItem(d, IDC_FRAME), classic);
    EnableWindow(GetDlgItem(d, IDC_HIDETITLE), !classic);
    EnableWindow(GetDlgItem(d, IDC_HIDEHOST), !classic);
    EnableWindow(GetDlgItem(d, IDC_HIDEIP), !classic);
    EnableWindow(GetDlgItem(d, IDC_HOSTNAME), !classic && IsDlgButtonChecked(d, IDC_HIDEHOST) != BST_CHECKED);
    EnableWindow(GetDlgItem(d, IDC_IPADDR), !classic && IsDlgButtonChecked(d, IDC_HIDEIP) != BST_CHECKED);
}

// Computer name and IPv4 address lists. Each entry's saved value is kept alongside it.
static std::vector<std::wstring> g_hostValues, g_ipValues;

static void FillNameAndIpLists(HWND d, const Settings& s) {
    auto fill = [&](int id, std::vector<std::wstring>& values, const std::vector<std::pair<std::wstring, std::wstring>>& items) {
        HWND cb = GetDlgItem(d, id);
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        values.clear();
        HDC dc = GetDC(cb);
        HGDIOBJ of = SelectObject(dc, (HFONT)SendMessageW(cb, WM_GETFONT, 0, 0));
        int widest = 0;
        for (const auto& it : items) {
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)it.first.c_str());
            values.push_back(it.second);
            SIZE sz = {};
            GetTextExtentPoint32W(dc, it.first.c_str(), (int)it.first.size(), &sz);
            widest = std::max(widest, (int)sz.cx);
        }
        SelectObject(dc, of);
        ReleaseDC(cb, dc);
        RECT r; GetWindowRect(cb, &r);
        SendMessageW(cb, CB_SETDROPPEDWIDTH, std::max((int)(r.right - r.left), widest + GetSystemMetrics(SM_CXVSCROLL) + 12), 0);
    };

    std::vector<std::pair<std::wstring, std::wstring>> hosts;
    hosts.push_back({ L"Default (" + LocalComputerName() + L")", L"" });
    bool haveSaved = s.hostName.empty() || s.hostName == NONE_VALUE;
    // With /showname (screenshots), only the stand-in is listed, not this PC's real names.
    std::vector<std::wstring> names = g_showName.empty() ? ComputerNames() : std::vector<std::wstring>{ g_showName };
    for (const auto& n : names) {
        hosts.push_back({ n, n });
        if (_wcsicmp(n.c_str(), s.hostName.c_str()) == 0) haveSaved = true;
    }
    if (!haveSaved) hosts.push_back({ s.hostName + L" (not found)", s.hostName });   // a name that has since been removed
    fill(IDC_HOSTNAME, g_hostValues, hosts);

    // With /showip (screenshots), only the stand-in address is shown, not the real network cards.
    // Every card's IPv4 address first, then the IPv6 addresses.
    std::vector<NetCard> cards = g_showIp.empty() ? NetCards() : std::vector<NetCard>{};
    std::wstring defIp = g_showIp.empty() ? DefaultIp(cards) : g_showIp;
    std::vector<std::pair<std::wstring, std::wstring>> ips;
    ips.push_back({ defIp.empty() ? std::wstring(L"Default (not connected)") : L"Default (" + defIp + L")", L"" });
    haveSaved = s.ipAdapter.empty() || s.ipAdapter == NONE_VALUE;
    for (const auto& c : cards) {
        if (c.ip.empty()) continue;
        ips.push_back({ c.ip + L"   " + c.name, c.guid });
        if (c.guid == s.ipAdapter) haveSaved = true;
    }
    for (const auto& c : cards)
        for (const auto& a : c.ip6) {
            std::wstring v = c.guid + IPV6_MARK + a;
            ips.push_back({ a + L"   " + c.name, v });
            if (_wcsicmp(v.c_str(), s.ipAdapter.c_str()) == 0) haveSaved = true;
        }
    if (!haveSaved) {
        size_t mark = s.ipAdapter.find(IPV6_MARK);
        ips.push_back({ mark == std::wstring::npos ? std::wstring(L"Saved network card (not connected)")
                                                   : s.ipAdapter.substr(mark + 3) + L" (not found)", s.ipAdapter });
    }
    fill(IDC_IPADDR, g_ipValues, ips);
}

// Monitor list: Primary, each monitor ("2: 2560 x 1440 (right)"), All monitors, Each monitor.
struct MonChoice { int mode; std::wstring id; int num; };
static std::vector<MonChoice> g_monValues;

static void FillMonitorList(HWND d, const Settings& s) {
    HWND cb = GetDlgItem(d, IDC_MONITOR);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    g_monValues.clear();
    std::vector<std::pair<std::wstring, MonChoice>> items;
    items.push_back({ L"Primary", { MONITOR_PRIMARY, L"", 0 } });
    std::vector<MonInfo> ms = Monitors();
    const MonInfo* prim = PrimaryMonitor(ms);
    bool haveSaved = s.monitorMode != MONITOR_ONE;
    for (const auto& m : ms) {
        items.push_back({ MonitorLabel(m, prim), { MONITOR_ONE, m.id, m.number } });
        if (s.monitorMode == MONITOR_ONE && (_wcsicmp(m.id.c_str(), s.monitorId.c_str()) == 0 ||
                                             (s.monitorId.empty() && m.number == s.monitorNum))) haveSaved = true;
    }
    if (!haveSaved)   // a monitor that is unplugged now: kept, and the chart uses the primary meanwhile
        items.push_back({ std::to_wstring(s.monitorNum) + L": (not connected)", { MONITOR_ONE, s.monitorId, s.monitorNum } });
    items.push_back({ L"All monitors", { MONITOR_ALL, L"", 0 } });
    items.push_back({ L"Each monitor", { MONITOR_EACH, L"", 0 } });
    HDC dc = GetDC(cb);
    HGDIOBJ of = SelectObject(dc, (HFONT)SendMessageW(cb, WM_GETFONT, 0, 0));
    int widest = 0;
    for (const auto& it : items) {
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)it.first.c_str());
        g_monValues.push_back(it.second);
        SIZE sz = {};
        GetTextExtentPoint32W(dc, it.first.c_str(), (int)it.first.size(), &sz);
        widest = std::max(widest, (int)sz.cx);
    }
    SelectObject(dc, of);
    ReleaseDC(cb, dc);
    RECT r; GetWindowRect(cb, &r);
    SendMessageW(cb, CB_SETDROPPEDWIDTH, std::max((int)(r.right - r.left), widest + GetSystemMetrics(SM_CXVSCROLL) + 12), 0);
    // Select the saved choice.
    int sel = 0;
    for (size_t i = 0; i < g_monValues.size(); i++) {
        const MonChoice& c = g_monValues[i];
        if (c.mode != s.monitorMode) continue;
        if (c.mode != MONITOR_ONE || _wcsicmp(c.id.c_str(), s.monitorId.c_str()) == 0 ||
            (s.monitorId.empty() && c.num == s.monitorNum)) { sel = (int)i; break; }
    }
    CbSel(d, IDC_MONITOR, sel);
}

static void SelectValue(HWND d, int id, const std::vector<std::wstring>& values, const std::wstring& v) {
    for (size_t i = 0; i < values.size(); i++)
        if (_wcsicmp(values[i].c_str(), v.c_str()) == 0) { CbSel(d, id, (int)i); return; }
    CbSel(d, id, 0);
}

static void ShowGeneral(HWND d) {
    const Settings& s = cfg.work;
    CbSel(d, IDC_DISPLAY, s.displayType);
    int ri = 0;
    for (int i = 0; i < NREFRESH; i++) if (kRefresh[i] <= s.refreshSec) ri = i;
    CbSel(d, IDC_REFRESH, ri);
    for (int i = 0; i < NSIZES; i++)          // list position whose item data is the saved index
        if ((int)SendDlgItemMessageW(d, IDC_WINSIZE, CB_GETITEMDATA, (WPARAM)i, 0) == s.windowSize) { CbSel(d, IDC_WINSIZE, i); break; }
    CbSel(d, IDC_MOVE, s.moveSpeed);
    CbSel(d, IDC_MOVESTYLE, s.moveStyle);
    CbSel(d, IDC_STARTPOS, s.startPos);
    for (int i = 0; i < NFRAMESTYLES; i++) if (kFrameOrder[i] == s.frameStyle) CbSel(d, IDC_FRAME, i);
    for (int i = 0; i < NTHEMES; i++) if (kThemeOrder[i] == s.theme) CbSel(d, IDC_THEME, i);
    FillMonitorList(d, s);
    CheckDlgButton(d, IDC_HIDETITLE, s.hideTitle ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_HIDEHOST, s.hideHost ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_HIDEIP, s.hideIp ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_HIDECTRHOST, s.hideCounterHost ? BST_CHECKED : BST_UNCHECKED);
    FillNameAndIpLists(d, s);
    SelectValue(d, IDC_HOSTNAME, g_hostValues, s.hostName);
    SelectValue(d, IDC_IPADDR, g_ipValues, s.ipAdapter);
    EnableThemeControls(d, s.theme);
    EnableWindow(GetDlgItem(d, IDC_MOVESTYLE), s.moveSpeed != 0);   // nothing moves at "None"
    CbSel(d, IDC_GRIDCOLOR, s.gridColor);
    InvalidateRect(GetDlgItem(d, IDC_GRIDSWATCH), nullptr, TRUE);
    CbSel(d, IDC_DETAIL, s.browseDetail);
    ShowDetailInfo(d);
    SetDlgItemInt(d, IDC_AUTOSCALE, s.autoScaleup, FALSE);
}

static void HarvestGeneral(HWND d) {
    Settings& s = cfg.work;
    s.displayType  = std::max(0, CbCur(d, IDC_DISPLAY));
    s.refreshSec   = kRefresh[std::max(0, CbCur(d, IDC_REFRESH))];
    int wsel = CbCur(d, IDC_WINSIZE);
    s.windowSize   = wsel >= 0 ? (int)SendDlgItemMessageW(d, IDC_WINSIZE, CB_GETITEMDATA, (WPARAM)wsel, 0) : 0;
    s.moveSpeed    = std::max(0, CbCur(d, IDC_MOVE));
    s.gridColor    = std::max(0, CbCur(d, IDC_GRIDCOLOR));
    s.moveStyle    = std::max(0, CbCur(d, IDC_MOVESTYLE));
    s.startPos     = std::max(0, CbCur(d, IDC_STARTPOS));
    s.frameStyle   = kFrameOrder[std::clamp(CbCur(d, IDC_FRAME), 0, NFRAMESTYLES - 1)];
    s.theme        = kThemeOrder[std::clamp(CbCur(d, IDC_THEME), 0, NTHEMES - 1)];
    int ms = CbCur(d, IDC_MONITOR);
    if (ms >= 0 && ms < (int)g_monValues.size()) {
        s.monitorMode = g_monValues[ms].mode;
        if (s.monitorMode == MONITOR_ONE) { s.monitorId = g_monValues[ms].id; s.monitorNum = g_monValues[ms].num; }
    }
    s.hideTitle    = IsDlgButtonChecked(d, IDC_HIDETITLE) == BST_CHECKED;
    s.hideHost     = IsDlgButtonChecked(d, IDC_HIDEHOST) == BST_CHECKED;
    s.hideIp       = IsDlgButtonChecked(d, IDC_HIDEIP) == BST_CHECKED;
    s.hideCounterHost = IsDlgButtonChecked(d, IDC_HIDECTRHOST) == BST_CHECKED;
    int hs = CbCur(d, IDC_HOSTNAME), is = CbCur(d, IDC_IPADDR);
    if (hs >= 0 && hs < (int)g_hostValues.size()) s.hostName = g_hostValues[hs];
    if (is >= 0 && is < (int)g_ipValues.size()) s.ipAdapter = g_ipValues[is];
    s.browseDetail = std::max(0, CbCur(d, IDC_DETAIL));
    BOOL ok = FALSE;
    UINT v = GetDlgItemInt(d, IDC_AUTOSCALE, &ok, FALSE);
    if (ok) s.autoScaleup = v;
}

static void HarvestAll(HWND d) { HarvestCounter(d); HarvestGeneral(d); }

static void FillSchemes(HWND d, const std::wstring& text) {
    SendDlgItemMessageW(d, IDC_SCHEME, CB_RESETCONTENT, 0, 0);
    for (auto& n : EnumSchemes()) CbAdd(d, IDC_SCHEME, n);
    SetDlgItemTextW(d, IDC_SCHEME, text.c_str());
}

static void DoBrowse(HWND d) {
    HarvestCounter(d);
    CounterCfg& c = cfg.work.c[cfg.cur];
    DWORD detail = kDetails[cfg.work.browseDetail].level;
    std::wstring path = c.path;
    bool picked = false;
    if (Use32Effective(c)) {
        // Hand the helper the objects the 64-bit side sees, so its Browse list can mark the
        // 32-bit-only ones with [32-bit*]. They go straight to the helper on a private pipe.
        std::string list;
        DWORD n = 0;
        if (PdhEnumObjectsW(nullptr, nullptr, nullptr, &n, PERF_DETAIL_WIZARD, TRUE) == PDH_MORE_DATA && n) {
            std::vector<wchar_t> buf(n + 2, L'\0');
            if (PdhEnumObjectsW(nullptr, nullptr, buf.data(), &n, PERF_DETAIL_WIZARD, FALSE) == ERROR_SUCCESS) {
                std::wstring w;
                for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) { w += p; w += L'\n'; }
                list = ToUtf8(w);
            }
        }
        wchar_t args[96];
        swprintf(args, 96, L"/browse32 %llu %lu %d ", (unsigned long long)(ULONG_PTR)d, detail, list.empty() ? 0 : 1);
        std::wstring out;
        if (RunHelperCapture(args + path, out, list.empty() ? nullptr : &list) == 0 && !out.empty()) { path = out; picked = true; }
    } else {
        picked = BrowseCounterNative(d, detail, path);
    }
    if (picked && path.find_first_of(L"*?") != std::wstring::npos) {
        MessageBoxW(d, L"QuickMon Forever charts one line per counter, so it needs a single instance.\n\n"
                       L"In Browse, pick one instance (such as _Total) instead of <All instances> "
                       L"or <All searched instances>.",
                    L"QuickMon Forever", MB_OK | MB_ICONINFORMATION);
        picked = false;
    }
    if (picked) {
        c.path = path;
        c.enabled = TRUE;
        ShowCounter(d);
    }
}

static void DoExplain(HWND d) {
    HarvestCounter(d);
    const CounterCfg& c = cfg.work.c[cfg.cur];
    if (c.path.empty()) return;
    std::wstring owner = std::to_wstring((unsigned long long)(ULONG_PTR)d);
    if (Use32Effective(c)) {
        std::wstring out;
        RunHelperCapture(L"/explain32 " + owner + (Only32(c.path) ? L" 1 " : L" 0 ") + c.path, out);
        return;
    }
    std::wstring text = ExplainCounterNative(c.path);
#ifdef _WIN64
    // Not visible to 64-bit processes? Ask the 32-bit providers.
    if (text.rfind(L"This counter is not available", 0) == 0 && HelperAvailable()) {
        std::wstring out;
        RunHelperCapture(L"/explain32 " + owner + (IsRemoteCounterPath(c.path) ? L" 0 " : L" 1 ") + c.path, out);
        return;
    }
#endif
    MessageBoxW(d, text.c_str(), (CounterTitle(c.path) + BitTag(c)).c_str(), MB_OK | MB_ICONINFORMATION);
}

// "Set Default" stores these settings for the logon desktop (HKEY_USERS\.DEFAULT),
// which is what the original did for NT's logon screen saver.
// True when path is inside a folder only administrators can change: the Windows folder
// (including System32) or Program Files. The sign-in screen runs its screen saver before anyone
// signs in, so registering a copy that ordinary users could replace would let them run their own
// program there.
static bool InProtectedFolder(const std::wstring& path) {
    auto under = [&](const std::wstring& dir) {
        if (dir.empty()) return false;
        std::wstring d = dir;
        if (d.back() != L'\\') d += L'\\';
        return path.size() > d.size() && _wcsnicmp(path.c_str(), d.c_str(), d.size()) == 0;
    };
    wchar_t buf[MAX_PATH] = L"";
    if (GetWindowsDirectoryW(buf, MAX_PATH) && under(buf)) return true;
    for (const wchar_t* var : { L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432" }) {
        DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);
        if (n && n < MAX_PATH && under(buf)) return true;
    }
    return false;
}

// ---- The sign-in screen saver (Set Default / Clear Default) ----
// The dialog's settings reach the elevated copy in a volatile registry key under the user's own
// settings (HKCU\Software\QuickMonForever\SetDefault-<process ID>): Windows keeps a volatile key
// only in memory and discards it at the next restart, so nothing is ever left on disk. The
// elevated copy may run as a different administrator account, so it opens the key through
// HKEY_USERS\<the user's SID>.

static const wchar_t SETDEFAULT_KEY_PREFIX[] = L"SetDefault-";

// The value names the program writes, and their types (REG_DWORD or REG_SZ): only these are
// copied to the sign-in screen, so the key can't put anything else in the registry.
static std::map<std::wstring, DWORD> AllowedSettingValues() {
    std::map<std::wstring, DWORD> out;
    std::wstring sub = std::wstring(REG_ROOT) + L"\\Names-" + std::to_wstring(GetCurrentProcessId());
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return out;
    Settings defaults;
    DefaultSettings(defaults);
    WriteSettings(k, defaults);
    for (DWORD i = 0;; i++) {
        wchar_t name[256]; DWORD nlen = 256, type = 0;
        if (RegEnumValueW(k, i, name, &nlen, nullptr, &type, nullptr, nullptr) != ERROR_SUCCESS) break;
        out[name] = type;
    }
    RegCloseKey(k);
    RegDeleteTreeW(HKEY_CURRENT_USER, sub.c_str());
    return out;
}

// Copies the allowed values from src to dst.
static bool CopyAllowedSettings(HKEY src, HKEY dst) {
    std::map<std::wstring, DWORD> allowed = AllowedSettingValues();
    if (allowed.empty()) return false;
    for (DWORD i = 0;; i++) {
        wchar_t name[256]; DWORD nlen = 256, type = 0;
        if (RegEnumValueW(src, i, name, &nlen, nullptr, &type, nullptr, nullptr) != ERROR_SUCCESS) break;
        auto it = allowed.find(name);
        if (it == allowed.end() || it->second != type) continue;
        if (type == REG_DWORD) RegSetDw(dst, name, RegGetDw(src, name, 0));
        else if (type == REG_SZ) RegSetSz(dst, name, RegGetSz(src, name, L""));
    }
    return true;
}

// Sets or clears the sign-in screen saver. Needs administrator rights; returns a Windows error
// code (ERROR_ACCESS_DENIED without them). For Set, settings is an open key holding the settings
// to use. Set Default remembers whether ScreenSaveActive was set before (PrevScreenSaveActive),
// so Clear Default can put it back, and removes the copy of the settings it made.
static DWORD ApplySignInScreenSaver(bool set, HKEY settings) {
    static const wchar_t DESKTOP[] = L".DEFAULT\\Control Panel\\Desktop";
    std::wstring settingsKey = std::wstring(L".DEFAULT\\") + REG_ROOT;
    HKEY desk;
    LONG r = RegOpenKeyExW(HKEY_USERS, DESKTOP, 0, KEY_READ | KEY_WRITE, &desk);
    if (r != ERROR_SUCCESS) return (DWORD)r;
    if (set) {
        if (!InProtectedFolder(ModulePath()) || !settings) { RegCloseKey(desk); return ERROR_ACCESS_DENIED; }
        HKEY sk;
        r = RegCreateKeyExW(HKEY_USERS, settingsKey.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &sk, nullptr);
        if (r != ERROR_SUCCESS) { RegCloseKey(desk); return (DWORD)r; }
        // Remember the earlier ScreenSaveActive once (a second Set Default keeps the original).
        if (RegGetSz(sk, L"PrevScreenSaveActive", L"").empty())
            RegSetSz(sk, L"PrevScreenSaveActive", RegGetSz(desk, L"ScreenSaveActive", L"<none>"));
        bool ok = CopyAllowedSettings(settings, sk);
        RegCloseKey(sk);
        if (!ok) { RegCloseKey(desk); return ERROR_INVALID_DATA; }
        RegSetSz(desk, L"SCRNSAVE.EXE", ModulePath());
        RegSetSz(desk, L"ScreenSaveActive", L"1");
    } else {
        RegDeleteValueW(desk, L"SCRNSAVE.EXE");
        HKEY sk;
        if (RegOpenKeyExW(HKEY_USERS, settingsKey.c_str(), 0, KEY_READ, &sk) == ERROR_SUCCESS) {
            std::wstring prev = RegGetSz(sk, L"PrevScreenSaveActive", L"");
            RegCloseKey(sk);
            if (prev == L"<none>") RegDeleteValueW(desk, L"ScreenSaveActive");
            else if (!prev.empty()) RegSetSz(desk, L"ScreenSaveActive", prev);
            RegDeleteTreeW(HKEY_USERS, settingsKey.c_str());
        }
    }
    RegCloseKey(desk);
    return ERROR_SUCCESS;
}

// The SID of the account this process runs as, as text ("S-1-5-21-...").
static std::wstring CurrentUserSid() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return L"";
    DWORD sz = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &sz);
    std::vector<BYTE> buf(sz ? sz : 1);
    std::wstring out;
    if (sz && GetTokenInformation(tok, TokenUser, buf.data(), sz, &sz)) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(((TOKEN_USER*)buf.data())->User.Sid, &s)) { out = s; LocalFree(s); }
    }
    CloseHandle(tok);
    return out;
}

// Runs this program again with administrator rights, showing the Windows UAC prompt, and waits.
// Windows doesn't offer "run as administrator" for .scr files, so the file is launched as an
// ordinary program (class "exefile"). Returns its exit code, -2 if the prompt was cancelled, or
// -1 if it could not start.
static int RunElevatedSelf(HWND owner, const std::wstring& params) {
    std::wstring exe = ModulePath();
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_CLASSNAME | SEE_MASK_NOASYNC;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.lpClass = L"exefile";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return GetLastError() == ERROR_CANCELLED ? -2 : -1;
    if (!sei.hProcess) return -1;
    for (;;) {   // keep the dialog painting while waiting
        DWORD w = MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, INFINITE, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) break;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}

static void DoSetDefault(HWND d, bool set) {
    HarvestAll(d);
    if (set && !InProtectedFolder(ModulePath())) {
        std::wstring msg =
            L"QuickMon Forever can only be set as the sign-in screen saver when it is installed in a "
            L"protected folder, such as C:\\Windows\\System32 (see Install in the README).\n\n"
            L"Windows runs the sign-in screen saver before anyone signs in. From a folder that ordinary "
            L"users can change, such as Downloads, someone could replace the file and run their own "
            L"program there.\n\nThis copy is in:\n" + ModulePath();
        MessageBoxW(d, msg.c_str(), L"QuickMon Forever", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return;
    }
    // Set Default: the dialog's current settings, in a volatile key under the user's settings.
    std::wstring keyName = std::wstring(SETDEFAULT_KEY_PREFIX) + std::to_wstring(GetCurrentProcessId());
    std::wstring keyPath = std::wstring(REG_ROOT) + L"\\" + keyName;
    HKEY settings = nullptr;
    if (set) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS,
                            nullptr, &settings, nullptr) != ERROR_SUCCESS) {
            MessageBoxW(d, L"Could not prepare the settings for the sign-in screen.", L"QuickMon Forever", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            return;
        }
        WriteSettings(settings, cfg.work);
    }
    DWORD r = ApplySignInScreenSaver(set, settings);
    if (settings) RegCloseKey(settings);
    if (r == ERROR_ACCESS_DENIED) {
        // Not running as administrator: ask Windows (UAC) to run just this change elevated.
        std::wstring params = set ? L"/setdefault " + CurrentUserSid() + L" " + keyName : L"/cleardefault";
        int code = RunElevatedSelf(d, params);
        SetForegroundWindow(d);   // back in front after the prompt, so the result isn't hidden
        if (code == -2) {
            if (set) RegDeleteTreeW(HKEY_CURRENT_USER, keyPath.c_str());
            MessageBoxW(d, set ? L"The sign-in screen saver was not changed: the administrator prompt was cancelled."
                               : L"The sign-in screen saver was not cleared: the administrator prompt was cancelled.",
                        L"QuickMon Forever", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            return;
        }
        r = code == 0 ? ERROR_SUCCESS : (code == -1 ? ERROR_ELEVATION_REQUIRED : (DWORD)code);
    }
    if (set) RegDeleteTreeW(HKEY_CURRENT_USER, keyPath.c_str());
    if (r != ERROR_SUCCESS) {
        wchar_t msg[256];
        swprintf(msg, 256, L"The sign-in screen saver could not be %s (error %lu).", set ? L"set" : L"cleared", r);
        MessageBoxW(d, msg, L"QuickMon Forever", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return;
    }
    if (set)
        MessageBoxW(d, L"QuickMon Forever is now the default screen saver for the logon desktop, using the current settings.\n\n"
                       L"Note: Windows 10 and 11 normally show the lock screen instead of running a screen saver "
                       L"when nobody is signed in, so this mainly affects new user profiles.",
                    L"QuickMon Forever", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    else
        MessageBoxW(d, L"The default (logon) screen saver has been cleared.", L"QuickMon Forever", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

// The elevated copy started by DoSetDefault: "/setdefault <user SID> SetDefault-<process ID>" or
// "/cleardefault". No windows; the exit code is the result (0 = done).
static int RunSignInScreenSaverCommand(const std::vector<std::wstring>& a) {
    // After the UAC prompt this process may bring windows to the front; pass that on, so the
    // setup dialog's result message doesn't open behind other windows.
    AllowSetForegroundWindow(ASFW_ANY);
    bool set = !a.empty() && _wcsicmp(a[0].c_str(), L"/setdefault") == 0;
    if (!set) return (int)ApplySignInScreenSaver(false, nullptr);
    if (a.size() < 3) return (int)ERROR_INVALID_PARAMETER;
    // Only a SID ("S-1-" then digits and dashes) and a key named SetDefault-<digits>.
    const std::wstring& sid = a[1];
    const std::wstring& name = a[2];
    bool sidOk = sid.size() > 4 && sid.compare(0, 4, L"S-1-") == 0 && sid.find_first_not_of(L"S0123456789-") == std::wstring::npos;
    size_t plen = wcslen(SETDEFAULT_KEY_PREFIX);
    bool nameOk = name.size() > plen && name.compare(0, plen, SETDEFAULT_KEY_PREFIX) == 0 &&
                  name.find_first_not_of(L"0123456789", plen) == std::wstring::npos;
    if (!sidOk || !nameOk) return (int)ERROR_INVALID_PARAMETER;
    HKEY settings;
    LONG r = RegOpenKeyExW(HKEY_USERS, (sid + L"\\" + REG_ROOT + L"\\" + name).c_str(), 0, KEY_READ, &settings);
    if (r != ERROR_SUCCESS) return (int)r;
    DWORD res = ApplySignInScreenSaver(true, settings);
    RegCloseKey(settings);
    return (int)res;
}

static INT_PTR CALLBACK ConfigProc(HWND d, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: {
        // Title-bar and taskbar/Alt+Tab icons: the settings (gear) icon. Shared resource icons, no cleanup.
        SendMessageW(d, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(g.hi, MAKEINTRESOURCEW(IDI_SETTINGS), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        SendMessageW(d, WM_SETICON, ICON_BIG, (LPARAM)LoadImageW(g.hi, MAKEINTRESOURCEW(IDI_SETTINGS), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
        for (int i = 0; i < MAX_COUNTERS; i++) CbAdd(d, IDC_COUNTER, L"Counter " + std::to_wstring(i + 1));
        for (int i = 0; i < NCOLORS; i++) CbAdd(d, IDC_COLOR, kColors[i].name);
        CbAdd(d, IDC_COLOR, L"Custom...");                      // index COLOR_CUSTOM
        CbAdd(d, IDC_SCALE, L"Auto");
        CbAdd(d, IDC_SCALE, L"Default");
        for (int e = 7; e >= -7; e--) CbAdd(d, IDC_SCALE, ScaleStr(e));
        for (int i = 0; i < 3; i++) CbAdd(d, IDC_DISPLAY, kDisplays[i]);
        for (int i = 0; i < NREFRESH; i++) CbAdd(d, IDC_REFRESH, RefreshLabel(kRefresh[i]));
        for (int i = 0; i < NSIZES; i++) {
            int idx = kSizeOrder[i];
            LRESULT pos = SendDlgItemMessageW(d, IDC_WINSIZE, CB_ADDSTRING, 0, (LPARAM)kSizes[idx].name);
            SendDlgItemMessageW(d, IDC_WINSIZE, CB_SETITEMDATA, (WPARAM)pos, (LPARAM)idx);
        }
        for (int i = 0; i < NMOVES; i++) CbAdd(d, IDC_MOVE, kMoves[i].name);
        for (int i = 0; i < NMOVESTYLES; i++) CbAdd(d, IDC_MOVESTYLE, kMoveStyles[i]);
        {
            // Let the open list be wider than the box so the long entry shows in full.
            HWND cb = GetDlgItem(d, IDC_MOVESTYLE);
            HDC dc = GetDC(cb);
            HGDIOBJ of = SelectObject(dc, (HFONT)SendMessageW(cb, WM_GETFONT, 0, 0));
            int widest = 0;
            for (int i = 0; i < NMOVESTYLES; i++) {
                SIZE sz; GetTextExtentPoint32W(dc, kMoveStyles[i], (int)wcslen(kMoveStyles[i]), &sz);
                widest = std::max(widest, (int)sz.cx);
            }
            SelectObject(dc, of); ReleaseDC(cb, dc);
            SendMessageW(cb, CB_SETDROPPEDWIDTH, widest + GetSystemMetrics(SM_CXVSCROLL) + 8, 0);
        }
        for (int i = 0; i < NSTARTPOS; i++) CbAdd(d, IDC_STARTPOS, kStartPositions[i]);
        for (int i = 0; i < NFRAMESTYLES; i++) CbAdd(d, IDC_FRAME, kFrameStyles[kFrameOrder[i]]);
        for (int i = 0; i < NTHEMES; i++) CbAdd(d, IDC_THEME, kThemes[kThemeOrder[i]]);
        for (int i = 0; i < NGRIDCOLORS; i++) CbAdd(d, IDC_GRIDCOLOR, kGridColors[i].name);
        for (int i = 0; i < NDETAILS; i++) CbAdd(d, IDC_DETAIL, kDetails[i].name);
        SendDlgItemMessageW(d, IDC_SCHEME, CB_LIMITTEXT, 200, 0);
        SetDlgItemTextW(d, IDC_VERSION, VERSION_STR);
#ifdef _WIN64
        if (!HelperAvailable()) {
            EnableWindow(GetDlgItem(d, IDC_USE32), FALSE);
            SetDlgItemTextW(d, IDC_USE32, L"&32-bit counter (QuickMonForever-32.exe not found next to the .scr)");
        }
#else
        SetDlgItemTextW(d, IDC_USE32, *Own32Tag() ? L"&32-bit counter (this is the 32-bit build: always on)"
                                                  : L"&32-bit counter (32-bit Windows: every counter is 32-bit)");
        EnableWindow(GetDlgItem(d, IDC_USE32), FALSE);
#endif
        FillSchemes(d, cfg.work.scheme);
        ShowGeneral(d);
        ShowCounter(d);
        if (HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX | TTS_BALLOON,
                                       CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, d, nullptr, nullptr, nullptr)) {
            SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, 360);
            SendMessageW(tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_DETAIL), kDetailTooltip);
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_DETAILINFO), kDetailTooltip);

            // Counter box.
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_COUNTER),
                L"Picks which of the 8 counter slots the settings in this box apply to. "
                L"Each slot is one line on the chart.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_ENABLED),
                L"Checked: This counter is charted.\n"
                L"Unchecked: It is hidden, but its settings are kept.\n"
                L"The text names the counter, its instance in parentheses, and the computer it is read from.");
            static const wchar_t kColorTip[] =
                L"The color of this counter's line on the chart and of its square in the legend.\n"
                L"Custom... (the last entry) opens the Windows color dialog to pick any color. "
                L"The entry then shows the color's hex value, such as #FF8800 (red, green and blue, "
                L"00 to FF each). Choose it again, or click the color square, to change it.";
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_COLOR), kColorTip);
            AddAreaTip(tip, d, GetDlgItem(d, IDC_SWATCH), 0x10000 + IDC_SWATCH, kColorTip);
            AddTipAlways(tip, d, IDC_EXPLAIN,
                L"Shows the Windows description of this counter: what it measures and how to read it.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_SCALE),
                L"What this counter's values are multiplied by before they are charted; the legend "
                L"shows it as a ratio such as 10:1.\n"
                L"Auto: Like QuickMon 1.10, it zooms out at once when the line goes off the top of the "
                L"chart, and zooms in after the Scale-up delay.\n"
                L"Default: The scale Windows suggests for this counter.\n"
                L"A number (10000000 down to 0.0000001): Always that scale.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_BROWSE),
                L"Opens the Windows Browse Performance Counters window to pick the counter for this "
                L"slot, on this computer or another one on the network.");
#ifdef _WIN64
            AddTipAlways(tip, d, IDC_USE32,
                L"Reads this slot's counter through QuickMonForever-32.exe, which the screen saver starts "
                L"hidden and stops when it closes. Each slot has its own box, so 64-bit and 32-bit "
                L"counters can be mixed on one chart.\n\n"
                L"Built-in counters come in both bitnesses: Windows has a 64-bit and a 32-bit version "
                L"of each, and both expose the same system performance value, so % Processor Time "
                L"reads the same through either. Only a few counters, from older or third-party "
                L"software that ships just a 32-bit provider DLL, exist in 32-bit form alone: they "
                L"appear in Browse only when this box is ticked.\n\n"
                L"If this checkbox is ticked, Browse and Explain go through the 32-bit side, and Browse lists the "
                L"built-in counters as well as the 32-bit-only ones.\n\n"
                L"[32-bit]: the counter is read through the 32-bit side.\n"
                L"[32-bit*]: a 32-bit-only counter (the 64-bit side cannot see it). It is read through "
                L"the 32-bit side even when this box is not ticked.\n\n"
                L"The markers show after the counter's name here, in the chart legend and in the "
                L"Explain title. In the 32-bit Browse window, the title says [32-bit] and each "
                L"32-bit-only object says [32-bit*].");
#else
            AddTipAlways(tip, d, IDC_USE32, *Own32Tag()
                ? L"This is QuickMonForever-32.exe, the 32-bit build, so it reads every counter through the "
                  L"32-bit side and this box is always on. Counters are marked [32-bit].\n\n"
                  L"On 64-bit Windows, use QuickMonForever.scr instead: it reads counters natively and "
                  L"uses this program only for the slots that need the 32-bit side."
                : L"This is 32-bit Windows, so every counter is 32-bit and there is no 64-bit side. "
                  L"This box does not apply, and counters carry no [32-bit] marker.");
#endif

            // Buttons.
            BrowseAddTip(tip, d, GetDlgItem(d, IDOK), L"Saves these settings and closes this window.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDCANCEL),
                L"Closes this window without saving. Settings tried out with Preview are put back as they were.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_SETDEFAULT),
                L"Makes QuickMon Forever, with these settings, the screen saver for the Windows sign-in "
                L"screen. Needs administrator rights.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_CLEARDEFAULT),
                L"Stops QuickMon Forever being the screen saver for the Windows sign-in screen. "
                L"Needs administrator rights.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_PREVIEW),
                L"Runs the screen saver full screen with these settings. Move the mouse or press a key "
                L"to come back. The settings are saved for the preview; Cancel puts back the old ones.");

            // General Settings.
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_DISPLAY),
                L"Both: The chart with the legend (each counter's value, scale and name) over it.\n"
                L"Chart: The chart only.\n"
                L"Legend: The legend only.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_REFRESH),
                L"How often the counters are read. Each reading moves the chart one step; a grid "
                L"square holds 10 readings, shown on the chart as Seconds/horizontal division.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_WINSIZE),
                L"\"The screen\" is the monitor the chart is on (see Monitor), or the whole area for "
                L"All monitors.\n"
                L"Auto (one third): One third of the screen each way, like QuickMon 1.10.\n"
                L"20% to 100% of screen: That share of the screen each way, for very large monitors.\n"
                L"3840 x 2160 to 640 x 400: Exactly that many pixels (the common Windows display "
                L"resolutions), or the whole screen if it is smaller.\n"
                L"Full Screen: Fills the screen and does not move.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_MOVE),
                L"How fast the window wanders around the screen, so nothing stays in one place long "
                L"enough to burn in.\n"
                L"None: It stays where it starts.\n"
                L"Slow, Medium, Fast: With Classic movement, a 5-pixel jump every 30, 15 or 5 seconds, "
                L"as in QuickMon 1.10.");
            static const wchar_t kGridTip[] =
                L"The color of the chart's grid lines; the window's frame follows it. Gray is the "
                L"original's grid. None hides the grid.\n"
                L"It does not change the rescale markers: they are in the counter's line color, or "
                L"white when several counters rescale at once.";
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_GRIDCOLOR), kGridTip);
            AddAreaTip(tip, d, GetDlgItem(d, IDC_GRIDSWATCH), 0x10000 + IDC_GRIDSWATCH, kGridTip);

            // Schemes and the rest.
            static const wchar_t kSchemeTip[] =
                L"A scheme is a saved, named set of all these settings. Pick one from the list to load "
                L"it, or type a new name and click Save.";
            HWND scheme = GetDlgItem(d, IDC_SCHEME);
            BrowseAddTip(tip, d, scheme, kSchemeTip);
            BrowseAddTip(tip, d, FindWindowExW(scheme, nullptr, L"Edit", nullptr), kSchemeTip);   // the typing area
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_SAVE), L"Saves the current settings as a scheme, under the name in the box.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_DELETE), L"Deletes the scheme named in the box.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_ABOUT), L"Version and credits.");
            AddTipAlways(tip, d, IDC_MOVESTYLE,
                L"Smooth: The window glides, moving a little on every screen refresh.\n"
                L"Stepped: It glides on a timer about 20 times a second, in 2-3 pixel steps; "
                L"thin grid lines can seem to flicker.\n"
                L"Classic: Like QuickMon 1.10, it stays still and jumps diagonally every "
                L"5 seconds (Fast), 15 seconds (Medium) or 30 seconds (Slow). "
                L"The jump is scaled to the screen height (5 pixels at 1080 lines, 7 at 1440, "
                L"10 at 2160), so it looks the same size on any screen.\n"
                L"Classic (5 px): Same as Classic, but it jumps exactly 5 pixels, "
                L"like QuickMon 1.10.\n"
                L"For the original Classic look, also set Start Position to Top Left.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_FRAME),
                L"Raised 3D: Like QuickMon 1.10. The original draws a raised 3D edge, 3 pixels on "
                L"the top and left and 2 on the right and bottom. With the Gray grid it uses the "
                L"original's colors; other grid colors get matching shades.\n"
                L"Flat: A 2-pixel border in a bright shade of the grid color.\n"
                L"Only the Classic theme has a frame; Modern draws its own thin edge.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_THEME),
                L"How the chart window looks.\n"
                L"Modern QuickMon 2.10 (default): This rewrite's own look. A title bar with the "
                L"computer, time and refresh rate, a 0-100 axis, and a legend table under the "
                L"chart with each counter's scale and its last, average, minimum and maximum "
                L"values.\n"
                L"Classic QuickMon 1.10: The original's look, in tribute to Tim Hill's QuickMon "
                L"(1998). A framed chart with a square grid, the legend over the top-left corner "
                L"(value, color, scale and counter) and the seconds per grid division.\n"
                L"Grid Color and Frame apply only to Classic; the name, Computer name and IPv4 "
                L"address settings below apply only to Modern.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_HIDETITLE),
                L"Modern theme: leaves \"QuickMon Forever\" out of the top-left corner of the chart "
                L"window. A scheme's name still shows there when a scheme is in use.\n"
                L"The Classic theme never shows the name.\n"
                L"Alt+Q moves here; the arrow keys move between the Hide boxes, Space ticks one.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_HIDEHOST),
                L"Modern theme: leaves the computer name out of the top-right corner of the chart "
                L"window. The Computer name list below is greyed out while this is ticked.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_HIDEIP),
                L"Modern theme: leaves the IP address out of the top-right corner of the chart "
                L"window. The IP address list below is greyed out while this is ticked.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_HIDECTRHOST),
                L"Both themes: leaves \" on \\\\THISPC\" off the end of each counter's name in the "
                L"legend, for counters read from this PC. Counters read from another computer keep "
                L"its name, so you can still tell where they come from.\n"
                L"The setup dialog always shows the full name.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_HOSTNAME),
                L"Modern theme: the computer name shown in the top-right corner, before the IP "
                L"address and the time.\n"
                L"Default: this PC's name. The list also has every other name this PC goes by: "
                L"its full DNS name and any alternate computer names. To leave the name out, tick "
                L"Hide the computer name.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_IPADDR),
                L"Modern theme: the IP address shown in the top-right corner, between the "
                L"computer name and the time.\n"
                L"Default: the network card Windows uses to reach the internet, with its IPv4 "
                L"address (its IPv6 address if the network has no IPv4). Or pick an address: each "
                L"card's IPv4 address is listed first, then its IPv6 addresses (global and unique "
                L"local; a card's stable address comes before its temporary ones). The choice is "
                L"remembered by card, so a new address from the router still shows.\n"
                L"Not listed: addresses of cards with no network (169.254.x.x, fe80::). To leave "
                L"the address out, tick Hide the IP address.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_AUTOSCALE),
                L"How long, in seconds, a counter with Scale set to Auto must stay low before it is "
                L"zoomed in. If its line stays below 8% of the chart height for this long, the scale "
                L"goes up 10 times (for example 1:1 to 10:1) so the line is easier to see.\n"
                L"900 seconds is 15 minutes, as in QuickMon 1.10, the value of 0 means never scale up.\n"
                L"Scaling down is immediate: as soon as a value goes above 110% of the chart, the "
                L"scale drops so it fits.\n"
                L"Each change is marked by a thick vertical line: in the counter's own line color "
                L"when one counter rescaled, or white when two or more rescaled on the same sample "
                L"(the grid color never changes it). Counters that stay low from start-up all reach "
                L"this delay together, so a white line about 15 minutes in is normal.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_STARTPOS),
                L"Where the chart window first appears on its monitor (see Monitor). It moves away "
                L"from that spot: down and right from Top Left (as QuickMon 1.10 did), down and left "
                L"from Top Right, straight down from Top Center.");
            BrowseAddTip(tip, d, GetDlgItem(d, IDC_MONITOR),
                L"Which monitor the chart uses. Every other monitor is blacked out.\n"
                L"Primary: the main monitor (as before).\n"
                L"1, 2, 3...: that monitor, numbered as Windows numbers its displays, with its size and "
                L"where it sits next to the primary one. It is remembered by the monitor itself, so it "
                L"stays on the same screen if the numbers change; if it is unplugged, the chart uses "
                L"the primary monitor meanwhile.\n"
                L"All monitors: one chart that floats across every monitor as one big screen. It keeps "
                L"to the area the monitors fully cover, so it never drifts into a gap between screens "
                L"of different sizes. Window Size percentages are of that whole area.\n"
                L"Each monitor: a separate chart on every monitor, each moving on its own.");
        }
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDC_COUNTER:
            if (code == CBN_SELCHANGE) {
                HarvestCounter(d);
                cfg.cur = std::max(0, CbCur(d, IDC_COUNTER));
                ShowCounter(d);
            }
            return TRUE;
        case IDC_COLOR:
            // Custom...: open the color dialog once the choice is final (the list has closed, or
            // the arrow keys moved onto it while closed). Posted so the drop-down finishes first.
            if (CbCur(d, IDC_COLOR) == COLOR_CUSTOM &&
                (code == CBN_SELENDOK || (code == CBN_SELCHANGE && !SendDlgItemMessageW(d, IDC_COLOR, CB_GETDROPPEDSTATE, 0, 0)))) {
                if (!g_pickPending) { g_pickPending = true; PostMessageW(d, WM_APP_PICKCOLOR, 0, 0); }
                return TRUE;
            }
            if (code == CBN_SELCHANGE) { HarvestCounter(d); InvalidateRect(GetDlgItem(d, IDC_SWATCH), nullptr, TRUE); }
            return TRUE;
        case IDC_SCALE:
            if (code == CBN_SELCHANGE) HarvestCounter(d);
            return TRUE;
        case IDC_ENABLED:
            if (code == BN_CLICKED) HarvestCounter(d);
            return TRUE;
        case IDC_USE32:
            if (code == BN_CLICKED) { HarvestCounter(d); ShowCounter(d); }   // updates the [32-bit] marker
            return TRUE;
        case IDC_DETAIL:
            if (code == CBN_SELCHANGE) ShowDetailInfo(d);
            return TRUE;
        case IDC_MOVE:
            if (code == CBN_SELCHANGE) EnableWindow(GetDlgItem(d, IDC_MOVESTYLE), CbCur(d, IDC_MOVE) > 0);
            return TRUE;
        case IDC_THEME:
            if (code == CBN_SELCHANGE) EnableThemeControls(d, kThemeOrder[std::clamp(CbCur(d, IDC_THEME), 0, NTHEMES - 1)]);
            return TRUE;
        case IDC_HIDEHOST: case IDC_HIDEIP:   // grey out the list whose value is hidden
            if (code == BN_CLICKED) EnableThemeControls(d, kThemeOrder[std::clamp(CbCur(d, IDC_THEME), 0, NTHEMES - 1)]);
            return TRUE;
        case IDC_GRIDCOLOR:
            if (code == CBN_SELCHANGE) {
                cfg.work.gridColor = std::max(0, CbCur(d, IDC_GRIDCOLOR));
                InvalidateRect(GetDlgItem(d, IDC_GRIDSWATCH), nullptr, TRUE);
            }
            return TRUE;
        case IDC_BROWSE:  DoBrowse(d); return TRUE;
        case IDC_EXPLAIN: DoExplain(d); return TRUE;
        case IDC_SCHEME:
            if (code == CBN_SELCHANGE) {
                int sel = CbCur(d, IDC_SCHEME);
                if (sel >= 0) {
                    wchar_t name[256] = L"";
                    SendDlgItemMessageW(d, IDC_SCHEME, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)name);
                    Settings s = cfg.work;
                    if (LoadScheme(name, s)) { cfg.work = s; ShowGeneral(d); ShowCounter(d); }
                }
            }
            return TRUE;
        case IDC_SAVE: {
            HarvestAll(d);
            std::wstring name = Trim(DlgText(d, IDC_SCHEME));
            if (name.empty() || name.find(L'\\') != std::wstring::npos) {
                MessageBoxW(d, L"Type a scheme name (without backslashes) in the Schemes box, then click Save.",
                            L"QuickMon Forever", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            cfg.work.scheme = name;
            SaveSettingsTo(HKEY_CURRENT_USER, SchemeKey(name), cfg.work);
            FillSchemes(d, name);
            return TRUE;
        }
        case IDC_DELETE: {
            std::wstring name = Trim(DlgText(d, IDC_SCHEME));
            // The same rule as Save: a backslash would reach a different registry key.
            if (name.empty() || name.find(L'\\') != std::wstring::npos) return TRUE;
            std::wstring q = L"Delete the scheme \"" + name + L"\"?";
            if (MessageBoxW(d, q.c_str(), L"QuickMon Forever", MB_YESNO | MB_ICONQUESTION) != IDYES) return TRUE;
            RegDeleteTreeW(HKEY_CURRENT_USER, SchemeKey(name).c_str());
            if (cfg.work.scheme == name) cfg.work.scheme.clear();
            FillSchemes(d, L"");
            return TRUE;
        }
        case IDC_SETDEFAULT:   DoSetDefault(d, true); return TRUE;
        case IDC_CLEARDEFAULT: DoSetDefault(d, false); return TRUE;
        case IDC_ABOUT:
            MessageBoxW(d, ABOUT_TEXT, L"About QuickMon Forever", MB_OK | MB_ICONINFORMATION);
            return TRUE;
        case IDC_PREVIEW: {
            HarvestAll(d);
            SaveSettings(cfg.work);
            cfg.previewed = true;
            std::wstring exe = ModulePath();
            std::wstring cmd = L"\"" + exe + L"\" /s";
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            }
            return TRUE;
        }
        case IDOK:
            HarvestAll(d);
            if (!SaveSettings(cfg.work))
                MessageBoxW(d, L"Could not save the settings.", L"QuickMon Forever", MB_OK | MB_ICONWARNING);
            EndDialog(d, IDOK);
            return TRUE;
        case IDCANCEL:
            if (cfg.previewed) SaveSettings(cfg.orig);
            EndDialog(d, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

// Swatch painting lives in a small subclass-free handler: the dialog proc above
// forwards WM_CTLCOLORSTATIC here.
static INT_PTR CALLBACK ConfigProcOuter(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP_PICKCOLOR) { g_pickPending = false; PickCustomColor(d); return TRUE; }
    // A click on the color swatch opens the color dialog too. The swatch is a plain static, so
    // its clicks reach the dialog (and its tooltip keeps working).
    if (msg == WM_LBUTTONDOWN && IsWindowEnabled(GetDlgItem(d, IDC_COLOR))) {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT r;
        GetWindowRect(GetDlgItem(d, IDC_SWATCH), &r);
        MapWindowPoints(nullptr, d, (POINT*)&r, 2);
        if (PtInRect(&r, pt)) { PickCustomColor(d); return TRUE; }
    }
    if (msg == WM_CTLCOLORSTATIC && GetDlgCtrlID((HWND)lp) == IDC_SWATCH) {
        if (cfg.swatch) DeleteObject(cfg.swatch);
        cfg.swatch = CreateSolidBrush(LineRgb(cfg.work.c[cfg.cur]));
        return (INT_PTR)cfg.swatch;
    }
    if (msg == WM_CTLCOLORSTATIC && GetDlgCtrlID((HWND)lp) == IDC_GRIDSWATCH) {
        // "None" shows the chart's black background.
        COLORREF c = kGridColors[std::clamp(cfg.work.gridColor, 0, NGRIDCOLORS - 1)].rgb;
        if (cfg.gridSwatch) DeleteObject(cfg.gridSwatch);
        cfg.gridSwatch = CreateSolidBrush(c == CLR_NONE ? RGB(0, 0, 0) : c);
        return (INT_PTR)cfg.gridSwatch;
    }
    return ConfigProc(d, msg, wp, lp);
}

static int RunConfig(HWND parent) {
    LoadSettings(cfg.orig);
    cfg.work = cfg.orig;
    INT_PTR r = DialogBoxParamW(g.hi, MAKEINTRESOURCEW(IDD_CONFIG), parent, ConfigProcOuter, 0);
    if (cfg.swatch) DeleteObject(cfg.swatch);
    if (cfg.gridSwatch) DeleteObject(cfg.gridSwatch);
    return r == IDOK ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

// Splits "<num> <rest>" off the front of s.
static unsigned long long TakeNumber(std::wstring& s) {
    s = Trim(s);
    wchar_t* end = nullptr;
    unsigned long long v = wcstoull(s.c_str(), &end, 10);
    s = Trim(std::wstring(end ? end : L""));
    return v;
}

// /render <file.bmp> <w> <h> <samples>: test aid. Samples once a second, then writes the chart.
static int RunRenderTest(const std::wstring& file, int w, int h, int samples) {
    // Up to 16384 pixels each way, so the image size can't overflow.
    if (file.empty() || w < 16 || h < 16 || w > 16384 || h > 16384 || samples < 0) return 2;
    LoadRunSettings(g.mon.s);
    g.mon.Open();
    for (int i = 0; i < samples; i++) { Sleep(1000); g.mon.Sample(); }
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {                       // not enough memory for an image that size
        if (bmp) DeleteObject(bmp);
        DeleteDC(dc); ReleaseDC(nullptr, screen);
        g.mon.Close();
        return 1;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    Render(dc, w, h, g.mon, false);
    GdiFlush();
    BITMAPFILEHEADER fh = {};
    DWORD imgSize = (DWORD)w * h * 4;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + imgSize;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    int rc = 1;
    if (f != INVALID_HANDLE_VALUE) {
        DWORD n1 = 0, n2 = 0, n3 = 0;
        bool ok = WriteFile(f, &fh, sizeof(fh), &n1, nullptr) && n1 == sizeof(fh) &&
                  WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &n2, nullptr) && n2 == sizeof(bi.bmiHeader) &&
                  WriteFile(f, bits, imgSize, &n3, nullptr) && n3 == imgSize;
        CloseHandle(f);
        rc = ok ? 0 : 1;                       // 1: the file could not be written in full
    }
    SelectObject(dc, old);
    DeleteObject(bmp); DeleteDC(dc); ReleaseDC(nullptr, screen);
    g.mon.Close();
    return rc;
}

// ---------------------------------------------------------------------------
// Command line: /? /help, /theme, /scheme, /counter:N, /linecolor, /scale, /default, /save,
// /render. Without /save these are a one-off run; with /save they are saved and nothing runs.
// ---------------------------------------------------------------------------

static const wchar_t HELP_TEXT[] =
    L"QuickMon Forever 2.10 command line\n"
    L"\n"
    L"SCREEN SAVER (used by Windows)\n"
    L"/s\n      Run full screen.\n"
    L"/c\n      Settings dialog (also with no options).\n"
    L"/p <hwnd>\n      Preview in Screen Saver Settings.\n"
    L"\n"
    L"THEME\n"
    L"/theme modern  or  /theme classic\n      Run once with that theme.\n"
    L"/theme modern /save  or  /theme classic /save\n      Save it as the screen saver's theme.\n"
    L"\n"
    L"SCHEME\n"
    L"/scheme <name>\n      Run once with that scheme.\n"
    L"/scheme <name> /default /save\n      Make it the screen saver's scheme.\n"
    L"/scheme <name> /counter:<1-8> <path> /linecolor <color> /scale <scale> /save\n"
    L"      Change the scheme's counters and save the scheme. The path, /linecolor and /scale "
    L"are each optional; repeat /counter:<1-8> for more counters. Add /default to also make it "
    L"the screen saver's scheme. Without /save it runs once with the changes. Without /scheme, "
    L"/counter changes the screen saver's current settings.\n"
    L"\n"
    L"Colors: Red, Green, Blue, Yellow, Magenta, Cyan, White, Orange, Lime, Pink, Purple, Sky, "
    L"Gold, Teal, Gray, Brown, or any color as hex: #RRGGBB (such as #FF8800).\n"
    L"Scales: Auto, Default, 10000000, 1000000 ... 1, 0.1 ... 0.0000001.\n"
    L"Put quotes around names and paths with spaces, for example:\n"
    L"/scheme \"Disk activity\" /counter:2 \"\\PhysicalDisk(_Total)\\Disk Bytes/sec\" /linecolor Blue /save\n"
    L"\n"
    L"TEST AID\n"
    L"/render <file.bmp> <width> <height> <samples>\n"
    L"      Sample once a second, then save the chart as a bitmap. Works with /theme and /scheme.\n"
    L"/showname <name>  and  /showip <address>\n"
    L"      For screenshots: show these instead of this PC's name and IP address (one-off runs, /render, "
    L"and the setup dialog with /c).\n"
    L"\n"
    L"/?  or  /help\n      This help.";

static int ShowHelp(const std::wstring& error = L"") {
    std::wstring text = error.empty() ? std::wstring(HELP_TEXT) : error + L"\n\n" + HELP_TEXT;
    MessageBoxW(nullptr, text.c_str(), L"QuickMon Forever", MB_OK | (error.empty() ? MB_ICONINFORMATION : MB_ICONWARNING));
    return error.empty() ? 0 : 1;
}

static std::wstring Lower(std::wstring s) { for (auto& ch : s) ch = (wchar_t)towlower(ch); return s; }

// The arguments after the program name, with quotes handled the Windows way.
static std::vector<std::wstring> CmdArgs() {
    std::vector<std::wstring> out;
    int n = 0;
    LPWSTR* a = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!a) return out;
    for (int i = 1; i < n; i++) out.push_back(a[i]);
    LocalFree(a);
    return out;
}

// "/name" or "-name", any case.
static bool IsOpt(const std::wstring& tok, const wchar_t* name) {
    return tok.size() >= 2 && (tok[0] == L'/' || tok[0] == L'-') && Lower(tok.substr(1)) == name;
}
static bool IsSwitch(const std::wstring& tok) { return tok.size() >= 2 && (tok[0] == L'/' || tok[0] == L'-'); }

// True when the arguments use any of the options handled by RunCommandLine.
static bool UsesCommandLineOptions(const std::vector<std::wstring>& a) {
    static const wchar_t* names[] = { L"?", L"help", L"theme", L"scheme", L"linecolor", L"scale",
                                      L"default", L"save", L"render", L"showname", L"showip" };
    for (const auto& t : a) {
        for (const wchar_t* nm : names) if (IsOpt(t, nm)) return true;
        if (IsSwitch(t) && Lower(t.substr(1, 7)) == L"counter") return true;
    }
    return false;
}

static int ParseScale(const std::wstring& v, int& mode, int& exp) {
    std::wstring l = Lower(v);
    if (l == L"auto")    { mode = SCALE_AUTO; return 1; }
    if (l == L"default") { mode = SCALE_DEFAULT; return 1; }
    for (int e = 7; e >= -7; e--) if (l == ScaleStr(e)) { mode = SCALE_MANUAL; exp = e; return 1; }
    wchar_t* end = nullptr;
    double d = wcstod(v.c_str(), &end);
    if (end && *end == L'\0' && d > 0) {
        double e = log10(d);
        int r = (int)lround(e);
        if (fabs(e - r) < 1e-9 && r >= -7 && r <= 7) { mode = SCALE_MANUAL; exp = r; return 1; }
    }
    return 0;
}

struct CounterEdit {
    int idx = -1;
    bool setPath = false; std::wstring path;
    int color = -1; COLORREF rgb = 0;    // color COLOR_CUSTOM uses rgb
    bool setScale = false; int scaleMode = SCALE_AUTO, exp = 0;
};

static int RunCommandLine(const std::vector<std::wstring>& a) {
    int theme = -1;
    bool haveScheme = false, makeDefault = false, save = false, render = false, config = false;
    std::wstring scheme, renderFile;
    int rw = 0, rh = 0, rs = 0;
    std::vector<CounterEdit> edits;

    for (size_t i = 0; i < a.size(); i++) {
        const std::wstring& t = a[i];
        auto next = [&](const wchar_t* what, std::wstring& out) -> bool {
            if (i + 1 >= a.size() || IsSwitch(a[i + 1])) return false;
            out = a[++i]; (void)what; return true;
        };
        std::wstring v;
        if (IsOpt(t, L"?") || IsOpt(t, L"help")) return ShowHelp();
        else if (IsOpt(t, L"s")) { /* a one-off run is the full-screen saver anyway */ }
        else if (IsOpt(t, L"c")) config = true;   // setup dialog, with /showname and /showip only
        else if (IsOpt(t, L"theme")) {
            if (!next(L"theme", v)) return ShowHelp(L"/theme needs modern or classic.");
            std::wstring l = Lower(v);
            if (l == L"modern") theme = THEME_MODERN;
            else if (l == L"classic") theme = THEME_CLASSIC;
            else return ShowHelp(L"Unknown theme \"" + v + L"\". Use modern or classic.");
        }
        else if (IsOpt(t, L"scheme")) {
            if (!next(L"scheme", scheme)) return ShowHelp(L"/scheme needs a scheme name.");
            haveScheme = true;
        }
        else if (IsSwitch(t) && Lower(t.substr(1, 7)) == L"counter") {
            std::wstring num = t.substr(8);
            if (!num.empty() && num[0] == L':') num.erase(0, 1);
            else if (num.empty() && !next(L"counter", num)) return ShowHelp(L"/counter needs a number from 1 to 8, as in /counter:1.");
            wchar_t* end = nullptr;
            long n = wcstol(num.c_str(), &end, 10);
            if (num.empty() || !end || *end || n < 1 || n > MAX_COUNTERS)
                return ShowHelp(L"\"" + t + L"\": the counter number must be 1 to 8, as in /counter:1.");
            CounterEdit e; e.idx = (int)n - 1;
            if (next(L"path", v)) { e.setPath = true; e.path = v; }
            edits.push_back(e);
        }
        else if (IsOpt(t, L"linecolor")) {
            if (edits.empty()) return ShowHelp(L"/linecolor must follow /counter:<1-8>.");
            if (!next(L"color", v)) return ShowHelp(L"/linecolor needs a color name.");
            int c = -1;
            for (int k = 0; k < NCOLORS; k++) if (Lower(v) == Lower(kColors[k].name)) c = k;
            COLORREF rgb = 0;
            if (c < 0 && ParseHexColor(v, rgb)) { c = COLOR_CUSTOM; edits.back().rgb = rgb; }
            if (c < 0) return ShowHelp(L"Unknown line color \"" + v + L"\". Use a color name or a hex value such as #FF8800.");
            edits.back().color = c;
        }
        else if (IsOpt(t, L"scale")) {
            if (edits.empty()) return ShowHelp(L"/scale must follow /counter:<1-8>.");
            if (!next(L"scale", v)) return ShowHelp(L"/scale needs a value, such as Auto or 100.");
            CounterEdit& e = edits.back();
            if (!ParseScale(v, e.scaleMode, e.exp)) return ShowHelp(L"Unknown scale \"" + v + L"\".");
            e.setScale = true;
        }
        else if (IsOpt(t, L"default") || Lower(t) == L"default") makeDefault = true;
        else if (IsOpt(t, L"save")) save = true;
        else if (IsOpt(t, L"showname")) {
            if (!next(L"name", g_showName)) return ShowHelp(L"/showname needs a name.");
        }
        else if (IsOpt(t, L"showip")) {
            if (!next(L"ip", g_showIp)) return ShowHelp(L"/showip needs an address.");
        }
        else if (IsOpt(t, L"render")) {
            std::wstring sw, sh, ss;
            if (!next(L"file", renderFile) || !next(L"w", sw) || !next(L"h", sh) || !next(L"n", ss))
                return ShowHelp(L"/render needs <file.bmp> <width> <height> <samples>.");
            rw = _wtoi(sw.c_str()); rh = _wtoi(sh.c_str()); rs = _wtoi(ss.c_str());
            render = true;
        }
        else return ShowHelp(L"Unknown option \"" + t + L"\".");
    }

    if (makeDefault && !haveScheme) return ShowHelp(L"/default needs /scheme <name>.");
    if (makeDefault && !save) return ShowHelp(L"/default needs /save, as in /scheme <name> /default /save.");
    if (save && render) return ShowHelp(L"/save and /render can't be used together.");
    if (save && (!g_showName.empty() || !g_showIp.empty()))
        return ShowHelp(L"/showname and /showip are for one-off runs and /render, not /save.");
    if (save && !haveScheme && theme < 0 && edits.empty()) return ShowHelp(L"/save needs /theme, /scheme or /counter.");
    if (config) {
        if (save || render || haveScheme || theme >= 0 || !edits.empty())
            return ShowHelp(L"/c works only with /showname and /showip (for screenshots of the setup dialog).");
        g.mode = MODE_CONFIG;
        return RunConfig(nullptr);
    }

    // Start from the saved settings; a scheme is loaded over them, as the settings dialog does.
    Settings s;
    LoadSettings(s);
    if (haveScheme) {
        std::wstring found;
        std::wstring list;
        for (const auto& n : EnumSchemes()) {
            if (Lower(n) == Lower(scheme)) found = n;
            list += (list.empty() ? L"" : L", ") + n;
        }
        if (found.empty())
            return ShowHelp(L"There is no scheme named \"" + scheme + L"\".\nSaved schemes: " +
                            (list.empty() ? std::wstring(L"(none)") : list));
        if (!LoadScheme(found, s)) return ShowHelp(L"Could not read the scheme \"" + found + L"\".");
        scheme = found;
    }
    if (theme >= 0) s.theme = theme;
    for (const auto& e : edits) {
        CounterCfg& c = s.c[e.idx];
        if (e.setPath) { c.path = e.path; c.enabled = !e.path.empty(); }
        if (e.color >= 0) { c.color = e.color; if (e.color == COLOR_CUSTOM) c.customRgb = e.rgb; }
        if (e.setScale) { c.scaleMode = e.scaleMode; if (e.scaleMode == SCALE_MANUAL) c.manualExp = e.exp; }
    }

    if (save) {
        bool ok = true;
        if (haveScheme) {
            ok = SaveSettingsTo(HKEY_CURRENT_USER, SchemeKey(scheme), s);   // the scheme itself
            if (ok && makeDefault) ok = SaveSettings(s);                    // and the screen saver's settings
        } else {
            ok = SaveSettings(s);
        }
        if (!ok) { MessageBoxW(nullptr, L"Could not save the settings.", L"QuickMon Forever", MB_OK | MB_ICONWARNING); return 1; }
        return 0;
    }

    g_override = s;
    g_useOverride = true;
    if (render) return RunRenderTest(renderFile, rw, rh, rs);
    g.mode = MODE_SAVER;
    return RunSaver();
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR cmdLine, int) {
    // Load system DLLs only from System32, never from the program's own folder, so a file with a
    // system DLL's name placed next to a downloaded .scr can't be loaded instead. (The DLLs the
    // program links to are covered by /DEPENDENTLOADFLAG in build.cmd; this covers later loads.)
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    g.hi = hi;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    std::wstring args = Trim(cmdLine ? cmdLine : L"");

    // First run after the rename from Quick Monitor 11: copy its settings and schemes once.
    // The old key is left as it was.
    HKEY kNew = nullptr, kOld = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_ROOT, 0, KEY_READ, &kNew) == ERROR_SUCCESS) RegCloseKey(kNew);
    else if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_OLD_ROOT, 0, KEY_READ, &kOld) == ERROR_SUCCESS) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_ROOT, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &kNew, nullptr) == ERROR_SUCCESS) {
            RegCopyTreeW(kOld, nullptr, kNew);
            RegCloseKey(kNew);
        }
        RegCloseKey(kOld);
    }

    // Helper-only switches (32-bit build, launched by the 64-bit saver).
    if (_wcsnicmp(args.c_str(), L"/helper32", 9) == 0) return RunHelperServer();
    if (_wcsnicmp(args.c_str(), L"/browse32", 9) == 0) {
        std::wstring rest = args.substr(9);
        HWND owner = (HWND)(ULONG_PTR)TakeNumber(rest);
        DWORD detail = (DWORD)TakeNumber(rest);
        bool haveList = TakeNumber(rest) != 0;   // 1: the 64-bit side's objects follow on stdin
        if (haveList) {
            HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
            if (in && in != INVALID_HANDLE_VALUE) {
                std::string data;
                char b[4096]; DWORD got = 0;
                while (data.size() < 1024 * 1024 && ReadFile(in, b, sizeof(b), &got, nullptr) && got) data.append(b, got);
                std::wstring all = FromUtf8(data);
                for (size_t p = 0; p < all.size(); ) {
                    size_t e = all.find(L'\n', p);
                    if (e == std::wstring::npos) e = all.size();
                    if (e > p) g_objs64.push_back(all.substr(p, e - p));
                    p = e + 1;
                }
            }
        }
        std::wstring path = rest;
        if (!BrowseCounterNative(IsWindow(owner) ? owner : nullptr, detail, path)) return 1;
        std::string out = ToUtf8(path);
        DWORD n;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out.data(), (DWORD)out.size(), &n, nullptr);
        return 0;
    }
    if (_wcsnicmp(args.c_str(), L"/explain32", 10) == 0) {
        std::wstring rest = args.substr(10);
        HWND owner = (HWND)(ULONG_PTR)TakeNumber(rest);
        bool only32 = TakeNumber(rest) != 0;       // the 64-bit side cannot see this counter
        std::wstring text = ExplainCounterNative(rest);
        MessageBoxW(IsWindow(owner) ? owner : nullptr, text.c_str(), (CounterTitle(rest) + (only32 ? TAG_32ONLY : TAG_32)).c_str(),
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    // Exit code 0 when the 32-bit side can add the counter.
    if (_wcsnicmp(args.c_str(), L"/check32", 8) == 0) return NativeSees(Trim(args.substr(8))) ? 0 : 1;

    // /? /help /theme /scheme /counter /render and friends.
    std::vector<std::wstring> argv = CmdArgs();
    // The elevated copy Set Default / Clear Default starts through the UAC prompt.
    if (!argv.empty() && (_wcsicmp(argv[0].c_str(), L"/setdefault") == 0 || _wcsicmp(argv[0].c_str(), L"/cleardefault") == 0))
        return RunSignInScreenSaverCommand(argv);
    if (UsesCommandLineOptions(argv)) return RunCommandLine(argv);

    // Standard screen saver switches: /s, /p <hwnd>, /c[:hwnd]. Accept '-' or '/', ':' or ' '.
    // Only the single letter counts, so /settings or /start is not taken for /s.
    wchar_t sw = L'c';
    std::wstring rest;
    if (!args.empty() && (args[0] == L'/' || args[0] == L'-') && args.size() >= 2 &&
        (args.size() == 2 || args[2] == L':' || args[2] == L' ')) {
        sw = (wchar_t)towlower(args[1]);
        rest = args.substr(2);
        if (!rest.empty() && rest[0] == L':') rest.erase(0, 1);
    } else if (!args.empty()) {
        return ShowHelp(L"Unknown option \"" + (argv.empty() ? args : argv[0]) + L"\".");
    }
    switch (sw) {
    case L's':
        g.mode = MODE_SAVER;
        return RunSaver();
    case L'p': case L'l':
        g.mode = MODE_PREVIEW;
        return RunPreview((HWND)(ULONG_PTR)TakeNumber(rest));
    case L'c': {
        g.mode = MODE_CONFIG;
        // Own the dialog only when a window handle is passed (Screen Saver Settings passes
        // /c:<hwnd>). Never borrow the foreground window: a modal dialog disables its owner,
        // and if this process were ended abnormally that unrelated window would stay disabled.
        unsigned long long h = TakeNumber(rest);
        HWND parent = (h && IsWindow((HWND)(ULONG_PTR)h)) ? (HWND)(ULONG_PTR)h : nullptr;
        return RunConfig(parent);
    }
    default:
        return ShowHelp(L"Unknown option \"" + (argv.empty() ? args : argv[0]) + L"\".");
    }
}
