/*
 * bfagent - BigFoston diagnostic agent for the FOSTON FS-707DC (Windows CE 5,
 * MediaTek MT3351).
 *
 * Purpose: give the PC everything needed to port Linux to this board
 * without writing to its flash: the physical memory map (CP15 + page
 * tables), RAM/ROM/register contents, raw NAND sectors (firmware backup),
 * the registry (drivers and their settings), loaded modules, files and
 * screenshots. Commands arrive over the USB Serial_Class port using the BFP
 * protocol (bfp.h); the host side is tools/bigfoston/bfagent.py.
 *
 * Safety rules built in:
 *   - physical memory is mapped read-only (PAGE_READONLY); no command writes
 *     physical memory, registers or raw disk sectors;
 *   - the registry is only read;
 *   - KernelIoControl only accepts a whitelist of information requests;
 *   - file writes go through the normal file system (to deploy test files).
 *
 * Runs from \Temp (copied there at start) so the USB storage can go away.
 * Quitting restores the USB client that was active before.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "bfp.h"
#include "boot.h"
#include "sha256.h"
#include "ui.h"

#define AGENT_VERSION "bfagent 0.7.0"
#define AGENT_VERSION_W L"bfagent 0.7.0"
#define AGENT_CLASS L"BigFostonAgent"
#define WM_APP_REDRAW (WM_APP + 1)
#define WM_APP_COUNT  (WM_APP + 2)     /* wParam 1: a contagem começou (anima a barra), 0: acabou */
#define TIMER_IDLE 1
#define TIMER_BAR  2

/* ------------------------------------------------------------------ state */

static HWND g_hwnd;
static int g_width, g_height;
static HANDLE g_port = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_write_lock;
static volatile LONG g_quit;
static volatile LONG g_restart;            /* QUIT with payload 1: start the copy on the card again */
static WCHAR g_origin_dir[MAX_PATH];
static WCHAR g_port_name[16];
static WCHAR g_prev_client[128];
static int g_switched_usb;
static DWORD g_requests;
static FILE *g_log;
/* boot automático do Linux da memória interna (autoboot.conf) */
static volatile LONG g_autoboot;           /* 0 nada, 1 contando, 2 cancelado, 3 agora */
static volatile LONG g_cd_total, g_cd_start;   /* contagem: duração e início (GetTickCount, ms) */
static volatile LONG g_linux_ok;           /* Linux completo na memória interna */
/* tela Dual Boot (ui.c): o registro em várias linhas, o cartão com o dedo em cima */
static ui_line_t g_lines[UI_LINES];
static int g_nlines;
static CRITICAL_SECTION g_ui_lock;
static int g_pressed;
static int g_ui_ok;
static WCHAR g_port_label[16];             /* a porta aberta, sem os dois-pontos ("COM9") */
static volatile LONG g_redraw_pending;     /* um redesenho já pedido: não acumula mensagens */
#define TIMER_REDRAW 3
#define REDRAW_MS    80                    /* no máximo ~12 quadros/s, mesmo com milhares de pedidos */

static void log_line(const char *fmt, ...)
{
    va_list ap;
    if (!g_log)
        return;
    fprintf(g_log, "[%lu] ", (unsigned long)GetTickCount());
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* sem wcscmp: só funções da CRT do coredll que o agente já usava com sucesso */
static int same_text(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* uma linha no registro da tela. Os pedidos do PC (milhares numa cópia) ocupam uma linha só,
 * a mais recente; um texto igual ao último não repete */
static void log_add(int kind, const WCHAR *src)
{
    int i;
    EnterCriticalSection(&g_ui_lock);
    if (g_nlines && ((kind == UI_L_PC && g_lines[g_nlines - 1].kind == UI_L_PC) ||
                     same_text(g_lines[g_nlines - 1].text, src)))
        i = g_nlines - 1;
    else {
        if (g_nlines == UI_LINES) {
            memmove(g_lines, g_lines + 1, sizeof(g_lines[0]) * (UI_LINES - 1));
            g_nlines--;
        }
        i = g_nlines++;
    }
    g_lines[i].kind = kind;
    wcsncpy(g_lines[i].text, src, UI_LINE_CHARS - 1);
    g_lines[i].text[UI_LINE_CHARS - 1] = 0;
    LeaveCriticalSection(&g_ui_lock);
    if (g_hwnd && !InterlockedExchange(&g_redraw_pending, 1))
        PostMessageW(g_hwnd, WM_APP_REDRAW, 0, 0);
}

/* this device's coredll has no _vsnwprintf: format with _snwprintf only */
#define LOGK(k, ...) do { WCHAR s_[200]; _snwprintf(s_, 199, __VA_ARGS__); s_[199] = 0; log_add((k), s_); } while (0)
#define STATUS(...) LOGK(UI_L_INFO, __VA_ARGS__)
#define OK_(...)    LOGK(UI_L_OK, __VA_ARGS__)
#define WARN(...)   LOGK(UI_L_WARN, __VA_ARGS__)
#define FAIL(...)   LOGK(UI_L_ERR, __VA_ARGS__)
#define LAST(...)   LOGK(UI_L_PC, __VA_ARGS__)

/* --------------------------------------------- kernel/coredll entry points */

typedef BOOL (WINAPI *VirtualCopy_t)(LPVOID, LPVOID, DWORD, DWORD);
typedef BOOL (WINAPI *SetKMode_t)(BOOL);
typedef DWORD (WINAPI *SetProcPermissions_t)(DWORD);
typedef void (WINAPI *CacheSync_t)(int);
typedef BOOL (WINAPI *KernelIoControl_t)(DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *CeSetThreadPriority_t)(HANDLE, int);
typedef HANDLE (WINAPI *FindFirstDevice_t)(int, const void *, void *);
typedef BOOL (WINAPI *FindClose_t)(HANDLE);
typedef void (WINAPI *SystemIdleTimerReset_t)(void);

static HMODULE g_coredll;
static VirtualCopy_t p_VirtualCopy;
static SetKMode_t p_SetKMode;
static SetProcPermissions_t p_SetProcPermissions;
static CacheSync_t p_CacheSync;
static KernelIoControl_t p_KernelIoControl;
static CeSetThreadPriority_t p_CeSetThreadPriority;
static SystemIdleTimerReset_t p_idle_reset;

static FARPROC core_proc(const WCHAR *name)
{
    if (!g_coredll)
        g_coredll = LoadLibraryW(L"coredll.dll");
    return g_coredll ? GetProcAddressW(g_coredll, name) : NULL;
}

static void resolve_kernel_calls(void)
{
    p_VirtualCopy = (VirtualCopy_t)core_proc(L"VirtualCopy");
    p_SetKMode = (SetKMode_t)core_proc(L"SetKMode");
    p_SetProcPermissions = (SetProcPermissions_t)core_proc(L"SetProcPermissions");
    p_CacheSync = (CacheSync_t)core_proc(L"CacheSync");
    p_KernelIoControl = (KernelIoControl_t)core_proc(L"KernelIoControl");
    p_CeSetThreadPriority = (CeSetThreadPriority_t)core_proc(L"CeSetThreadPriority");
    p_idle_reset = (SystemIdleTimerReset_t)core_proc(L"SystemIdleTimerReset");
}

/* --------------------------------------------------------- output buffer */

typedef struct {
    uint8_t *data;
    uint32_t len, cap;
    int overflow;
} buf_t;

static int buf_init(buf_t *b, uint32_t cap)
{
    b->data = (uint8_t *)malloc(cap);
    b->len = 0;
    b->cap = b->data ? cap : 0;
    b->overflow = 0;
    return b->data != NULL;
}

static void buf_add(buf_t *b, const void *data, uint32_t len)
{
    if (b->len + len > b->cap) {
        b->overflow = 1;
        len = b->cap - b->len;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

static void buf_w(buf_t *b, const WCHAR *text)
{
    char tmp[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, tmp, sizeof(tmp), NULL, NULL);
    if (n > 1)
        buf_add(b, tmp, (uint32_t)(n - 1));
}

#define BUFF(b, ...) do { WCHAR t_[512]; _snwprintf(t_, 511, __VA_ARGS__); t_[511] = 0; buf_w((b), t_); } while (0)

/* request payload helpers: NUL-terminated UTF-8 string -> wide */
static int arg_path(const uint8_t *p, uint32_t len, WCHAR *out, int out_chars, uint32_t *used)
{
    uint32_t n = 0;
    int w;
    while (n < len && p[n])
        n++;
    if (!n || n >= 1024)
        return 0;
    w = MultiByteToWideChar(CP_UTF8, 0, (const char *)p, (int)n, out, out_chars - 1);
    if (w <= 0)
        return 0;
    out[w] = 0;
    if (used)
        *used = n < len ? n + 1 : n;
    return 1;
}

/* ------------------------------------------------------------- transport */

static int send_raw(const uint8_t *data, uint32_t len)
{
    while (len) {
        DWORD chunk = len > 16384 ? 16384 : len, written = 0;
        if (!WriteFile(g_port, data, chunk, &written, NULL) || !written)
            return 0;
        data += written;
        len -= written;
    }
    return 1;
}

static void respond(const bfp_msg *req, uint16_t status, const uint8_t *payload, uint32_t len)
{
    uint8_t header[BFP_HEADER];
    bfp_header(header, (uint8_t)(req->type | BFP_RESPONSE), req->seq, status, payload, len);
    EnterCriticalSection(&g_write_lock);
    if (g_port != INVALID_HANDLE_VALUE && send_raw(header, BFP_HEADER) && len)
        send_raw(payload, len);
    LeaveCriticalSection(&g_write_lock);
}

static void respond_error(const bfp_msg *req, uint16_t status, DWORD err, const char *message)
{
    uint8_t p[260];
    size_t n = strlen(message);
    if (n > 250)
        n = 250;
    bfp_put32(p, err);
    memcpy(p + 4, message, n);
    respond(req, status, p, (uint32_t)(4 + n));
}

static void respond_buf(const bfp_msg *req, buf_t *b)
{
    respond(req, b->overflow ? BFP_E_TOOBIG : BFP_OK, b->data, b->len);
}

/* ------------------------------------------------------------- commands */

static void cmd_info(const bfp_msg *req)
{
    buf_t b;
    OSVERSIONINFOW os;
    SYSTEM_INFO si;
    MEMORYSTATUS mem;
    WCHAR text[128];
    HDC dc;
    if (!buf_init(&b, 8192)) {
        respond_error(req, BFP_E_SYSTEM, 0, "out of memory");
        return;
    }
    BUFF(&b, L"agent: %hs\n", AGENT_VERSION);
    os.dwOSVersionInfoSize = sizeof(os);
    GetVersionExW(&os);
    BUFF(&b, L"os: Windows CE %lu.%lu build %lu %ls\n", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber,
         os.szCSDVersion);
    text[0] = 0;
    SystemParametersInfoW(258 /* SPI_GETOEMINFO */, sizeof(text), text, 0);
    BUFF(&b, L"oem: %ls\n", text);
    text[0] = 0;
    SystemParametersInfoW(257 /* SPI_GETPLATFORMTYPE */, sizeof(text), text, 0);
    BUFF(&b, L"platform: %ls\n", text);
    GetSystemInfo(&si);
    BUFF(&b, L"cpu: arch %u type %lu level %u revision 0x%04x page %lu\n", si.wProcessorArchitecture,
         si.dwProcessorType, si.wProcessorLevel, si.wProcessorRevision, si.dwPageSize);
    BUFF(&b, L"address space: 0x%08lx-0x%08lx\n", (DWORD)si.lpMinimumApplicationAddress,
         (DWORD)si.lpMaximumApplicationAddress);
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatus(&mem);
    BUFF(&b, L"memory: physical %lu KB, free %lu KB, virtual %lu KB\n", mem.dwTotalPhys / 1024,
         mem.dwAvailPhys / 1024, mem.dwTotalVirtual / 1024);
    dc = GetDC(NULL);
    BUFF(&b, L"screen: %dx%d %d bpp\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
         GetDeviceCaps(dc, BITSPIXEL));
    ReleaseDC(NULL, dc);
    BUFF(&b, L"tick: %lu\n", GetTickCount());
    BUFF(&b, L"origin: %ls\nport: %ls\nprevious usb client: %ls\n", g_origin_dir, g_port_name,
         g_prev_client[0] ? g_prev_client : L"-");
    BUFF(&b, L"kernel calls: VirtualCopy=%d SetKMode=%d SetProcPermissions=%d CacheSync=%d KernelIoControl=%d\n",
         p_VirtualCopy != NULL, p_SetKMode != NULL, p_SetProcPermissions != NULL, p_CacheSync != NULL,
         p_KernelIoControl != NULL);
    respond_buf(req, &b);
    free(b.data);
}

static void cmd_list(const bfp_msg *req)
{
    WCHAR path[MAX_PATH], pattern[MAX_PATH + 4];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    buf_t b;
    if (!arg_path(req->payload, req->len, path, MAX_PATH - 4, NULL)) {
        respond_error(req, BFP_E_ARGS, 0, "path expected");
        return;
    }
    if (!buf_init(&b, 256 * 1024)) {
        respond_error(req, BFP_E_SYSTEM, 0, "out of memory");
        return;
    }
    _snwprintf(pattern, MAX_PATH + 3, L"%ls%ls*", path, path[wcslen(path) - 1] == L'\\' ? L"" : L"\\");
    pattern[MAX_PATH + 3] = 0;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        free(b.data);
        respond_error(req, BFP_E_SYSTEM, GetLastError(), "FindFirstFile failed");
        return;
    }
    do {
        BUFF(&b, L"%lc %lu 0x%lx %ls\n", (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? L'D' : L'F',
             fd.nFileSizeLow, fd.dwFileAttributes, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    respond_buf(req, &b);
    free(b.data);
}

static void cmd_read(const bfp_msg *req)
{
    WCHAR path[MAX_PATH];
    uint32_t offset, length;
    HANDLE f;
    uint8_t *data;
    DWORD got = 0, size;
    if (req->len < 9 || !arg_path(req->payload + 8, req->len - 8, path, MAX_PATH, NULL)) {
        respond_error(req, BFP_E_ARGS, 0, "offset, length, path expected");
        return;
    }
    offset = bfp_get32(req->payload);
    length = bfp_get32(req->payload + 4);
    if (length > BFP_MAX_PAYLOAD - 8)
        length = BFP_MAX_PAYLOAD - 8;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        respond_error(req, BFP_E_SYSTEM, GetLastError(), "open failed");
        return;
    }
    size = GetFileSize(f, NULL);
    data = (uint8_t *)malloc(length + 8);
    if (!data) {
        CloseHandle(f);
        respond_error(req, BFP_E_SYSTEM, 0, "out of memory");
        return;
    }
    /* response: u32 file size, u32 offset, bytes */
    bfp_put32(data, size);
    bfp_put32(data + 4, offset);
    if (SetFilePointer(f, (LONG)offset, NULL, FILE_BEGIN) == 0xFFFFFFFF && GetLastError() != NO_ERROR) {
        free(data);
        CloseHandle(f);
        respond_error(req, BFP_E_SYSTEM, GetLastError(), "seek failed");
        return;
    }
    if (length && !ReadFile(f, data + 8, length, &got, NULL)) {
        DWORD err = GetLastError();
        free(data);
        CloseHandle(f);
        respond_error(req, BFP_E_SYSTEM, err, "read failed");
        return;
    }
    CloseHandle(f);
    respond(req, BFP_OK, data, 8 + got);
    free(data);
}

static void cmd_write(const bfp_msg *req)
{
    WCHAR path[MAX_PATH];
    uint32_t offset, used = 0;
    uint8_t truncate, reply[4];
    HANDLE f;
    DWORD written = 0;
    if (req->len < 6 || !arg_path(req->payload + 5, req->len - 5, path, MAX_PATH, &used)) {
        respond_error(req, BFP_E_ARGS, 0, "offset, truncate, path expected");
        return;
    }
    offset = bfp_get32(req->payload);
    truncate = req->payload[4];
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, truncate ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                    NULL);
    if (f == INVALID_HANDLE_VALUE) {
        respond_error(req, BFP_E_SYSTEM, GetLastError(), "create failed");
        return;
    }
    SetFilePointer(f, (LONG)offset, NULL, FILE_BEGIN);
    if (req->len > 5 + used &&
        !WriteFile(f, req->payload + 5 + used, req->len - 5 - used, &written, NULL)) {
        DWORD err = GetLastError();
        CloseHandle(f);
        respond_error(req, BFP_E_SYSTEM, err, "write failed");
        return;
    }
    FlushFileBuffers(f);
    CloseHandle(f);
    bfp_put32(reply, written);
    respond(req, BFP_OK, reply, 4);
}

static void cmd_path_op(const bfp_msg *req)
{
    WCHAR path[MAX_PATH];
    BOOL ok;
    if (!arg_path(req->payload, req->len, path, MAX_PATH, NULL)) {
        respond_error(req, BFP_E_ARGS, 0, "path expected");
        return;
    }
    ok = req->type == BFP_MKDIR ? CreateDirectoryW(path, NULL) : DeleteFileW(path);
    if (ok)
        respond(req, BFP_OK, NULL, 0);
    else
        respond_error(req, BFP_E_SYSTEM, GetLastError(), req->type == BFP_MKDIR ? "mkdir failed" : "delete failed");
}

static void reg_dump(buf_t *b, HKEY root, const WCHAR *path, int depth)
{
    HKEY key;
    DWORD index, i;
    WCHAR name[256], sub[512];
    static BYTE data[4096];
    if (RegOpenKeyExW(root, path, 0, 0, &key) != ERROR_SUCCESS) {
        BUFF(b, L"[%ls] (absent)\n", path);
        return;
    }
    BUFF(b, L"[%ls]\n", path);
    for (index = 0;; index++) {
        DWORD name_len = 256, data_len = sizeof(data) - 4, type = 0;
        if (RegEnumValueW(key, index, name, &name_len, NULL, &type, data, &data_len) != ERROR_SUCCESS)
            break;
        data[data_len] = data[data_len + 1] = data[data_len + 2] = data[data_len + 3] = 0;
        if (type == REG_DWORD && data_len == 4) {
            DWORD v;
            memcpy(&v, data, 4);
            BUFF(b, L"  \"%ls\"=dword:%08lx\n", name, v);
        } else if (type == REG_SZ || type == REG_EXPAND_SZ) {
            BUFF(b, L"  \"%ls\"=\"%ls\"\n", name, (WCHAR *)data);
        } else if (type == REG_MULTI_SZ) {
            WCHAR *s = (WCHAR *)data;
            BUFF(b, L"  \"%ls\"=multi:", name);
            while (*s && (BYTE *)s < data + data_len) {
                BUFF(b, L"\"%ls\" ", s);
                s += wcslen(s) + 1;
            }
            buf_add(b, "\n", 1);
        } else {
            BUFF(b, L"  \"%ls\"=hex(%lu):", name, type);
            for (i = 0; i < data_len && i < 256; i++)
                BUFF(b, L"%02x", data[i]);
            buf_add(b, "\n", 1);
        }
    }
    if (depth > 0) {
        for (index = 0;; index++) {
            DWORD name_len = 256;
            if (RegEnumKeyExW(key, index, name, &name_len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            _snwprintf(sub, 511, L"%ls%ls%ls", path, path[0] ? L"\\" : L"", name);
            sub[511] = 0;
            reg_dump(b, root, sub, depth - 1);
            if (b->overflow)
                break;
        }
    }
    RegCloseKey(key);
}

static void cmd_reg(const bfp_msg *req)
{
    static const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER, HKEY_CLASSES_ROOT, HKEY_USERS};
    WCHAR path[512];
    buf_t b;
    if (req->len < 2 || req->payload[0] > 3) {
        respond_error(req, BFP_E_ARGS, 0, "root, depth, path expected");
        return;
    }
    path[0] = 0;
    if (req->len > 2 && req->payload[2])
        arg_path(req->payload + 2, req->len - 2, path, 512, NULL);
    if (!buf_init(&b, BFP_MAX_PAYLOAD)) {
        respond_error(req, BFP_E_SYSTEM, 0, "out of memory");
        return;
    }
    reg_dump(&b, roots[req->payload[0]], path, req->payload[1]);
    respond_buf(req, &b);
    free(b.data);
}

/*
 * The only registry writes a client may ask for: the agent's own autostart
 * entry in HKLM\init (Launch90 / Depend90). Anything else is refused. The
 * registry of this device is a hive in flash, so the key is flushed.
 */
static void write_default_client(const WCHAR *value);

static void cmd_regset(const bfp_msg *req)
{
    typedef LONG (WINAPI *RegFlushKey_t)(HKEY);
    RegFlushKey_t flush = (RegFlushKey_t)core_proc(L"RegFlushKey");
    WCHAR name[32], value[260];
    uint32_t used = 0, n;
    const uint8_t *data;
    uint8_t op;
    HKEY key;
    DWORD disp;
    LONG rc;
    if (req->len < 2 || (op = req->payload[0]) < 1 || op > 4 ||
        !arg_path(req->payload + 1, req->len - 1, name, 32, &used)) {
        respond_error(req, BFP_E_ARGS, 0, "op, name expected");
        return;
    }
    data = req->payload + 1 + used;
    n = req->len - 1 - used;
    /* the USB boot default may only be put back to one of the two known clients */
    if (!_wcsicmp(name, L"DefaultClientDriver")) {
        if (op != 1 || !arg_path(data, n, value, 64, NULL) ||
            (_wcsicmp(value, L"Mass_Storage_Class") && _wcsicmp(value, L"Serial_Class"))) {
            respond_error(req, BFP_E_DENIED, 0, "DefaultClientDriver: Mass_Storage_Class or Serial_Class only");
            return;
        }
        write_default_client(value);
        respond(req, BFP_OK, NULL, 0);
        return;
    }
    if (_wcsicmp(name, L"Launch90") && _wcsicmp(name, L"Depend90")) {
        respond_error(req, BFP_E_DENIED, 0, "only HKLM\\init Launch90/Depend90 may be written");
        return;
    }
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"init", 0, NULL, 0, 0, NULL, &key, &disp) != ERROR_SUCCESS) {
        respond_error(req, BFP_E_SYSTEM, GetLastError(), "open HKLM\\init failed");
        return;
    }
    if (op == 4) {
        rc = RegDeleteValueW(key, name);
    } else if (op == 3) {
        rc = RegSetValueExW(key, name, 0, REG_BINARY, data, n);
    } else {
        /* REG_SZ / REG_MULTI_SZ from one UTF-8 string */
        DWORD chars;
        if (!arg_path(data, n, value, 258, NULL)) {
            RegCloseKey(key);
            respond_error(req, BFP_E_ARGS, 0, "string value expected");
            return;
        }
        chars = (DWORD)wcslen(value) + 1;
        if (op == 2)
            value[chars++] = 0;     /* MULTI_SZ: second terminator */
        rc = RegSetValueExW(key, name, 0, op == 2 ? REG_MULTI_SZ : REG_SZ, (const BYTE *)value,
                            chars * sizeof(WCHAR));
    }
    if (rc == ERROR_SUCCESS && flush)
        flush(key);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS && !(op == 4 && rc == ERROR_FILE_NOT_FOUND)) {
        respond_error(req, BFP_E_SYSTEM, (uint32_t)rc, "registry write failed");
        return;
    }
    log_line("HKLM\\init\\%ls %ls", name, op == 4 ? L"deleted" : L"written");
    respond(req, BFP_OK, NULL, 0);
}

/* Restart the whole device: answered first, done from a thread a moment later. */
static DWORD WINAPI reboot_thread(LPVOID unused)
{
    typedef DWORD (WINAPI *SetSystemPowerState_t)(LPCWSTR, DWORD, DWORD);
    SetSystemPowerState_t power = (SetSystemPowerState_t)core_proc(L"SetSystemPowerState");
    Sleep(500);
    /* the USB client switch is saved as the boot default on this device:
     * make sure the device boots as USB storage, as it did originally */
    write_default_client(L"Mass_Storage_Class");
    log_line("reboot requested");
    if (power)
        power(NULL, 0x00800000 /* POWER_STATE_RESET */, 0x1000 /* POWER_FORCE */);
    Sleep(3000);
    if (p_KernelIoControl)
        p_KernelIoControl(0x0101003C /* IOCTL_HAL_REBOOT */, NULL, 0, NULL, 0, NULL);
    log_line("reboot did not happen");
    return 0;
}

/* --------------------------------------------------------------- clock */

#define EPOCH_2024 1704067200u      /* relógio antes disso = não acertado */

/* SYSTEMTIME (UTC) <-> segundos desde 1970 (algoritmos de H. Hinnant) */
static uint32_t systime_epoch(const SYSTEMTIME *st)
{
    long y = st->wYear, m = st->wMonth, d = st->wDay, era, yoe, doy, doe, days;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468;
    return (uint32_t)(days * 86400 + st->wHour * 3600 + st->wMinute * 60 + st->wSecond);
}

static void epoch_systime(uint32_t t, SYSTEMTIME *st)
{
    long z = (long)(t / 86400) + 719468, era, doe, yoe, y, doy, mp, d, m;
    uint32_t s = t % 86400;
    era = z / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    memset(st, 0, sizeof(*st));
    st->wYear = (WORD)(y + (m <= 2));
    st->wMonth = (WORD)m;
    st->wDay = (WORD)d;
    st->wDayOfWeek = (WORD)((t / 86400 + 4) % 7);     /* 1/1/1970 foi quinta */
    st->wHour = (WORD)(s / 3600);
    st->wMinute = (WORD)(s / 60 % 60);
    st->wSecond = (WORD)(s % 60);
}

/* relógio do Windows CE (UTC) em segundos desde 1970; 0 se não está acertado */
static uint32_t ce_epoch(void)
{
    SYSTEMTIME st;
    uint32_t t;
    GetSystemTime(&st);
    t = systime_epoch(&st);
    return t >= EPOCH_2024 ? t : 0;
}

/* O PC acerta o relógio do CE (que o mantém no RTC): o boot automático sem
 * PC passa a levar a hora certa ao Linux. */
static void cmd_settime(const bfp_msg *req)
{
    SYSTEMTIME st, old;
    uint8_t reply[4];
    uint32_t t;
    if (req->len < 4) {
        respond_error(req, BFP_E_ARGS, 0, "u32 epoch expected");
        return;
    }
    t = bfp_get32(req->payload);
    if (t < EPOCH_2024) {
        respond_error(req, BFP_E_ARGS, 0, "epoch before 2024");
        return;
    }
    GetSystemTime(&old);
    epoch_systime(t, &st);
    if (!SetSystemTime(&st)) {
        respond_error(req, BFP_E_SYSTEM, GetLastError(), "SetSystemTime failed");
        return;
    }
    log_line("relogio: %u -> %u", (unsigned)systime_epoch(&old), (unsigned)t);
    bfp_put32(reply, systime_epoch(&old));
    respond(req, BFP_OK, reply, 4);
}

static void cmd_reboot(const bfp_msg *req)
{
    HANDLE t = CreateThread(NULL, 0, reboot_thread, NULL, 0, NULL);
    if (!t) {
        respond_error(req, BFP_E_SYSTEM, GetLastError(), "thread failed");
        return;
    }
    CloseHandle(t);
    respond(req, BFP_OK, NULL, 0);
}

static void cmd_exec(const bfp_msg *req)
{
    WCHAR path[MAX_PATH], args[512];
    uint32_t used = 0;
    PROCESS_INFORMATION pi;
    uint8_t reply[4];
    if (!arg_path(req->payload, req->len, path, MAX_PATH, &used)) {
        respond_error(req, BFP_E_ARGS, 0, "path expected");
        return;
    }
    args[0] = 0;
    if (used < req->len)
        arg_path(req->payload + used, req->len - used, args, 512, NULL);
    if (!CreateProcessW(path, args[0] ? args : NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) {
        respond_error(req, BFP_E_SYSTEM, GetLastError(), "CreateProcess failed");
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    bfp_put32(reply, pi.dwProcessId);
    respond(req, BFP_OK, reply, 4);
}

typedef struct {
    DWORD dwSize, cntUsage, th32ProcessID, th32DefaultHeapID, th32ModuleID, cntThreads, th32ParentProcessID;
    LONG pcPriClassBase;
    DWORD dwFlags;
    WCHAR szExeFile[MAX_PATH];
    DWORD th32MemoryBase, th32AccessKey;
} PROCESSENTRY32_CE;

typedef struct {
    DWORD dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage;
    BYTE *modBaseAddr;
    DWORD modBaseSize;
    HMODULE hModule;
    WCHAR szModule[256];
    WCHAR szExePath[MAX_PATH];
    DWORD dwFlags;
} MODULEENTRY32_CE;

static void cmd_ps(const bfp_msg *req)
{
    typedef HANDLE (WINAPI *Snap_t)(DWORD, DWORD);
    typedef BOOL (WINAPI *Walk_t)(HANDLE, void *);
    typedef BOOL (WINAPI *Close_t)(HANDLE);
    HMODULE th = LoadLibraryW(L"toolhelp.dll");
    Snap_t snap;
    Walk_t pfirst, pnext, mfirst, mnext;
    Close_t close_snap;
    PROCESSENTRY32_CE pe;
    MODULEENTRY32_CE me;
    HANDLE s;
    BOOL more;
    buf_t b;
    int modules = req->len >= 1 && req->payload[0];
    if (!th) {
        respond_error(req, BFP_E_SYSTEM, GetLastError(), "toolhelp.dll missing");
        return;
    }
    snap = (Snap_t)GetProcAddressW(th, L"CreateToolhelp32Snapshot");
    pfirst = (Walk_t)GetProcAddressW(th, L"Process32First");
    pnext = (Walk_t)GetProcAddressW(th, L"Process32Next");
    mfirst = (Walk_t)GetProcAddressW(th, L"Module32First");
    mnext = (Walk_t)GetProcAddressW(th, L"Module32Next");
    close_snap = (Close_t)GetProcAddressW(th, L"CloseToolhelp32Snapshot");
    if (!snap || !pfirst || !pnext || !buf_init(&b, 256 * 1024)) {
        FreeLibrary(th);
        respond_error(req, BFP_E_SYSTEM, 0, "toolhelp incomplete");
        return;
    }
    s = snap(2 /* TH32CS_SNAPPROCESS */, 0);
    pe.dwSize = sizeof(pe);
    for (more = s != INVALID_HANDLE_VALUE && pfirst(s, &pe); more; more = pnext(s, &pe)) {
        BUFF(&b, L"P %08lx base=0x%08lx threads=%lu %ls\n", pe.th32ProcessID, pe.th32MemoryBase, pe.cntThreads,
             pe.szExeFile);
        if (modules && mfirst && mnext) {
            HANDLE ms = snap(8 /* TH32CS_SNAPMODULE */ | 0x40000000 /* NOHEAPS */, pe.th32ProcessID);
            BOOL mm;
            me.dwSize = sizeof(me);
            for (mm = ms != INVALID_HANDLE_VALUE && mfirst(ms, &me); mm; mm = mnext(ms, &me)) {
                BUFF(&b, L"  M 0x%08lx %lu %ls\n", (DWORD)me.modBaseAddr, me.modBaseSize, me.szModule);
                me.dwSize = sizeof(me);
            }
            if (ms != INVALID_HANDLE_VALUE && close_snap)
                close_snap(ms);
        }
        pe.dwSize = sizeof(pe);
    }
    if (s != INVALID_HANDLE_VALUE && close_snap)
        close_snap(s);
    FreeLibrary(th);
    respond_buf(req, &b);
    free(b.data);
}

static void cmd_kill(const bfp_msg *req)
{
    HANDLE p;
    if (req->len < 4) {
        respond_error(req, BFP_E_ARGS, 0, "pid expected");
        return;
    }
    p = OpenProcess(0, FALSE, bfp_get32(req->payload));
    if (!p || !TerminateProcess(p, 0)) {
        DWORD err = GetLastError();
        if (p)
            CloseHandle(p);
        respond_error(req, BFP_E_SYSTEM, err, "terminate failed");
        return;
    }
    CloseHandle(p);
    respond(req, BFP_OK, NULL, 0);
}

/* Physical memory, read-only. width 4 reads 32-bit words (registers);
 * width 1 copies bytes (RAM/ROM). The caller (host tool) only asks for
 * ranges that Windows CE itself maps, taken from the page tables. */
#define PAGE_PHYSICAL_CE 0x400
#define MAX_PHYS_READ (256u * 1024u)

static void cmd_phys(const bfp_msg *req)
{
    uint32_t address, length, base, offset, span, width;
    void *va;
    uint8_t *out;
    if (req->len < 9) {
        respond_error(req, BFP_E_ARGS, 0, "address, length, width expected");
        return;
    }
    if (!p_VirtualCopy) {
        respond_error(req, BFP_E_DENIED, 0, "VirtualCopy unavailable");
        return;
    }
    address = bfp_get32(req->payload);
    length = bfp_get32(req->payload + 4);
    width = req->payload[8];
    if (!length || length > MAX_PHYS_READ || (width == 4 && ((address | length) & 3))) {
        respond_error(req, BFP_E_ARGS, 0, "bad length or alignment");
        return;
    }
    base = address & ~0xFFFu;
    offset = address - base;
    span = (offset + length + 0xFFFu) & ~0xFFFu;
    va = VirtualAlloc(NULL, span, MEM_RESERVE, PAGE_NOACCESS);
    if (!va) {
        respond_error(req, BFP_E_SYSTEM, GetLastError(), "VirtualAlloc failed");
        return;
    }
    if (!p_VirtualCopy(va, (LPVOID)(base >> 8), span, PAGE_READONLY | PAGE_NOCACHE | PAGE_PHYSICAL_CE)) {
        DWORD err = GetLastError();
        VirtualFree(va, 0, MEM_RELEASE);
        respond_error(req, BFP_E_SYSTEM, err, "VirtualCopy failed");
        return;
    }
    out = (uint8_t *)malloc(length);
    if (!out) {
        VirtualFree(va, 0, MEM_RELEASE);
        respond_error(req, BFP_E_SYSTEM, 0, "out of memory");
        return;
    }
    if (p_CacheSync)
        p_CacheSync(4 /* CACHE_SYNC_WRITEBACK: what the kernel wrote reaches RAM before the uncached read */);
    if (width == 4) {
        volatile const uint32_t *src = (volatile const uint32_t *)((uint8_t *)va + offset);
        uint32_t i;
        for (i = 0; i < length / 4; i++)
            bfp_put32(out + i * 4, src[i]);
    } else {
        memcpy(out, (uint8_t *)va + offset, length);
    }
    VirtualFree(va, 0, MEM_RELEASE);
    respond(req, BFP_OK, out, length);
    free(out);
}

/* CP15 and CPSR, read in kernel mode (SetKMode). ARM926EJ-S expected. */
static void cmd_cpu(const bfp_msg *req)
{
    DWORD id = 0, cache = 0, tcm = 0, ctrl = 0, ttb = 0, dacr = 0, pid = 0, cpsr = 0;
    DWORD aux = 0, cpacr = 0, fpsid = 0, dtcm = 0, itcm = 0;
    DWORD dlock = 0, ilock = 0, cbor = 0, tlblock = 0, secure = 0;
    BOOL previous;
    buf_t b;
    if (!p_SetKMode) {
        respond_error(req, BFP_E_DENIED, 0, "SetKMode unavailable");
        return;
    }
    previous = p_SetKMode(TRUE);
    __asm__ __volatile__(
        "mrc p15, 0, %0, c0, c0, 0\n\t"
        "mrc p15, 0, %1, c0, c0, 1\n\t"
        "mrc p15, 0, %2, c0, c0, 2\n\t"
        "mrc p15, 0, %3, c1, c0, 0\n\t"
        : "=r"(id), "=r"(cache), "=r"(tcm), "=r"(ctrl));
    __asm__ __volatile__(
        "mrc p15, 0, %0, c2, c0, 0\n\t"
        "mrc p15, 0, %1, c3, c0, 0\n\t"
        "mrc p15, 0, %2, c13, c0, 0\n\t"
        "mrs %3, cpsr\n\t"
        : "=r"(ttb), "=r"(dacr), "=r"(pid), "=r"(cpsr));
    __asm__ __volatile__(
        "mrc p15, 0, %0, c1, c0, 1\n\t"     /* auxiliary control */
        "mrc p15, 0, %1, c1, c0, 2\n\t"     /* coprocessor access (cp10/cp11 = VFP) */
        : "=r"(aux), "=r"(cpacr));
    __asm__ __volatile__(
        "mrc p15, 0, %0, c9, c1, 0\n\t"     /* data TCM region (base, size, enable) */
        "mrc p15, 0, %1, c9, c1, 1\n\t"     /* instruction TCM region */
        : "=r"(dtcm), "=r"(itcm));
    /* ARM1176 state a Linux kernel never sets up and Windows CE might:
     * cache lockdown, cache behaviour override, TLB lockdown, secure config */
    __asm__ __volatile__(
        "mrc p15, 0, %0, c9, c0, 0\n\t"     /* data cache lockdown */
        "mrc p15, 0, %1, c9, c0, 1\n\t"     /* instruction cache lockdown */
        "mrc p15, 0, %2, c9, c8, 0\n\t"     /* cache behaviour override */
        "mrc p15, 0, %3, c10, c0, 0\n\t"    /* TLB lockdown */
        "mrc p15, 0, %4, c1, c1, 0\n\t"     /* secure configuration */
        : "=r"(dlock), "=r"(ilock), "=r"(cbor), "=r"(tlblock), "=r"(secure));
    /* FPSID only when the OS granted cp10 access: otherwise the read is undefined */
    if ((cpacr & (3u << 20)) == (3u << 20))
        __asm__ __volatile__("mrc p10, 7, %0, c0, c0, 0\n\t" : "=r"(fpsid));
    p_SetKMode(previous);
    if (!buf_init(&b, 1024)) {
        respond_error(req, BFP_E_SYSTEM, 0, "out of memory");
        return;
    }
    BUFF(&b, L"main_id=0x%08lx\ncache_type=0x%08lx\ntcm_status=0x%08lx\ncontrol=0x%08lx\n", id, cache, tcm, ctrl);
    BUFF(&b, L"ttb=0x%08lx\ndacr=0x%08lx\nfcse_pid=0x%08lx\ncpsr=0x%08lx\n", ttb, dacr, pid, cpsr);
    BUFF(&b, L"aux_control=0x%08lx\ncpacr=0x%08lx\nfpsid=0x%08lx\n", aux, cpacr, fpsid);
    BUFF(&b, L"dtcm_region=0x%08lx\nitcm_region=0x%08lx\n", dtcm, itcm);
    BUFF(&b, L"dcache_lockdown=0x%08lx\nicache_lockdown=0x%08lx\ncache_behaviour_override=0x%08lx\n", dlock, ilock, cbor);
    BUFF(&b, L"tlb_lockdown=0x%08lx\nsecure_config=0x%08lx\n", tlblock, secure);
    respond_buf(req, &b);
    free(b.data);
}

static void cmd_kioctl(const bfp_msg *req)
{
    /* information requests only (FILE_DEVICE_HAL functions) */
    static const DWORD allowed[] = {
        0x01010000, /* IOCTL_HAL_GET_DEVICE_INFO */
        0x0101004C, /* IOCTL_HAL_GET_UUID */
        0x01010054, /* IOCTL_HAL_GET_DEVICEID */
        0x01010064, /* IOCTL_PROCESSOR_INFORMATION */
    };
    DWORD code, ret = 0, i;
    uint8_t out[1024];
    if (req->len < 4 || !p_KernelIoControl) {
        respond_error(req, BFP_E_ARGS, 0, "code expected / KernelIoControl unavailable");
        return;
    }
    code = bfp_get32(req->payload);
    for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]) && allowed[i] != code; i++)
        ;
    if (i == sizeof(allowed) / sizeof(allowed[0])) {
        respond_error(req, BFP_E_DENIED, 0, "ioctl not in the information whitelist");
        return;
    }
    memset(out, 0, sizeof(out));
    if (code == 0x01010054)
        bfp_put32(out, sizeof(out));      /* DEVICE_ID.dwSize */
    if (!p_KernelIoControl(code, req->len > 4 ? (LPVOID)(req->payload + 4) : NULL, req->len - 4, out, sizeof(out),
                           &ret)) {
        respond_error(req, BFP_E_SYSTEM, GetLastError(), "KernelIoControl failed");
        return;
    }
    respond(req, BFP_OK, out, ret && ret <= sizeof(out) ? ret : sizeof(out));
}

/* block devices (diskio.h) */
#define IOCTL_DISK_GETINFO_CE 0x00071C00
#define IOCTL_DISK_READ_CE    0x00071C08
#define DISK_IOCTL_GETINFO_CE 1
#define DISK_IOCTL_READ_CE    2

typedef struct { DWORD total_sectors, bytes_per_sector, cylinders, heads, sectors, flags; } DISK_INFO_CE;
typedef struct { uint8_t *buf; DWORD len; } SG_BUF_CE;
typedef struct { DWORD start, num_sec, num_sg, status; void *callback; SG_BUF_CE sg[1]; } SG_REQ_CE;

static HANDLE open_disk(const bfp_msg *req, const uint8_t *name_bytes, uint32_t len)
{
    WCHAR name[32];
    HANDLE h;
    if (!arg_path(name_bytes, len, name, 32, NULL)) {
        respond_error(req, BFP_E_ARGS, 0, "device name expected");
        return INVALID_HANDLE_VALUE;
    }
    h = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        h = CreateFileW(name, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        respond_error(req, BFP_E_SYSTEM, GetLastError(), "device open failed");
    return h;
}

static int disk_info(HANDLE h, DISK_INFO_CE *di)
{
    DWORD ret = 0;
    memset(di, 0, sizeof(*di));
    if (DeviceIoControl(h, IOCTL_DISK_GETINFO_CE, NULL, 0, di, sizeof(*di), &ret, NULL) && di->bytes_per_sector)
        return 1;
    memset(di, 0, sizeof(*di));
    return DeviceIoControl(h, DISK_IOCTL_GETINFO_CE, di, sizeof(*di), NULL, 0, &ret, NULL) && di->bytes_per_sector;
}

static void cmd_diskinfo(const bfp_msg *req)
{
    DISK_INFO_CE di;
    uint8_t out[24];
    HANDLE h = open_disk(req, req->payload, req->len);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (!disk_info(h, &di)) {
        DWORD err = GetLastError();
        CloseHandle(h);
        respond_error(req, BFP_E_SYSTEM, err, "DISK GETINFO failed");
        return;
    }
    CloseHandle(h);
    bfp_put32(out, di.total_sectors);
    bfp_put32(out + 4, di.bytes_per_sector);
    bfp_put32(out + 8, di.cylinders);
    bfp_put32(out + 12, di.heads);
    bfp_put32(out + 16, di.sectors);
    bfp_put32(out + 20, di.flags);
    respond(req, BFP_OK, out, 24);
}

static void cmd_diskread(const bfp_msg *req)
{
    DISK_INFO_CE di;
    SG_REQ_CE sg;
    DWORD sector, count, bytes, ret = 0;
    uint8_t *data;
    HANDLE h;
    if (req->len < 9) {
        respond_error(req, BFP_E_ARGS, 0, "sector, count, device expected");
        return;
    }
    sector = bfp_get32(req->payload);
    count = bfp_get32(req->payload + 4);
    h = open_disk(req, req->payload + 8, req->len - 8);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (!disk_info(h, &di) || !count || di.bytes_per_sector > 4096 ||
        count * di.bytes_per_sector > BFP_MAX_PAYLOAD || sector + count > di.total_sectors) {
        CloseHandle(h);
        respond_error(req, BFP_E_ARGS, 0, "bad range");
        return;
    }
    bytes = count * di.bytes_per_sector;
    data = (uint8_t *)malloc(bytes);
    if (!data) {
        CloseHandle(h);
        respond_error(req, BFP_E_SYSTEM, 0, "out of memory");
        return;
    }
    memset(&sg, 0, sizeof(sg));
    sg.start = sector;
    sg.num_sec = count;
    sg.num_sg = 1;
    sg.sg[0].buf = data;
    sg.sg[0].len = bytes;
    if (!DeviceIoControl(h, IOCTL_DISK_READ_CE, &sg, sizeof(sg), NULL, 0, &ret, NULL) || sg.status) {
        sg.status = 0;
        if (!DeviceIoControl(h, DISK_IOCTL_READ_CE, &sg, sizeof(sg), NULL, 0, &ret, NULL) || sg.status) {
            DWORD err = sg.status ? sg.status : GetLastError();
            free(data);
            CloseHandle(h);
            respond_error(req, BFP_E_SYSTEM, err, "DISK READ failed");
            return;
        }
    }
    CloseHandle(h);
    respond(req, BFP_OK, data, bytes);
    free(data);
}

/* Screenshot of the whole screen; payload[0] = 1 hides the agent window first
 * so the application underneath is captured. */
static void cmd_shot(const bfp_msg *req)
{
    struct { BITMAPINFOHEADER h; DWORD masks[3]; } bi;
    HDC screen, mem;
    HBITMAP dib, old;
    void *bits = NULL;
    uint8_t *out;
    int w = GetSystemMetrics(SM_CXSCREEN), hgt = GetSystemMetrics(SM_CYSCREEN), y;
    int hide = req->len >= 1 && req->payload[0];
    if (hide) {
        ShowWindow(g_hwnd, SW_HIDE);
        Sleep(400);
    }
    memset(&bi, 0, sizeof(bi));
    bi.h.biSize = sizeof(bi.h);
    bi.h.biWidth = w;
    bi.h.biHeight = -hgt;
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 16;
    bi.h.biCompression = BI_BITFIELDS;
    bi.masks[0] = 0xF800;
    bi.masks[1] = 0x07E0;
    bi.masks[2] = 0x001F;
    screen = GetDC(NULL);
    mem = CreateCompatibleDC(screen);
    dib = CreateDIBSection(screen, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    out = (uint8_t *)malloc(4 + w * hgt * 2);
    if (!mem || !dib || !bits || !out) {
        if (dib)
            DeleteObject(dib);
        if (mem)
            DeleteDC(mem);
        ReleaseDC(NULL, screen);
        free(out);
        if (hide)
            ShowWindow(g_hwnd, SW_SHOW);
        respond_error(req, BFP_E_SYSTEM, GetLastError(), "screen capture setup failed");
        return;
    }
    old = (HBITMAP)SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, hgt, screen, 0, 0, SRCCOPY);
    bfp_put16(out, (uint16_t)w);
    bfp_put16(out + 2, (uint16_t)hgt);
    for (y = 0; y < hgt; y++)                /* DIB rows are DWORD aligned: 800*2 already is */
        memcpy(out + 4 + y * w * 2, (uint8_t *)bits + y * ((w * 2 + 3) & ~3), w * 2);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    if (hide) {
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
    }
    respond(req, BFP_OK, out, 4 + w * hgt * 2);
    free(out);
}

/* Start a kernel / bare-metal payload. Replies first (dry run: replies with
 * the plan), then never returns: Windows CE is gone until the next reset. */
static void cmd_boot(const bfp_msg *req)
{
    static bf_boot_args args;
    char err[240];
    unsigned delay;
    uint32_t used = 0, pos = 17;
    if (req->len < 18) {
        respond_error(req, BFP_E_ARGS, 0, "boot arguments expected");
        return;
    }
    memset(&args, 0, sizeof(args));
    args.kernel_pa = bfp_get32(req->payload);
    args.dtb_pa = bfp_get32(req->payload + 4);
    args.initrd_pa = bfp_get32(req->payload + 8);
    args.machine = bfp_get32(req->payload + 12);
    args.dry_run = req->payload[16] & 1;
    delay = req->payload[16] >> 1;      /* seconds to wait before the jump (unplug the USB cable) */
    /* layout (tools/bigfoston/mkboot.py): zImage 0x00800000 (8 MB), initrd 0x01000000 (12 MB),
     * DTB 0x01F00000 (64 KB); the kernel decompresses itself to 0x00008000 */
    args.kernel_max = 8u << 20;
    args.dtb_max = 64u << 10;
    args.initrd_max = 12u << 20;
    if (!arg_path(req->payload + pos, req->len - pos, args.kernel, MAX_PATH, &used)) {
        respond_error(req, BFP_E_ARGS, 0, "kernel path expected");
        return;
    }
    pos += used;
    if (pos < req->len && req->payload[pos] && arg_path(req->payload + pos, req->len - pos, args.dtb, MAX_PATH, &used))
        pos += used;
    else
        pos++;
    if (pos < req->len && req->payload[pos])
        arg_path(req->payload + pos, req->len - pos, args.initrd, MAX_PATH, NULL);
    /* destinations must stay below the pages the loader uses (boot.c BOOT_SAFE_MIN) */
    if (args.kernel_pa < 0x4000 || args.kernel_pa + args.kernel_max > 0x02000000u ||
        (args.dtb[0] && args.dtb_pa + args.dtb_max > 0x02000000u) ||
        (args.initrd[0] && args.initrd_pa + args.initrd_max > 0x02000000u)) {
        respond_error(req, BFP_E_ARGS, 0, "destinations must lie in 0x4000..0x02000000");
        return;
    }
    log_line("boot %ls dtb=%ls initrd=%ls dry=%d", args.kernel, args.dtb, args.initrd, args.dry_run);
    if (args.dry_run) {
        int ok = bf_boot(&args, err, sizeof(err));
        respond_error(req, ok ? BFP_OK : BFP_E_SYSTEM, ok ? 0 : GetLastError(), err);
        return;
    }
    respond(req, BFP_OK, (const uint8_t *)"booting", 7);
    /* A connected host keeps the USB function controller busy: received
     * packets are written by DMA into Windows CE buffers that Linux then owns
     * (suspected cause of initramfs corruption, 2026-11-19). The delay lets
     * the user unplug the cable first. */
    while (delay) {
        WARN(L"Desconecte o cabo USB: o Linux inicia em %u s (o reset volta ao Windows CE).", delay);
        Sleep(1000);
        delay--;
    }
    STATUS(L"Iniciando %ls... (o reset volta ao Windows CE)", args.kernel);
    Sleep(300);                                  /* let the reply and the screen update out */
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
    if (!bf_boot(&args, err, sizeof(err))) {
        WCHAR w[240];
        MultiByteToWideChar(CP_UTF8, 0, err, -1, w, 239);
        w[239] = 0;
        FAIL(L"O início falhou: %ls", w);
    }
}

/* Fill an in-memory boot source (boot.c "mem:N"): offset 0 (re)allocates the
 * buffer for `total` bytes. Chunks may be resent (idempotent by offset). */
static void cmd_stage(const bfp_msg *req)
{
    uint8_t reply[4];
    unsigned idx;
    uint32_t total, offset, n;
    if (req->len < 9 || req->payload[0] >= BF_STAGES) {
        respond_error(req, BFP_E_ARGS, 0, "index, total, offset expected");
        return;
    }
    idx = req->payload[0];
    total = bfp_get32(req->payload + 1);
    offset = bfp_get32(req->payload + 5);
    n = req->len - 9;
    if (total > (12u << 20) || offset + n > total) {
        respond_error(req, BFP_E_ARGS, 0, "bad size or offset");
        return;
    }
    if (offset == 0 && bf_stage_len[idx] != total) {
        if (bf_stage_data[idx])
            VirtualFree(bf_stage_data[idx], 0, MEM_RELEASE);
        bf_stage_data[idx] = (uint8_t *)VirtualAlloc(NULL, total ? total : 1, MEM_COMMIT, PAGE_READWRITE);
        bf_stage_len[idx] = bf_stage_data[idx] ? total : 0;
        if (!bf_stage_data[idx]) {
            respond_error(req, BFP_E_SYSTEM, GetLastError(), "allocation failed");
            return;
        }
    }
    if (!bf_stage_data[idx] || bf_stage_len[idx] != total) {
        respond_error(req, BFP_E_ARGS, 0, "stage not started at offset 0");
        return;
    }
    memcpy(bf_stage_data[idx] + offset, req->payload + 9, n);
    bfp_put32(reply, offset + n);
    respond(req, BFP_OK, reply, 4);
}

static const char *command_name(uint8_t type)
{
    switch (type) {
    case BFP_INFO: return "info";
    case BFP_PING: return "ping";
    case BFP_LIST: return "list";
    case BFP_READ: return "read";
    case BFP_WRITE: return "write";
    case BFP_MKDIR: return "mkdir";
    case BFP_DELETE: return "delete";
    case BFP_REG: return "reg";
    case BFP_REGSET: return "regset";
    case BFP_REBOOT: return "reboot";
    case BFP_SETTIME: return "settime";
    case BFP_EXEC: return "exec";
    case BFP_PS: return "ps";
    case BFP_KILL: return "kill";
    case BFP_PHYS: return "phys";
    case BFP_CPU: return "cpu";
    case BFP_KIOCTL: return "kioctl";
    case BFP_DISKINFO: return "diskinfo";
    case BFP_DISKREAD: return "diskread";
    case BFP_SHOT: return "shot";
    case BFP_BOOT: return "boot";
    case BFP_STAGE: return "stage";
    case BFP_QUIT: return "quit";
    default: return "?";
    }
}

static void handle(const bfp_msg *m)
{
    g_requests++;
    LAST(L"#%lu %hs (%lu bytes)", g_requests, command_name(m->type), m->len);
    if (p_idle_reset)
        p_idle_reset();
    /* o PC falou com o agente durante a contagem: ele assume o controle */
    if (g_autoboot == 1) {
        InterlockedExchange(&g_autoboot, 2);
        WARN(L"PC conectado: início automático cancelado.");
        PostMessageW(g_hwnd, WM_APP_REDRAW, 0, 0);
    }
    if (!m->crc_ok) {
        respond_error(m, BFP_E_CRC, 0, "payload CRC mismatch");
        return;
    }
    switch (m->type) {
    case BFP_INFO: cmd_info(m); break;
    case BFP_PING: respond(m, BFP_OK, m->payload, m->len); break;
    case BFP_LIST: cmd_list(m); break;
    case BFP_READ: cmd_read(m); break;
    case BFP_WRITE: cmd_write(m); break;
    case BFP_MKDIR:
    case BFP_DELETE: cmd_path_op(m); break;
    case BFP_REG: cmd_reg(m); break;
    case BFP_REGSET: cmd_regset(m); break;
    case BFP_REBOOT: cmd_reboot(m); break;
    case BFP_SETTIME: cmd_settime(m); break;
    case BFP_EXEC: cmd_exec(m); break;
    case BFP_PS: cmd_ps(m); break;
    case BFP_KILL: cmd_kill(m); break;
    case BFP_PHYS: cmd_phys(m); break;
    case BFP_CPU: cmd_cpu(m); break;
    case BFP_KIOCTL: cmd_kioctl(m); break;
    case BFP_DISKINFO: cmd_diskinfo(m); break;
    case BFP_DISKREAD: cmd_diskread(m); break;
    case BFP_SHOT: cmd_shot(m); break;
    case BFP_BOOT: cmd_boot(m); break;
    case BFP_STAGE: cmd_stage(m); break;
    case BFP_QUIT:
        if (m->len >= 1 && m->payload[0] == 1)
            InterlockedExchange(&g_restart, 1);
        respond(m, BFP_OK, NULL, 0);
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        break;
    default:
        respond_error(m, BFP_E_UNKNOWN, 0, "unknown request");
        break;
    }
}

/* ------------------------------------------------------- USB function */

typedef struct {
    DWORD dwSize;
    HANDLE hDevice, hParentDevice;
    WCHAR szLegacyName[6];
    WCHAR szDeviceKey[MAX_PATH], szDeviceName[MAX_PATH], szBusName[MAX_PATH];
} DEVINFO_CE;
typedef struct { WCHAR szName[128]; } UFN_NAME;
typedef struct { WCHAR szName[128]; WCHAR szDescription[250]; } UFN_INFO;
#define UFN_IOCTL(n) ((0x22UL << 16) | ((n) << 2))
#define IOCTL_UFN_GET_CURRENT_CLIENT    UFN_IOCTL(3)
#define IOCTL_UFN_CHANGE_CURRENT_CLIENT UFN_IOCTL(4)
static const GUID GUID_USBFN = {0xE2BDC372, 0x598F, 0x4619, {0xBC, 0x50, 0x54, 0xB3, 0xF7, 0x84, 0x8D, 0x35}};

/* Same open sequence that bsmd-ce 1.2.0 validated on this device. */
static HANDLE ufn_open(void)
{
    FindFirstDevice_t find_first = (FindFirstDevice_t)core_proc(L"FindFirstDevice");
    FindClose_t find_close = (FindClose_t)core_proc(L"FindClose");
    static const WCHAR *legacy[] = {L"UFN1:", L"UFN0:", L"UFN2:", NULL};
    DEVINFO_CE di;
    HANDLE h = INVALID_HANDLE_VALUE;
    int i;
    memset(&di, 0, sizeof(di));
    if (find_first) {
        HANDLE search;
        di.dwSize = sizeof(di);
        search = find_first(3 /* DeviceSearchByGuid */, &GUID_USBFN, &di);
        if (search != INVALID_HANDLE_VALUE && search != NULL) {
            if (find_close)
                find_close(search);
            h = CreateFileW(di.szBusName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE)
                h = CreateFileW(di.szBusName, 0x0100, 0, NULL, OPEN_EXISTING, 0, NULL);
        }
    }
    for (i = 0; h == INVALID_HANDLE_VALUE && legacy[i]; i++)
        h = CreateFileW(legacy[i], 0x0100, 0, NULL, OPEN_EXISTING, 0, NULL);
    return h;
}

static int ufn_change(HANDLE h, const WCHAR *name)
{
    UFN_NAME n;
    memset(&n, 0, sizeof(n));
    wcsncpy(n.szName, name, 127);
    return DeviceIoControl(h, IOCTL_UFN_CHANGE_CURRENT_CLIENT, &n, sizeof(n), NULL, 0, NULL, NULL) ? 1 : 0;
}

#define UFN_KEY L"Drivers\\USB\\FunctionDrivers"

static int read_default_client(WCHAR *out, DWORD chars)
{
    HKEY key;
    DWORD len = chars * sizeof(WCHAR), type = 0;
    int ok = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UFN_KEY, 0, 0, &key) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExW(key, L"DefaultClientDriver", NULL, &type, (BYTE *)out, &len) == ERROR_SUCCESS &&
        type == REG_SZ && out[0]) {
        out[chars - 1] = 0;
        ok = 1;
    }
    RegCloseKey(key);
    return ok;
}

/* The only registry write of the agent: restoring a value it caused to change. */
static void write_default_client(const WCHAR *value)
{
    typedef LONG (WINAPI *RegFlushKey_t)(HKEY);
    RegFlushKey_t flush = (RegFlushKey_t)core_proc(L"RegFlushKey");
    WCHAR now[128];
    HKEY key;
    if (!read_default_client(now, 128) || !_wcsicmp(now, value))
        return;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UFN_KEY, 0, 0, &key) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, L"DefaultClientDriver", 0, REG_SZ, (const BYTE *)value,
                   (DWORD)((wcslen(value) + 1) * sizeof(WCHAR)));
    if (flush)
        flush(key);
    RegCloseKey(key);
    log_line("DefaultClientDriver %ls -> %ls (restored)", now, value);
}

static int ensure_serial_client(void)
{
    HANDLE h = ufn_open();
    UFN_INFO info;
    DWORD got = 0;
    int ok = 0;
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    memset(&info, 0, sizeof(info));
    if (DeviceIoControl(h, IOCTL_UFN_GET_CURRENT_CLIENT, NULL, 0, &info, sizeof(info), &got, NULL)) {
        if (!_wcsicmp(info.szName, L"Serial_Class")) {
            ok = 1;
        } else {
            WCHAR before[128];
            int had_default = read_default_client(before, 128);
            if (ufn_change(h, L"Serial_Class")) {
                if (!g_switched_usb) {
                    wcsncpy(g_prev_client, info.szName, 127);
                    g_switched_usb = 1;
                }
                log_line("usb client %ls -> Serial_Class", info.szName);
                /* On this device the change is also saved as the boot default
                 * (found 2026-11-19: it kept booting as a serial port). Put the
                 * saved default back so a reset always returns to USB storage. */
                if (had_default)
                    write_default_client(before);
                ok = 1;
            }
        }
    }
    CloseHandle(h);
    return ok;
}

static void restore_usb_client(void)
{
    HANDLE h;
    if (!g_switched_usb || !g_prev_client[0])
        return;
    h = ufn_open();
    if (h == INVALID_HANDLE_VALUE)
        return;
    log_line("restore usb client %ls: %d", g_prev_client, ufn_change(h, g_prev_client));
    CloseHandle(h);
}

static int find_usb_serial_port(WCHAR *out, size_t out_chars)
{
    HKEY active, entry;
    DWORD index;
    WCHAR name[64], key_path[MAX_PATH], dev[32];
    int found = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Drivers\\Active", 0, 0, &active) != ERROR_SUCCESS)
        return 0;
    for (index = 0; !found; index++) {
        DWORD name_len = 64, len, type;
        if (RegEnumKeyExW(active, index, name, &name_len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExW(active, name, 0, 0, &entry) != ERROR_SUCCESS)
            continue;
        len = sizeof(key_path);
        key_path[0] = 0;
        if (RegQueryValueExW(entry, L"Key", NULL, &type, (BYTE *)key_path, &len) == ERROR_SUCCESS) {
            WCHAR lower[MAX_PATH];
            size_t i;
            for (i = 0; key_path[i] && i < MAX_PATH - 1; i++)
                lower[i] = towlower(key_path[i]);
            lower[i] = 0;
            if (wcsstr(lower, L"serial_class") || wcsstr(lower, L"usbser") || wcsstr(lower, L"usbfnserial")) {
                len = sizeof(dev);
                if (RegQueryValueExW(entry, L"Name", NULL, &type, (BYTE *)dev, &len) == ERROR_SUCCESS && dev[0]) {
                    wcsncpy(out, dev, out_chars - 1);
                    out[out_chars - 1] = 0;
                    found = 1;
                }
            }
        }
        RegCloseKey(entry);
    }
    RegCloseKey(active);
    return found;
}

static HANDLE open_port(const WCHAR *name)
{
    HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    DCB dcb;
    COMMTIMEOUTS t;
    if (h == INVALID_HANDLE_VALUE)
        return h;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (GetCommState(h, &dcb)) {
        dcb.BaudRate = 115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fOutxCtsFlow = dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutX = dcb.fInX = FALSE;
        SetCommState(h, &dcb);
    }
    t.ReadIntervalTimeout = MAXDWORD;     /* return at once: the driver drops what it cannot buffer */
    t.ReadTotalTimeoutMultiplier = 0;
    t.ReadTotalTimeoutConstant = 0;
    t.WriteTotalTimeoutMultiplier = 0;
    t.WriteTotalTimeoutConstant = 10000;
    SetCommTimeouts(h, &t);
    SetupComm(h, 64 * 1024, 64 * 1024);
    return h;
}

/* ------------------------------------------------------------ comm thread */

static DWORD WINAPI comm_thread(LPVOID unused)
{
    uint8_t *rx = (uint8_t *)malloc(BFP_HEADER + BFP_MAX_REQUEST + 4096);
    uint8_t tmp[512];
    bfp_parser parser;
    (void)unused;
    if (!rx) {
        FAIL(L"Sem memória.");
        return 1;
    }
    if (p_CeSetThreadPriority)
        p_CeSetThreadPriority(GetCurrentThread(), 200);
    while (!g_quit) {
        DWORD idle = 0;
        if (!ensure_serial_client())
            log_line("usb function switching unavailable");
        if (!find_usb_serial_port(g_port_name, 16)) {
            STATUS(L"Aguardando a porta USB serial (cabo USB ao PC).");
            Sleep(1500);
            continue;
        }
        g_port = open_port(g_port_name);
        if (g_port == INVALID_HANDLE_VALUE) {
            WARN(L"Porta %ls ocupada (erro %lu).", g_port_name, GetLastError());
            Sleep(1500);
            continue;
        }
        {   /* "COM9:" -> "COM9" para a tela */
            int k;
            for (k = 0; k < 15 && g_port_name[k] && g_port_name[k] != L':'; k++)
                g_port_label[k] = g_port_name[k];
            g_port_label[k] = 0;
        }
        OK_(L"Pronto na porta %ls. No PC: tools/bigfoston/bfagent.py info", g_port_label);
        log_line("port %ls open", g_port_name);
        bfp_parser_init(&parser, rx, BFP_HEADER + BFP_MAX_REQUEST + 4096);
        while (!g_quit) {
            DWORD got = 0;
            bfp_msg m;
            if (!ReadFile(g_port, tmp, sizeof(tmp), &got, NULL)) {
                log_line("read failed: %lu", GetLastError());
                break;
            }
            if (!got) {
                /* never sleep longer than 1 ms: the Serial_Class driver loses
                 * data beyond a few KB while nobody reads (measured: 6 KB
                 * requests failed after the 5 ms idle sleep of 0.1.0) */
                Sleep(1);
                if (parser.used && ++idle > 500) {
                    log_line("stalled partial request (%u bytes): dropped", (unsigned)parser.used);
                    parser.used = 0;           /* the host times out and retries */
                    idle = 0;
                }
                continue;
            }
            idle = 0;
            {
                DWORD i = 0;
                while (i < got) {
                    size_t n = bfp_feed(&parser, tmp + i, got - i);
                    i += (DWORD)n;
                    while (bfp_next(&parser, &m)) {
                        handle(&m);
                        bfp_consume(&parser);
                    }
                    if (!n && parser.used == parser.cap)
                        parser.used = 0;   /* oversized request: drop it, the host times out */
                }
            }
        }
        EnterCriticalSection(&g_write_lock);
        CloseHandle(g_port);
        g_port = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&g_write_lock);
        Sleep(500);
    }
    free(rx);
    return 0;
}

/* ------------------------------------------------------------------ window */

/* ------------------------------------------------------- autoboot */

#define AUTOBOOT_DIR L"\\ResidentFlash\\BigFoston"
#define LINUX_DIR    AUTOBOOT_DIR L"\\linux"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* grava um u32 numa propriedade existente do DTB (in-place, sem mudar o tamanho) */
static int fdt_set_u32(uint8_t *fdt, DWORD n, const char *prop, uint32_t v)
{
    uint32_t off_struct, off_strings, i;
    if (n < 40 || be32(fdt) != 0xd00dfeed)
        return 0;
    off_struct = be32(fdt + 8);
    off_strings = be32(fdt + 12);
    for (i = off_struct; i + 4 <= n;) {
        uint32_t tok = be32(fdt + i);
        i += 4;
        if (tok == 1) {                                  /* BEGIN_NODE: nome + alinhamento */
            while (i < n && fdt[i]) i++;
            i = (i + 4) & ~3u;
        } else if (tok == 3) {                           /* PROP: len, nameoff, valor */
            uint32_t len = be32(fdt + i), nameoff = be32(fdt + i + 4);
            const char *name = (const char *)fdt + off_strings + nameoff;
            if (off_strings + nameoff < n && !strcmp(name, prop) && len == 4) {
                uint8_t *val = fdt + i + 8;
                val[0] = (uint8_t)(v >> 24); val[1] = (uint8_t)(v >> 16); val[2] = (uint8_t)(v >> 8); val[3] = (uint8_t)v;
                return 1;
            }
            i += 8 + ((len + 3) & ~3u);
        } else if (tok == 2 || tok == 4) {
            continue;
        } else
            break;                                       /* END ou inválido */
    }
    return 0;
}

static int file_exists(const WCHAR *p)
{
    return GetFileAttributesW(p) != 0xFFFFFFFF;
}

/* ------------------------------------------- update staged on the SD card
 *
 * The running Linux (fast USB) leaves a new version in \SDMMC\BigFoston\update
 * (zImage, initrd.img, fs707dc.dtb and SHA256SUMS; the directory is renamed to
 * "update" only after every file was checked there).  At power-on, before the
 * autoboot countdown, the agent checks each file on the card, copies it to
 * linux\<name>.new, checks the copy, keeps the current one as <name>.old and
 * only then swaps them.  The flash is written by Windows CE's own file system;
 * a bad update is renamed to update.bad and the installed Linux boots as before.
 * Result lines go to \SDMMC\BigFoston\update.log. */
#define SD_DIR     L"\\SDMMC\\BigFoston"
#define UPDATE_DIR SD_DIR L"\\update"
#define UPD_FILES  3
static const WCHAR *const upd_names[UPD_FILES] = {L"zImage", L"initrd.img", L"fs707dc.dtb"};
static const DWORD upd_max[UPD_FILES] = {8u << 20, 12u << 20, 64u << 10};

static void upd_log(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    SYSTEMTIME st;
    FILE *f;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = 0;
    log_line("update: %s", line);
    f = _wfopen(SD_DIR L"\\update.log", L"a");
    if (!f)
        return;
    GetLocalTime(&st);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u (horario do CE) %s\n", st.wYear, st.wMonth, st.wDay, st.wHour,
            st.wMinute, st.wSecond, line);
    fclose(f);
}

/* SHA-256 of a file; 0 when it cannot be read whole or is larger than max */
static int file_sha256(const WCHAR *path, DWORD max, char hex[65], DWORD *size)
{
    static uint8_t chunk[32768];
    sha256_ctx c;
    uint8_t d[32];
    DWORD n, got, total = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    n = GetFileSize(f, NULL);
    if (n == 0xFFFFFFFF || n == 0 || n > max) {
        CloseHandle(f);
        return 0;
    }
    sha256_init(&c);
    while (ReadFile(f, chunk, sizeof(chunk), &got, NULL) && got) {
        sha256_update(&c, chunk, got);
        total += got;
    }
    CloseHandle(f);
    if (total != n)
        return 0;
    sha256_final(&c, d);
    sha256_hex(d, hex);
    if (size)
        *size = n;
    return 1;
}

/* the DTB must be a flattened tree with the bigfoston,boot-epoch slot the autoboot fills */
static int dtb_has_epoch(const WCHAR *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n, got = 0;
    uint8_t *b;
    int ok = 0;
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    n = GetFileSize(f, NULL);
    if (n >= 40 && n <= (64u << 10) && (b = (uint8_t *)malloc(n)) != NULL) {
        if (ReadFile(f, b, n, &got, NULL) && got == n)
            ok = fdt_set_u32(b, n, "bigfoston,boot-epoch", 0);
        free(b);
    }
    CloseHandle(f);
    return ok;
}

static void remove_tree(const WCHAR *dir)
{
    WIN32_FIND_DATAW fd;
    WCHAR pat[MAX_PATH], p[MAX_PATH];
    HANDLE h;
    _snwprintf(pat, MAX_PATH - 1, L"%ls\\*", dir);
    pat[MAX_PATH - 1] = 0;
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            _snwprintf(p, MAX_PATH - 1, L"%ls\\%ls", dir, fd.cFileName);
            p[MAX_PATH - 1] = 0;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(p);
            else
                DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* move update -> update.ok / update.bad (replacing an older one) */
static void update_retire(const WCHAR *to)
{
    remove_tree(to);
    if (!MoveFileW(UPDATE_DIR, to))
        remove_tree(UPDATE_DIR);           /* never apply the same update twice */
}

static void update_reject(const char *why)
{
    upd_log("RECUSADA: %s; o Linux instalado continua", why);
    update_retire(UPDATE_DIR L".bad");
    FAIL(L"Atualização do cartão recusada (ver BigFoston\\update.log no cartão).");
    PostMessageW(g_hwnd, WM_APP_REDRAW, 0, 0);
    Sleep(3000);
}

/* expected hash of name from SHA256SUMS ("<64 hex>  <name>" lines) */
static int sums_lookup(const char *sums, const WCHAR *name, char hex[65])
{
    char nm[32];
    const char *line = sums;
    WideCharToMultiByte(CP_ACP, 0, name, -1, nm, sizeof(nm), NULL, NULL);
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        size_t nl = strlen(nm);
        if (len >= 66 + nl && line[64] == ' ' && line[65] == ' ' && !strncmp(line + 66, nm, nl) &&
            (len == 66 + nl || line[66 + nl] == '\r')) {
            int i;
            for (i = 0; i < 64; i++) {
                char ch = line[i];
                if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
                    return 0;
                hex[i] = ch;
            }
            hex[64] = 0;
            return 1;
        }
        line = end ? end + 1 : NULL;
    }
    return 0;
}

static void apply_sd_update(void)
{
    static char sums[1024];
    char want[UPD_FILES][65], have[65], why[160];
    WCHAR src[MAX_PATH], dst[MAX_PATH], old[MAX_PATH];
    DWORD size[UPD_FILES], got = 0;
    FILE *f;
    int i, wait;

    for (wait = 0; wait < 6 && !file_exists(SD_DIR); wait++)
        Sleep(500);                        /* the card may still be mounting at power-on */
    if (!file_exists(UPDATE_DIR L"\\SHA256SUMS"))
        return;
    STATUS(L"Atualização do BigFoston encontrada no cartão SD: conferindo...");
    PostMessageW(g_hwnd, WM_APP_REDRAW, 0, 0);

    f = _wfopen(UPDATE_DIR L"\\SHA256SUMS", L"rb");
    if (f) {
        got = (DWORD)fread(sums, 1, sizeof(sums) - 1, f);
        fclose(f);
    }
    sums[got] = 0;
    for (i = 0; i < UPD_FILES; i++) {
        if (!sums_lookup(sums, upd_names[i], want[i])) {
            _snprintf(why, sizeof(why) - 1, "SHA256SUMS sem %ls", upd_names[i]);
            why[sizeof(why) - 1] = 0;
            update_reject(why);
            return;
        }
        _snwprintf(src, MAX_PATH - 1, L"%ls\\%ls", UPDATE_DIR, upd_names[i]);
        src[MAX_PATH - 1] = 0;
        if (!file_sha256(src, upd_max[i], have, &size[i]) || strcmp(have, want[i])) {
            _snprintf(why, sizeof(why) - 1, "%ls ausente, grande demais ou com SHA-256 diferente no cartao",
                      upd_names[i]);
            why[sizeof(why) - 1] = 0;
            update_reject(why);
            return;
        }
    }
    if (!dtb_has_epoch(UPDATE_DIR L"\\fs707dc.dtb")) {
        update_reject("fs707dc.dtb sem /chosen/bigfoston,boot-epoch");
        return;
    }

    /* Install one file at a time (agent 0.6.2). The internal flash (\ResidentFlash, 12 MB)
     * cannot hold two full versions side by side (0.3.0 with Bluetooth: 5.8 MB), so:
     *  - leftovers (<name>.old from agents < 0.6.2, <name>.new) are deleted first;
     *  - the installed SHA256SUMS is deleted before touching anything: autoboot needs it, so a
     *    half-installed set is never booted;
     *  - each file: skip it if it already is the new one (resuming), else copy to <name>.new,
     *    check, replace; if there is no room for the .new, delete the installed file and copy
     *    straight over it, checking again;
     *  - SHA256SUMS is written last. On any failure the update stays on the card (it is not
     *    retired) and the next boot carries on; the previous version is kept on the card
     *    (BigFoston/recuperacao), not in the internal flash. */
    CreateDirectoryW(AUTOBOOT_DIR, NULL);
    CreateDirectoryW(LINUX_DIR, NULL);
    for (i = 0; i < UPD_FILES; i++) {
        _snwprintf(old, MAX_PATH - 1, L"%ls\\%ls.old", LINUX_DIR, upd_names[i]);
        old[MAX_PATH - 1] = 0;
        DeleteFileW(old);
        _snwprintf(old, MAX_PATH - 1, L"%ls\\%ls.new", LINUX_DIR, upd_names[i]);
        old[MAX_PATH - 1] = 0;
        DeleteFileW(old);
    }
    DeleteFileW(LINUX_DIR L"\\SHA256SUMS");
    for (i = 0; i < UPD_FILES; i++) {
        WCHAR tmp[MAX_PATH];
        DWORD err;
        STATUS(L"Gravando %ls na memória interna...", upd_names[i]);
        PostMessageW(g_hwnd, WM_APP_REDRAW, 0, 0);
        _snwprintf(src, MAX_PATH - 1, L"%ls\\%ls", UPDATE_DIR, upd_names[i]);
        _snwprintf(dst, MAX_PATH - 1, L"%ls\\%ls", LINUX_DIR, upd_names[i]);
        _snwprintf(tmp, MAX_PATH - 1, L"%ls\\%ls.new", LINUX_DIR, upd_names[i]);
        src[MAX_PATH - 1] = dst[MAX_PATH - 1] = tmp[MAX_PATH - 1] = 0;
        if (file_sha256(dst, upd_max[i], have, NULL) && !strcmp(have, want[i]))
            continue;                               /* already the new one (resumed update) */
        if (CopyFileW(src, tmp, FALSE) && file_sha256(tmp, upd_max[i], have, NULL) && !strcmp(have, want[i])) {
            DeleteFileW(dst);
            if (MoveFileW(tmp, dst))
                continue;
        }
        DeleteFileW(tmp);                           /* no room for the copy: replace in place */
        DeleteFileW(dst);
        if (CopyFileW(src, dst, FALSE) && file_sha256(dst, upd_max[i], have, NULL) && !strcmp(have, want[i]))
            continue;
        err = GetLastError();
        upd_log("INCOMPLETA: %ls nao foi gravado na memoria interna (erro %lu); a atualizacao fica no "
                "cartao e continua no proximo boot", upd_names[i], (unsigned long)err);
        FAIL(L"Atualização incompleta: desligue e ligue de novo (ver BigFoston\\update.log).");
        PostMessageW(g_hwnd, WM_APP_REDRAW, 0, 0);
        Sleep(3000);
        return;
    }
    f = _wfopen(LINUX_DIR L"\\SHA256SUMS", L"wb");
    if (!f) {
        upd_log("INCOMPLETA: SHA256SUMS nao foi gravado (erro %lu); a atualizacao continua no proximo boot",
                (unsigned long)GetLastError());
        return;
    }
    for (i = 0; i < UPD_FILES; i++)
        fprintf(f, "%s  %ls\n", want[i], upd_names[i]);
    fclose(f);
    upd_log("APLICADA: zImage %lu, initrd.img %lu, fs707dc.dtb %lu bytes; zImage %.16s",
            (unsigned long)size[0], (unsigned long)size[1], (unsigned long)size[2], want[0]);
    update_retire(UPDATE_DIR L".ok");
    OK_(L"Atualização do BigFoston instalada e conferida.");
    PostMessageW(g_hwnd, WM_APP_REDRAW, 0, 0);
    Sleep(1500);
}

static void do_autoboot(void)
{
    static bf_boot_args args;
    char err[240];
    HANDLE f;
    DWORD n = 0, got = 0;
    uint8_t *dtb;
    memset(&args, 0, sizeof(args));
    _snwprintf(args.kernel, MAX_PATH - 1, L"%ls", LINUX_DIR L"\\zImage");
    _snwprintf(args.initrd, MAX_PATH - 1, L"%ls", LINUX_DIR L"\\initrd.img");
    wcscpy(args.dtb, L"mem:1");
    args.kernel_pa = 0x00800000;
    args.dtb_pa = 0x01F00000;
    args.initrd_pa = 0x01000000;
    args.machine = 0xFFFFFFFF;
    args.kernel_max = 8u << 20;
    args.dtb_max = 64u << 10;
    args.initrd_max = 12u << 20;
    /* o DTB vai para a memória e recebe a hora do relógio do CE (não há driver de RTC no Linux) */
    f = CreateFileW(LINUX_DIR L"\\fs707dc.dtb", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        n = GetFileSize(f, NULL);
        if (n > 0 && n <= args.dtb_max && (dtb = (uint8_t *)malloc(n)) != NULL) {
            if (ReadFile(f, dtb, n, &got, NULL) && got == n) {
                uint32_t now = ce_epoch();
                if (!now)
                    log_line("autoboot: relogio do CE nao acertado; o Linux fica sem hora (use bfagent.py settime)");
                else if (!fdt_set_u32(dtb, n, "bigfoston,boot-epoch", now + 3))
                    log_line("autoboot: DTB sem bigfoston,boot-epoch");
                free(bf_stage_data[1]);
                bf_stage_data[1] = dtb;
                bf_stage_len[1] = n;
            } else
                free(dtb);
        }
        CloseHandle(f);
    }
    if (!bf_stage_data[1]) {
        FAIL(L"Início: fs707dc.dtb ausente ou ilegível.");
        InterlockedExchange(&g_autoboot, 2);
        return;
    }
    STATUS(L"Iniciando o BigFoston (Linux)... (o reset volta ao Windows CE)");
    PostMessageW(g_hwnd, WM_APP_REDRAW, 0, 0);
    Sleep(300);
    log_line("autoboot: %ls", args.kernel);
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
    if (!bf_boot(&args, err, sizeof(err))) {
        WCHAR w[240];
        MultiByteToWideChar(CP_UTF8, 0, err, -1, w, 239);
        w[239] = 0;
        FAIL(L"O início do BigFoston falhou: %ls", w);
        InterlockedExchange(&g_autoboot, 2);
        PostMessageW(g_hwnd, WM_APP_REDRAW, 0, 0);
    }
}

/* installed Linux complete: the three files and the SHA256SUMS written last by an update */
static int linux_installed(void)
{
    return file_exists(LINUX_DIR L"\\zImage") && file_exists(LINUX_DIR L"\\initrd.img") &&
           file_exists(LINUX_DIR L"\\fs707dc.dtb") && file_exists(LINUX_DIR L"\\SHA256SUMS");
}

static DWORD WINAPI autoboot_thread(LPVOID arg)
{
    int left = (int)(intptr_t)arg;
    DWORD total;
    apply_sd_update();
    InterlockedExchange(&g_linux_ok, linux_installed());
    if (!g_linux_ok) {
        InterlockedExchange(&g_autoboot, 2);
        FAIL(L"Linux da memória interna incompleto: ficando no Windows CE. Desligue e ligue para continuar "
             L"a atualização do cartão, ou copie BigFoston\\recuperacao\\linux-0.2.0 para BigFoston\\update.");
        return 0;
    }
    /* a contagem anda em milissegundos: a barra de progresso avança suave (WM_APP_COUNT liga o
     * temporizador da tela); um toque, o PC ou "Iniciar agora" mudam g_autoboot e encerram */
    total = (DWORD)left * 1000;
    InterlockedExchange(&g_cd_total, (LONG)total);
    InterlockedExchange(&g_cd_start, (LONG)GetTickCount());
    STATUS(L"Início automático do BigFoston em %d s.", left);
    PostMessageW(g_hwnd, WM_APP_COUNT, 1, 0);
    while (g_autoboot == 1 && GetTickCount() - (DWORD)g_cd_start < total)
        Sleep(50);
    PostMessageW(g_hwnd, WM_APP_COUNT, 0, 0);
    if (g_autoboot == 2) {
        WARN(L"Início automático cancelado: ficando no Windows CE.");
        return 0;
    }
    InterlockedExchange(&g_autoboot, 3);
    do_autoboot();
    return 0;
}

/* "Iniciar o BigFoston" tocado fora da contagem (cancelada, ou o agente aberto à mão) */
static DWORD WINAPI manual_boot_thread(LPVOID unused)
{
    do_autoboot();
    return 0;
}

static void start_linux_now(void)
{
    HANDLE t;
    if (!g_linux_ok || g_autoboot == 3)
        return;
    if (g_autoboot == 1) {                 /* a contagem segue para o início na hora */
        InterlockedExchange(&g_autoboot, 3);
        return;
    }
    InterlockedExchange(&g_autoboot, 3);
    t = CreateThread(NULL, 0, manual_boot_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else {
        InterlockedExchange(&g_autoboot, 2);
        FAIL(L"Não foi possível iniciar o BigFoston (sem memória).");
    }
}

/* Só no início do aparelho (agente aberto pelo Launch90 a partir da memória
 * interna), com autoboot.conf ("delay=N") e o Linux instalado. */
static void autoboot_start(void)
{
    FILE *f;
    int delay = 5;
    HANDLE t;
    if (_wcsnicmp(g_origin_dir, AUTOBOOT_DIR, wcslen(AUTOBOOT_DIR)) || GetTickCount() > 180000)
        return;
    /* an update on the card is applied even when the installed set is incomplete (it may be
     * an interrupted update, which carries on from where it stopped) */
    if (!linux_installed() && !file_exists(UPDATE_DIR L"\\SHA256SUMS"))
        return;
    f = _wfopen(AUTOBOOT_DIR L"\\autoboot.conf", L"r");
    if (!f)
        return;
    if (fscanf(f, "delay=%d", &delay) != 1)
        delay = 5;
    fclose(f);
    if (delay < 1) delay = 1;
    if (delay > 30) delay = 30;
    log_line("autoboot em %d s", delay);
    InterlockedExchange(&g_autoboot, 1);
    t = CreateThread(NULL, 0, autoboot_thread, (LPVOID)(intptr_t)delay, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        InterlockedExchange(&g_autoboot, 0);
}

static void paint(HDC dc, const RECT *clip)
{
    ui_state_t st;
    ui_line_t lines[UI_LINES];
    DWORD now = GetTickCount();
    memset(&st, 0, sizeof(st));
    EnterCriticalSection(&g_ui_lock);
    memcpy(lines, g_lines, sizeof(lines));
    st.nlines = g_nlines;
    LeaveCriticalSection(&g_ui_lock);
    st.lines = lines;
    st.autoboot = (int)g_autoboot;
    st.linux_ok = (int)g_linux_ok;
    st.cd_total_ms = (DWORD)g_cd_total;
    st.cd_elapsed_ms = now - (DWORD)g_cd_start;
    if (st.cd_elapsed_ms > st.cd_total_ms)
        st.cd_elapsed_ms = st.cd_total_ms;
    st.requests = g_requests;
    st.pressed = g_pressed;
    st.version = AGENT_VERSION_W;
    st.port = g_port_label;
    if (g_ui_ok) {
        ui_paint(dc, &st, clip);
        return;
    }
    {   /* sem memória para a tela própria: o mínimo, para não ficar preta */
        RECT r;
        HBRUSH bg = CreateSolidBrush(RGB(0x13, 0x20, 0x4a));
        SetRect(&r, 0, 0, g_width, g_height);
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0xee, 0xf1, 0xf6));
        SetRect(&r, 24, 24, g_width - 24, g_height - 24);
        DrawTextW(dc, st.nlines ? lines[st.nlines - 1].text : AGENT_VERSION_W, -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
}

/* toque solto em cima de um cartão: o que ele faz depende da contagem */
static void card_action(HWND hwnd, int hit)
{
    if (hit == UI_HIT_LINUX)
        start_linux_now();
    else if (hit == UI_HIT_CE) {
        if (g_autoboot == 1)
            InterlockedExchange(&g_autoboot, 2);          /* ficar no Windows CE */
        else if (g_autoboot != 3)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);           /* voltar ao Windows CE: fecha o agente */
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc, &ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;                              /* a tela inteira é desenhada de uma vez: sem piscar */
    case WM_APP_REDRAW:
        /* junta os pedidos de redesenho de REDRAW_MS em um (uma cópia grande gera milhares) */
        SetTimer(hwnd, TIMER_REDRAW, REDRAW_MS, NULL);
        return 0;
    case WM_APP_COUNT:
        if (wp)
            SetTimer(hwnd, TIMER_BAR, 100, NULL);
        else
            KillTimer(hwnd, TIMER_BAR);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        if (g_ui_ok) {
            int hit = ui_hit((short)LOWORD(lp), (short)HIWORD(lp));
            /* só destaca o cartão que faz algo agora */
            if ((hit == UI_HIT_LINUX && (!g_linux_ok || g_autoboot == 3)) || (hit == UI_HIT_CE && g_autoboot == 3))
                hit = UI_HIT_NONE;
            g_pressed = hit;
            if (g_pressed)
                InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        /* o dedo saiu do cartão: solta o destaque (e soltar ali não faz nada) */
        if (g_pressed && ui_hit((short)LOWORD(lp), (short)HIWORD(lp)) != g_pressed) {
            g_pressed = UI_HIT_NONE;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        int was = g_pressed;
        if (!g_ui_ok) {                        /* tela mínima: um toque fica no CE (cancela ou fecha) */
            card_action(hwnd, UI_HIT_CE);
            return 0;
        }
        g_pressed = UI_HIT_NONE;
        if (was && ui_hit((short)LOWORD(lp), (short)HIWORD(lp)) == was)
            card_action(hwnd, was);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_REDRAW) {
            KillTimer(hwnd, TIMER_REDRAW);
            InterlockedExchange(&g_redraw_pending, 0);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (wp == TIMER_BAR) {
            RECT r;
            ui_progress_rect(&r);
            InvalidateRect(hwnd, &r, FALSE);
            return 0;
        }
        if (p_idle_reset)
            p_idle_reset();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int relocate(const WCHAR *self)
{
    WCHAR args[MAX_PATH + 16];
    PROCESS_INFORMATION pi;
    if (!_wcsnicmp(self, L"\\Temp\\", 6))
        return 0;
    CreateDirectoryW(L"\\Temp", NULL);
    {
        /* after a remote restart the previous instance may still hold the copy for a moment */
        int tries = 0;
        while (!CopyFileW(self, L"\\Temp\\bfagent.exe", FALSE)) {
            if (++tries == 20)
                return 0;
            Sleep(250);
        }
    }
    _snwprintf(args, MAX_PATH + 15, L"/from \"%ls\"", g_origin_dir);
    args[MAX_PATH + 15] = 0;
    if (!CreateProcessW(L"\\Temp\\bfagent.exe", args, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi))
        return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    WCHAR self[MAX_PATH], *slash, *from;
    WNDCLASSW wc;
    MSG msg;
    HWND taskbar, other;
    HANDLE thread;
    (void)prev;
    (void)show;

    other = FindWindowW(AGENT_CLASS, NULL);
    if (other && cmd && wcsstr(cmd, L"/wait")) {
        /* started by the previous agent for an update: let it quit first */
        int i;
        for (i = 0; i < 100 && (other = FindWindowW(AGENT_CLASS, NULL)) != NULL; i++)
            Sleep(100);
    }
    if (other) {
        SetForegroundWindow(other);
        return 0;
    }
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wcscpy(g_origin_dir, self);
    slash = wcsrchr(g_origin_dir, L'\\');
    if (slash)
        *slash = 0;
    from = cmd ? wcsstr(cmd, L"/from \"") : NULL;
    if (from) {
        WCHAR *end;
        wcsncpy(g_origin_dir, from + 7, MAX_PATH - 1);
        g_origin_dir[MAX_PATH - 1] = 0;
        end = wcschr(g_origin_dir, L'"');
        if (end)
            *end = 0;
    } else if (!(cmd && wcsstr(cmd, L"/norelocate")) && relocate(self)) {
        return 0;
    }
    InitializeCriticalSection(&g_ui_lock);
    g_log = _wfopen(L"\\Temp\\bfagent.log", L"w");
    log_line("%s starting from %ls (origin %ls)", AGENT_VERSION, self, g_origin_dir);
    InitializeCriticalSection(&g_write_lock);
    resolve_kernel_calls();
    if (p_SetProcPermissions)
        p_SetProcPermissions(0xFFFFFFFF);     /* see every process slot (needed by VirtualCopy on CE 5) */
    g_width = GetSystemMetrics(SM_CXSCREEN);
    g_height = GetSystemMetrics(SM_CYSCREEN);
    {
        HDC screen = GetDC(NULL);
        g_ui_ok = ui_init(screen, g_width, g_height);
        ReleaseDC(NULL, screen);
        if (!g_ui_ok)
            log_line("tela: sem memoria para os buffers; usando a tela minima");
    }
    InterlockedExchange(&g_linux_ok, linux_installed());
    STATUS(L"%ls iniciado (%ls).", AGENT_VERSION_W, g_origin_dir);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = AGENT_CLASS;
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST, AGENT_CLASS, L"BigFoston Dual Boot", WS_VISIBLE | WS_POPUP, 0, 0, g_width,
                             g_height, NULL, NULL, inst, NULL);
    if (!g_hwnd)
        return 1;
    taskbar = FindWindowW(L"HHTaskBar", NULL);
    if (taskbar)
        ShowWindow(taskbar, SW_HIDE);
    SetForegroundWindow(g_hwnd);
    SetTimer(g_hwnd, TIMER_IDLE, 5000, NULL);

    thread = CreateThread(NULL, 0, comm_thread, NULL, 0, NULL);
    autoboot_start();
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    InterlockedExchange(&g_quit, 1);
    EnterCriticalSection(&g_write_lock);
    if (g_port != INVALID_HANDLE_VALUE) {
        CloseHandle(g_port);
        g_port = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_write_lock);
    if (thread) {
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
    }
    if (g_restart) {
        /* the new instance keeps the serial client: no USB re-enumeration */
        WCHAR exe[MAX_PATH];
        PROCESS_INFORMATION pi;
        _snwprintf(exe, MAX_PATH - 1, L"%ls\\bfagent.exe", g_origin_dir);
        exe[MAX_PATH - 1] = 0;
        log_line("restarting from %ls", exe);
        if (g_log)
            fclose(g_log);
        if (CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 0;
        }
        g_log = NULL;
    }
    ui_free();
    restore_usb_client();
    if (taskbar)
        ShowWindow(taskbar, SW_SHOW);
    log_line("bye");
    if (g_log)
        fclose(g_log);
    return 0;
}
