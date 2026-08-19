#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define IDI_APP 101

#define WM_TRAYICON (WM_APP + 1)
#define IDT_MAIN 1

#define MENU_START   1001
#define MENU_STOP    1002
#define MENU_RESTART 1003
#define MENU_CONSOLE 1004
#define MENU_EDITCFG 1005
#define MENU_EXIT    1006

#define IDC_LOG_EDIT 2001
#define IDC_LOG_CLEAR 2002
#define IDC_LOG_AUTO  2003

#define LOG_MAX (4 * 1024 * 1024)

static HINSTANCE g_hInst;
static HWND g_hwnd;
static HWND g_hLog, g_hEdit, g_hChkAuto, g_hClearBtn;
static HFONT g_hLogFont;
static BOOL g_autoScroll = TRUE;
static BOOL g_consoleVisible = FALSE;

static NOTIFYICONDATAW g_nid;
static wchar_t g_lastTip[128];

static char *g_log = NULL;
static size_t g_logLen = 0, g_logCap = 0, g_logShown = 0;
static CRITICAL_SECTION g_logLock;
static FILE *g_logFile = NULL;

static HANDLE g_hProc = NULL;
static HANDLE g_hJob = NULL;
static HANDLE g_hReadPipe = NULL;
static HANDLE g_hReadThread = NULL;
static volatile BOOL g_running = FALSE;
static volatile BOOL g_stopping = FALSE;

static wchar_t g_exeDir[MAX_PATH];
static wchar_t g_cfgPath[MAX_PATH];
static wchar_t g_command[4096];
static wchar_t g_port[32];
static BOOL g_autostart = FALSE;
static BOOL g_freshLog = FALSE;

static const wchar_t *DEFAULT_CMD = L"ping -t 127.0.0.1";

static void log_append(const char *txt, size_t n) {
    if (n == 0) return;
    EnterCriticalSection(&g_logLock);
    if (g_logCap - g_logLen < n + 2) {
        size_t ncap = g_logCap ? g_logCap : 65536;
        while (ncap < g_logLen + n + 2) ncap *= 2;
        if (ncap > LOG_MAX) {
            size_t drop = g_logLen / 2;
            memmove(g_log, g_log + drop, g_logLen - drop);
            g_logLen -= drop;
            if (g_logShown > drop) g_logShown -= drop; else g_logShown = 0;
            ncap = LOG_MAX;
        }
        char *np = realloc(g_log, ncap);
        if (!np) { LeaveCriticalSection(&g_logLock); return; }
        g_log = np;
        g_logCap = ncap;
    }
    memcpy(g_log + g_logLen, txt, n);
    g_logLen += n;
    g_log[g_logLen] = 0;
    if (g_logFile) { fwrite(txt, 1, n, g_logFile); fflush(g_logFile); }
    LeaveCriticalSection(&g_logLock);
}

static void log_line(const char *s) {
    log_append(s, strlen(s));
    log_append("\r\n", 2);
}

static void log_wline(const wchar_t *s) {
    char buf[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf), NULL, NULL);
    if (n > 0) log_append(buf, (size_t)n - 1);
}

static void clear_log(void) {
    EnterCriticalSection(&g_logLock);
    g_logLen = 0;
    g_logShown = 0;
    if (g_log) g_log[0] = 0;
    LeaveCriticalSection(&g_logLock);
    if (g_hEdit) SetWindowTextW(g_hEdit, L"");
}

static void log_append_to_edit(void) {
    if (!g_hEdit) return;
    size_t start, end;
    EnterCriticalSection(&g_logLock);
    start = g_logShown;
    end = g_logLen;
    LeaveCriticalSection(&g_logLock);
    if (end <= start) return;

    size_t nbytes = end - start;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, g_log + start, (int)nbytes, NULL, 0);
    if (wlen <= 0) return;
    wchar_t *buf = malloc(((size_t)wlen + 1) * sizeof(wchar_t));
    if (!buf) return;
    MultiByteToWideChar(CP_UTF8, 0, g_log + start, (int)nbytes, buf, wlen);
    buf[wlen] = 0;

    int firstLine = -1;
    if (!g_autoScroll)
        firstLine = (int)SendMessageW(g_hEdit, EM_GETFIRSTVISIBLELINE, 0, 0);

    SendMessageW(g_hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(g_hEdit, EM_REPLACESEL, FALSE, (LPARAM)buf);
    free(buf);

    EnterCriticalSection(&g_logLock);
    g_logShown = end;
    LeaveCriticalSection(&g_logLock);

    if (g_autoScroll) {
        SendMessageW(g_hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageW(g_hEdit, EM_SCROLLCARET, 0, 0);
    } else if (firstLine >= 0) {
        int cur = (int)SendMessageW(g_hEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessageW(g_hEdit, EM_LINESCROLL, 0, (LPARAM)(firstLine - cur));
    }
}

static DWORD WINAPI read_thread(LPVOID p) {
    (void)p;
    char buf[4096];
    DWORD rd;
    while (ReadFile(g_hReadPipe, buf, sizeof(buf), &rd, NULL) && rd > 0)
        log_append(buf, rd);
    return 0;
}

static void parse_port(void) {
    wcscpy(g_port, L"8888");
    wchar_t *dup = _wcsdup(g_command);
    if (!dup) return;
    wchar_t *tok = wcstok(dup, L" \t");
    int nextIsPort = 0;
    while (tok) {
        if (nextIsPort) {
            wcsncpy(g_port, tok, 31);
            g_port[31] = 0;
            break;
        }
        if (wcscmp(tok, L"-p") == 0 || wcscmp(tok, L"--port") == 0)
            nextIsPort = 1;
        tok = wcstok(NULL, L" \t");
    }
    free(dup);
}

static void load_config(void) {
    GetPrivateProfileStringW(L"config", L"command", DEFAULT_CMD,
                             g_command, 4096, g_cfgPath);
    if (g_command[0] == 0) wcscpy(g_command, DEFAULT_CMD);
    g_autostart = GetPrivateProfileIntW(L"config", L"autostart", 0, g_cfgPath) != 0;
    g_freshLog = GetPrivateProfileIntW(L"config", L"fresh_log", 0, g_cfgPath) != 0;
    parse_port();
}

static wchar_t *build_environment(void) {
    wchar_t *env = GetEnvironmentStringsW();
    if (!env) return NULL;
    size_t total = 0;
    wchar_t *p = env;
    while (*p) { total += wcslen(p) + 1; p += wcslen(p) + 1; }
    const wchar_t *add = L"PYTHONUNBUFFERED=1";
    size_t addlen = wcslen(add);
    size_t cap = total + addlen + 2;
    wchar_t *nb = malloc(cap * sizeof(wchar_t));
    if (!nb) { FreeEnvironmentStringsW(env); return NULL; }
    wchar_t *d = nb;
    p = env;
    while (*p) {
        size_t l = wcslen(p) + 1;
        memcpy(d, p, l * sizeof(wchar_t));
        d += l;
        p += l;
    }
    memcpy(d, add, addlen * sizeof(wchar_t));
    d += addlen;
    *d++ = 0;
    *d = 0;
    FreeEnvironmentStringsW(env);
    return nb;
}

static BOOL start_process(const wchar_t *cmdline) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hr = NULL, hw = NULL;
    if (!CreatePipe(&hr, &hw, &sa, 0)) return FALSE;
    SetHandleInformation(hr, HANDLE_FLAG_INHERIT, 0);

    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji;
        ZeroMemory(&ji, sizeof(ji));
        ji.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &ji, sizeof(ji));
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hw;
    si.hStdError = hw;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;

    wchar_t *cmd = _wcsdup(cmdline);
    if (!cmd) { CloseHandle(hw); CloseHandle(hr); if (job) CloseHandle(job); return FALSE; }
    wchar_t *env = build_environment();
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             env, g_exeDir, &si, &pi);
    free(env);
    free(cmd);
    if (!ok) {
        CloseHandle(hw);
        CloseHandle(hr);
        if (job) CloseHandle(job);
        return FALSE;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    CloseHandle(hw);
    g_hReadPipe = hr;
    g_hReadThread = CreateThread(NULL, 0, read_thread, NULL, 0, NULL);
    g_hProc = pi.hProcess;
    g_hJob = job;
    CloseHandle(pi.hThread);
    g_running = TRUE;
    g_stopping = FALSE;
    return TRUE;
}

static void stop_process(void) {
    if (!g_running) return;
    g_stopping = TRUE;
    if (g_hJob) {
        CloseHandle(g_hJob);
        g_hJob = NULL;
    }
    if (g_hProc) {
        TerminateProcess(g_hProc, 0);
        WaitForSingleObject(g_hProc, 3000);
        CloseHandle(g_hProc);
        g_hProc = NULL;
    }
    if (g_hReadThread) {
        WaitForSingleObject(g_hReadThread, 3000);
        CloseHandle(g_hReadThread);
        g_hReadThread = NULL;
    }
    if (g_hReadPipe) {
        CloseHandle(g_hReadPipe);
        g_hReadPipe = NULL;
    }
    g_running = FALSE;
    g_stopping = FALSE;
    log_line("[clitray] stopped");
}

static void balloon(const wchar_t *title, const wchar_t *msg) {
    NOTIFYICONDATAW n;
    ZeroMemory(&n, sizeof(n));
    n.cbSize = sizeof(n);
    n.hWnd = g_hwnd;
    n.uID = 1;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_ERROR;
    wcsncpy(n.szInfoTitle, title, 63);
    wcsncpy(n.szInfo, msg, 255);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void start_app(void) {
    if (g_running) return;
    load_config();
    log_wline(L"[clitray] starting: ");
    log_wline(g_command);
    log_line("");
    if (!start_process(g_command)) {
        log_line("[clitray] FAILED to start process");
        balloon(L"CLI Tray", L"Failed to start the command. Check clitray.ini (see console).");
    }
}

static void extract_prog(wchar_t *out, size_t outsz, const wchar_t *cmd) {
    const wchar_t *p = cmd;
    size_t n;
    while (*p == L' ' || *p == L'\t') p++;
    if (*p == L'"') {
        const wchar_t *q = ++p;
        while (*q && *q != L'"') q++;
        n = (size_t)(q - p);
        if (n >= outsz) n = outsz - 1;
        wcsncpy(out, p, n);
        out[n] = 0;
    } else {
        const wchar_t *s = p;
        while (*p && *p != L' ' && *p != L'\t') p++;
        n = (size_t)(p - s);
        if (n >= outsz) n = outsz - 1;
        wcsncpy(out, s, n);
        out[n] = 0;
    }
    wchar_t *bs = wcsrchr(out, L'\\');
    wchar_t *fs = wcsrchr(out, L'/');
    wchar_t *last = bs && fs ? (fs > bs ? fs : bs) : (fs ? fs : bs);
    if (last) memmove(out, last + 1, (wcslen(last + 1) + 1) * sizeof(wchar_t));
}

static void format_tip_text(wchar_t *out, size_t cap) {
    static wchar_t prog[100];
    extract_prog(prog, 100, g_command);
    wcscpy(out, L"CLITray: ");
    wcsncat(out, prog, cap - wcslen(out) - 1);
    if (!g_running)
        wcsncat(out, L" (stopped)", cap - wcslen(out) - 1);
}

static void compute_tip(void) {
    format_tip_text(g_nid.szTip, 128);
}

static void update_tray_tip(void) {
    compute_tip();
    if (wcscmp(g_nid.szTip, g_lastTip) == 0) return;
    wcscpy(g_lastTip, g_nid.szTip);
    g_nid.uFlags = NIF_TIP | NIF_ICON | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void timer_tick(void) {
    if (g_running && g_hProc && !g_stopping) {
        DWORD rc = WaitForSingleObject(g_hProc, 0);
        if (rc == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(g_hProc, &code);
            log_line("[clitray] process exited");
            if (g_hJob) { CloseHandle(g_hJob); g_hJob = NULL; }
            CloseHandle(g_hProc);
            g_hProc = NULL;
            g_running = FALSE;
            if (g_hReadThread) {
                WaitForSingleObject(g_hReadThread, 3000);
                CloseHandle(g_hReadThread);
                g_hReadThread = NULL;
            }
            if (g_hReadPipe) { CloseHandle(g_hReadPipe); g_hReadPipe = NULL; }
        }
    }
    log_append_to_edit();
    update_tray_tip();
}

static LRESULT CALLBACK log_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_LOG_CLEAR) { clear_log(); return 0; }
        if (LOWORD(wp) == IDC_LOG_AUTO) {
            g_autoScroll = SendMessageW(g_hChkAuto, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return 0;
        }
        break;
    case WM_SIZE: {
        RECT r;
        GetClientRect(hwnd, &r);
        int bh = 32;
        MoveWindow(g_hEdit, 0, 0, r.right, r.bottom - bh, TRUE);
        MoveWindow(g_hChkAuto, 8, r.bottom - bh + 8, 90, 20, TRUE);
        MoveWindow(g_hClearBtn, r.right - 78, r.bottom - bh + 6, 70, 24, TRUE);
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        g_consoleVisible = FALSE;
        return 0;
    case WM_DESTROY:
        g_hEdit = NULL;
        g_hChkAuto = NULL;
        g_hClearBtn = NULL;
        g_hLog = NULL;
        g_consoleVisible = FALSE;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void create_log_window(void) {
    if (g_hLog) return;
    static const wchar_t *cls = L"CLITrayConsoleWnd";
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = log_wndproc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = cls;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);

    g_hLog = CreateWindowW(cls, L"CLI Tray Console",
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, 720, 460,
                           NULL, NULL, g_hInst, NULL);
    if (!g_hLog) return;
    g_hEdit = CreateWindowW(L"EDIT", NULL,
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                            ES_MULTILINE | ES_READONLY,
                            0, 0, 100, 100, g_hLog, (HMENU)IDC_LOG_EDIT, g_hInst, NULL);
    SendMessageW(g_hEdit, WM_SETFONT, (WPARAM)g_hLogFont, TRUE);
    g_hChkAuto = CreateWindowW(L"BUTTON", L"Auto-scroll",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               0, 0, 0, 0, g_hLog, (HMENU)IDC_LOG_AUTO, g_hInst, NULL);
    SendMessageW(g_hChkAuto, BM_SETCHECK, BST_CHECKED, 0);
    g_hClearBtn = CreateWindowW(L"BUTTON", L"Clear",
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                0, 0, 0, 0, g_hLog, (HMENU)IDC_LOG_CLEAR, g_hInst, NULL);
}

static void toggle_console(void) {
    if (!g_hLog) create_log_window();
    if (!g_hLog) return;
    if (g_consoleVisible) {
        ShowWindow(g_hLog, SW_HIDE);
        g_consoleVisible = FALSE;
    } else {
        ShowWindow(g_hLog, SW_SHOW);
        SetForegroundWindow(g_hLog);
        g_consoleVisible = TRUE;
        log_append_to_edit();
    }
}

static HMENU build_menu(void) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_running ? MF_DISABLED : MF_ENABLED),
                MENU_START, L"Start");
    AppendMenuW(m, MF_STRING | (g_running ? MF_ENABLED : MF_DISABLED),
                MENU_STOP, L"Stop");
    AppendMenuW(m, MF_STRING | (g_running ? MF_ENABLED : MF_DISABLED),
                MENU_RESTART, L"Restart");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_consoleVisible ? MF_CHECKED : MF_UNCHECKED),
                MENU_CONSOLE, g_consoleVisible ? L"Hide Console" : L"Show Console");
    AppendMenuW(m, MF_STRING, MENU_EDITCFG, L"Edit Config");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, MENU_EXIT, L"Exit");
    return m;
}

static void handle_menu(int id) {
    switch (id) {
    case MENU_START:
        start_app();
        break;
    case MENU_STOP:
        stop_process();
        break;
    case MENU_RESTART:
        stop_process();
        start_app();
        break;
    case MENU_CONSOLE:
        toggle_console();
        break;
    case MENU_EDITCFG:
        ShellExecuteW(NULL, L"open", g_cfgPath, NULL, NULL, SW_SHOWNORMAL);
        break;
    case MENU_EXIT: {
        stop_process();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        DestroyWindow(g_hwnd);
        PostQuitMessage(0);
        break;
    }
    }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            HMENU m = build_menu();
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            int id = (int)TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                         pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);
            if (id) handle_menu(id);
            return 0;
        }
        if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP) {
            if (!g_consoleVisible) {
                toggle_console();
            } else if (g_hLog) {
                SetForegroundWindow(g_hLog);
            }
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == IDT_MAIN) timer_tick();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdline, int show) {
    (void)hPrev; (void)cmdline; (void)show;
    g_hInst = hInst;

    HANDLE hm = CreateMutexW(NULL, TRUE, L"CLITray_SingleInstance_Mutex");
    (void)hm;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"CLI Tray is already running.",
                    L"CLI Tray", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    InitializeCriticalSection(&g_logLock);

    wchar_t mod[MAX_PATH];
    GetModuleFileNameW(NULL, mod, MAX_PATH);
    wcscpy(g_exeDir, mod);
    PathRemoveFileSpecW(g_exeDir);
    wcscpy(g_cfgPath, g_exeDir);
    wcsncat(g_cfgPath, L"\\clitray.ini", MAX_PATH - wcslen(g_cfgPath) - 1);

    load_config();

    wchar_t lf[MAX_PATH];
    wcscpy(lf, g_exeDir);
    wcsncat(lf, L"\\clitray.log", MAX_PATH - wcslen(lf) - 1);
    g_logFile = _wfopen(lf, g_freshLog ? L"wb" : L"ab");
    log_line("[clitray] started");

    g_hLogFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"CLITrayWnd";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowW(L"CLITrayWnd", L"CLITray", 0,
                           0, 0, 0, 0, NULL, NULL, hInst, NULL);

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    compute_tip();
    wcscpy(g_lastTip, g_nid.szTip);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        log_line("[clitray] ERROR: NIM_ADD failed");
    }
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &g_nid)) {
        log_line("[clitray] ERROR: NIM_SETVERSION failed");
    }
    log_line("[clitray] tray icon added, tip set");

    SetTimer(g_hwnd, IDT_MAIN, 500, NULL);

    if (g_autostart) {
        log_line("[clitray] autostart enabled, starting app...");
        start_app();
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    stop_process();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    KillTimer(g_hwnd, IDT_MAIN);
    if (g_logFile) fclose(g_logFile);
    if (g_log) free(g_log);
    return 0;
}