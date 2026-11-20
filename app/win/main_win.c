/* main_win.c -- the Braille Lite Millennium 40 on Windows: one window, its menu, the keyboard, the sound card.
 *
 * While the window is in front, the braille keys (F D S J K L = dots 1-6, A = dot 7, ; = dot 8, the space bar) go to
 * the unit as they go down and come up: its keyboard controller reports the keys held, as on the unit, and the
 * firmware makes the chords.  Enter and Backspace are dots 8 and 7; the arrows the dot-1, -4, -3 and -6 chords.  Every
 * other key is Windows' (Alt opens the menu).
 * Settings > Braille display in the window (Alt+Shift+B) shows the unit's 40 cells in a read-only field, as Unicode
 * braille, which the screen reader puts on the person's own braille display.
 * The firmware is brought in on the first start (Unit > Bring in ...): the Millennium 40 update file, and RC Studio's
 * firmware for the DoubleTalk (found by itself when RC Studio is installed).
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "audio_wasapi.h"
#include "fwstore.h"
#include "session.h"
#include "settings.h"
#include "u8file.h"

#ifndef BLMEMU_VERSION
#define BLMEMU_VERSION "dev"
#endif

enum {
    ID_RESTART = 100, ID_HARDRESET, ID_FACTORY, ID_BNS, ID_RC, ID_EXIT,
    ID_BRAILLE, ID_DISPLAYMODE, ID_KEYS, ID_ABOUT, ID_FIELD
};
#define TITLE L"Braille Lite Millennium 40"
#define WM_NOTE (WM_APP + 1)
#define TIMER_POLL 1
#define TIMER_RELEASE 2
#define TIMER_SILENT 3

static HWND g_wnd, g_field;
static ULONGLONG g_silent_at;
static int g_no_sound, g_braille_now;          /* the command line's --no-sound and --braille */
static WNDPROC g_field_proc;
static blm_session *g_s;
static settings *g_ini;
static int g_braille, g_dots, g_space;
static int g_reset_hold;                       /* the hard reset's I-chord is held on the unit (TIMER_RELEASE ends it) */
static unsigned g_seen;
static uint8_t g_shown[64];
static int g_n_shown = -1;

/* ---- messages ------------------------------------------------------------------------------------------------------ */
static void msg(const char *text, UINT icon)
{
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 1024);
    MessageBoxW(g_wnd, w, TITLE, MB_OK | icon);
}

static void note(void *ctx, const char *text)   /* from the session (setting up, a failed save) */
{
    (void)ctx;
    SetWindowTextA(g_wnd, text);              /* the title: the screen reader says it, and it does not block */
}

/* ---- the firmware ------------------------------------------------------------------------------------------------- */
static int pick_file(const wchar_t *title, const wchar_t *filter, char *out, size_t cap)
{
    wchar_t w[MAX_PATH] = L"";
    OPENFILENAMEW of;
    memset(&of, 0, sizeof of);
    of.lStructSize = sizeof of;
    of.hwndOwner = g_wnd;
    of.lpstrFilter = filter;
    of.lpstrFile = w;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = title;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&of)) return -1;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    return 0;
}

static int bring_in_bns(void)
{
    char path[FW_PATH_MAX], what[256], err[512], m[900];
    for (;;) {
        if (pick_file(L"The Millennium 40 update file (BLM40ENU.BNS)",
                      L"Millennium update (*.bns)\0*.bns\0All files\0*.*\0", path, sizeof path))
            return -1;
        if (fw_import_bns(path, what, sizeof what, err, sizeof err) == 0) return 0;
        snprintf(m, sizeof m, "That file cannot be used: %s.", err);
        msg(m, MB_ICONWARNING);
    }
}

/* RC Studio installed: its firmware, without asking.  Its folder is the one its installer wrote down (the uninstall
   entry's InstallLocation; version 2.4.9 goes to Program Files (x86)\RC Systems\RCStudio), else that default under
   each Program Files, and the older folder without "RC Systems".  0 when the firmware was brought in. */
static int bring_in_installed_rc(void)
{
    static const wchar_t *const roots[] = {L"ProgramFiles(x86)", L"ProgramFiles", L"ProgramW6432"};
    static const wchar_t *const under[] = {L"RC Systems\\RCStudio", L"RCStudio"};
    static const REGSAM views[] = {KEY_WOW64_32KEY, KEY_WOW64_64KEY};
    static const HKEY hives[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
    wchar_t w[MAX_PATH];
    char path[FW_PATH_MAX], what[256], err[512];
    int i, k;
    for (i = 0; i < 2; i++)
        for (k = 0; k < 2; k++) {
            HKEY key;
            DWORD n = sizeof w - sizeof *w, type = 0;
            if (RegOpenKeyExW(hives[i], L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RCStudio_is1", 0,
                              KEY_QUERY_VALUE | views[k], &key) != ERROR_SUCCESS)
                continue;
            if (RegQueryValueExW(key, L"InstallLocation", NULL, &type, (BYTE *)w, &n) == ERROR_SUCCESS && type == REG_SZ) {
                w[n / sizeof *w] = 0;
                WideCharToMultiByte(CP_UTF8, 0, w, -1, path, (int)sizeof path, NULL, NULL);
                if (fw_import_rc8650(path, what, sizeof what, err, sizeof err) == 0) { RegCloseKey(key); return 0; }
            }
            RegCloseKey(key);
        }
    for (i = 0; i < 3; i++)
        for (k = 0; k < 2; k++) {
            const wchar_t *root = _wgetenv(roots[i]);
            if (!root || _snwprintf(w, MAX_PATH, L"%ls\\%ls", root, under[k]) < 0) continue;
            w[MAX_PATH - 1] = 0;
            WideCharToMultiByte(CP_UTF8, 0, w, -1, path, (int)sizeof path, NULL, NULL);
            if (fw_import_rc8650(path, what, sizeof what, err, sizeof err) == 0) return 0;
        }
    return -1;
}

static int bring_in_rc(void)
{
    char path[FW_PATH_MAX], what[256], err[512], m[900];
    for (;;) {
        if (pick_file(L"The DoubleTalk's firmware: RC Studio's m$4a95b2.bin (in its Bin folder), or an RC8650 .srec",
                      L"RC Studio firmware or RC8650 image (*.bin;*.srec)\0*.bin;*.srec\0All files\0*.*\0", path,
                      sizeof path))
            return -1;
        if (fw_import_rc8650(path, what, sizeof what, err, sizeof err) == 0) return 0;
        snprintf(m, sizeof m, "That file cannot be used: %s.", err);
        msg(m, MB_ICONWARNING);
    }
}

static int first_start(void)
{
    char p[FW_PATH_MAX];
    FILE *f;
    if (fw_path(FW_BNS_NAME, p, sizeof p) || !(f = u8_fopen(p, "rb"))) {   /* the path is UTF-8: not fopen's */
        msg("This emulator runs the Millennium's own firmware, which does not come with it. Choose the Millennium 40 "
            "update file, BLM40ENU.BNS (from Freedom Scientific's update disk).", MB_ICONINFORMATION);
        if (bring_in_bns()) return -1;
    } else fclose(f);
    if (fw_path(FW_RC_NAME, p, sizeof p) || !(f = u8_fopen(p, "rb"))) {
        if (bring_in_installed_rc()) {
            msg("The unit's speech is a DoubleTalk, whose firmware comes with RC Studio, RC Systems' free development "
                "software. Install RC Studio, or choose its firmware file: m$4a95b2.bin in RC Studio's Bin folder "
                "(or an RC8650 .srec image).", MB_ICONINFORMATION);
            if (bring_in_rc()) return -1;
        }
    } else fclose(f);
    return 0;
}

/* ---- sound -------------------------------------------------------------------------------------------------------- */
static void render(void *ctx, int16_t *out, size_t n, unsigned rate)
{
    (void)ctx;
    session_render(g_s, out, n, rate);
}

/* The sound thread runs the unit as it renders.  With no sound device to open, a timer runs it by the clock instead,
   unheard: its keys, its braille display and its files work as before. */
static void start_sound(void)
{
    char err[512] = "";
    KillTimer(g_wnd, TIMER_SILENT);
    if (!g_no_sound && wa_start(render, NULL, err, sizeof err) == 0) return;
    g_silent_at = GetTickCount64();
    SetTimer(g_wnd, TIMER_SILENT, 10, NULL);
    if (err[0]) msg(err, MB_ICONWARNING);
}

static void stop_sound(void)
{
    KillTimer(g_wnd, TIMER_SILENT);
    wa_stop();
}

static void run_silent(void)                   /* the timer's: the time gone by since its last tick */
{
    ULONGLONG now = GetTickCount64();
    double s = (double)(now - g_silent_at) / 1000.0;
    g_silent_at = now;
    session_run_silent(g_s, s > 0.1 ? 0.1 : s);  /* a stalled window (a menu, a dialog) is not caught up on */
}

/* ---- keys --------------------------------------------------------------------------------------------------------- */
static int key_dot(WPARAM vk)
{
    switch (vk) {
    case 'F': return 0x01;
    case 'D': return 0x02;
    case 'S': return 0x04;
    case 'J': return 0x08;
    case 'K': return 0x10;
    case 'L': return 0x20;
    case 'A': case VK_BACK: return 0x40;
    case VK_OEM_1: case VK_RETURN: return 0x80;
    }
    return 0;
}

static int arrow_chord(WPARAM vk)
{
    switch (vk) {
    case VK_UP: return 0x01;
    case VK_DOWN: return 0x08;
    case VK_LEFT: return 0x04;
    case VK_RIGHT: return 0x20;
    }
    return 0;
}

/* a key down or up: 1 when it was the unit's */
static int unit_key(WPARAM vk, LPARAM lp, int down)
{
    int d;
    if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) return 0;
    if (down && (lp & (1 << 30))) return key_dot(vk) || vk == VK_SPACE || arrow_chord(vk);   /* auto-repeat */
    if ((d = arrow_chord(vk)) != 0) {
        if (down && !g_reset_hold) session_chord(g_s, d, 1);
        return 1;
    }
    d = key_dot(vk);
    if (!d && vk != VK_SPACE) return 0;
    if (d) g_dots = down ? g_dots | d : g_dots & ~d;
    else g_space = down;
    /* While the hard reset's I-chord is held, the keyboard's keys wait: the Enter that chose the menu item comes up
       a moment after the restart, and telling the unit "no keys down" then let the I-chord go before the firmware
       had read it, so the unit started as usual and asked nothing. */
    if (!g_reset_hold) session_keys(g_s, g_dots, g_space);
    return 1;
}

static void let_go(void)                       /* the window lost the keyboard: every key held comes up */
{
    if (g_dots || g_space) {
        g_dots = g_space = 0;
        if (!g_reset_hold) session_keys(g_s, 0, 0);
    }
}

/* ---- the braille field -------------------------------------------------------------------------------------------- */
static LRESULT CALLBACK field_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN || m == WM_KEYUP) {   /* the unit's keys go to the window; the rest stay the field's */
        if (unit_key(w, l, m == WM_KEYDOWN)) return 0;
    }
    if (m == WM_CHAR && (w == ' ' || key_dot(VkKeyScanW((WCHAR)w) & 0xFF))) return 0;
    return CallWindowProcW(g_field_proc, h, m, w, l);
}

static void show_cells(int force)
{
    uint8_t c[64];
    unsigned changes;
    int n, i;
    wchar_t w[65];
    if (!g_field || !g_braille) return;
    n = session_cells(g_s, c, sizeof c, &changes);
    if (!force && (changes == g_seen || (n == g_n_shown && !memcmp(c, g_shown, (size_t)n)))) { g_seen = changes; return; }
    g_seen = changes;
    for (i = 0; i < n; i++) w[i] = (wchar_t)(0x2800 + c[i]);
    w[n] = 0;
    SetWindowTextW(g_field, w);
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, g_field, OBJID_CLIENT, CHILDID_SELF);
    memcpy(g_shown, c, (size_t)n);
    g_n_shown = n;
}

static void show_braille(int on)
{
    g_braille = on;
    CheckMenuItem(GetMenu(g_wnd), ID_BRAILLE, on ? MF_CHECKED : MF_UNCHECKED);
    ShowWindow(g_field, on ? SW_SHOW : SW_HIDE);
    SetFocus(on ? g_field : g_wnd);
    show_cells(1);
}

static void set_braille(int on)                /* the menu's: shown or hidden, and kept for the next start */
{
    show_braille(on);
    settings_set_int(g_ini, "display", "braille", on);
    settings_save(g_ini);
}

/* ---- the menu ----------------------------------------------------------------------------------------------------- */
static HMENU make_menu(void)
{
    HMENU bar = CreateMenu(), unit = CreatePopupMenu(), set = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(unit, MF_STRING, ID_RESTART, L"&Restart the unit (keeps its files)\tCtrl+R");
    AppendMenuW(unit, MF_STRING, ID_HARDRESET, L"Restart holding &I-chord (the hard reset: the unit asks its questions)");
    AppendMenuW(unit, MF_STRING, ID_FACTORY, L"Back to the &factory state (erases this unit's files)...");
    AppendMenuW(unit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(unit, MF_STRING, ID_BNS, L"Bring in the &Millennium's firmware...");
    AppendMenuW(unit, MF_STRING, ID_RC, L"Bring in the &DoubleTalk's firmware...");
    AppendMenuW(unit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(unit, MF_STRING, ID_EXIT, L"E&xit");
    AppendMenuW(set, MF_STRING, ID_BRAILLE, L"Braille &display in the window\tAlt+Shift+B");
    AppendMenuW(set, MF_STRING, ID_DISPLAYMODE, L"Braille Display &Mode switch (on: a braille display for a PC)");
    AppendMenuW(help, MF_STRING, ID_KEYS, L"&Keys");
    AppendMenuW(help, MF_STRING, ID_ABOUT, L"&About");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)unit, L"&Unit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)set, L"&Settings");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    return bar;
}

static void reopen(void)                       /* new firmware: the unit switched off, saved, and started on it */
{
    char err[512], m[700];
    stop_sound();
    session_close(g_s);
    g_s = session_open(note, NULL, err, sizeof err);
    if (!g_s) {
        snprintf(m, sizeof m, "The unit cannot start: %s.", err);
        msg(m, MB_ICONERROR);
        PostQuitMessage(1);
        return;
    }
    session_display_mode(g_s, settings_int(g_ini, "unit", "display_mode", 0));
    start_sound();
    SetWindowTextW(g_wnd, TITLE);
}

static void command(int id)
{
    char err[512], m[1024], home[FW_PATH_MAX];
    switch (id) {
    case ID_RESTART:
        let_go();
        KillTimer(g_wnd, TIMER_RELEASE);
        g_reset_hold = 0;
        session_restart(g_s, 0, 0);
        break;
    case ID_HARDRESET:
        let_go();
        g_reset_hold = 1;
        session_restart(g_s, 0x0A, 1);
        SetTimer(g_wnd, TIMER_RELEASE, 1500, NULL);
        break;
    case ID_FACTORY:
        if (MessageBoxW(g_wnd, L"Erase every file on this unit and set it up as new?", TITLE,
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) == IDYES) {
            SetWindowTextW(g_wnd, L"Setting up a new unit...");
            if (session_factory(g_s, err, sizeof err)) msg(err, MB_ICONERROR);
            SetWindowTextW(g_wnd, TITLE);
        }
        break;
    case ID_BNS:
        if (bring_in_bns() == 0) reopen();
        break;
    case ID_RC:
        if (bring_in_rc() == 0) reopen();
        break;
    case ID_EXIT:
        DestroyWindow(g_wnd);
        break;
    case ID_BRAILLE:
        set_braille(!g_braille);
        break;
    case ID_DISPLAYMODE: {
        int on = !session_display_mode_on(g_s);
        session_display_mode(g_s, on);
        CheckMenuItem(GetMenu(g_wnd), ID_DISPLAYMODE, on ? MF_CHECKED : MF_UNCHECKED);
        settings_set_int(g_ini, "unit", "display_mode", on);
        settings_save(g_ini);
        break;
    }
    case ID_KEYS:
        msg("The braille keys: F D S J K L are dots 1 2 3 4 5 6, A is dot 7 and ; is dot 8 (the unit's own order, "
            "left to right: 7 3 2 1 4 5 6 8), with the space bar. Hold them together for a chord, as on the unit.\n\n"
            "Enter is dot 8, Backspace dot 7. The arrows are the dot-1, dot-4, dot-3 and dot-6 chords.\n\n"
            "Alt+Shift+B shows the braille display in the window. Ctrl+R restarts the unit. Alt opens the menu.",
            MB_ICONINFORMATION);
        break;
    case ID_ABOUT: {
        char sound[64];
        if (wa_rate()) snprintf(sound, sizeof sound, "Sound at %u Hz.", wa_rate());
        else snprintf(sound, sizeof sound, "No sound: the unit runs silent.");
        snprintf(m, sizeof m, "Braille Lite Millennium emulator %s\n\nFreedom Scientific's firmware on an emulated Z180 "
                 "(MAME's core, BSD-3-Clause); its DoubleTalk on librc8650, RC Systems' firmware on an emulated M16C "
                 "(GPL-3). This program: GPL-3.\n\n%s\nData folder: %s", BLMEMU_VERSION, sound,
                 fw_home(home, sizeof home) == 0 ? home : "?");
        msg(m, MB_ICONINFORMATION);
        break;
    }
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        g_field = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP, 10, 10, 600, 24,
                                  h, (HMENU)ID_FIELD, GetModuleHandleW(NULL), NULL);
        g_field_proc = (WNDPROC)SetWindowLongPtrW(g_field, GWLP_WNDPROC, (LONG_PTR)field_proc);
        return 0;
    case WM_KEYDOWN: case WM_KEYUP:
        if (unit_key(w, l, m == WM_KEYDOWN)) return 0;
        break;
    case WM_CHAR:
        return 0;                              /* typed keys are the unit's: no beeps */
    case WM_KILLFOCUS: case WM_ACTIVATE:
        if (m == WM_KILLFOCUS || LOWORD(w) == WA_INACTIVE) let_go();
        if (m == WM_ACTIVATE && LOWORD(w) != WA_INACTIVE) SetFocus(g_braille ? g_field : h);
        break;
    case WM_TIMER:
        if (w == TIMER_POLL) show_cells(0);
        if (w == TIMER_SILENT) run_silent();
        if (w == TIMER_RELEASE) {              /* the hard reset's I-chord comes up */
            KillTimer(h, TIMER_RELEASE);
            g_reset_hold = 0;
            session_keys(g_s, g_dots, g_space);
        }
        return 0;
    case WM_COMMAND:
        command(LOWORD(w));
        return 0;
    case WM_QUERYENDSESSION:                   /* Windows logging off or shutting down: it may not let the program
                                                  quit, so the unit's memory is saved now, as each minute's save is */
        if (g_s) {
            char err[256];
            session_save(g_s, err, sizeof err);
        }
        return TRUE;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---- the command line ---------------------------------------------------------------------------------------------- */
/* The terminal program's options, as far as a window has them: a window has no terminal to print to, so what it has
   to say comes in a message box.  0 to go on, else the exit code + 1. */
static int command_line(void)
{
    static const char usage[] =
        "blmemu [--no-sound] [--braille] [--home DIR] [--import-firmware PATH] [--import-doubletalk PATH]\n\n"
        "--no-sound\trun silent\n"
        "--braille\tshow the braille display from the start\n"
        "--home DIR\tkeep the firmware, memory and settings in DIR\n"
        "--import-firmware PATH\tbring in BLM40ENU.BNS without asking\n"
        "--import-doubletalk PATH\tbring in RC Studio's folder or an RC8650 .srec without asking\n"
        "--version";
    const wchar_t *imp[2] = {NULL, NULL};
    char path[FW_PATH_MAX], what[256], err[512], m[FW_PATH_MAX + 600];
    int argc = 0, i, r = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 0;
    for (i = 1; i < argc && !r; i++) {
        const wchar_t *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!wcscmp(a, L"--no-sound")) g_no_sound = 1;
        else if (!wcscmp(a, L"--braille")) g_braille_now = 1;
        else if (!wcscmp(a, L"--home") && v) { _wputenv_s(L"BLMEMU_HOME", v); i++; }
        else if (!wcscmp(a, L"--import-firmware") && v) imp[0] = argv[++i];
        else if (!wcscmp(a, L"--import-doubletalk") && v) imp[1] = argv[++i];
        else if (!wcscmp(a, L"--version")) { msg("blmemu " BLMEMU_VERSION, MB_ICONINFORMATION); r = 1; }
        else if (!wcscmp(a, L"--help")) { msg(usage, MB_ICONINFORMATION); r = 1; }
        else { msg(usage, MB_ICONWARNING); r = 3; }
    }
    for (i = 0; i < 2 && !r; i++) {            /* after --home, wherever it stood: they go into that folder */
        if (!imp[i]) continue;
        WideCharToMultiByte(CP_UTF8, 0, imp[i], -1, path, (int)sizeof path, NULL, NULL);
        if ((i ? fw_import_rc8650 : fw_import_bns)(path, what, sizeof what, err, sizeof err)) {
            snprintf(m, sizeof m, "%s cannot be used: %s.", path, err);
            msg(m, MB_ICONWARNING);
            r = 2;
        }
    }
    LocalFree(argv);
    return r;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSW wc;
    ACCEL acc[] = {{FVIRTKEY | FCONTROL, 'R', ID_RESTART}, {FVIRTKEY | FALT | FSHIFT, 'B', ID_BRAILLE}};
    HACCEL hacc;
    MSG mm;
    char err[512], m[700];
    int r;
    (void)prev; (void)cmd;
    SetProcessDPIAware();
    if ((r = command_line()) != 0) return r - 1;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(inst, L"APPICON");
    wc.lpszClassName = L"blmemu";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(0, L"blmemu", TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 200, NULL,
                            make_menu(), inst, NULL);
    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);

    if (first_start()) {
        msg("The emulator needs both firmwares to start: Unit > Bring in... next time, or see README.txt.", MB_ICONINFORMATION);
        return 1;
    }
    SetWindowTextW(g_wnd, L"Setting up the unit...");
    g_s = session_open(note, NULL, err, sizeof err);
    if (!g_s) {
        snprintf(m, sizeof m, "The unit cannot start: %s.", err);
        msg(m, MB_ICONERROR);
        return 1;
    }
    SetWindowTextW(g_wnd, TITLE);
    g_ini = settings_load();
    session_display_mode(g_s, settings_int(g_ini, "unit", "display_mode", 0));
    CheckMenuItem(GetMenu(g_wnd), ID_DISPLAYMODE, session_display_mode_on(g_s) ? MF_CHECKED : MF_UNCHECKED);
    start_sound();
    if (g_braille_now || settings_int(g_ini, "display", "braille", 0)) show_braille(1);
    SetTimer(g_wnd, TIMER_POLL, 50, NULL);

    hacc = CreateAcceleratorTableW(acc, 2);
    while (GetMessageW(&mm, NULL, 0, 0) > 0) {
        if (TranslateAcceleratorW(g_wnd, hacc, &mm)) continue;
        TranslateMessage(&mm);
        DispatchMessageW(&mm);
    }
    stop_sound();
    session_close(g_s);
    settings_free(g_ini);
    return 0;
}
