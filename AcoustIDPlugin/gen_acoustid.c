/*
 * gen_acoustid.c
 * Winamp General Purpose Plugin - AcoustID AutoTag & Rename (estilo Shazan)
 *
 * Detecta la cancion via fingerprint acustico (fpcalc/Chromaprint + AcoustID API)
 * y renombra el archivo al formato correcto: "Artista - Titulo".
 *
 * Compilar: abrir AcoustIDPlugin.sln en Visual Studio, Release|x86, Ctrl+Shift+B.
 *
 * Requiere fpcalc.exe (Chromaprint, MIT) en <Winamp>\Plugins\
 *   Descarga: https://acoustid.org/chromaprint
 * API key gratis: https://acoustid.org/api-key
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <wininet.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "gdi32.lib")

#define GPPHDR_VER 0x10

#define IPC_GETLISTPOS       125
#define IPC_GETPLAYLISTFILE  211
#define IPC_ISPLAYING        104
#define IPC_REFRESHPLCACHE   247
#define IPC_GETOUTPUTTIME    105
#define IPC_SETPLAYLISTPOS   121
#define IPC_UPDTITLE         281   /* fuerza regenerar titulo -> dispara toaster */
#define IPC_GETPLAYLISTTITLE 212

/* Comandos WM_COMMAND de Winamp */
#define WINAMP_PLAY          40045
#define WINAMP_PAUSE         40046
#define WINAMP_STOP          40047
#define WINAMP_JUMPTOTIME    40048

typedef struct {
    int version;
    char *description;
    int (*init)();
    void (*config)();
    void (*quit)();
    HWND hwndParent;
    HINSTANCE hDllInstance;
} winampGeneralPurposePlugin;

static int  plugin_init(void);
static void plugin_config(void);
static void plugin_quit(void);

static winampGeneralPurposePlugin plugin = {
    GPPHDR_VER,
    "AcoustID AutoTag v1.0",
    plugin_init,
    plugin_config,
    plugin_quit,
    0,
    0,
};

/* ---------- Config (en winamp.ini) ---------- */
static char g_api_key[64]  = "YOUR_ACOUSTID_KEY";
static char g_tpl[128]     = "{artist} - {title}";
static int  g_auto         = 1;
static int  g_threshold    = 70;
static int  g_notify       = 1;  /* mostrar notificaciones toast */
static char g_ini[MAX_PATH] = {0};

/* ---------- Estado ---------- */
static UINT_PTR g_timer = 0;
static int  g_last_pos  = -1;
static char g_last_file[MAX_PATH] = {0};
static volatile LONG g_busy = 0;

/* Tags pendientes (cuando el archivo estaba en uso y no se pudo escribir) */
static CRITICAL_SECTION g_pending_cs;
static int  g_has_pending = 0;
static char    g_pending_file[MAX_PATH] = {0};
static wchar_t g_pending_artist[256] = {0};
static wchar_t g_pending_title[256]  = {0};
static wchar_t g_pending_album[256]  = {0};

/* ---------- HTTPS GET via WinINet ---------- */
static int https_get(const char* host, const char* path, char* out, int out_sz) {
    HINTERNET hi, hc, hr;
    DWORD rd;
    int total = 0;
    out[0] = 0;

    hi = InternetOpenA("gen_acoustid/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hi) return 0;
    hc = InternetConnectA(hi, host, INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hc) { InternetCloseHandle(hi); return 0; }
    {
        const char* accept[] = { "application/json", NULL };
        hr = HttpOpenRequestA(hc, "GET", path, NULL, NULL, accept,
            INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    }
    if (!hr) { InternetCloseHandle(hc); InternetCloseHandle(hi); return 0; }

    if (HttpSendRequestA(hr, NULL, 0, NULL, 0)) {
        while (total < out_sz - 1 &&
               InternetReadFile(hr, out + total, out_sz - 1 - total, &rd) && rd > 0) {
            total += rd;
        }
        out[total] = 0;
    }
    InternetCloseHandle(hr);
    InternetCloseHandle(hc);
    InternetCloseHandle(hi);
    return total;
}

/* ---------- Extraccion minimal de JSON ---------- */
/* Busca "key":"valor" y copia valor a dst */
static int json_str(const char* json, const char* key, char* dst, int dst_sz) {
    char needle[64];
    const char *p, *end;
    int len;
    dst[0] = 0;

    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\":\"", key);
    p = strstr(json, needle);
    if (!p) {
        _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\": \"", key);
        p = strstr(json, needle);
    }
    if (!p) return 0;
    p += strlen(needle);
    end = strchr(p, '"');
    if (!end) return 0;
    len = (int)(end - p);
    if (len >= dst_sz) len = dst_sz - 1;
    memcpy(dst, p, len);
    dst[len] = 0;
    return 1;
}

/* Extrae el valor de "key":"..." como UTF-16 (wchar_t), decodificando
   secuencias \uXXXX y escapes JSON. Maneja correctamente japones, acentos,
   etc. Retorna numero de wchars escritos (sin contar el nul). */
static int json_wstr(const char* json, const char* key, wchar_t* dst, int dst_cap) {
    char needle[64];
    const char *p, *end;
    int n = 0;
    dst[0] = 0;

    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\":\"", key);
    p = strstr(json, needle);
    if (!p) {
        _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\": \"", key);
        p = strstr(json, needle);
    }
    if (!p) return 0;
    p += strlen(needle);

    /* Encontrar fin del string respetando escapes */
    end = p;
    while (*end) {
        if (*end == '\\' && end[1]) { end += 2; continue; }
        if (*end == '"') break;
        end++;
    }

    while (p < end && n < dst_cap - 1) {
        if (*p == '\\' && p + 1 < end) {
            char c = p[1];
            if (c == 'u' && p + 5 < end + 1) {
                /* \uXXXX */
                int v = 0, i;
                for (i = 0; i < 4; i++) {
                    char h = p[2 + i];
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (h - 'A' + 10);
                }
                dst[n++] = (wchar_t)v;
                p += 6;
            } else {
                /* escapes simples */
                switch (c) {
                    case 'n': dst[n++] = L'\n'; break;
                    case 't': dst[n++] = L'\t'; break;
                    case 'r': dst[n++] = L'\r'; break;
                    case '/': dst[n++] = L'/';  break;
                    case '\\':dst[n++] = L'\\'; break;
                    case '"': dst[n++] = L'"';  break;
                    default:  dst[n++] = (wchar_t)(unsigned char)c; break;
                }
                p += 2;
            }
        } else {
            /* byte UTF-8 -> convertir a wchar_t */
            unsigned char b = (unsigned char)*p;
            if (b < 0x80) { dst[n++] = (wchar_t)b; p++; }
            else if ((b & 0xE0) == 0xC0 && p + 1 < end) {
                wchar_t w = ((b & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
                dst[n++] = w; p += 2;
            } else if ((b & 0xF0) == 0xE0 && p + 2 < end) {
                wchar_t w = ((b & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) | ((unsigned char)p[2] & 0x3F);
                dst[n++] = w; p += 3;
            } else {
                dst[n++] = L'?'; p++;
            }
        }
    }
    dst[n] = 0;
    return n;
}

/* Busca "key":numero y retorna double */
static double json_num(const char* json, const char* key) {
    char needle[64];
    const char* p;
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\":", key);
    p = strstr(json, needle);
    if (!p) return 0.0;
    p += strlen(needle);
    while (*p == ' ') p++;
    return atof(p);
}

/* ---------- URL encode ---------- */
static void url_encode(const char* s, char* dst, int dst_sz) {
    static const char hex[] = "0123456789ABCDEF";
    int i = 0;
    unsigned char c;
    while ((c = (unsigned char)*s++) && i < dst_sz - 4) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[i++] = c;
        } else {
            dst[i++] = '%';
            dst[i++] = hex[c >> 4];
            dst[i++] = hex[c & 15];
        }
    }
    dst[i] = 0;
}

/* ---------- Ejecuta fpcalc.exe y captura stdout ---------- */
static int run_fpcalc(const char* filepath, char* out, int out_sz) {
    char dir[MAX_PATH], exe[MAX_PATH], cmd[MAX_PATH * 3];
    SECURITY_ATTRIBUTES sa;
    HANDLE hRead = NULL, hWrite = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD rd;
    int total = 0;
    BOOL ok;

    out[0] = 0;

    GetModuleFileNameA(plugin.hDllInstance, dir, MAX_PATH);
    PathRemoveFileSpecA(dir);
    _snprintf_s(exe, sizeof(exe), _TRUNCATE, "%s\\fpcalc.exe", dir);
    if (!PathFileExistsA(exe)) strcpy_s(exe, sizeof(exe), "fpcalc.exe");

    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "\"%s\" -json \"%s\"", exe, filepath);

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return 0;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(hWrite);
    if (!ok) { CloseHandle(hRead); return 0; }

    while (total < out_sz - 1 &&
           ReadFile(hRead, out + total, out_sz - 1 - total, &rd, NULL) && rd > 0) {
        total += rd;
    }
    out[total] = 0;

    WaitForSingleObject(pi.hProcess, 20000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return total;
}

/* ---------- Aplica template y sanitiza nombre ---------- */
static void build_filename(const char* artist, const char* title, const char* album,
                           char* dst, int dst_sz) {
    char tmp[256];
    char *w = dst;
    const char *r = g_tpl;
    int rem = dst_sz - 1;

    tmp[0] = 0;
    while (*r && rem > 0) {
        if (strncmp(r, "{artist}", 8) == 0) {
            int l = (int)strlen(artist); if (l > rem) l = rem;
            memcpy(w, artist, l); w += l; rem -= l; r += 8;
        } else if (strncmp(r, "{title}", 7) == 0) {
            int l = (int)strlen(title); if (l > rem) l = rem;
            memcpy(w, title, l); w += l; rem -= l; r += 7;
        } else if (strncmp(r, "{album}", 7) == 0) {
            const char* a = (album && album[0]) ? album : "Unknown Album";
            int l = (int)strlen(a); if (l > rem) l = rem;
            memcpy(w, a, l); w += l; rem -= l; r += 7;
        } else {
            *w++ = *r++; rem--;
        }
    }
    *w = 0;

    /* Sanitizar caracteres ilegales en NTFS */
    for (w = dst; *w; w++) {
        if (*w == '/' || *w == '\\' || *w == ':' || *w == '*' ||
            *w == '?' || *w == '"' || *w == '<' || *w == '>' || *w == '|')
            *w = '_';
    }
}

/* ---------- Disparar el toaster NATIVO de Winamp ---------- */
/* Tras actualizar el tag, fuerza a Winamp a re-leer el titulo de la pista
   actual. En Modern skin / Notification Area Control esto dispara el
   toaster nativo mostrando el nuevo "Artista - Titulo". */
static void winamp_refresh_and_toast(const char* filepath) {
    int cur, playing;
    char* curf;
    if (!plugin.hwndParent) return;

    /* refrescar cache de titulos de la playlist */
    SendMessageA(plugin.hwndParent, WM_USER, 0, IPC_REFRESHPLCACHE);

    cur  = (int)SendMessageA(plugin.hwndParent, WM_USER, 0, IPC_GETLISTPOS);
    curf = (char*)SendMessageA(plugin.hwndParent, WM_USER, cur, IPC_GETPLAYLISTFILE);
    playing = (int)SendMessageA(plugin.hwndParent, WM_USER, 0, IPC_ISPLAYING);

    /* Si es la pista que esta sonando, forzar regeneracion de titulo.
       IPC_UPDTITLE hace que Winamp relea metadata y notifique al toaster. */
    if (curf && _stricmp(curf, filepath) == 0 && playing == 1) {
        SendMessageA(plugin.hwndParent, WM_USER, 0, IPC_UPDTITLE);
    }
}

/* ---------- Notificacion TOAST (esquina inferior derecha, auto-desvanece) ---------- */
typedef struct { char title[64]; char msg[512]; } NotifyArg;

#define TOAST_W      340
#define TOAST_H      130
#define TOAST_MARGIN 16
#define TOAST_SHOWMS 4000   /* tiempo visible antes de desvanecer */

static const char* g_toast_title = NULL;
static const char* g_toast_msg   = NULL;

static LRESULT CALLBACK toast_wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hw, &ps);
        RECT rc; GetClientRect(hw, &rc);

        /* Fondo oscuro tipo tarjeta */
        HBRUSH bg = CreateSolidBrush(RGB(28, 30, 36));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        /* Barra de acento izquierda (verde/azul) */
        RECT bar = rc; bar.right = 5;
        HBRUSH accent = CreateSolidBrush(RGB(64, 196, 140));
        FillRect(hdc, &bar, accent);
        DeleteObject(accent);

        SetBkMode(hdc, TRANSPARENT);

        /* Titulo */
        {
            HFONT hf = CreateFontA(20, 0, 0, 0, FW_BOLD, 0, 0, 0,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            HFONT old = (HFONT)SelectObject(hdc, hf);
            RECT tr = rc; tr.left = 18; tr.top = 12; tr.right -= 12; tr.bottom = 38;
            SetTextColor(hdc, RGB(120, 220, 170));
            DrawTextA(hdc, ((NotifyArg*)GetWindowLongPtrA(hw, GWLP_USERDATA))->title,
                      -1, &tr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(hdc, old);
            DeleteObject(hf);
        }
        /* Mensaje */
        {
            HFONT hf = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            HFONT old = (HFONT)SelectObject(hdc, hf);
            RECT mr = rc; mr.left = 18; mr.top = 40; mr.right -= 14; mr.bottom -= 10;
            SetTextColor(hdc, RGB(225, 228, 235));
            DrawTextA(hdc, ((NotifyArg*)GetWindowLongPtrA(hw, GWLP_USERDATA))->msg,
                      -1, &mr, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
            SelectObject(hdc, old);
            DeleteObject(hf);
        }
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_LBUTTONUP:        /* clic = cerrar */
        DestroyWindow(hw);
        return 0;
    case WM_TIMER:
        if (wp == 1) {        /* iniciar fade-out */
            KillTimer(hw, 1);
            SetTimer(hw, 2, 30, NULL);
        } else if (wp == 2) { /* fade out */
            BYTE a;
            COLORREF ck; DWORD fl;
            GetLayeredWindowAttributes(hw, &ck, &a, &fl);
            if (a <= 18) { DestroyWindow(hw); }
            else SetLayeredWindowAttributes(hw, 0, (BYTE)(a - 18), LWA_ALPHA);
        }
        return 0;
    case WM_DESTROY: {
        NotifyArg* a = (NotifyArg*)GetWindowLongPtrA(hw, GWLP_USERDATA);
        if (a) HeapFree(GetProcessHeap(), 0, a);
        return 0;
    }
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

static DWORD WINAPI toast_thread(LPVOID p) {
    NotifyArg* a = (NotifyArg*)p;
    WNDCLASSA wc;
    HWND hw;
    MSG m;
    RECT wa;
    int x, y, lines;
    int h = TOAST_H;
    static int registered = 0;

    if (!registered) {
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = toast_wndproc;
        wc.hInstance     = plugin.hDllInstance;
        wc.hCursor       = LoadCursor(NULL, IDC_HAND);
        wc.lpszClassName = "AcoustIDToast";
        RegisterClassA(&wc);
        registered = 1;
    }

    /* Altura dinamica segun longitud del mensaje */
    lines = 1; { const char* s = a->msg; int col = 0; while (*s) { if (*s=='\n'||col>42){lines++;col=0;} else col++; s++; } }
    h = 50 + lines * 18; if (h < TOAST_H) h = TOAST_H; if (h > 260) h = 260;

    /* Posicion: esquina inferior derecha del area de trabajo */
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    x = wa.right  - TOAST_W - TOAST_MARGIN;
    y = wa.bottom - h - TOAST_MARGIN;

    hw = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "AcoustIDToast", "", WS_POPUP,
        x, y, TOAST_W, h, NULL, NULL, plugin.hDllInstance, NULL);
    if (!hw) { HeapFree(GetProcessHeap(), 0, a); return 0; }

    SetWindowLongPtrA(hw, GWLP_USERDATA, (LONG_PTR)a);
    SetLayeredWindowAttributes(hw, 0, 245, LWA_ALPHA);
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    UpdateWindow(hw);

    SetTimer(hw, 1, TOAST_SHOWMS, NULL);  /* tras X ms, empezar fade */

    while (GetMessageA(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
        if (!IsWindow(hw)) break;
    }
    return 0;
}

static void notify(const char* title, const char* msg) {
    NotifyArg* a;
    HANDLE h;
    if (!g_notify) return;   /* notificaciones silenciadas */
    a = (NotifyArg*)HeapAlloc(GetProcessHeap(), 0, sizeof(NotifyArg));
    if (!a) return;
    strncpy_s(a->title, sizeof(a->title), title, _TRUNCATE);
    strncpy_s(a->msg,   sizeof(a->msg),   msg,   _TRUNCATE);
    h = CreateThread(NULL, 0, toast_thread, a, 0, NULL);
    if (h) CloseHandle(h);
    else HeapFree(GetProcessHeap(), 0, a);
}

/* ---------- Escritura de tags ID3v2.3 (texto UTF-16) ---------- */
/* Escribe un frame de texto con encoding UTF-16 con BOM (encoding byte 0x01).
   text es wchar_t (UTF-16LE en Windows). Soporta japones, acentos, etc. */
static int write_id3_frame(unsigned char* buf, const char* id, const wchar_t* wtext) {
    int wlen = (int)wcslen(wtext);
    int i;
    /* contenido = 1 (encoding) + 2 (BOM) + (wlen+1)*2 (texto UTF-16 + nul) */
    int content = 1 + 2 + (wlen + 1) * 2;
    int total = 0;

    memcpy(buf, id, 4); buf += 4; total += 4;
    /* frame size big-endian (v2.3, no synchsafe) */
    buf[0] = (content >> 24) & 0xFF;
    buf[1] = (content >> 16) & 0xFF;
    buf[2] = (content >> 8)  & 0xFF;
    buf[3] = content & 0xFF;
    buf += 4; total += 4;
    buf[0] = 0; buf[1] = 0; buf += 2; total += 2;  /* flags */

    buf[0] = 0x01; buf += 1; total += 1;           /* encoding = UTF-16 con BOM */
    /* BOM UTF-16LE: FF FE */
    buf[0] = 0xFF; buf[1] = 0xFE; buf += 2; total += 2;
    /* texto en UTF-16LE */
    for (i = 0; i < wlen; i++) {
        unsigned short w = (unsigned short)wtext[i];
        buf[0] = w & 0xFF;
        buf[1] = (w >> 8) & 0xFF;
        buf += 2; total += 2;
    }
    /* terminador nul UTF-16 */
    buf[0] = 0; buf[1] = 0; total += 2;
    return total;
}

static int existing_id3v2_size(const char* filepath) {
    FILE* f = NULL;
    unsigned char hdr[10];
    int size = 0;
    fopen_s(&f, filepath, "rb");
    if (!f) return 0;
    if (fread(hdr, 1, 10, f) == 10 &&
        hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        size = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) |
               ((hdr[8] & 0x7F) << 7)  |  (hdr[9] & 0x7F);
        size += 10;
    }
    fclose(f);
    return size;
}

/* Intenta escritura IN-PLACE: si el tag ID3v2 existente es >= al nuevo,
   sobreescribe los frames sin cambiar el tamanio total. El audio no se
   mueve, por lo que Winamp puede tener el archivo abierto (lectura
   compartida) sin problema. Retorna 1 si logro escribir in-place. */
static int write_metadata_inplace(const char* filepath,
                                  const wchar_t* artist, const wchar_t* title, const wchar_t* album) {
    HANDLE h;
    unsigned char hdr[10];
    DWORD rd, wr;
    int old_body, new_body, frames_len;
    unsigned char framebuf[4096];
    unsigned char* newtag = NULL;
    int ok = 0;

    /* Abrir compartido lectura+escritura (convive con Winamp) */
    h = CreateFileA(filepath, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    /* Leer header existente */
    if (!ReadFile(h, hdr, 10, &rd, NULL) || rd != 10 ||
        hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') {
        CloseHandle(h);
        return 0; /* no hay tag ID3v2 -> no se puede in-place */
    }

    old_body = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) |
               ((hdr[8] & 0x7F) << 7)  |  (hdr[9] & 0x7F);

    /* Construir nuevos frames (UTF-16) */
    frames_len  = write_id3_frame(framebuf, "TIT2", title);
    frames_len += write_id3_frame(framebuf + frames_len, "TPE1", artist);
    if (album && album[0])
        frames_len += write_id3_frame(framebuf + frames_len, "TALB", album);

    /* El nuevo contenido (frames + ceros de padding) debe caber en old_body */
    if (frames_len > old_body) {
        CloseHandle(h);
        return 0; /* no cabe -> recrear (otra funcion) */
    }

    new_body = old_body; /* mantenemos tamanio para no mover audio */
    newtag = (unsigned char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 10 + new_body);
    if (!newtag) { CloseHandle(h); return 0; }

    /* Header (mismo tamanio) */
    newtag[0]='I'; newtag[1]='D'; newtag[2]='3';
    newtag[3]=0x03; newtag[4]=0x00; newtag[5]=0x00;
    newtag[6]=(new_body>>21)&0x7F;
    newtag[7]=(new_body>>14)&0x7F;
    newtag[8]=(new_body>>7)&0x7F;
    newtag[9]= new_body&0x7F;
    memcpy(newtag + 10, framebuf, frames_len);
    /* resto = padding en cero (ya inicializado) */

    /* Escribir desde el inicio (sobreescribe header+body, audio intacto) */
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    if (WriteFile(h, newtag, 10 + new_body, &wr, NULL) && wr == (DWORD)(10 + new_body))
        ok = 1;

    FlushFileBuffers(h);
    HeapFree(GetProcessHeap(), 0, newtag);
    CloseHandle(h);
    return ok;
}

static int write_metadata(const char* filepath,
                          const wchar_t* artist, const wchar_t* title, const wchar_t* album) {
    char tmppath[MAX_PATH];
    FILE *fin = NULL, *fout = NULL;
    unsigned char *tag = NULL;
    unsigned char framebuf[4096];
    int frames_len = 0, padded_len, old_tag;
    int ok = 0;
    static unsigned char copybuf[65536];
    size_t r;

    /* 1er intento: in-place (no mueve audio, no requiere parar Winamp) */
    if (write_metadata_inplace(filepath, artist, title, album))
        return 1;

    /* 2do intento: recrear el archivo (requiere que NO este en uso exclusivo) */
    frames_len += write_id3_frame(framebuf + frames_len, "TIT2", title);
    frames_len += write_id3_frame(framebuf + frames_len, "TPE1", artist);
    if (album && album[0])
        frames_len += write_id3_frame(framebuf + frames_len, "TALB", album);

    /* padding generoso (1KB) para permitir futuras ediciones in-place */
    padded_len = frames_len + 1024;

    tag = (unsigned char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 10 + padded_len);
    if (!tag) return 0;

    tag[0] = 'I'; tag[1] = 'D'; tag[2] = '3';
    tag[3] = 0x03; tag[4] = 0x00; tag[5] = 0x00;
    tag[6] = (padded_len >> 21) & 0x7F;
    tag[7] = (padded_len >> 14) & 0x7F;
    tag[8] = (padded_len >> 7)  & 0x7F;
    tag[9] =  padded_len & 0x7F;
    memcpy(tag + 10, framebuf, frames_len);

    _snprintf_s(tmppath, sizeof(tmppath), _TRUNCATE, "%s.acoustid_tmp", filepath);

    fopen_s(&fout, tmppath, "wb");
    if (!fout) { HeapFree(GetProcessHeap(), 0, tag); return 0; }
    fwrite(tag, 1, 10 + padded_len, fout);

    fopen_s(&fin, filepath, "rb");
    if (!fin) { fclose(fout); DeleteFileA(tmppath); HeapFree(GetProcessHeap(), 0, tag); return 0; }

    old_tag = existing_id3v2_size(filepath);
    if (old_tag > 0) fseek(fin, old_tag, SEEK_SET);

    while ((r = fread(copybuf, 1, sizeof(copybuf), fin)) > 0)
        fwrite(copybuf, 1, r, fout);

    fclose(fin);
    fclose(fout);
    HeapFree(GetProcessHeap(), 0, tag);

    if (MoveFileExA(tmppath, filepath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        ok = 1;
    } else {
        DeleteFileA(tmppath);
        ok = 0;
    }
    return ok;
}

/* Actualiza la fecha de modificacion del archivo a AHORA, para que la
   Media Library de Winamp detecte el cambio en su proximo rescan.
   Sin esto, la escritura in-place (que no cambia el tamanio) pasa
   desapercibida y la ML sigue mostrando los datos viejos. */
static void touch_mtime(const char* filepath) {
    HANDLE h;
    FILETIME ft;
    SYSTEMTIME st;
    h = CreateFileA(filepath, FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &ft);
    SetFileTime(h, NULL, NULL, &ft);  /* solo mtime */
    CloseHandle(h);
}

/* ---------- Worker: fingerprint + lookup + tag ---------- */
typedef struct { char filepath[MAX_PATH]; } WorkArg;

static DWORD WINAPI work_thread(LPVOID param) {
    WorkArg* wa = (WorkArg*)param;
    char filepath[MAX_PATH];
    static char fpjson[16384];
    static char resp[32768];
    char fp_enc[12000];
    char key_enc[128];
    char fingerprint[8192];
    char path[16000];
    wchar_t artist[256], title[256], album[256];
    char artist_a[512], title_a[512], album_a[512];
    const char* dur_p;
    int duration = 0;
    double score;
    char msg[768];
    int ok_write = 0;

    strcpy_s(filepath, sizeof(filepath), wa->filepath);
    HeapFree(GetProcessHeap(), 0, wa);

    /* 1. Fingerprint */
    if (run_fpcalc(filepath, fpjson, sizeof(fpjson)) == 0 || fpjson[0] == 0) {
        notify("AcoustID", "fpcalc.exe no encontrado.\nColoca fpcalc.exe en <Winamp>\\Plugins\\\nDescarga: https://acoustid.org/chromaprint");
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    if (!json_str(fpjson, "fingerprint", fingerprint, sizeof(fingerprint))) {
        notify("AcoustID", "fpcalc no genero fingerprint valido.");
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    dur_p = strstr(fpjson, "\"duration\":");
    if (dur_p) { dur_p += 11; while (*dur_p == ' ') dur_p++; duration = atoi(dur_p); }
    if (duration <= 0) duration = 120;

    /* 2. AcoustID lookup */
    url_encode(fingerprint, fp_enc, sizeof(fp_enc));
    url_encode(g_api_key, key_enc, sizeof(key_enc));
    _snprintf_s(path, sizeof(path), _TRUNCATE,
        "/v2/lookup?client=%s&duration=%d&fingerprint=%s&meta=recordings+releasegroups",
        key_enc, duration, fp_enc);

    if (https_get("api.acoustid.org", path, resp, sizeof(resp)) == 0 || resp[0] == 0) {
        notify("AcoustID", "Sin respuesta de api.acoustid.org\nRevisa tu conexion y API key.");
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    score = json_num(resp, "score");
    artist[0] = title[0] = album[0] = 0;
    json_wstr(resp, "title", title, 256);
    json_wstr(resp, "name", artist, 256);
    {
        const char* rg = strstr(resp, "\"releasegroups\"");
        if (rg) json_wstr(rg, "title", album, 256);
    }

    /* Versiones char (UTF-8) para mostrar en mensajes */
    WideCharToMultiByte(CP_UTF8, 0, artist, -1, artist_a, sizeof(artist_a), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, title,  -1, title_a,  sizeof(title_a),  NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, album,  -1, album_a,  sizeof(album_a),  NULL, NULL);

    if (!title[0] || !artist[0] || score < (g_threshold / 100.0)) {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
            "Sin coincidencia (score %.0f%%)\nArchivo: %s",
            score * 100.0, PathFindFileNameA(filepath));
        notify("AcoustID", msg);
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    /* 3. Escribir metadata ID3v2 UTF-16 (in-place, sin parar audio) */
    ok_write = write_metadata(filepath, artist, title, album);
    if (ok_write) touch_mtime(filepath);

    if (ok_write) {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
            "Tags actualizados (%.0f%%)\n\nArtista: %s\nTitulo:  %s\nAlbum:   %s",
            score * 100.0, artist_a, title_a, (album_a[0] ? album_a : "(sin album)"));
        winamp_refresh_and_toast(filepath);
        notify("AcoustID AutoTag", msg);
    } else {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
            "Identificado:\n%s - %s\n\nNo se pudo escribir el tag (archivo en uso).\nSe aplicara al cambiar de pista.",
            artist_a, title_a);
        notify("AcoustID AutoTag", msg);
        EnterCriticalSection(&g_pending_cs);
        strcpy_s(g_pending_file, sizeof(g_pending_file), filepath);
        wcscpy_s(g_pending_artist, 256, artist);
        wcscpy_s(g_pending_title,  256, title);
        wcscpy_s(g_pending_album,  256, album);
        g_has_pending = 1;
        LeaveCriticalSection(&g_pending_cs);
    }

    InterlockedExchange(&g_busy, 0);
    return 0;
}

static void launch_identify(const char* filepath) {
    WorkArg* wa;
    HANDLE h;
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return; /* ya ocupado */
    wa = (WorkArg*)HeapAlloc(GetProcessHeap(), 0, sizeof(WorkArg));
    if (!wa) { InterlockedExchange(&g_busy, 0); return; }
    strcpy_s(wa->filepath, sizeof(wa->filepath), filepath);
    h = CreateThread(NULL, 0, work_thread, wa, 0, NULL);
    if (h) CloseHandle(h);
    else { HeapFree(GetProcessHeap(), 0, wa); InterlockedExchange(&g_busy, 0); }
}

/* ---------- Timer: detecta cambio de pista ---------- */
static VOID CALLBACK on_timer(HWND hwnd, UINT msg, UINT_PTR id, DWORD t) {
    int playing, pos;
    char* file;
    (void)hwnd; (void)msg; (void)id; (void)t;

    /* Aplicar tag pendiente si el archivo ya no es la pista actual */
    if (g_has_pending) {
        int cur = (int)SendMessageA(plugin.hwndParent, WM_USER, 0, IPC_GETLISTPOS);
        char* curf = (char*)SendMessageA(plugin.hwndParent, WM_USER, cur, IPC_GETPLAYLISTFILE);
        EnterCriticalSection(&g_pending_cs);
        if (g_has_pending && (!curf || _stricmp(curf, g_pending_file) != 0)) {
            if (write_metadata(g_pending_file, g_pending_artist, g_pending_title, g_pending_album)) {
                touch_mtime(g_pending_file);
                g_has_pending = 0;
                winamp_refresh_and_toast(g_pending_file);
            }
        }
        LeaveCriticalSection(&g_pending_cs);
    }

    if (!g_auto) return;
    if (InterlockedCompareExchange(&g_busy, 0, 0) != 0) return;

    playing = (int)SendMessageA(plugin.hwndParent, WM_USER, 0, IPC_ISPLAYING);
    if (playing != 1) return;

    pos = (int)SendMessageA(plugin.hwndParent, WM_USER, 0, IPC_GETLISTPOS);
    if (pos == g_last_pos) return;
    g_last_pos = pos;

    file = (char*)SendMessageA(plugin.hwndParent, WM_USER, pos, IPC_GETPLAYLISTFILE);
    if (!file || !file[0]) return;
    if (_stricmp(file, g_last_file) == 0) return;
    strcpy_s(g_last_file, sizeof(g_last_file), file);

    launch_identify(file);
}

/* ---------- INI ---------- */
static void load_ini(void) {
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    PathRemoveFileSpecA(dir);
    _snprintf_s(g_ini, sizeof(g_ini), _TRUNCATE, "%s\\winamp.ini", dir);

    GetPrivateProfileStringA("gen_acoustid", "api_key",  g_api_key, g_api_key, sizeof(g_api_key), g_ini);
    GetPrivateProfileStringA("gen_acoustid", "template", g_tpl,     g_tpl,     sizeof(g_tpl),     g_ini);
    g_auto      = GetPrivateProfileIntA("gen_acoustid", "auto",      g_auto,      g_ini);
    g_threshold = GetPrivateProfileIntA("gen_acoustid", "threshold", g_threshold, g_ini);
    g_notify    = GetPrivateProfileIntA("gen_acoustid", "notify",    g_notify,    g_ini);
}

static void save_ini(void) {
    char buf[16];
    WritePrivateProfileStringA("gen_acoustid", "api_key",  g_api_key, g_ini);
    WritePrivateProfileStringA("gen_acoustid", "template", g_tpl,     g_ini);
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", g_threshold);
    WritePrivateProfileStringA("gen_acoustid", "threshold", buf, g_ini);
    WritePrivateProfileStringA("gen_acoustid", "auto", g_auto ? "1" : "0", g_ini);
    WritePrivateProfileStringA("gen_acoustid", "notify", g_notify ? "1" : "0", g_ini);
}

/* ---------- Dialogo de configuracion ---------- */
#define ID_KEY  201
#define ID_TPL  202
#define ID_AUTO 203
#define ID_THR  204
#define ID_NOW  205
#define ID_STA  206
#define ID_NOTIFY 207

static HWND g_cfg = NULL;

static LRESULT CALLBACK cfg_wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_NOW: {
            int pos = (int)SendMessageA(plugin.hwndParent, WM_USER, 0, IPC_GETLISTPOS);
            char* file = (char*)SendMessageA(plugin.hwndParent, WM_USER, pos, IPC_GETPLAYLISTFILE);
            HWND hsta = GetDlgItem(hw, ID_STA);
            if (file && file[0] && InterlockedCompareExchange(&g_busy, 0, 0) == 0) {
                SetWindowTextA(hsta, "Identificando... espera la ventana.");
                launch_identify(file);
            } else {
                SetWindowTextA(hsta, (InterlockedCompareExchange(&g_busy,0,0)!=0) ? "Ocupado..." : "No hay pista.");
            }
            return 0;
        }
        case IDOK: {
            GetDlgItemTextA(hw, ID_KEY, g_api_key, sizeof(g_api_key));
            GetDlgItemTextA(hw, ID_TPL, g_tpl, sizeof(g_tpl));
            g_auto      = (IsDlgButtonChecked(hw, ID_AUTO) == BST_CHECKED) ? 1 : 0;
            g_notify    = (IsDlgButtonChecked(hw, ID_NOTIFY) == BST_CHECKED) ? 1 : 0;
            g_threshold = GetDlgItemInt(hw, ID_THR, NULL, FALSE);
            save_ini();
            DestroyWindow(hw);
            return 0;
        }
        case IDCANCEL:
            DestroyWindow(hw);
            return 0;
        }
        break;
    case WM_DESTROY:
        g_cfg = NULL;
        break;
    case WM_CLOSE:
        DestroyWindow(hw);
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

static void make_label(HWND parent, int x, int y, int w, int h, const char* txt) {
    CreateWindowExA(0, "STATIC", txt, WS_CHILD | WS_VISIBLE,
        x, y, w, h, parent, NULL, plugin.hDllInstance, NULL);
}
static HWND make_edit(HWND parent, int id, int x, int y, int w, int h, const char* txt) {
    return CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", txt,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        x, y, w, h, parent, (HMENU)(UINT_PTR)id, plugin.hDllInstance, NULL);
}
static void make_button(HWND parent, int id, int x, int y, int w, int h, const char* txt) {
    CreateWindowExA(0, "BUTTON", txt, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, y, w, h, parent, (HMENU)(UINT_PTR)id, plugin.hDllInstance, NULL);
}

static void open_config(void) {
    WNDCLASSA wc;
    HWND hw, hck;
    char thr[16];
    static int registered = 0;

    if (g_cfg) { SetForegroundWindow(g_cfg); return; }

    if (!registered) {
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = cfg_wndproc;
        wc.hInstance     = plugin.hDllInstance;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "AcoustIDCfgWnd";
        RegisterClassA(&wc);
        registered = 1;
    }

    hw = CreateWindowExA(WS_EX_TOOLWINDOW, "AcoustIDCfgWnd",
        "AcoustID AutoTag - Configuracion",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 430, 280,
        plugin.hwndParent, NULL, plugin.hDllInstance, NULL);
    if (!hw) return;

    make_label(hw, 12, 14, 120, 20, "AcoustID API Key:");
    make_edit (hw, ID_KEY, 140, 12, 265, 22, g_api_key);

    make_label(hw, 12, 44, 120, 20, "Formato nombre:");
    make_edit (hw, ID_TPL, 140, 42, 265, 22, g_tpl);

    make_label(hw, 12, 74, 180, 20, "Confianza minima (0-100):");
    _snprintf_s(thr, sizeof(thr), _TRUNCATE, "%d", g_threshold);
    make_edit (hw, ID_THR, 200, 72, 55, 22, thr);

    hck = CreateWindowExA(0, "BUTTON", "Auto-identificar al cambiar de pista",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        12, 102, 330, 20, hw, (HMENU)(UINT_PTR)ID_AUTO, plugin.hDllInstance, NULL);
    if (g_auto) SendMessageA(hck, BM_SETCHECK, BST_CHECKED, 0);

    hck = CreateWindowExA(0, "BUTTON", "Mostrar notificaciones",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        12, 126, 330, 20, hw, (HMENU)(UINT_PTR)ID_NOTIFY, plugin.hDllInstance, NULL);
    if (g_notify) SendMessageA(hck, BM_SETCHECK, BST_CHECKED, 0);

    make_label(hw, 12, 152, 400, 18, "Variables: {artist}  {title}  {album}");

    CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT,
        12, 174, 400, 18, hw, (HMENU)(UINT_PTR)ID_STA, plugin.hDllInstance, NULL);

    make_button(hw, ID_NOW,   12,  200, 175, 28, "Identificar pista actual");
    make_button(hw, IDOK,     230, 200, 80,  28, "Guardar");
    make_button(hw, IDCANCEL, 320, 200, 80,  28, "Cancelar");

    g_cfg = hw;
    ShowWindow(hw, SW_SHOW);
}

/* ---------- Lifecycle ---------- */
static int plugin_init(void) {
    InitializeCriticalSection(&g_pending_cs);
    load_ini();
    g_timer = SetTimer(plugin.hwndParent, 0xAC01, 2000, on_timer);
    return 0;
}

static void plugin_config(void) {
    open_config();
}

static void plugin_quit(void) {
    int i;
    if (g_timer) { KillTimer(plugin.hwndParent, g_timer); g_timer = 0; }
    for (i = 0; i < 30 && InterlockedCompareExchange(&g_busy, 0, 0) != 0; i++)
        Sleep(100);
    DeleteCriticalSection(&g_pending_cs);
}

__declspec(dllexport) winampGeneralPurposePlugin* winampGetGeneralPurposePlugin(void) {
    return &plugin;
}
