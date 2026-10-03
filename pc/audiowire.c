#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <propidl.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "sprites.h"

#define PUERTO 5005
#define MAX_CLIENTES 16

static const GUID G_CLSID_Enum   = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID G_IID_Enum     = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID G_IID_Client   = {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const GUID G_IID_Capture  = {0xC8ADBD64,0xE71E,0x48A0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};
static const GUID G_SUB_FLOAT    = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID G_SUB_PCM      = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const PROPERTYKEY G_PKEY_Nombre = {{0xA45C254E,0xDF1C,0x4EFD,{0x80,0x20,0x67,0xD1,0x46,0xA8,0x50,0xE0}},14};

static CRITICAL_SECTION cs;
static SOCKET clientes[MAX_CLIENTES];
static int nclientes = 0;
static volatile LONG g_rate = 0, g_canales = 0;
static volatile ULONGLONG g_ultimoEnvio = 0;
static wchar_t g_disp[256] = L"Iniciando…";
static wchar_t g_errRed[128] = L"";
static volatile LONG g_vol = 100;
#define VOL_MAX 150

static void cerrar_clientes(void) {
    for (int i = 0; i < nclientes; i++) closesocket(clientes[i]);
    nclientes = 0;
}

static int enviar_todo(SOCKET s, const char *p, int n) {
    while (n > 0) {
        int r = send(s, p, n, 0);
        if (r <= 0) return 0;
        p += r; n -= r;
    }
    return 1;
}

static void enviar(const char *datos, int n, int conSonido) {
    EnterCriticalSection(&cs);
    if (nclientes && conSonido) g_ultimoEnvio = GetTickCount64();
    for (int i = 0; i < nclientes; ) {
        if (!enviar_todo(clientes[i], datos, n)) {
            closesocket(clientes[i]);
            clientes[i] = clientes[--nclientes];
        } else i++;
    }
    LeaveCriticalSection(&cs);
}

static DWORD WINAPI hilo_aceptar(LPVOID arg) {
    (void)arg;
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL excl = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&excl, sizeof excl);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(PUERTO);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (struct sockaddr *)&a, sizeof a) || listen(srv, 5)) {
        EnterCriticalSection(&cs);
        swprintf(g_errRed, 128, L"Puerto %d ocupado", PUERTO);
        LeaveCriticalSection(&cs);
        return 0;
    }
    for (;;) {
        SOCKET c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) { Sleep(100); continue; }
        BOOL nd = TRUE; DWORD to = 2000;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (char *)&nd, sizeof nd);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (char *)&to, sizeof to);
        while (g_rate == 0) Sleep(100);

        EnterCriticalSection(&cs);
        unsigned char cab[10] = {'S', 'W', 'X', '1'};
        DWORD r = (DWORD)g_rate; WORD ch = (WORD)g_canales;
        memcpy(cab + 4, &r, 4); memcpy(cab + 8, &ch, 2);
        if (nclientes < MAX_CLIENTES && enviar_todo(c, (char *)cab, 10)) clientes[nclientes++] = c;
        else closesocket(c);
        LeaveCriticalSection(&cs);
    }
}

static void ip_local(wchar_t *out, int n) {
    wcscpy(out, L"Sin red");
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(1);
    a.sin_addr.s_addr = inet_addr("10.255.255.255");
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        struct sockaddr_in yo; int len = sizeof yo;
        if (getsockname(s, (struct sockaddr *)&yo, &len) == 0) {
            unsigned char *b = (unsigned char *)&yo.sin_addr;
            swprintf(out, n, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        }
    }
    closesocket(s);
}

static int tipo_formato(WAVEFORMATEX *w) {
    if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return 1;
    if (w->wFormatTag == WAVE_FORMAT_PCM) return 2;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE *e = (WAVEFORMATEXTENSIBLE *)w;
        if (IsEqualGUID(&e->SubFormat, &G_SUB_FLOAT)) return 1;
        if (IsEqualGUID(&e->SubFormat, &G_SUB_PCM)) return 2;
    }
    return 0;
}

static void convertir(const BYTE *in, short *out, UINT32 muestras, int tipo, int bytes) {
    float g = g_vol / 100.0f;
    for (UINT32 i = 0; i < muestras; i++, in += bytes) {
        float f;
        if (tipo == 1) f = *(const float *)in * 32767.0f;
        else if (bytes == 2) f = *(const short *)in;
        else if (bytes == 3) f = (short)(in[1] | (in[2] << 8));
        else f = (float)(*(const int *)in >> 16);
        f *= g;
        out[i] = (short)(f > 32767.f ? 32767 : f < -32768.f ? -32768 : (int)f);
    }
}

static void poner_disp(const wchar_t *t) {
    EnterCriticalSection(&cs);
    wcsncpy(g_disp, t, 255); g_disp[255] = 0;
    LeaveCriticalSection(&cs);
}

static DWORD WINAPI hilo_captura(LPVOID arg) {
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        IMMDeviceEnumerator *en = NULL; IMMDevice *dev = NULL; IAudioClient *ac = NULL;
        IAudioCaptureClient *cc = NULL; WAVEFORMATEX *wfx = NULL; LPWSTR id = NULL;
        short *buf = NULL; const wchar_t *err = NULL; HRESULT hr;

        if (FAILED(CoCreateInstance(&G_CLSID_Enum, NULL, CLSCTX_ALL, &G_IID_Enum, (void **)&en))) { err = L"No se pudo iniciar el audio"; goto fin; }
        if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) { err = L"No hay dispositivo de salida"; goto fin; }
        IMMDevice_GetId(dev, &id);

        IPropertyStore *ps = NULL;
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(IPropertyStore_GetValue(ps, &G_PKEY_Nombre, &pv)) && pv.vt == VT_LPWSTR) poner_disp(pv.pwszVal);
            PropVariantClear(&pv);
            IPropertyStore_Release(ps);
        }

        if (FAILED(IMMDevice_Activate(dev, &G_IID_Client, CLSCTX_ALL, NULL, (void **)&ac))) { err = L"No se pudo abrir el dispositivo"; goto fin; }
        if (FAILED(IAudioClient_GetMixFormat(ac, &wfx))) { err = L"Formato de audio desconocido"; goto fin; }
        int tipo = tipo_formato(wfx), bytes = wfx->wBitsPerSample / 8;
        if (!tipo || (tipo == 1 && bytes != 4) || (tipo == 2 && (bytes < 2 || bytes > 4))) { err = L"Formato de audio no soportado"; goto fin; }

        hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, wfx, NULL);
        if (FAILED(hr)) { err = L"No se pudo iniciar la captura"; goto fin; }
        UINT32 tamBuf = 0;
        IAudioClient_GetBufferSize(ac, &tamBuf);
        if (FAILED(IAudioClient_GetService(ac, &G_IID_Capture, (void **)&cc))) { err = L"No se pudo iniciar la captura"; goto fin; }

        int canales = wfx->nChannels;
        buf = (short *)malloc((size_t)tamBuf * canales * sizeof(short));
        EnterCriticalSection(&cs);
        if ((LONG)wfx->nSamplesPerSec != g_rate || canales != g_canales) cerrar_clientes();
        g_rate = (LONG)wfx->nSamplesPerSec; g_canales = canales;
        LeaveCriticalSection(&cs);

        IAudioClient_Start(ac);
        for (int vuelta = 1;; vuelta++) {
            Sleep(10);
            UINT32 paquete = 0;
            hr = IAudioCaptureClient_GetNextPacketSize(cc, &paquete);
            if (FAILED(hr)) break;
            while (paquete) {
                BYTE *datos; UINT32 frames; DWORD flags;
                if (FAILED(IAudioCaptureClient_GetBuffer(cc, &datos, &frames, &flags, NULL, NULL))) { paquete = 0; break; }
                int silencio = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                if (frames > tamBuf) frames = tamBuf;
                if (silencio) memset(buf, 0, (size_t)frames * canales * sizeof(short));
                else convertir(datos, buf, frames * canales, tipo, bytes);
                IAudioCaptureClient_ReleaseBuffer(cc, frames);
                enviar((char *)buf, (int)(frames * canales * sizeof(short)), !silencio);
                if (FAILED(IAudioCaptureClient_GetNextPacketSize(cc, &paquete))) break;
            }

            if (vuelta % 200 == 0) {
                IMMDevice *d2 = NULL; LPWSTR id2 = NULL; int cambio = 0;
                if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &d2))) {
                    if (SUCCEEDED(IMMDevice_GetId(d2, &id2))) { cambio = !id || wcscmp(id, id2) != 0; CoTaskMemFree(id2); }
                    IMMDevice_Release(d2);
                }
                if (cambio) break;
            }
        }
        IAudioClient_Stop(ac);
    fin:
        if (err) { poner_disp(err); Sleep(1000); }
        free(buf);
        if (cc) IAudioCaptureClient_Release(cc);
        if (wfx) CoTaskMemFree(wfx);
        if (ac) IAudioClient_Release(ac);
        if (id) CoTaskMemFree(id);
        if (dev) IMMDevice_Release(dev);
        if (en) IMMDeviceEnumerator_Release(en);
        Sleep(200);
    }
    return 0;
}

#define COL_FONDO  RGB(250, 247, 242)
#define COL_TEXTO  RGB(43, 38, 34)
#define COL_SUAVE  RGB(154, 145, 138)
#define COL_ACENTO RGB(240, 101, 30)
#define COL_ERROR  RGB(192, 57, 43)
#define T_ANIM 1
#define T_ESTADO 2

static int dpi = 96;
static int S(int v) { return MulDiv(v, dpi, 96); }

static HBITMAP fotogramas[2][SPR_N];
static int sprW, sprH;
static int modo = 0, cuadro = 0;
static HFONT fIP, fEstado, fDisp;
static HBRUSH brFondo;
static HDC memDC; static HBITMAP memBmp; static HGDIOBJ memViejo;
static wchar_t txtIP[32] = L"", txtEstado[96] = L"", txtDisp[300] = L"";
static COLORREF colEstado = COL_SUAVE;
static ULONGLONG tIP = 0;
static int arrastrando = 0;

#define WM_BANDEJA (WM_APP + 1)
#define WM_MOSTRAR (WM_APP + 2)
#define CMD_ABRIR 1
#define CMD_SILENCIAR 2
#define CMD_SALIR 3
static NOTIFYICONDATAW nid;
static UINT WM_TASKBAR_CREADA;
static int enBandeja = 0, iconoActivo = -1;
static HICON icoIdle, icoActivo;
static wchar_t tipActual[128] = L"";
static LONG volPrevio = 100;

static void vol_cargar(void) {
    DWORD v, t = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\AudioWire", L"Volumen", RRF_RT_REG_DWORD, NULL, &v, &t) == ERROR_SUCCESS && v <= VOL_MAX)
        g_vol = (LONG)v;
}
static void vol_guardar(void) {
    DWORD v = (DWORD)g_vol;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\AudioWire", L"Volumen", REG_DWORD, &v, sizeof v);
}

static int y_base(void) { return S(18) + sprH + S(8); }
static int vol_y(void) { return y_base() + S(112); }
static int vol_izq(void) { return S(54); }
static int vol_der(int ancho) { return ancho - S(66); }

static void circulo(HDC dc, float cx, float cy, float r, COLORREF col) {
    int x0 = (int)floorf(cx - r) - 1, y0 = (int)floorf(cy - r) - 1, n = (int)ceilf(2 * r) + 3;
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = n; bi.bmiHeader.biHeight = -n;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    unsigned int *px;
    HBITMAP b = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int c = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float dx = x0 + x + (sx + .5f) / 4 - cx, dy = y0 + y + (sy + .5f) / 4 - cy;
                    c += dx * dx + dy * dy <= r * r;
                }
            unsigned a = c * 255 / 16;
            px[y * n + x] = (a << 24) | ((GetRValue(col) * a / 255) << 16) | ((GetGValue(col) * a / 255) << 8) | (GetBValue(col) * a / 255);
        }
    HDC m = CreateCompatibleDC(dc);
    HGDIOBJ v = SelectObject(m, b);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, x0, y0, n, n, m, 0, 0, n, n, bf);
    SelectObject(m, v); DeleteDC(m); DeleteObject(b);
}

static void altavoz(HDC dc, int cx, int cy, int t, int ondas, COLORREF col) {
    const int K = 4, n = t * K;
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = n; bi.bmiHeader.biHeight = -n;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    unsigned int *big, *px;
    HBITMAP bb = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&big, NULL, 0);
    HDC m = CreateCompatibleDC(dc);
    HGDIOBJ v = SelectObject(m, bb);
    HBRUSH blanco = (HBRUSH)GetStockObject(WHITE_BRUSH);
    HPEN sinBorde = (HPEN)GetStockObject(NULL_PEN);
    SelectObject(m, blanco); SelectObject(m, sinBorde);
    float u = n / 24.0f;
    POINT cuerpo[6] = {{(int)(3*u),(int)(9*u)},{(int)(7*u),(int)(9*u)},{(int)(12*u),(int)(4.5f*u)},
                       {(int)(12*u),(int)(19.5f*u)},{(int)(7*u),(int)(15*u)},{(int)(3*u),(int)(15*u)}};
    Polygon(m, cuerpo, 6);
    HPEN p = CreatePen(PS_SOLID, (int)(2.2f*u), RGB(255, 255, 255));
    SelectObject(m, p); SelectObject(m, GetStockObject(NULL_BRUSH));
    int c = (int)(12*u), my = n / 2;
    if (ondas == 0) {
        MoveToEx(m, (int)(15.5f*u), (int)(9*u), NULL); LineTo(m, (int)(21.5f*u), (int)(15*u));
        MoveToEx(m, (int)(21.5f*u), (int)(9*u), NULL); LineTo(m, (int)(15.5f*u), (int)(15*u));
    }
    for (int w = 1; w <= ondas; w++) {
        int r = (int)((w == 1 ? 4.5f : 8.5f) * u);
        Arc(m, c - r, my - r, c + r, my + r, c + r, my + r * 3 / 4, c + r, my - r * 3 / 4);
    }
    SelectObject(m, v); DeleteDC(m); DeleteObject(p);

    bi.bmiHeader.biWidth = t; bi.bmiHeader.biHeight = -t;
    HBITMAP sb = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    for (int y = 0; y < t; y++)
        for (int x = 0; x < t; x++) {
            unsigned sum = 0;
            for (int sy = 0; sy < K; sy++)
                for (int sx = 0; sx < K; sx++) sum += big[(y * K + sy) * n + x * K + sx] & 0xFF;
            unsigned a = sum / (K * K);
            px[y * t + x] = (a << 24) | ((GetRValue(col) * a / 255) << 16) | ((GetGValue(col) * a / 255) << 8) | (GetBValue(col) * a / 255);
        }
    DeleteObject(bb);
    m = CreateCompatibleDC(dc);
    v = SelectObject(m, sb);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, cx - t / 2, cy - t / 2, t, t, m, 0, 0, t, t, bf);
    SelectObject(m, v); DeleteDC(m); DeleteObject(sb);
}

static void barra(HDC dc, int x1, int x2, int y, int grosor, COLORREF col) {
    if (x2 <= x1) return;
    HBRUSH br = CreateSolidBrush(col);
    RECT r = {x1, y - grosor / 2, x2, y - grosor / 2 + grosor};
    FillRect(dc, &r, br);
    DeleteObject(br);
    circulo(dc, x1, y - grosor / 2 + grosor / 2.0f, grosor / 2.0f, col);
    circulo(dc, x2, y - grosor / 2 + grosor / 2.0f, grosor / 2.0f, col);
}

static void pintar_volumen(HDC dc, int ancho) {
    int y = vol_y(), x1 = vol_izq(), x2 = vol_der(ancho);
    int xv = x1 + (x2 - x1) * g_vol / VOL_MAX;
    int x100 = x1 + (x2 - x1) * 100 / VOL_MAX;

    altavoz(dc, x1 - S(22), y, S(22), g_vol == 0 ? 0 : g_vol < 50 ? 1 : 2, g_vol ? COL_TEXTO : COL_SUAVE);

    barra(dc, x1, x2, y, S(4), RGB(229, 221, 210));
    HBRUSH bm = CreateSolidBrush(RGB(205, 196, 186));
    RECT rm = {x100 - S(1) / 2, y - S(7), x100 - S(1) / 2 + (S(1) < 1 ? 1 : S(1)), y - S(4)};
    FillRect(dc, &rm, bm); DeleteObject(bm);
    barra(dc, x1, xv, y, S(4), g_vol > 100 ? RGB(214, 69, 20) : COL_ACENTO);

    float r = (float)S(arrastrando ? 9 : 8);
    circulo(dc, xv, y + .5f, r + 1, RGB(236, 228, 218));
    circulo(dc, xv, y, r, g_vol > 100 ? RGB(214, 69, 20) : COL_ACENTO);
    circulo(dc, xv, y, r - S(3), RGB(255, 255, 255));

    wchar_t t[8]; swprintf(t, 8, L"%ld %%", (long)g_vol);
    RECT rt = {x2 + S(14), y - S(12), ancho - S(14), y + S(12)};
    SelectObject(dc, fEstado);
    SetTextColor(dc, COL_TEXTO);
    DrawTextW(dc, t, -1, &rt, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static int en_icono(int x, int y) {
    int cx = vol_izq() - S(22), cy = vol_y();
    return x >= cx - S(14) && x <= cx + S(14) && y >= cy - S(14) && y <= cy + S(14);
}

static int en_volumen(HWND h, int x, int y) {
    RECT rc; GetClientRect(h, &rc);
    int vy = vol_y();
    return x >= vol_izq() - S(12) && x <= vol_der(rc.right) + S(12) && y >= vy - S(16) && y <= vy + S(16);
}

static void vol_desde_x(HWND h, int x) {
    RECT rc; GetClientRect(h, &rc);
    int x1 = vol_izq(), x2 = vol_der(rc.right);
    int v = (int)lround((double)(x - x1) * VOL_MAX / (x2 - x1));
    if (v > 95 && v < 105) v = 100;
    v = v < 0 ? 0 : v > VOL_MAX ? VOL_MAX : v;
    if (v != g_vol) { g_vol = v; InvalidateRect(h, NULL, FALSE); }
}

static void vol_sumar(HWND h, int d) {
    int v = g_vol + d;
    v = v < 0 ? 0 : v > VOL_MAX ? VOL_MAX : v;
    if (v != g_vol) { g_vol = v; vol_guardar(); InvalidateRect(h, NULL, FALSE); }
}

static void bandeja_actualizar(int sonando) {
    if (!enBandeja) return;
    wchar_t tip[128];
    const wchar_t *e = txtEstado; if (wcsncmp(e, L"●  ", 3) == 0) e += 3;
    swprintf(tip, 128, L"AudioWire · %ls\n%ls", e, txtIP);
    if (sonando != iconoActivo || wcscmp(tip, tipActual)) {
        iconoActivo = sonando;
        wcscpy(tipActual, tip);
        nid.hIcon = sonando ? icoActivo : icoIdle;
        wcsncpy(nid.szTip, tip, 127); nid.szTip[127] = 0;
        nid.uFlags = NIF_ICON | NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

static void a_bandeja(HWND h) {
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = h;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_BANDEJA;
    nid.hIcon = modo ? icoActivo : icoIdle;
    wcscpy(nid.szTip, L"AudioWire");
    Shell_NotifyIconW(NIM_ADD, &nid);
    enBandeja = 1; iconoActivo = -1; tipActual[0] = 0;
    ShowWindow(h, SW_HIDE);
    bandeja_actualizar(modo);
}

static void restaurar(HWND h) {
    if (enBandeja) { Shell_NotifyIconW(NIM_DELETE, &nid); enBandeja = 0; }
    ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
}

static void menu_bandeja(HWND h) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_ABRIR, L"Abrir AudioWire");
    AppendMenuW(m, MF_STRING | (g_vol == 0 ? MF_CHECKED : 0), CMD_SILENCIAR, L"Silenciar");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_SALIR, L"Salir");
    SetMenuDefaultItem(m, CMD_ABRIR, FALSE);
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(h);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, h, NULL);
    PostMessageW(h, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void crear_fotogramas(void) {
    int k = (2 * dpi + 48) / 96; if (k < 1) k = 1;
    sprW = SPR_W * k; sprH = SPR_H * k;
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = sprW; bi.bmiHeader.biHeight = -sprH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    for (int m = 0; m < 2; m++)
        for (int f = 0; f < SPR_N; f++) {
            unsigned int *px;
            fotogramas[m][f] = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
            for (int y = 0; y < sprH; y++)
                for (int x = 0; x < sprW; x++)
                    px[y * sprW + x] = SPR[m][f][(y / k) * SPR_W + x / k];
        }
}

static HFONT fuente(int pt, int peso) {
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, peso, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void texto(HDC dc, HFONT f, COLORREF col, const wchar_t *t, int y, int alto, int ancho) {
    RECT r = {S(16), y, ancho - S(16), y + alto};
    SelectObject(dc, f);
    SetTextColor(dc, col);
    DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void pintar(HWND h) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    FillRect(memDC, &rc, brFondo);

    HDC sdc = CreateCompatibleDC(hdc);
    HGDIOBJ viejo = SelectObject(sdc, fotogramas[modo][cuadro]);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(memDC, (rc.right - sprW) / 2, S(18), sprW, sprH, sdc, 0, 0, sprW, sprH, bf);
    SelectObject(sdc, viejo); DeleteDC(sdc);

    SetBkMode(memDC, TRANSPARENT);
    int y = y_base();
    texto(memDC, fIP, COL_TEXTO, txtIP, y, S(44), rc.right);
    texto(memDC, fEstado, colEstado, txtEstado, y + S(46), S(24), rc.right);
    texto(memDC, fDisp, COL_SUAVE, txtDisp, y + S(72), S(20), rc.right);
    pintar_volumen(memDC, rc.right);

    BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
           ps.rcPaint.bottom - ps.rcPaint.top, memDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    EndPaint(h, &ps);
}

static void actualizar_estado(HWND h) {
    ULONGLONG ahora = GetTickCount64();
    wchar_t ip[32], est[96], disp[300]; COLORREF col;

    if (ahora - tIP > 5000 || !txtIP[0]) { ip_local(ip, 32); tIP = ahora; } else wcscpy(ip, txtIP);

    EnterCriticalSection(&cs);
    int n = nclientes;
    int sonando = n > 0 && ahora - g_ultimoEnvio < 1000;
    if (g_rate && wcsncmp(g_disp, L"No ", 3) != 0)
        swprintf(disp, 300, L"%ls  ·  %ld kHz", g_disp, (long)(g_rate / 1000));
    else wcscpy(disp, g_disp);
    int errRed = g_errRed[0] != 0;
    if (errRed) swprintf(est, 96, L"●  %ls", g_errRed);
    LeaveCriticalSection(&cs);

    wchar_t moviles[24];
    if (n == 1) wcscpy(moviles, L"1 móvil"); else swprintf(moviles, 24, L"%d móviles", n);
    if (errRed) col = COL_ERROR;
    else if (sonando) { swprintf(est, 96, L"●  Transmitiendo · %ls", moviles); col = COL_ACENTO; }
    else if (n) { swprintf(est, 96, L"●  Conectado · %ls", moviles); col = COL_TEXTO; }
    else { wcscpy(est, L"●  Esperando"); col = COL_SUAVE; }

    int nuevoModo = sonando ? 1 : 0;
    if (nuevoModo != modo) {
        modo = nuevoModo; cuadro = 0;
        SetTimer(h, T_ANIM, modo ? 90 : 140, NULL);
    }
    if (wcscmp(ip, txtIP) || wcscmp(est, txtEstado) || wcscmp(disp, txtDisp) || col != colEstado) {
        wcscpy(txtIP, ip); wcscpy(txtEstado, est); wcscpy(txtDisp, disp); colEstado = col;
        if (!enBandeja) InvalidateRect(h, NULL, FALSE);
    }
    bandeja_actualizar(sonando);
}

static LRESULT CALLBACK ventana(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HDC dc = GetDC(h);
        RECT rc; GetClientRect(h, &rc);
        memDC = CreateCompatibleDC(dc);
        memBmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        memViejo = SelectObject(memDC, memBmp);
        ReleaseDC(h, dc);
        actualizar_estado(h);
        SetTimer(h, T_ANIM, 140, NULL);
        SetTimer(h, T_ESTADO, 500, NULL);
        return 0;
    }
    case WM_TIMER:
        if (wp == T_ANIM) {
            if (enBandeja || IsIconic(h)) return 0;
            cuadro = (cuadro + 1) % SPR_N;
            RECT rc; GetClientRect(h, &rc);
            RECT r = {(rc.right - sprW) / 2, S(18), (rc.right + sprW) / 2, S(18) + sprH};
            InvalidateRect(h, &r, FALSE);
        } else actualizar_estado(h);
        return 0;
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (en_icono(x, y)) {
            if (g_vol) { volPrevio = g_vol; g_vol = 0; } else g_vol = volPrevio ? volPrevio : 100;
            vol_guardar(); InvalidateRect(h, NULL, FALSE);
        } else if (en_volumen(h, x, y)) { arrastrando = 1; SetCapture(h); vol_desde_x(h, x); InvalidateRect(h, NULL, FALSE); }
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (en_volumen(h, (short)LOWORD(lp), (short)HIWORD(lp))) { g_vol = 100; vol_guardar(); InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_MOUSEMOVE:
        if (arrastrando) vol_desde_x(h, (short)LOWORD(lp));
        return 0;
    case WM_LBUTTONUP:
        if (arrastrando) { arrastrando = 0; ReleaseCapture(); vol_guardar(); InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_CAPTURECHANGED:
        if (arrastrando) { arrastrando = 0; vol_guardar(); InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_MOUSEWHEEL:
        vol_sumar(h, (short)HIWORD(wp) > 0 ? 5 : -5);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_RIGHT || wp == VK_UP) vol_sumar(h, 5);
        else if (wp == VK_LEFT || wp == VK_DOWN) vol_sumar(h, -5);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            SetCursor(LoadCursor(NULL, en_volumen(h, p.x, p.y) || en_icono(p.x, p.y) || arrastrando ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED && !enBandeja) a_bandeja(h);
        return 0;
    case WM_BANDEJA:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) restaurar(h);
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) menu_bandeja(h);
        return 0;
    case WM_MOSTRAR:
        restaurar(h);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == CMD_ABRIR) restaurar(h);
        else if (LOWORD(wp) == CMD_SALIR) DestroyWindow(h);
        else if (LOWORD(wp) == CMD_SILENCIAR) {
            if (g_vol) { volPrevio = g_vol; g_vol = 0; } else g_vol = volPrevio ? volPrevio : 100;
            vol_guardar(); InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: pintar(h); return 0;
    case WM_DESTROY:
        if (enBandeja) Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_TASKBAR_CREADA && enBandeja) {
        Shell_NotifyIconW(NIM_ADD, &nid);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd;
    SetProcessDPIAware();
    HDC sdc = GetDC(NULL); dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(NULL, sdc);

    CreateMutexW(NULL, TRUE, L"AudioWire_instancia_unica");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND otra = FindWindowW(L"AudioWire", NULL);
        if (otra) { AllowSetForegroundWindow(ASFW_ANY); PostMessageW(otra, WM_MOSTRAR, 0, 0); }
        return 0;
    }

    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    InitializeCriticalSection(&cs);
    timeBeginPeriod(1);
    CreateThread(NULL, 0, hilo_aceptar, NULL, 0, NULL);
    CreateThread(NULL, 0, hilo_captura, NULL, 0, NULL);

    crear_fotogramas();
    fIP = fuente(24, FW_BOLD);
    fEstado = fuente(11, FW_NORMAL);
    fDisp = fuente(8, FW_NORMAL);
    vol_cargar();
    brFondo = CreateSolidBrush(COL_FONDO);

    WM_TASKBAR_CREADA = RegisterWindowMessageW(L"TaskbarCreated");
    int ci = GetSystemMetrics(SM_CXSMICON), cj = GetSystemMetrics(SM_CYSMICON);
    icoIdle = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, ci, cj, 0);
    icoActivo = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(2), IMAGE_ICON, ci, cj, 0);

    WNDCLASSEXW wc = {sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = ventana;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"AudioWire";
    RegisterClassExW(&wc);

    DWORD estilo = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, S(340), S(18) + sprH + S(8) + S(138)};
    AdjustWindowRect(&r, estilo, FALSE);
    int ancho = r.right - r.left, alto = r.bottom - r.top;
    HWND h = CreateWindowExW(0, L"AudioWire", L"AudioWire", estilo,
                             (GetSystemMetrics(SM_CXSCREEN) - ancho) / 2, (GetSystemMetrics(SM_CYSCREEN) - alto) / 2,
                             ancho, alto, NULL, NULL, inst, NULL);
    ShowWindow(h, show);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    ExitProcess(0);
}
