// breakdown-recomp patch: the installer of a portable release, one window with three tabs.
//   Game: the player's disc image or extracted game folder (chosen, dropped, or found beside the exe), checked against
//         the game profile on the spot, and the optional extras.
//   Prepare: on Play, the game is copied into resources\game, its code is built for this PC (game_code.cpp) and the
//         extras are applied.
//   Play: what was set up and the controls; Play starts the game.
// It also opens when only the game code is out of date (an update): the Game tab then shows the imported copy. The
// player's own files are only read. Nothing is searched beyond the folder of the executable and the folder above it.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <xinput.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/game_import.h"
#include "core/input.h"
#include "core/log.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "msimg32.lib")

namespace mechassault {
int CheckCopyCommand(const std::wstring& path, bool open);  // mechassault-recomp patch: src/common/check_copy.cpp
}

namespace app {

namespace {

namespace fs = std::filesystem;

// mechassault-recomp patch: MechAssault's look ("red glow", chosen by j from the mockups): the logo's dark red on
// near black with chrome-grey text, a white title, and a soft red glow under the active tab and around Play
constexpr COLORREF kPage = RGB(9, 9, 10);
constexpr COLORREF kFooter = RGB(9, 9, 11);
constexpr COLORREF kCard = RGB(20, 20, 23);
constexpr COLORREF kField = RGB(9, 9, 11);
constexpr COLORREF kLine = RGB(44, 45, 50);
constexpr COLORREF kLineHi = RGB(90, 26, 31);
constexpr COLORREF kFieldLine = RGB(52, 53, 59);
constexpr COLORREF kBlood = RGB(142, 24, 32);
constexpr COLORREF kBloodHi = RGB(200, 50, 58);
constexpr COLORREF kGlow = RGB(224, 70, 78);
constexpr COLORREF kBone = RGB(228, 228, 231);
constexpr COLORREF kWhite = RGB(255, 255, 255);
constexpr COLORREF kAsh = RGB(154, 155, 161);
constexpr COLORREF kSteel = RGB(216, 216, 220);
constexpr COLORREF kGoodText = RGB(216, 216, 220);
constexpr COLORREF kBad = RGB(240, 130, 110);
constexpr COLORREF kOff = RGB(85, 86, 92);
constexpr COLORREF kButton = RGB(28, 29, 33);
constexpr COLORREF kButtonHover = RGB(42, 43, 48);
constexpr COLORREF kButtonLine = RGB(69, 70, 77);
constexpr COLORREF kBoxOff = RGB(85, 86, 92);

constexpr UINT kInspected = WM_APP + 10, kImported = WM_APP + 11, kBuilt = WM_APP + 12;
constexpr int kWidth = 660, kHeight = 700;  // client, at 96 DPI

enum class Tab { Game, Prepare, Play };
enum class Step { Idle, Checking, Importing, Building, Ready };
enum class Source { None, Checking, Ok, Bad, Imported };

// clickable things, found again by the paint that drew them
enum Hit {
    kNone = -1,
    kTabGame,
    kTabPrepare,
    kTabPlay,
    kBrowseImage,
    kBrowseFolder,
    kClose,
    kPlay,
    kExtraFullscreen,
    kExtraShortcut,
    kExtraDelete,
    kLanguage,   // mechassault-recomp patch: GameInfo::languages, a click steps to the next
    kBrowseSaves,  // mechassault-recomp patch: the save location's Browse and Default
    kDefaultSaves,
    kExtraGame,  // GameInfo::extras from here
};

struct Fonts {
    HFONT title = nullptr, tab = nullptr, play = nullptr, heading = nullptr, body = nullptr, bold = nullptr,
          caption = nullptr, mono = nullptr;
    void Free() {
        for (HFONT f : {title, tab, play, heading, body, bold, caption, mono})
            if (f) DeleteObject(f);
        *this = {};
    }
};

struct State {
    const gameimport::Profile* profile = nullptr;
    GameFiles* game = nullptr;
    bool have_game = false;
    HWND hwnd = nullptr;
    UINT dpi = 96;
    Fonts fonts;
    HICON icon = nullptr;

    Tab tab = Tab::Game;
    Step step = Step::Idle;
    Source source = Source::None;
    bool imported = false;  // resources\game holds an accepted copy
    bool built = false;
    fs::path chosen;
    gameimport::Check check;
    std::wstring message, fix;  // the last problem
    gameimport::Progress progress;
    std::chrono::steady_clock::time_point started;
    std::thread worker;
    bool result = false;

    // the extras
    bool fullscreen = true, shortcut = true, delete_compiler = false;
    std::vector<bool> game_extras;
    size_t language = 0;  // mechassault-recomp patch: index in GameInfo::languages
    fs::path saves;       // mechassault-recomp patch: the save location (SavesDir), written when Play is pressed
    uint64_t compiler_bytes = 0;
    bool shortcut_made = false;

    int hover = kNone;
    std::vector<std::pair<int, RECT>> hits;  // this frame's
};
State* g = nullptr;

int S(int v) { return MulDiv(v, int(g->dpi), 96); }
int Pen1() { return std::max(1, S(2)); }

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring Megabytes(uint64_t b) { return std::to_wstring((b + (1 << 19)) >> 20) + L" MB"; }

std::wstring Upper(std::wstring s) {
    for (wchar_t& c : s) c = wchar_t(towupper(c));
    return s;
}

HFONT MakeFont(const wchar_t* face, int px, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -S(px);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    return CreateFontIndirectW(&lf);
}

// Bahnschrift (Windows 10 and later) for the display type, after the game's lettering; Segoe UI for the text
void MakeFonts() {
    g->fonts.Free();
    g->fonts.title = MakeFont(L"Bahnschrift SemiBold Condensed", 38, FW_SEMIBOLD);
    g->fonts.tab = MakeFont(L"Bahnschrift SemiBold Condensed", 16, FW_SEMIBOLD);
    g->fonts.play = MakeFont(L"Bahnschrift Bold Condensed", 24, FW_BOLD);
    g->fonts.heading = MakeFont(L"Segoe UI", 21, FW_NORMAL);
    g->fonts.body = MakeFont(L"Segoe UI", 14, FW_NORMAL);
    g->fonts.bold = MakeFont(L"Segoe UI Semibold", 14, FW_SEMIBOLD);
    g->fonts.caption = MakeFont(L"Segoe UI", 12, FW_NORMAL);
    g->fonts.mono = MakeFont(L"Consolas", 12, FW_NORMAL);
}

void Fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void Frame(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &r, b);
    DeleteObject(b);
}

void Box(HDC dc, const RECT& r, COLORREF fill, COLORREF line) {
    Fill(dc, r, fill);
    Frame(dc, r, line);
}

// mechassault-recomp patch: a and b mixed, f of b
COLORREF Mix(COLORREF a, COLORREF b, float f) {
    auto ch = [&](int x, int y) { return BYTE(std::lround(x + (y - x) * f)); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}
// a glow rising from the bottom of r over the ground colour: strongest at the bottom edge, gone at the top
void GlowUp(HDC dc, const RECT& r, COLORREF ground, COLORREF c, float strength) {
    int h = r.bottom - r.top;
    for (int i = 0; i < h; i++) {
        float f = float(i + 1) / float(h);
        Fill(dc, {r.left, r.top + i, r.right, r.top + i + 1}, Mix(ground, c, strength * f * f));
    }
}
// a halo around r over the ground colour, fading out over radius pixels
void Halo(HDC dc, const RECT& r, COLORREF ground, COLORREF c, int radius, float strength) {
    for (int d = radius; d >= 1; d--) {
        float f = 1.0f - float(d) / float(radius + 1);
        RECT o{r.left - d, r.top - d, r.right + d, r.bottom + d};
        Frame(dc, o, Mix(ground, c, strength * f * f));
    }
}

// a line of text, letter spaced (tenths of a pixel) for the display type; returns its width
int Text(HDC dc, RECT r, const std::wstring& t, HFONT font, COLORREF c, UINT flags = DT_LEFT, int spacing = 0) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, c);
    SetTextCharacterExtra(dc, S(spacing) / 10);
    RECT m = r;
    DrawTextW(dc, t.c_str(), int(t.size()), &m, flags | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_CALCRECT);
    DrawTextW(dc, t.c_str(), int(t.size()), &r, flags | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SetTextCharacterExtra(dc, 0);
    SelectObject(dc, old);
    return m.right - m.left;
}

// wrapped text from the top of r; returns the height used
int Paragraph(HDC dc, RECT r, const std::wstring& t, HFONT font, COLORREF c) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, c);
    RECT m = r;
    DrawTextW(dc, t.c_str(), int(t.size()), &m, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    DrawTextW(dc, t.c_str(), int(t.size()), &r, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return m.bottom - m.top;
}

// a group's label: small spaced capitals in red
void Label(HDC dc, int x, int& y, int right, const std::wstring& t) {
    Text(dc, {x, y, right, y + S(16)}, Upper(t), g->fonts.mono, kBloodHi, DT_LEFT, 12);
    y += S(24);
}

void AddHit(int id, const RECT& r) { g->hits.push_back({id, r}); }

void Tick(HDC dc, int x, int y, int size, COLORREF c, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, c);
    HGDIOBJ old = SelectObject(dc, pen);
    POINT p[3] = {{x, y + size / 2}, {x + size * 2 / 5, y + size * 9 / 10}, {x + size, y + size / 10}};
    Polyline(dc, p, 3);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void Button(HDC dc, int id, RECT r, const std::wstring& label, bool enabled = true) {
    Box(dc, r, enabled && g->hover == id ? kButtonHover : kButton, kButtonLine);
    Text(dc, r, label, g->fonts.body, enabled ? kBone : kOff, DT_CENTER);
    if (enabled) AddHit(id, r);
}

// a checkbox with its label and a line under it; returns the height used
int Check(HDC dc, int id, int x, int y, int right, bool on, const std::wstring& label, const std::wstring& detail,
          bool enabled) {
    int box = S(18);
    RECT b{x, y + S(2), x + box, y + S(2) + box};
    if (on) {
        Box(dc, b, enabled ? kBlood : kOff, enabled ? kBloodHi : kOff);
        Tick(dc, b.left + S(4), b.top + S(5), box - S(8), kBone, Pen1());
    } else {
        Box(dc, b, kField, g->hover == id && enabled ? kBloodHi : kBoxOff);
    }
    int tx = x + box + S(12);
    Text(dc, {tx, y, right, y + S(22)}, label, g->fonts.body, enabled ? kBone : kAsh);
    int h = S(22);
    if (!detail.empty()) h += Paragraph(dc, {tx, y + S(21), right, y + S(60)}, detail, g->fonts.caption, kAsh);
    if (enabled) AddHit(id, {x, y, right, y + h});
    return h;
}

// mechassault-recomp patch: a choice row lined up with the extras: the label, a line under it, and the value in a box
// at the right that steps to the next one when clicked
int Choice(HDC dc, int id, int x, int y, int right, const std::wstring& label, const std::wstring& value,
           const std::wstring& detail, bool enabled) {
    int tx = x + S(18) + S(12);
    RECT v{right - S(150), y - S(2), right, y + S(30)};
    Box(dc, v, kField, g->hover == id && enabled ? kBloodHi : kButtonLine);
    Text(dc, v, L"‹  " + value + L"  ›", g->fonts.bold, enabled ? kWhite : kAsh, DT_CENTER);
    Text(dc, {tx, y, v.left - S(12), y + S(22)}, label, g->fonts.body, enabled ? kBone : kAsh);
    int h = S(22);
    if (!detail.empty()) h += Paragraph(dc, {tx, y + S(21), v.left - S(12), y + S(60)}, detail, g->fonts.caption, kAsh);
    if (enabled) AddHit(id, {x, y, right, y + h});
    return h;
}

// a line of the progress and summary lists: 0 waiting, 1 now, 2 done
void StepLine(HDC dc, int x, int& y, int right, int state, const std::wstring& t) {
    if (state == 2) Tick(dc, x, y + S(6), S(10), kBloodHi, Pen1());
    else Text(dc, {x, y, x + S(14), y + S(22)}, state == 1 ? L"›" : L"·", g->fonts.bold,
              state == 1 ? kBone : kOff);
    Text(dc, {x + S(22), y, right, y + S(22)}, t, g->fonts.body, state == 0 ? kOff : state == 1 ? kBone : kSteel);
    y += S(28);
}

void Bar(HDC dc, int x, int& y, int right, double frac, const std::wstring& left, const std::wstring& right_text) {
    RECT bar{x, y, right, y + S(10)};
    Box(dc, bar, kButton, kFieldLine);
    RECT full{x + 1, y + 1, x + 1 + LONG(double(right - x - 2) * std::clamp(frac, 0.0, 1.0)), y + S(10) - 1};
    if (full.right > full.left) {
        TRIVERTEX v[2] = {{full.left, full.top, 0x4a00, 0x0600, 0x0900, 0xff00},
                          {full.right, full.bottom, 0xd400, 0x2a00, 0x2f00, 0xff00}};
        GRADIENT_RECT gr{0, 1};
        GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);
    }
    y += S(18);
    Text(dc, {x, y, right, y + S(18)}, left, g->fonts.mono, kAsh);
    Text(dc, {x, y, right, y + S(18)}, right_text, g->fonts.mono, kAsh, DT_RIGHT);
    y += S(22);
}

bool Busy() { return g->step == Step::Checking || g->step == Step::Importing || g->step == Step::Building; }

bool CanPlay() {
    if (Busy()) return false;
    if (g->tab == Tab::Play) return g->step == Step::Ready;
    if (g->tab == Tab::Game)
        return g->step == Step::Ready || g->source == Source::Ok || g->source == Source::Imported;
    return false;
}

// ---- the tabs ----

void PaintGame(HDC dc, int x, int y, int right) {
    std::wstring name = Wide(g->profile->name);
    Text(dc, {x, y, right, y + S(30)}, L"Set up your game", g->fonts.heading, kBone);
    y += S(42);
    Label(dc, x, y, right, L"Game files");

    int top = y, in = x + S(16), inr = right - S(16);
    int h = S(150);
    Box(dc, {x, top, right, top + h}, kCard, kLineHi);
    int cy = top + S(14);
    bool imported = g->source == Source::Imported;
    bool can = !Busy() && g->step != Step::Ready;
    Text(dc, {in, cy, inr, cy + S(20)},
         imported ? L"Your game is imported. Press Play to finish preparing it for this PC."
                  : L"Select your " + name + L" disc image or extracted game folder",
         g->fonts.body, kBone);
    cy += S(30);
    // the path field and its buttons
    int bw = S(104), gap = S(8);
    RECT field{in, cy, imported ? inr : inr - 2 * bw - 2 * gap, cy + S(36)};
    Box(dc, field, kField, kFieldLine);
    std::wstring path = imported           ? Paths().game.wstring()
                        : g->chosen.empty() ? std::wstring(L"Drop it on this window, or browse")
                                            : g->chosen.wstring();
    RECT pr = field;
    pr.left += S(10);
    pr.right -= S(10);
    Text(dc, pr, path, g->fonts.body, g->chosen.empty() && !imported ? kAsh : kBone, DT_LEFT | DT_PATH_ELLIPSIS);
    if (!imported) {
        Button(dc, kBrowseImage, {inr - 2 * bw - gap, cy, inr - bw - gap, cy + S(36)}, L"Disc image…", can);
        Button(dc, kBrowseFolder, {inr - bw, cy, inr, cy + S(36)}, L"Folder…", can);
    }
    cy += S(46);
    // what the check found
    switch (g->source) {
        case Source::None:
            break;
        case Source::Checking:
            Text(dc, {in, cy, inr, cy + S(20)}, L"Checking " + g->chosen.filename().wstring() + L"…",
                 g->fonts.body, kAsh);
            break;
        case Source::Ok:
        case Source::Imported: {
            const gameimport::Check& c = g->check;
            Tick(dc, in, cy + S(5), S(10), kBloodHi, Pen1());
            std::wstring line = imported ? name + L" · imported into resources\\game"
                                         : Wide(c.title_name) + L" · " + std::to_wstring(c.files) +
                                               L" files · " + Megabytes(c.bytes) + L" · supported release";
            Text(dc, {in + S(20), cy, inr, cy + S(20)}, line, g->fonts.body, kGoodText);
            break;
        }
        case Source::Bad:
            Text(dc, {in, cy, inr, cy + S(20)}, g->message, g->fonts.body, kBad);
            break;
    }
    cy += S(24);
    std::wstring hint = g->source == Source::Bad && !g->fix.empty()
                            ? g->fix
                            : L"An .iso, a full disc dump, an xiso or the extracted folder. Your files are only read.";
    if (!imported) Paragraph(dc, {in, cy, inr, top + h - S(6)}, hint, g->fonts.caption, kAsh);
    y = top + h + S(18);

    Label(dc, x, y, right, L"Optional extras · applied when you press Play");
    top = y;
    int rows = 3 + int(kGame.extras.size()) + (kGame.languages.empty() ? 0 : 1);
    h = S(16) + rows * S(46) - S(4);
    Box(dc, {x, top, right, top + h}, kCard, kLineHi);
    cy = top + S(14);
    Check(dc, kExtraFullscreen, in, cy, inr, g->fullscreen, L"Start in fullscreen",
          L"Borderless on your main display. Switch any time in the settings (F1).", can);
    cy += S(46);
    Check(dc, kExtraShortcut, in, cy, inr, g->shortcut, L"Create a desktop shortcut",
          L"A " + name + L" icon on your desktop that starts this copy.", can);
    cy += S(46);
    for (size_t i = 0; i < kGame.extras.size(); i++) {
        Check(dc, int(kExtraGame + int(i)), in, cy, inr, g->game_extras[i], kGame.extras[i].label,
              kGame.extras[i].detail ? kGame.extras[i].detail : L"", can);
        cy += S(46);
    }
    if (!kGame.languages.empty()) {
        Choice(dc, kLanguage, in, cy, inr, L"Language", kGame.languages[g->language].name,
               L"The game's text and speech. Change it any time in the settings (F1).", can);
        cy += S(46);
    }
    // mechassault-recomp patch: the save location in place of deleting the compiler (j, 2026-11-09): the default
    // unless the player browses for another folder
    {
        int tx = in + S(18) + S(12), bw = S(96), gap = S(8);
        bool custom = g->saves != Paths().saves;
        int buttons = custom ? 2 * bw + gap : bw;
        Text(dc, {tx, cy, inr - buttons - S(12), cy + S(22)}, L"Game save location", g->fonts.body, can ? kBone : kAsh);
        std::wstring where = custom ? g->saves.wstring() : L"Default: resources\\saves beside " + name + L".exe";
        Text(dc, {tx, cy + S(21), inr - buttons - S(12), cy + S(40)}, where, g->fonts.caption, kAsh,
             DT_LEFT | DT_PATH_ELLIPSIS);
        Button(dc, kBrowseSaves, {inr - bw, cy - S(2), inr, cy + S(30)}, L"Browse…", can);
        if (custom) Button(dc, kDefaultSaves, {inr - 2 * bw - gap, cy - S(2), inr - bw - gap, cy + S(30)}, L"Default", can);
    }
}

void PaintPrepare(HDC dc, int x, int y, int right) {
    std::wstring name = Wide(g->profile->name);
    Text(dc, {x, y, right, y + S(30)}, L"Preparing " + name + L" for this PC", g->fonts.heading, kBone);
    y += S(42);
    Label(dc, x, y, right, L"This only happens once");
    int top = y, in = x + S(16), inr = right - S(16), h = S(232);
    Box(dc, {x, top, right, top + h}, kCard, kLineHi);
    int cy = top + S(16);
    bool copying = g->step == Step::Importing, building = g->step == Step::Building;
    StepLine(dc, in, cy, inr, 2, L"Checked the release");
    StepLine(dc, in, cy, inr, copying ? 1 : g->imported ? 2 : 0,
             g->imported ? L"Game copied into resources\\game" : L"Copying the game into resources\\game");
    GameCodeStatus code = GameCodeProgress();
    StepLine(dc, in, cy, inr, building ? 1 : g->built ? 2 : 0,
             building ? code.stage : g->built ? L"Game code built for this PC" : L"Building the game's code");
    StepLine(dc, in, cy, inr, g->built ? 2 : 0, L"Extras applied");
    cy += S(8);
    if (copying) {
        uint64_t done = g->progress.done, total = std::max<uint64_t>(g->progress.total, 1);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g->started).count();
        double rate = secs > 0.5 ? double(done) / secs : 0;
        std::wstring left = std::to_wstring(int(100.0 * double(done) / double(total))) + L"%  ·  " +
                            Megabytes(done) + L" of " + Megabytes(total);
        std::wstring rt;
        if (rate > 0 && done < total) rt = L"about " + std::to_wstring(int(double(total - done) / rate) + 1) + L" s left";
        Bar(dc, in, cy, inr, double(done) / double(total), left, rt);
    } else if (building || g->built) {
        std::wstring tl = code.time_left;
        for (auto& c : tl) c = wchar_t(towlower(c));
        Bar(dc, in, cy, inr, code.fraction, std::to_wstring(int(code.fraction * 100.0f)) + L"%", tl);
    }
    y = top + h + S(16);
    Paragraph(dc, {x, y, right, y + S(40)},
              L"You can close the window: what is done is kept, and the next start carries on.", g->fonts.caption, kAsh);
}

void PaintPlay(HDC dc, int x, int y, int right) {
    std::wstring name = Wide(g->profile->name);
    Text(dc, {x, y, right, y + S(30)}, name + L" is ready", g->fonts.heading, kBone);
    y += S(42);
    Label(dc, x, y, right, L"Your setup");
    int in = x + S(16), inr = right - S(16);
    std::vector<std::wstring> lines;
    lines.push_back(L"Game imported · " + (g->check.title_name.empty() ? name : Wide(g->check.title_name)));
    lines.push_back(L"Game code built for this PC");
    std::wstring extras = g->fullscreen ? L"Fullscreen" : L"Windowed";
    for (size_t i = 0; i < kGame.extras.size(); i++)
        if (g->game_extras[i]) extras += L" · " + std::wstring(kGame.extras[i].label);
    lines.push_back(extras);
    if (g->shortcut_made) lines.push_back(L"Desktop shortcut created");
    if (g->delete_compiler) lines.push_back(L"The compiler is deleted when you press Play");
    int top = y, h = S(16) + int(lines.size()) * S(28);
    Box(dc, {x, top, right, top + h}, kCard, kLineHi);
    int cy = top + S(12);
    for (auto& l : lines) StepLine(dc, in, cy, inr, 2, l);
    y = top + h + S(18);
    if (kGame.controls) {
        Label(dc, x, y, right, L"Controls");
        std::wstring c = kGame.controls;
        HGDIOBJ old = SelectObject(dc, g->fonts.body);
        RECT m{in, 0, inr, 0};
        DrawTextW(dc, c.c_str(), int(c.size()), &m, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        SelectObject(dc, old);
        int ch = m.bottom - m.top;
        top = y;
        h = S(14) + ch + S(10) + S(20) + S(12);
        Box(dc, {x, top, right, top + h}, kCard, kLineHi);
        Paragraph(dc, {in, top + S(14), inr, top + S(14) + ch}, c, g->fonts.body, kBone);
        Paragraph(dc, {in, top + S(14) + ch + S(10), inr, top + h},
                  L"Change any of these in the settings (F1). Later starts go straight into the game.", g->fonts.caption,
                  kAsh);
    }
}

void Paint(HDC dc, const RECT& client) {
    g->hits.clear();
    Fill(dc, client, kPage);
    SetBkMode(dc, TRANSPARENT);
    int x = S(28), right = client.right - S(28);

    // the header: a red rule, the icon, the name in white, what this is
    Fill(dc, {0, 0, client.right, S(3)}, kBlood);
    if (g->icon) DrawIconEx(dc, x, S(26), g->icon, S(42), S(42), 0, nullptr, DI_NORMAL);
    int tx = x + S(56);
    Text(dc, {tx, S(20), right, S(58)}, Upper(Wide(g->profile->name)), g->fonts.title, kWhite, DT_LEFT, 22);
    Text(dc, {tx, S(56), right, S(74)}, L"XBOX PC PORT  ·  SETUP", g->fonts.mono, kAsh, DT_LEFT, 14);

    // the tabs: Game any time it is not working, the others once there is something on them
    int ty = S(96), th = S(36);
    const wchar_t* names[] = {L"GAME", L"PREPARE", L"PLAY"};
    int w = (right - x) / 3;
    for (int i = 0; i < 3; i++) {
        RECT r{x + i * w, ty, x + (i + 1) * w, ty + th};
        bool on = int(g->tab) == i;
        bool reach = !Busy() && (i == 0 || g->step == Step::Ready);
        COLORREF c = on ? kWhite : reach && g->hover == kTabGame + i ? kSteel : reach ? kAsh : kOff;
        if (on) GlowUp(dc, r, kPage, kBloodHi, 0.30f);
        int tw = Text(dc, r, names[i], g->fonts.tab, c, DT_CENTER, 18);
        if (on) {
            int cx = (r.left + r.right) / 2;
            Fill(dc, {cx - tw / 2, r.bottom - S(3), cx + tw / 2, r.bottom}, kBloodHi);
        }
        if (reach && !on) AddHit(kTabGame + i, r);
    }
    Fill(dc, {x, ty + th, right, ty + th + 1}, kLine);

    int by = ty + th + S(22);
    switch (g->tab) {
        case Tab::Game:
            PaintGame(dc, x, by, right);
            break;
        case Tab::Prepare:
            PaintPrepare(dc, x, by, right);
            break;
        case Tab::Play:
            PaintPlay(dc, x, by, right);
            break;
    }

    // the footer: Close and the big Play
    int fy = client.bottom - S(76);
    Fill(dc, {0, fy, client.right, client.bottom}, kFooter);
    Fill(dc, {0, fy, client.right, fy + 1}, kLine);
    RECT play{right - S(176), fy + S(16), right, fy + S(60)};
    bool can = CanPlay();
    if (can) Halo(dc, play, kFooter, kBloodHi, S(12), g->hover == kPlay ? 0.75f : 0.55f);
    Fill(dc, play, can ? (g->hover == kPlay ? kBloodHi : kBlood) : kButton);
    Frame(dc, play, can ? kGlow : kFieldLine);
    Text(dc, play, L"PLAY", g->fonts.play, can ? kWhite : kOff, DT_CENTER, 30);
    if (can) AddHit(kPlay, play);
    RECT close{play.left - S(12) - S(104), play.top, play.left - S(12), play.bottom};
    Button(dc, kClose, close, Busy() && g->step != Step::Checking ? L"Stop" : L"Close", g->step != Step::Checking);
    if (kGame.platform_line)
        Text(dc, {x, play.top, close.left - S(12), play.bottom}, kGame.platform_line, g->fonts.caption, kAsh);
}

void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ ob = SelectObject(mem, bmp);
    Paint(mem, client);
    BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

void Redraw() { InvalidateRect(g->hwnd, nullptr, FALSE); }

// ---- what the buttons do ----

fs::path Pick(HWND hwnd, bool folder, const wchar_t* title = nullptr) {
    fs::path out;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folder) {
        COMDLG_FILTERSPEC types[] = {{L"Xbox disc images (*.iso, *.xiso)", L"*.iso;*.xiso"}, {L"All files", L"*.*"}};
        d->SetFileTypes(2, types);
    }
    d->SetTitle(title ? title : folder ? L"Choose the folder with default.xbe" : L"Choose the disc image");
    if (SUCCEEDED(d->Show(hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(d->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    d->Release();
    return out;
}

void Join() {
    if (g->worker.joinable()) g->worker.join();
}

// the chosen copy is checked on its own thread
// mechassault-recomp patch: XBE_SETUP_TEST=1 runs the setup unattended for tests: no desktop shortcut, windowed,
// and Play is pressed by itself once the game found beside the executable (or named by XBE_SETUP_SOURCE) checks out
// and again once the code is built
bool SetupTest() {
    static const bool on = std::getenv("XBE_SETUP_TEST") != nullptr;
    return on;
}

void Choose(const fs::path& source) {
    if (Busy() || source.empty() || g->source == Source::Imported || g->step == Step::Ready) return;
    Join();
    g->chosen = source;
    g->source = Source::Checking;
    g->step = Step::Checking;
    LOGR("Setup: checking %s", gameimport::Utf8(source).c_str());
    HWND hwnd = g->hwnd;
    g->worker = std::thread([hwnd, source] {
        g->check = gameimport::Inspect(source, *g->profile);
        PostMessageW(hwnd, kInspected, 0, 0);
    });
    Redraw();
}

void OnInspected() {
    Join();
    g->step = Step::Idle;
    const gameimport::Check& c = g->check;
    LOGR("Setup: %s, %s, title %08X '%s', sha1 %s, code %s, %u files, %llu bytes: %s", gameimport::Utf8(g->chosen).c_str(),
         gameimport::KindName(c.kind), c.title_id, c.title_name.c_str(), c.xbe_sha1.c_str(), c.xbe_code_sha1.c_str(), c.files,
         (unsigned long long)c.bytes, c.ok ? "ok" : c.problem.c_str());
    g->source = c.ok ? Source::Ok : Source::Bad;
    g->message = Wide(c.problem);
    g->fix = Wide(c.fix);
    // mechassault-recomp patch: MechAssault but not a release this build runs: a report on the copy for the player to
    // send (MechAssault-copy-check.txt on the Desktop, src/common/check_copy.cpp), in place of the old .bat
    if (!c.ok && g->profile->title_id && c.title_id == g->profile->title_id && !c.release_match &&
        mechassault::CheckCopyCommand(g->chosen.wstring(), false) == 0)
        g->fix += L" A report on your copy was saved on your Desktop as MechAssault-copy-check.txt: send it if you ask "
                  L"for help.";
    if (SetupTest() && c.ok) PostMessageW(g->hwnd, WM_KEYDOWN, VK_RETURN, 0);  // mechassault-recomp patch
}

// a shortcut to this exe on the desktop
bool MakeShortcut() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) return false;
    fs::path link = fs::path(desktop) / (std::wstring(kGame.title) + L".lnk");
    CoTaskMemFree(desktop);
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(exe);
    sl->SetWorkingDirectory(ExeDir().c_str());
    sl->SetIconLocation(exe, 0);
    sl->SetDescription(kGame.title);
    IPersistFile* pf = nullptr;
    bool ok = SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->Save(link.c_str(), TRUE));
    if (pf) pf->Release();
    sl->Release();
    LOGR("Setup: desktop shortcut %s: %s", gameimport::Utf8(link).c_str(), ok ? "made" : "failed");
    return ok;
}

// the ticked extras into the settings file, and the shortcut
void ApplyExtras() {
    Settings& s = Get();
    if (!g->fullscreen) s.display_mode = DisplayMode::Windowed;
    else if (s.display_mode == DisplayMode::Windowed) s.display_mode = DisplayMode::Borderless;
    for (size_t i = 0; i < kGame.extras.size(); i++) *kGame.extras[i].flag = g->game_extras[i];
    if (!kGame.languages.empty()) s.language = kGame.languages[g->language].xc;
    Save();
    SetSavesDir(g->saves);  // mechassault-recomp patch
    LOGR("Setup: saves in %s", gameimport::Utf8(SavesDir()).c_str());
    if (g->shortcut && !g->shortcut_made) g->shortcut_made = MakeShortcut();
    LOGR("Setup: extras: %s, shortcut %d, delete compiler %d", g->fullscreen ? "fullscreen" : "windowed",
         int(g->shortcut_made), int(g->delete_compiler));
}

void Fail(const std::wstring& what) {
    g->step = Step::Idle;
    g->tab = Tab::Game;
    g->message = what;
    g->fix.clear();
    if (g->source != Source::Imported) g->source = Source::Bad;
    Redraw();
    MessageBoxW(g->hwnd, what.c_str(), kGame.title, MB_ICONERROR);
}

void StartBuild() {
    if (!g->have_game) {
        if (!FindGame(*g->game)) {  // it has said why
            Fail(L"The imported game could not be read. Choose your copy again.");
            return;
        }
        g->have_game = true;
    }
    g->step = Step::Building;
    if (std::wstring blocker = StartGameCodeBuild(*g->game, g->hwnd, kBuilt); !blocker.empty()) Fail(blocker);
}

void StartImport() {
    g->step = Step::Importing;
    g->progress.cancel = false;
    g->progress.done = 0;
    g->progress.total = 0;
    g->started = std::chrono::steady_clock::now();
    HWND hwnd = g->hwnd;
    g->worker = std::thread([hwnd] {
        std::string err;
        bool ok = gameimport::Import(g->chosen, Paths().game, *g->profile, g->progress, err);
        if (!ok) g->message = Wide(err);
        PostMessageW(hwnd, kImported, ok, 0);
    });
}

void Play() {
    if (!CanPlay()) return;
    if (g->step == Step::Ready) {  // from either tab once it is all done
        ApplyExtras();
        if (g->delete_compiler) DeleteCompilerFolder();
        g->result = true;
        DestroyWindow(g->hwnd);
        return;
    }
    ApplyExtras();
    g->tab = Tab::Prepare;
    if (!g->imported) StartImport();
    else StartBuild();
    Redraw();
}

// Close, Escape or the window's close button; while working it asks, then stops (what is done is kept)
void Close() {
    if (g->step == Step::Checking) return;
    if (Busy()) {
        const wchar_t* q = L"Stop preparing the game? What is done so far is kept, and the next start carries on.";
        if (MessageBoxW(g->hwnd, q, kGame.title, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        if (g->step == Step::Importing) g->progress.cancel = true;
        if (g->step == Step::Building) CancelGameCodeBuild();
        return;  // the worker's message closes the window
    }
    DestroyWindow(g->hwnd);
}

void Toggle(int id) {
    if (Busy()) return;
    if (id == kExtraDelete) g->delete_compiler = !g->delete_compiler;
    if (g->step == Step::Ready) return;  // the rest were applied
    if (id == kExtraFullscreen) g->fullscreen = !g->fullscreen;
    else if (id == kExtraShortcut) g->shortcut = !g->shortcut;
    else if (id == kLanguage && !kGame.languages.empty()) g->language = (g->language + 1) % kGame.languages.size();
    else if (id >= kExtraGame && size_t(id - kExtraGame) < g->game_extras.size())
        g->game_extras[size_t(id - kExtraGame)] = !g->game_extras[size_t(id - kExtraGame)];
}

int HitTest(LPARAM lp) {
    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    for (auto& [id, r] : g->hits)
        if (PtInRect(&r, p)) return id;
    return kNone;
}

void OnClick(int h) {
    switch (h) {
        case kNone:
            return;
        case kTabGame:
        case kTabPrepare:
        case kTabPlay:
            g->tab = Tab(h - kTabGame);
            break;
        case kBrowseImage:
            Choose(Pick(g->hwnd, false));
            break;
        case kBrowseFolder:
            Choose(Pick(g->hwnd, true));
            break;
        case kBrowseSaves:  // mechassault-recomp patch
            if (!Busy() && g->step != Step::Ready)
                if (fs::path p = Pick(g->hwnd, true, L"Choose where to keep saved games and profiles"); !p.empty())
                    g->saves = p;
            break;
        case kDefaultSaves:
            if (!Busy() && g->step != Step::Ready) g->saves = Paths().saves;
            break;
        case kClose:
            Close();
            return;
        case kPlay:
            Play();
            return;
        default:
            Toggle(h);
            break;
    }
    Redraw();
}

// a controller's A plays and B closes; on the press, not the hold
void PollController() {
    static uint16_t held = 0xFFFF;  // nothing until every button has been let go once
    uint16_t now = input::HeldOnAnyController();
    uint16_t pressed = now & ~held;
    held = now;
    if (pressed & XINPUT_GAMEPAD_A) Play();
    else if (pressed & XINPUT_GAMEPAD_B) Close();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            PaintWindow(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (Busy()) Redraw();
            if (GetForegroundWindow() == hwnd) PollController();
            return 0;
        case WM_MOUSEMOVE: {
            int h = HitTest(lp);
            if (h != g->hover) {
                g->hover = h;
                Redraw();
            }
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSELEAVE:
            g->hover = kNone;
            Redraw();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, g->hover == kNone ? IDC_ARROW : IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP:
            OnClick(HitTest(lp));
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) Close();
            else if (wp == VK_RETURN) Play();
            return 0;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            wchar_t path[MAX_PATH * 4];
            if (g->tab == Tab::Game && DragQueryFileW(drop, 0, path, UINT(std::size(path)))) Choose(fs::path(path));
            DragFinish(drop);
            return 0;
        }
        case WM_DPICHANGED: {
            g->dpi = HIWORD(wp);
            MakeFonts();
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Redraw();
            return 0;
        }
        case kInspected:
            OnInspected();
            Redraw();
            return 0;
        case kImported:
            Join();
            if (wp) {
                LOGR("Setup: imported into %s", gameimport::Utf8(Paths().game).c_str());
                g->imported = true;
                StartBuild();
            } else {
                LOGR("Setup: import failed: %ls", g->message.c_str());
                if (g->progress.cancel) {
                    DestroyWindow(hwnd);
                    return 0;
                }
                Fail(g->message + L" Your files were not changed. Try again, or choose another copy.");
            }
            Redraw();
            return 0;
        case kBuilt:
            JoinGameCodeBuild();
            if (wp) {
                g->built = true;
                g->step = Step::Ready;
                g->tab = Tab::Play;
                FlashWindow(hwnd, TRUE);
                if (SetupTest()) PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0);  // mechassault-recomp patch
            } else if (GameCodeBuildCancelled()) {
                DestroyWindow(hwnd);
                return 0;
            } else {
                g->step = Step::Idle;
                g->tab = Tab::Game;
                Redraw();
                if (MessageBoxW(hwnd, GameCodeFailure().c_str(), kGame.title, MB_ICONERROR | MB_RETRYCANCEL) ==
                    IDRETRY) {
                    g->tab = Tab::Prepare;
                    StartBuild();
                }
            }
            Redraw();
            return 0;
        case WM_CLOSE:
            Close();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool RunSetup(const gameimport::Profile& profile, GameFiles& game, bool have_game) {
    State state;
    g = &state;
    state.profile = &profile;
    state.game = &game;
    state.have_game = have_game;
    state.imported = gameimport::Imported(Paths().game, profile);
    state.source = state.imported ? Source::Imported : Source::None;
    state.fullscreen = Get().display_mode != DisplayMode::Windowed && !SetupTest();
    if (SetupTest()) state.shortcut = false;  // mechassault-recomp patch
    for (const auto& e : kGame.extras) state.game_extras.push_back(*e.flag);
    for (size_t i = 0; i < kGame.languages.size(); i++)
        if (kGame.languages[i].xc == Get().language) state.language = i;
    state.compiler_bytes = CompilerFolderBytes();
    state.saves = SavesDir();  // mechassault-recomp patch
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    input::Init();

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    std::wstring cls = std::wstring(kGame.name) + L"Setup";
    wc.lpszClassName = cls.c_str();
    RegisterClassExW(&wc);
    state.dpi = GetDpiForSystem();
    state.icon = HICON(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S(42), S(42), LR_DEFAULTCOLOR));
    MakeFonts();
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    // mechassault-recomp patch: taller by a row for each extra past the four the layout was drawn for
    int rows = 3 + int(kGame.extras.size()) + (kGame.languages.empty() ? 0 : 1);
    RECT r{0, 0, S(kWidth), S(kHeight + 46 * std::max(0, rows - 4))};
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_ACCEPTFILES, state.dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, cls.c_str(), (std::wstring(kGame.title) + L" setup").c_str(), style,
                                (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                                std::max(0, (GetSystemMetrics(SM_CYSCREEN) - h) / 2), w, h, nullptr, nullptr, inst,
                                nullptr);
    state.hwnd = hwnd;
    BOOL dark = TRUE;  // a dark title bar to match (DWMWA_USE_IMMERSIVE_DARK_MODE, Windows 10 2004 and later)
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    DragAcceptFiles(hwnd, TRUE);
    SetTimer(hwnd, 1, 100, nullptr);
    // mechassault-recomp patch: XBE_BACKGROUND (tests) shows the window without taking the focus
    bool background = std::getenv("XBE_BACKGROUND") != nullptr;
    ShowWindow(hwnd, background ? SW_SHOWNOACTIVATE : SW_SHOW);
    if (!background) SetForegroundWindow(hwnd);

    if (!state.imported) {
        // mechassault-recomp patch: XBE_SETUP_SOURCE (with XBE_SETUP_TEST) names the copy to check, wherever it is
        const char* source = SetupTest() ? std::getenv("XBE_SETUP_SOURCE") : nullptr;
        std::vector<fs::path> found = source ? std::vector<fs::path>{fs::u8path(source)}
                                             : gameimport::Candidates(ExeDir(), profile);
        LOGR("Setup: game not imported yet, %zu candidate(s) beside the executable", found.size());
        if (!found.empty()) Choose(found[0]);  // checked at once; the player can still pick another
    } else {
        LOGR("Setup: the game code needs building");
        if (SetupTest()) PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0);  // mechassault-recomp patch
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    state.progress.cancel = true;
    if (!state.result) CancelGameCodeBuild();
    Join();
    JoinGameCodeBuild();
    state.fonts.Free();
    if (state.icon) DestroyIcon(state.icon);
    g = nullptr;
    UnregisterClassW(cls.c_str(), inst);
    return state.result;
}

}  // namespace app
