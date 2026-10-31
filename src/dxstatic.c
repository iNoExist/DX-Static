/*
 * dxstatic - hold a DX Light "PC SYNC Backlight" strip (Robobloq, USB HID 1A86:FE07)
 * on one static colour without the official DX Light app.
 *
 * Every packet sent here is one the official app (DX Light 3.11.0) sends itself; see
 * README.md for where each one comes from.  Packet framing ("RB" protocol, webpack
 * module 60895 in app.asar):
 *
 *   'R' 'B' len seq action payload... checksum      checksum = sum of all prior bytes & 0xFF
 *
 * sent as a 64-byte HID output report with report ID 0 on interface MI_00
 * (usage page 0xFF00).
 *
 * Build: see build.cmd.  CLI_ONLY builds the console tool (dxctl.exe); without it you
 * get the tray app (dxstatic.exe).  Both accept the same command-line options.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <dbt.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define VID                0x1A86
#define PID                0xFE07
#define VENDOR_USAGE_PAGE  0xFF00
#define REPORT_LEN         65            /* report ID byte + 64 data bytes */
#define DEFAULT_COLOR      0xFF6400      /* 0xRRGGBB */
#define DEFAULT_BRIGHTNESS 100           /* percent */

/* Action codes from DX Light's protocol table (module 60895) */
#define ACT_READ_DEVICE_INFO 0x82
#define ACT_SET_SECTION_LED  0x86
#define ACT_SET_BRIGHTNESS   0x87
#define ACT_TURN_OFF_LIGHT   0x97

#define REG_KEY    L"Software\\DXStatic"
#define RUN_KEY    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE  L"DXStatic"

enum { OK = 0, ERR_USAGE = 1, ERR_NODEV = 2, ERR_IO = 3 };

typedef struct {
    BYTE id[3];
    BYTE display_size;
    BYTE lamps;
    BYTE uuid[8];
    BYTE version[3];
} DeviceInfo;

/* ---------------------------------------------------------------- protocol */

static BYTE g_seq = 1;

/* Same sequence rule as the app (module 72299): pre-increment, wrap 255 -> 1. */
static BYTE next_seq(void)
{
    if (++g_seq >= 255)
        g_seq = 1;
    return g_seq;
}

static int build_packet(BYTE *p, BYTE action, const BYTE *payload, int n)
{
    int len = 6 + n;
    BYTE sum = 0;
    p[0] = 'R';
    p[1] = 'B';
    p[2] = (BYTE)len;
    p[3] = next_seq();
    p[4] = action;
    if (n)
        memcpy(p + 5, payload, n);
    for (int i = 0; i < len - 1; i++)
        sum += p[i];
    p[len - 1] = sum;
    return len;
}

/* The app's lowLight() (module 79539, "isLightCompression", on by default): if
   R+G+B > 255 scale all channels so the total is ~255.  Kept as a supply-current limit. */
static void compress(int c[3])
{
    int sum = c[0] + c[1] + c[2];
    if (sum > 255) {
        double k = sum / 255.0;
        for (int i = 0; i < 3; i++)
            c[i] = (int)(c[i] / k);
    }
}

/* Section frame as built by the app's getUsbProtocol(): runs of
   [first LED, R, G, B, last LED], 1-based.  With a known LED count the LEDs past the
   end are blanked; otherwise the whole 1..254 range gets the colour. */
static int build_color_packet(BYTE *p, DWORD rgb, int lamps)
{
    int c[3] = { (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF };
    BYTE f[10];
    int n;
    compress(c);
    f[0] = 1; f[1] = (BYTE)c[0]; f[2] = (BYTE)c[1]; f[3] = (BYTE)c[2];
    if (lamps > 0 && lamps < 254) {
        f[4] = (BYTE)lamps;
        f[5] = (BYTE)(lamps + 1); f[6] = 0; f[7] = 0; f[8] = 0; f[9] = 254;
        n = 10;
    } else {
        f[4] = 254;
        n = 5;
    }
    return build_packet(p, ACT_SET_SECTION_LED, f, n);
}

/* The app's slider value a (0..1) goes out as floor(255 * a), at least 5
   (renderer, on connect); percent here is that slider * 100. */
static int build_brightness_packet(BYTE *p, int percent)
{
    BYTE v;
    int raw = (int)(255 * (percent / 100.0));
    v = (BYTE)(raw >= 5 ? raw : 5);
    return build_packet(p, ACT_SET_BRIGHTNESS, &v, 1);
}

/* --------------------------------------------------------------------- HID */

static HANDLE open_strip(void)
{
    GUID g;
    HANDLE found = INVALID_HANDLE_VALUE;
    HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return found;

    SP_DEVICE_INTERFACE_DATA ifd = { sizeof ifd };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &g, i, &ifd); i++) {
        DWORD buf[300];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *d = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buf;
        d->cbSize = sizeof *d;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, d, sizeof buf, NULL, NULL))
            continue;
        /* the keyboard interface (MI_01) refuses read/write opens, which skips it */
        HANDLE h = CreateFileW(d->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        HIDD_ATTRIBUTES a = { sizeof a };
        PHIDP_PREPARSED_DATA pp;
        HIDP_CAPS caps = { 0 };
        if (HidD_GetAttributes(h, &a) && a.VendorID == VID && a.ProductID == PID &&
            HidD_GetPreparsedData(h, &pp)) {
            HidP_GetCaps(pp, &caps);
            HidD_FreePreparsedData(pp);
            if (caps.UsagePage == VENDOR_USAGE_PAGE && caps.OutputReportByteLength == REPORT_LEN &&
                caps.InputReportByteLength == REPORT_LEN) {
                found = h;
                break;
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static BOOL hid_io(HANDLE h, BOOL write, BYTE *buf, DWORD timeout_ms)
{
    OVERLAPPED ov = { 0 };
    DWORD n = 0;
    BOOL ok;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok = write ? WriteFile(h, buf, REPORT_LEN, NULL, &ov) : ReadFile(h, buf, REPORT_LEN, NULL, &ov);
    if (ok || GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, timeout_ms) != WAIT_OBJECT_0)
            CancelIo(h);
        ok = GetOverlappedResult(h, &ov, &n, TRUE) && n == REPORT_LEN;
    }
    CloseHandle(ov.hEvent);
    return ok;
}

static BOOL send_packet(HANDLE h, const BYTE *pkt, int len)
{
    BYTE rep[REPORT_LEN] = { 0 };        /* rep[0] = report ID 0, rest zero padded */
    memcpy(rep + 1, pkt, len);
    return hid_io(h, TRUE, rep, 1000);
}

/* readDeviceInfo, as the app sends on every connect.  The app sees replies without the
   report ID byte, so its offsets are one less than the ones used here. */
static BOOL read_info(HANDLE h, DeviceInfo *info)
{
    BYTE pkt[8], rep[REPORT_LEN];
    int len = build_packet(pkt, ACT_READ_DEVICE_INFO, NULL, 0);
    if (!send_packet(h, pkt, len))
        return FALSE;
    for (int tries = 0; tries < 8; tries++) {
        if (!hid_io(h, FALSE, rep, 400))
            return FALSE;
        if (rep[1] == 'R' && rep[2] == 'B' && rep[4] == pkt[3]) {
            memcpy(info->id, rep + 6, 3);
            info->display_size = rep[9];
            info->lamps = rep[12];
            memcpy(info->uuid, rep + 13, 8);
            memcpy(info->version, rep + 22, 3);
            return TRUE;
        }
    }
    return FALSE;
}

/* Same order as the app's brightness handler (module 88492) for a USB strip in static
   colour mode: setBrightness, then the colour frame.  The app's write() waits 200 ms
   after each command and the handler 20 ms more, so keep that gap. */
static int apply_color(DWORD rgb, int brightness, DeviceInfo *info_out)
{
    DeviceInfo info = { 0 };
    BYTE pkt[24];
    HANDLE h = open_strip();
    if (h == INVALID_HANDLE_VALUE)
        return ERR_NODEV;
    BOOL have_info = read_info(h, &info);
    int len = build_brightness_packet(pkt, brightness);
    BOOL ok = send_packet(h, pkt, len);
    Sleep(220);
    len = build_color_packet(pkt, rgb, have_info ? info.lamps : 0);
    ok = send_packet(h, pkt, len) && ok;
    CloseHandle(h);
    if (info_out)
        *info_out = info;
    return ok ? OK : ERR_IO;
}

/* Turn the strip off the way the app does when it quits
   (turnOffLights in module 51772): firmware >= 1.8.0 gets turnOffLight x4; older
   firmware gets an all-black section frame x4. */
static int turn_off(void)
{
    DeviceInfo info = { 0 };
    BYTE pkt[24];
    BOOL ok = TRUE;
    HANDLE h = open_strip();
    if (h == INVALID_HANDLE_VALUE)
        return ERR_NODEV;
    BOOL have_info = read_info(h, &info);
    int v = info.version[0] * 10000 + info.version[1] * 100 + info.version[2];
    for (int i = 0; i < 4; i++) {
        int len;
        if (!have_info || v >= 10800) {
            len = build_packet(pkt, ACT_TURN_OFF_LIGHT, NULL, 0);
        } else {
            BYTE black[10] = { 1, 0, 0, 0, info.lamps, (BYTE)(info.lamps + 1), 0, 0, 0, 254 };
            int n = (info.lamps > 0 && info.lamps < 254) ? 10 : 5;
            if (n == 5)
                black[4] = 254;
            len = build_packet(pkt, ACT_SET_SECTION_LED, black, n);
        }
        ok = send_packet(h, pkt, len) && ok;
        Sleep(20);
    }
    CloseHandle(h);
    return ok ? OK : ERR_IO;
}

/* ---------------------------------------------------------- settings / startup */

static DWORD load_color(void)
{
    DWORD v = DEFAULT_COLOR, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Color", RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS)
        v = DEFAULT_COLOR;
    return v & 0xFFFFFF;
}

static void save_color(DWORD rgb)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"Color", REG_DWORD, &rgb, sizeof rgb);
}

static int load_brightness(void)
{
    DWORD v = DEFAULT_BRIGHTNESS, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Brightness", RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS ||
        v < 1 || v > 100)
        v = DEFAULT_BRIGHTNESS;
    return (int)v;
}

static void save_brightness(int percent)
{
    DWORD v = (DWORD)percent;
    RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"Brightness", REG_DWORD, &v, sizeof v);
}

/* Startup always launches the tray app, which sits next to the running exe. */
static void tray_exe_path(wchar_t *out, DWORD cap)
{
    GetModuleFileNameW(NULL, out, cap);
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash)
        wcscpy(slash + 1, L"dxstatic.exe");
}

static BOOL startup_enabled(void)
{
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static BOOL set_startup(BOOL on)
{
    if (!on) {
        LSTATUS s = RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE);
        return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
    }
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 4];
    tray_exe_path(exe, MAX_PATH);
    swprintf(cmd, MAX_PATH + 4, L"\"%ls\"", exe);
    return RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, REG_SZ, cmd,
                           (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

/* -------------------------------------------------------------------- CLI */

static BOOL parse_hex_color(const wchar_t *s, DWORD *out)
{
    wchar_t *end;
    if (*s == L'#')
        s++;
    if (wcslen(s) != 6)
        return FALSE;
    unsigned long v = wcstoul(s, &end, 16);
    if (*end)
        return FALSE;
    *out = (DWORD)v;
    return TRUE;
}

static void print_packet(const char *label, const BYTE *p, int len)
{
    printf("%s:", label);
    for (int i = 0; i < len; i++)
        printf(" %02x", p[i]);
    printf("\n");
}

static const char *err_text(int rc)
{
    switch (rc) {
    case ERR_NODEV: return "strip not found (USB 1A86:FE07, interface MI_00)";
    case ERR_IO:    return "write to the strip failed";
    default:        return "ok";
    }
}

static const char USAGE_TEXT[] =
    "dxstatic / dxctl - static colour for the DX Light PC SYNC backlight (USB 1A86:FE07)\n"
    "\n"
    "  (no options)        dxstatic.exe: run in the tray; dxctl.exe: same as --apply\n"
    "  --apply [RRGGBB]    send the saved colour (or RRGGBB, not saved) once and exit\n"
    "  --set RRGGBB        save RRGGBB as the colour and send it\n"
    "  --brightness PCT    save brightness 1-100 % and send colour + brightness\n"
    "  --off             turn the strip off, exactly as DX Light does when it quits\n"
    "                      (the strip button then brings back its built-in rainbow)\n"
    "  --info              read and print device info\n"
    "  --startup on|off    start the tray app with Windows (HKCU Run)\n"
    "  --dump RRGGBB [N] [PCT]  print the packets for RRGGBB / N LEDs / PCT % brightness\n"
    "                      without sending\n"
    "\n"
    "Exit codes: 0 ok, 1 bad usage, 2 strip not found, 3 write failed.\n";

/* Returns -1 when argv has no command (tray mode), else an exit code. */
static int run_cli(int argc, wchar_t **argv)
{
    if (argc < 2)
        return -1;
    const wchar_t *cmd = argv[1];
    DWORD rgb;
    int rc;

    if (!wcscmp(cmd, L"--apply") || !wcscmp(cmd, L"--set")) {
        BOOL save = !wcscmp(cmd, L"--set");
        rgb = load_color();
        if (argc > 2 && !parse_hex_color(argv[2], &rgb)) {
            fputs(USAGE_TEXT, stderr);
            return ERR_USAGE;
        }
        if (save && argc < 3) {
            fputs(USAGE_TEXT, stderr);
            return ERR_USAGE;
        }
        if (save)
            save_color(rgb);
        DeviceInfo info;
        int pct = load_brightness();
        rc = apply_color(rgb, pct, &info);
        if (rc == OK)
            printf("applied #%06lX at %d%% to %d LEDs\n", (unsigned long)rgb, pct, info.lamps ? info.lamps : 254);
        else
            fprintf(stderr, "error: %s\n", err_text(rc));
        return rc;
    }
    if (!wcscmp(cmd, L"--brightness")) {
        int pct = argc > 2 ? _wtoi(argv[2]) : 0;
        if (pct < 1 || pct > 100) {
            fputs(USAGE_TEXT, stderr);
            return ERR_USAGE;
        }
        save_brightness(pct);
        rgb = load_color();
        rc = apply_color(rgb, pct, NULL);
        if (rc == OK)
            printf("applied #%06lX at %d%%\n", (unsigned long)rgb, pct);
        else
            fprintf(stderr, "error: %s\n", err_text(rc));
        return rc;
    }
    if (!wcscmp(cmd, L"--off")) {
        rc = turn_off();
        if (rc == OK)
            printf("strip turned off (press its button for the built-in effect)\n");
        else
            fprintf(stderr, "error: %s\n", err_text(rc));
        return rc;
    }
    if (!wcscmp(cmd, L"--info")) {
        DeviceInfo i;
        HANDLE h = open_strip();
        if (h == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "error: %s\n", err_text(ERR_NODEV));
            return ERR_NODEV;
        }
        BOOL ok = read_info(h, &i);
        CloseHandle(h);
        if (!ok) {
            fprintf(stderr, "error: no reply to readDeviceInfo\n");
            return ERR_IO;
        }
        printf("id %02x%02x%02x  firmware %d.%d.%d  LEDs %d  display %d\"  uuid ",
               i.id[0], i.id[1], i.id[2], i.version[0], i.version[1], i.version[2],
               i.lamps, i.display_size);
        for (int k = 0; k < 8; k++)
            printf("%02x", i.uuid[k]);
        printf("\nsaved colour #%06lX at %d%%, start with Windows: %s\n",
               (unsigned long)load_color(), load_brightness(), startup_enabled() ? "on" : "off");
        return OK;
    }
    if (!wcscmp(cmd, L"--startup") && argc > 2) {
        BOOL on = !wcscmp(argv[2], L"on");
        if (!on && wcscmp(argv[2], L"off")) {
            fputs(USAGE_TEXT, stderr);
            return ERR_USAGE;
        }
        if (!set_startup(on)) {
            fprintf(stderr, "error: could not change the Run key\n");
            return ERR_IO;
        }
        printf("start with Windows: %s\n", on ? "on" : "off");
        return OK;
    }
    if (!wcscmp(cmd, L"--dump") && argc > 2 && parse_hex_color(argv[2], &rgb)) {
        BYTE p[24];
        int lamps = argc > 3 ? _wtoi(argv[3]) : 0, len;
        len = build_color_packet(p, rgb, lamps);
        print_packet("setSectionLED", p, len);
        len = build_packet(p, ACT_TURN_OFF_LIGHT, NULL, 0);
        print_packet("turnOffLight ", p, len);
        len = build_packet(p, ACT_READ_DEVICE_INFO, NULL, 0);
        print_packet("getDeviceInfo", p, len);
        len = build_brightness_packet(p, argc > 4 ? _wtoi(argv[4]) : DEFAULT_BRIGHTNESS);
        print_packet("setBrightness", p, len);
        return OK;
    }
    fputs(USAGE_TEXT, (!wcscmp(cmd, L"--help") || !wcscmp(cmd, L"-h")) ? stdout : stderr);
    return (!wcscmp(cmd, L"--help") || !wcscmp(cmd, L"-h")) ? OK : ERR_USAGE;
}

#ifdef CLI_ONLY

int wmain(int argc, wchar_t **argv)
{
    int rc = run_cli(argc, argv);
    if (rc < 0) {
        wchar_t *apply[] = { argv[0], L"--apply" };
        rc = run_cli(2, apply);
    }
    return rc;
}

#else /* tray app */

#define WM_TRAY      (WM_APP + 1)
#define WM_REAPPLY   (WM_APP + 2)
#define TIMER_APPLY  1
#define CLASS_NAME   L"DXStaticTray"

enum { IDM_APPLY = 100, IDM_CHOOSE, IDM_STARTUP, IDM_OFF, IDM_EXIT, IDM_BRIGHTNESS = 200 };  /* 200 + percent */

static HWND  g_hwnd;
static DWORD g_color;
static int   g_brightness;
static int   g_retries;
static UINT  g_taskbar_created;
static HICON g_icon;
static wchar_t g_status[64] = L"starting";
static HDEVNOTIFY g_devnotify;

static HICON make_icon(DWORD rgb)
{
    int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, cx, cy);
    HBITMAP mask = CreateBitmap(cx, cy, 1, 1, NULL);
    RECT rc = { 0, 0, cx, cy };

    HGDIOBJ old = SelectObject(dc, mask);
    PatBlt(dc, 0, 0, cx, cy, WHITENESS);                     /* transparent corners */
    SelectObject(dc, GetStockObject(BLACK_BRUSH));
    SelectObject(dc, GetStockObject(BLACK_PEN));
    Ellipse(dc, 0, 0, cx, cy);
    SelectObject(dc, color);
    FillRect(dc, &rc, GetStockObject(BLACK_BRUSH));
    HBRUSH b = CreateSolidBrush(RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
    HPEN p = CreatePen(PS_SOLID, 1, RGB(40, 40, 40));
    SelectObject(dc, b);
    SelectObject(dc, p);
    Ellipse(dc, 0, 0, cx, cy);
    SelectObject(dc, old);
    DeleteObject(b);
    DeleteObject(p);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);

    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

static void update_tray(DWORD msg)
{
    NOTIFYICONDATAW nid = { sizeof nid };
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon;
    swprintf(nid.szTip, ARRAYSIZE(nid.szTip), L"DX static colour #%06lX at %d%% - %ls",
             (unsigned long)g_color, g_brightness, g_status);
    Shell_NotifyIconW(msg, &nid);
}

static void set_color(DWORD rgb)
{
    g_color = rgb;
    if (g_icon)
        DestroyIcon(g_icon);
    g_icon = make_icon(rgb);
}

static void set_status(int rc)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (rc == OK)
        swprintf(g_status, ARRAYSIZE(g_status), L"applied %02d:%02d", t.wHour, t.wMinute);
    else
        wcscpy(g_status, rc == ERR_NODEV ? L"strip not found" : L"write failed");
    update_tray(NIM_MODIFY);
}

/* Re-apply after a delay, retrying while the strip is still enumerating. */
static void schedule_apply(UINT delay_ms)
{
    g_retries = 10;
    SetTimer(g_hwnd, TIMER_APPLY, delay_ms, NULL);
}

static int apply_now(void)
{
    int rc = apply_color(g_color, g_brightness, NULL);
    set_status(rc);
    return rc;
}

static void report_failure(int rc)
{
    if (rc != OK) {
        wchar_t msg[160];
        swprintf(msg, ARRAYSIZE(msg), L"Could not reach the LED strip: %hs.", err_text(rc));
        MessageBoxW(g_hwnd, msg, L"DX static colour", MB_ICONWARNING);
    }
}

static void choose_color(void)
{
    static COLORREF custom[16];
    DWORD prev = g_color;
    CHOOSECOLORW cc = { sizeof cc };
    custom[0] = RGB((DEFAULT_COLOR >> 16) & 0xFF, (DEFAULT_COLOR >> 8) & 0xFF, DEFAULT_COLOR & 0xFF);
    cc.hwndOwner = g_hwnd;
    cc.rgbResult = RGB((prev >> 16) & 0xFF, (prev >> 8) & 0xFF, prev & 0xFF);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc))
        return;

    DWORD next = (GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
    int rc = apply_color(next, g_brightness, NULL);           /* the test: show it on the strip */
    if (rc != OK) {
        report_failure(rc);
        return;
    }
    wchar_t q[96];
    swprintf(q, ARRAYSIZE(q), L"The strip now shows #%06lX.\n\nKeep this colour?", (unsigned long)next);
    if (MessageBoxW(g_hwnd, q, L"DX static colour", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        save_color(next);
        set_color(next);
    }
    set_status(apply_color(g_color, g_brightness, NULL));     /* either the new colour again or the old one back */
}

static void show_menu(void)
{
    static const int presets[] = { 10, 25, 50, 75, 100 };
    POINT pt;
    HMENU m = CreatePopupMenu(), b = CreatePopupMenu();
    BOOL custom = TRUE;
    for (int i = 0; i < (int)ARRAYSIZE(presets); i++) {
        wchar_t label[16];
        swprintf(label, ARRAYSIZE(label), L"%d%%", presets[i]);
        AppendMenuW(b, MF_STRING | (presets[i] == g_brightness ? MF_CHECKED : 0), IDM_BRIGHTNESS + presets[i], label);
        if (presets[i] == g_brightness)
            custom = FALSE;
    }
    if (custom) {                               /* set with dxctl --brightness */
        wchar_t label[16];
        swprintf(label, ARRAYSIZE(label), L"%d%%", g_brightness);
        AppendMenuW(b, MF_STRING | MF_CHECKED, IDM_BRIGHTNESS + g_brightness, label);
    }
    AppendMenuW(m, MF_STRING, IDM_APPLY, L"&Apply colour now");
    AppendMenuW(m, MF_STRING, IDM_CHOOSE, L"&Choose colour...");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)b, L"&Brightness");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (startup_enabled() ? MF_CHECKED : 0), IDM_STARTUP, L"&Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_OFF, L"Turn strip &off and exit");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit (keep colour)");
    SetMenuDefaultItem(m, IDM_APPLY, FALSE);
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static BOOL is_strip_interface(const wchar_t *name)
{
    wchar_t low[512];
    size_t i;
    for (i = 0; name[i] && i < ARRAYSIZE(low) - 1; i++)
        low[i] = towlower(name[i]);
    low[i] = 0;
    return wcsstr(low, L"vid_1a86&pid_fe07&mi_00") != NULL;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_APPLY) {
            KillTimer(hwnd, TIMER_APPLY);
            if (apply_now() != OK && --g_retries > 0)
                SetTimer(hwnd, TIMER_APPLY, 2000, NULL);
        }
        return 0;

    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND)
            schedule_apply(1500);
        return TRUE;

    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL && lp &&
            ((DEV_BROADCAST_HDR *)lp)->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE &&
            is_strip_interface(((DEV_BROADCAST_DEVICEINTERFACE_W *)lp)->dbcc_name))
            schedule_apply(800);
        return TRUE;

    case WM_REAPPLY:                            /* another launch: pick up saved settings */
        g_brightness = load_brightness();
        set_color(load_color());
        schedule_apply(10);
        return 0;

    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
            show_menu();
        else if (lp == WM_LBUTTONDBLCLK)
            report_failure(apply_now());
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_APPLY:
            report_failure(apply_now());
            break;
        case IDM_CHOOSE:
            choose_color();
            break;
        case IDM_STARTUP:
            set_startup(!startup_enabled());
            break;
        case IDM_OFF: {
            int rc = turn_off();
            report_failure(rc);
            if (rc == OK)
                DestroyWindow(hwnd);
            break;
        }
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        default:
            if (LOWORD(wp) > IDM_BRIGHTNESS && LOWORD(wp) <= IDM_BRIGHTNESS + 100) {
                g_brightness = LOWORD(wp) - IDM_BRIGHTNESS;
                save_brightness(g_brightness);
                report_failure(apply_now());
            }
            break;
        }
        return 0;

    case WM_DESTROY:
        update_tray(NIM_DELETE);
        if (g_devnotify)
            UnregisterDeviceNotification(g_devnotify);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_taskbar_created && msg) {      /* Explorer restarted */
        update_tray(NIM_ADD);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline; (void)show;
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argc > 1) {                     /* CLI options also work on the tray exe */
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
        return run_cli(argc, argv);
    }

    CreateMutexW(NULL, TRUE, L"Local\\DXStaticTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(CLASS_NAME, NULL);
        if (other)
            PostMessageW(other, WM_REAPPLY, 0, 0);
        return 0;
    }

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);
    /* a hidden top-level window: message-only windows don't get WM_POWERBROADCAST */
    g_hwnd = CreateWindowExW(0, CLASS_NAME, L"DX static colour", WS_OVERLAPPED,
                             0, 0, 0, 0, NULL, NULL, inst, NULL);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    DEV_BROADCAST_DEVICEINTERFACE_W filter = { sizeof filter };
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    HidD_GetHidGuid(&filter.dbcc_classguid);
    g_devnotify = RegisterDeviceNotificationW(g_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    g_brightness = load_brightness();
    set_color(load_color());
    update_tray(NIM_ADD);
    schedule_apply(10);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

#endif
