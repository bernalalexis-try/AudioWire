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
/* id y nombre llegan en el saludo de las apps nuevas ("SWXH" + id + "\n" + nombre). */
typedef struct { struct sockaddr_in dir; ULONGLONG visto; char id[17]; wchar_t nombre[48]; } Cliente;
static Cliente clientes[MAX_CLIENTES];
static int nclientes = 0;
static SOCKET g_sock = INVALID_SOCKET;
static DWORD g_seq = 0;
static volatile LONG g_rate = 0, g_canales = 0;
static volatile ULONGLONG g_ultimoEnvio = 0;
static wchar_t g_disp[256] = L"Iniciando…";
static wchar_t g_errRed[128] = L"";
static volatile LONG g_vol = 100;
#define VOL_MAX 150
#define MAX_DATOS 1200
#define CADUCIDAD 3500

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

/* Móviles por IP: desactivados (siguen conectados pero no reciben audio, solo
   durante esta sesión) y bloqueados (se ignoran; se guardan en el registro). */
#define MAX_LISTA 32
static ULONG pausados[MAX_LISTA]; static int npausados = 0;
static ULONG bloqueados[MAX_LISTA]; static int nbloqueados = 0;
typedef struct { ULONG ip; wchar_t nombre[64]; } Nombre;
static Nombre nombres[MAX_LISTA]; static int nnombres = 0;

static int en_lista(const ULONG *l, int n, ULONG ip) {
    for (int i = 0; i < n; i++) if (l[i] == ip) return i;
    return -1;
}
static void agregar_a(ULONG *l, int *n, ULONG ip) { if (en_lista(l, *n, ip) < 0 && *n < MAX_LISTA) l[(*n)++] = ip; }
static void quitar_de(ULONG *l, int *n, ULONG ip) { int i = en_lista(l, *n, ip); if (i >= 0) l[i] = l[--*n]; }

static void ip_texto(ULONG ip, wchar_t *out) {
    const unsigned char *b = (const unsigned char *)&ip;
    swprintf(out, 16, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static const wchar_t *nombre_de(ULONG ip) {
    for (int i = 0; i < nnombres; i++) if (nombres[i].ip == ip) return nombres[i].nombre;
    return NULL;
}

/* El nombre de red del móvil (si el router lo conoce), sin frenar la red. */
static DWORD WINAPI hilo_nombre(LPVOID arg) {
    ULONG ip = (ULONG)(ULONG_PTR)arg;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    wchar_t n[64];
    if (GetNameInfoW((struct sockaddr *)&a, sizeof a, n, 64, NULL, 0, NI_NAMEREQD) != 0) return 0;
    wchar_t *punto = wcschr(n, L'.'); if (punto) *punto = 0;
    EnterCriticalSection(&cs);
    for (int i = 0; i < nnombres; i++)
        if (nombres[i].ip == ip) { wcsncpy(nombres[i].nombre, n, 63); nombres[i].nombre[63] = 0; break; }
    LeaveCriticalSection(&cs);
    return 0;
}

static void bloqueados_cargar(void) {
    DWORD t = sizeof bloqueados;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\AudioWire", L"Bloqueados", RRF_RT_REG_BINARY, NULL, bloqueados, &t) == ERROR_SUCCESS)
        nbloqueados = (int)(t / sizeof bloqueados[0]);
}
static void bloqueados_guardar(void) {
    ULONG copia[MAX_LISTA];
    EnterCriticalSection(&cs);
    int n = nbloqueados;
    memcpy(copia, bloqueados, n * sizeof copia[0]);
    LeaveCriticalSection(&cs);
    if (n) RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\AudioWire", L"Bloqueados", REG_BINARY, copia, n * sizeof copia[0]);
    else RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\AudioWire", L"Bloqueados");
}

/* Nombres puestos desde el PC, por id del teléfono (o "ip:x.x.x.x" si la app no manda id).
   Se guardan en el registro como "clave<TAB>nombre". */
typedef struct { wchar_t clave[24]; wchar_t nombre[48]; } Alias;
static Alias alias[MAX_LISTA]; static int nalias = 0;

static void clave_de(const char *id, ULONG ip, wchar_t *out) {
    if (id && id[0]) swprintf(out, 24, L"%hs", id);
    else { wchar_t t[16]; ip_texto(ip, t); swprintf(out, 24, L"ip:%ls", t); }
}

static const wchar_t *alias_de(const wchar_t *clave) {
    for (int i = 0; i < nalias; i++) if (wcscmp(alias[i].clave, clave) == 0) return alias[i].nombre;
    return NULL;
}

static void alias_poner(const wchar_t *clave, const wchar_t *nombre) {
    int i = 0;
    while (i < nalias && wcscmp(alias[i].clave, clave)) i++;
    if (!nombre[0]) { if (i < nalias) alias[i] = alias[--nalias]; return; }
    if (i == nalias) { if (nalias == MAX_LISTA) return; nalias++; }
    wcsncpy(alias[i].clave, clave, 23); alias[i].clave[23] = 0;
    wcsncpy(alias[i].nombre, nombre, 47); alias[i].nombre[47] = 0;
}

static void alias_cargar(void) {
    wchar_t buf[MAX_LISTA * 80]; DWORD t = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\AudioWire", L"Alias", RRF_RT_REG_MULTI_SZ, NULL, buf, &t) != ERROR_SUCCESS) return;
    for (wchar_t *p = buf; *p && nalias < MAX_LISTA;) {
        size_t largo = wcslen(p);
        wchar_t *tab = wcschr(p, L'\t');
        if (tab) { *tab = 0; alias_poner(p, tab + 1); }
        p += largo + 1;
    }
}

static void alias_guardar(void) {
    wchar_t buf[MAX_LISTA * 80]; size_t k = 0;
    EnterCriticalSection(&cs);
    for (int i = 0; i < nalias; i++) {
        k += swprintf(buf + k, 80, L"%ls\t%ls", alias[i].clave, alias[i].nombre);
        buf[k++] = 0;
    }
    int n = nalias;
    LeaveCriticalSection(&cs);
    buf[k++] = 0;
    if (n) RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\AudioWire", L"Alias", REG_MULTI_SZ, buf, (DWORD)(k * sizeof buf[0]));
    else RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\AudioWire", L"Alias");
}

/* id y nombre del saludo de una app nueva; una vieja manda solo "SWXH". */
static void leer_saludo(Cliente *c, const char *b, int n) {
    if (n <= 4) return;
    const char *p = b + 4, *nl = memchr(p, '\n', (size_t)(n - 4));
    if (!nl) return;
    int li = 0;
    for (const char *q = p; q < nl && li < 16; q++)
        if ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')) c->id[li++] = *q;
    c->id[li] = 0;
    int w = MultiByteToWideChar(CP_UTF8, 0, nl + 1, (int)(b + n - nl - 1), c->nombre, 47);
    c->nombre[w > 0 ? w : 0] = 0;
}

static Cliente *cliente_de(ULONG ip) {
    for (int i = 0; i < nclientes; i++) if (clientes[i].dir.sin_addr.s_addr == ip) return &clientes[i];
    return NULL;
}

/* Cómo se muestra un móvil: el nombre puesto en el PC, el que manda el teléfono o el de la red.
   Vacío si no hay ninguno (se muestra solo la IP). Se llama dentro de cs. */
static void etiqueta(ULONG ip, wchar_t *out, int n) {
    const Cliente *c = cliente_de(ip);
    wchar_t clave[24]; clave_de(c ? c->id : NULL, ip, clave);
    const wchar_t *a = alias_de(clave), *r = nombre_de(ip);
    const wchar_t *t = a ? a : c && c->nombre[0] ? c->nombre : r ? r : L"";
    wcsncpy(out, t, n - 1); out[n - 1] = 0;
}

static void purgar(ULONGLONG ahora) {
    for (int i = 0; i < nclientes;) {
        if (ahora - clientes[i].visto > CADUCIDAD) clientes[i] = clientes[--nclientes];
        else i++;
    }
}

static int buscar(const struct sockaddr_in *d) {
    for (int i = 0; i < nclientes; i++)
        if (clientes[i].dir.sin_addr.s_addr == d->sin_addr.s_addr && clientes[i].dir.sin_port == d->sin_port) return i;
    return -1;
}

static void enviar(const short *pcm, int frames, int conSonido) {
    EnterCriticalSection(&cs);
    ULONGLONG ahora = GetTickCount64();
    purgar(ahora);
    int activos = 0;
    for (int i = 0; i < nclientes; i++)
        if (en_lista(pausados, npausados, clientes[i].dir.sin_addr.s_addr) < 0) activos++;
    if (activos && g_sock != INVALID_SOCKET) {
        if (conSonido) g_ultimoEnvio = ahora;
        int ch = (int)g_canales, maxF = MAX_DATOS / (ch * 2);
        DWORD rate = (DWORD)g_rate;
        char paq[12 + MAX_DATOS];
        paq[0] = 'S'; paq[1] = 'W'; paq[2] = 'A'; paq[3] = (char)ch;
        memcpy(paq + 4, &rate, 4);
        for (int off = 0; off < frames; off += maxF) {
            int f = frames - off < maxF ? frames - off : maxF;
            DWORD seq = g_seq++;
            memcpy(paq + 8, &seq, 4);
            memcpy(paq + 12, pcm + (size_t)off * ch, (size_t)f * ch * 2);
            for (int i = 0; i < nclientes; i++)
                if (en_lista(pausados, npausados, clientes[i].dir.sin_addr.s_addr) < 0)
                    sendto(g_sock, paq, 12 + f * ch * 2, 0, (struct sockaddr *)&clientes[i].dir, sizeof clientes[i].dir);
        }
    }
    LeaveCriticalSection(&cs);
}

static DWORD WINAPI hilo_red(LPVOID arg) {
    (void)arg;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&excl, sizeof excl);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(PUERTO);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&a, sizeof a)) {
        EnterCriticalSection(&cs);
        swprintf(g_errRed, 128, L"Puerto %d ocupado", PUERTO);
        LeaveCriticalSection(&cs);
        return 0;
    }
    BOOL no = FALSE; DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &no, sizeof no, NULL, 0, &ret, NULL, NULL);
    int tam = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *)&tam, sizeof tam);
    g_sock = s;

    for (;;) {
        char b[64];
        struct sockaddr_in de; int l = sizeof de;
        int n = recvfrom(s, b, sizeof b, 0, (struct sockaddr *)&de, &l);
        if (n < 4 || b[0] != 'S' || b[1] != 'W' || b[2] != 'X') { if (n < 0) Sleep(10); continue; }
        EnterCriticalSection(&cs);
        ULONGLONG ahora = GetTickCount64();
        ULONG ip = de.sin_addr.s_addr;
        int bloqueado = en_lista(bloqueados, nbloqueados, ip) >= 0;
        int i = buscar(&de);
        if (b[3] == 'H' && !bloqueado) {
            if (i >= 0) { clientes[i].visto = ahora; leer_saludo(&clientes[i], b, n); }
            else if (nclientes < MAX_CLIENTES) {
                Cliente *c = &clientes[nclientes++];
                c->dir = de; c->visto = ahora; c->id[0] = 0; c->nombre[0] = 0;
                leer_saludo(c, b, n);
                if (!nombre_de(ip) && nnombres < MAX_LISTA) {
                    nombres[nnombres].ip = ip; nombres[nnombres].nombre[0] = 0; nnombres++;
                    HANDLE t = CreateThread(NULL, 0, hilo_nombre, (LPVOID)(ULONG_PTR)ip, 0, NULL);
                    if (t) CloseHandle(t);
                }
            }
            sendto(s, "SWXP", 4, 0, (struct sockaddr *)&de, sizeof de);
        } else if (b[3] == 'B' && i >= 0) clientes[i] = clientes[--nclientes];
        LeaveCriticalSection(&cs);
        if (b[3] == 'D' && !bloqueado) {
            char r[80] = "SWXI";
            wchar_t nombre[MAX_COMPUTERNAME_LENGTH + 1]; DWORD tn = MAX_COMPUTERNAME_LENGTH + 1;
            if (!GetComputerNameW(nombre, &tn)) wcscpy(nombre, L"PC");
            int nb = WideCharToMultiByte(CP_UTF8, 0, nombre, -1, r + 4, sizeof r - 5, NULL, NULL);
            sendto(s, r, nb > 0 ? 4 + nb - 1 : 4, 0, (struct sockaddr *)&de, sizeof de);
        }
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

        int canales = wfx->nChannels, salida = canales > 2 ? 2 : canales;
        buf = (short *)malloc((size_t)tamBuf * canales * sizeof(short));
        EnterCriticalSection(&cs);
        g_rate = (LONG)wfx->nSamplesPerSec; g_canales = salida;
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
                if (canales > 2)
                    for (UINT32 f = 0; f < frames; f++) {
                        buf[f * 2] = buf[f * canales];
                        buf[f * 2 + 1] = buf[f * canales + 1];
                    }
                enviar(buf, (int)frames, !silencio);
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
#define CMD_DISP 100     /* + móvil * 3 + acción (0 activar/desactivar, 1 renombrar, 2 bloquear) */
#define CMD_DESBLOQ 200  /* + posición en la lista de bloqueados */
static ULONG menuIPs[MAX_CLIENTES], menuBloq[MAX_LISTA];
static int nmenuIPs = 0, nmenuBloq = 0, estadoClic = 0;
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

/* La línea de estado abre la lista de dispositivos cuando hay alguno. */
static int en_estado(HWND h, int x, int y) {
    RECT rc; GetClientRect(h, &rc);
    int ey = y_base() + S(46);
    return estadoClic && x >= S(16) && x <= rc.right - S(16) && y >= ey && y <= ey + S(24);
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
    wchar_t e[96]; wcscpy(e, wcsncmp(txtEstado, L"●  ", 3) == 0 ? txtEstado + 3 : txtEstado);
    wchar_t *flecha = wcsstr(e, L"  ▾"); if (flecha) *flecha = 0;
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

/* Un submenú por móvil conectado y, si hay, la lista de bloqueados para desbloquearlos. */
static void menu_dispositivos(HMENU m) {
    EnterCriticalSection(&cs);
    purgar(GetTickCount64());
    int n = 0;
    for (int i = 0; i < nclientes; i++) {
        ULONG ip = clientes[i].dir.sin_addr.s_addr;
        if (en_lista(menuIPs, n, ip) >= 0) continue;
        menuIPs[n] = ip;
        wchar_t ipt[16], nom[48], t[128];
        ip_texto(ip, ipt);
        etiqueta(ip, nom, 48);
        int pausa = en_lista(pausados, npausados, ip) >= 0;
        const wchar_t *marca = pausa ? L"  ·  desactivado" : L"";
        if (nom[0]) swprintf(t, 128, L"%ls  (%ls)%ls", nom, ipt, marca);
        else swprintf(t, 128, L"%ls%ls", ipt, marca);
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING, CMD_DISP + n * 3, pausa ? L"Activar audio" : L"Desactivar audio");
        AppendMenuW(sub, MF_STRING, CMD_DISP + n * 3 + 1, L"Renombrar…");
        AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
        AppendMenuW(sub, MF_STRING, CMD_DISP + n * 3 + 2, L"Bloquear");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, t);
        n++;
    }
    nmenuIPs = n;
    if (!n) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Ningún móvil conectado");
    nmenuBloq = nbloqueados;
    if (nbloqueados) {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        HMENU sub = CreatePopupMenu();
        for (int j = 0; j < nbloqueados; j++) {
            menuBloq[j] = bloqueados[j];
            wchar_t ipt[16], nom[48], t[96];
            ip_texto(bloqueados[j], ipt);
            etiqueta(bloqueados[j], nom, 48);
            if (nom[0]) swprintf(t, 96, L"Desbloquear %ls  (%ls)", nom, ipt);
            else swprintf(t, 96, L"Desbloquear %ls", ipt);
            AppendMenuW(sub, MF_STRING, CMD_DESBLOQ + j, t);
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"Bloqueados");
    }
    LeaveCriticalSection(&cs);
}

static void menu_ventana(HWND h) {
    HMENU m = CreatePopupMenu();
    menu_dispositivos(m);
    POINT p; GetCursorPos(&p);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, 0, h, NULL);
    DestroyMenu(m);
}

static void actualizar_estado(HWND h);

/* Cuadro para escribir un nombre. Devuelve 1 con "Guardar" (texto vacío = volver al original). */
static int dlgOk, dlgFin;
static wchar_t dlgTexto[48];

static LRESULT CALLBACK ventana_nombre(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) { GetDlgItemTextW(h, 10, dlgTexto, 48); dlgOk = 1; DestroyWindow(h); }
        else if (LOWORD(wp) == IDCANCEL) DestroyWindow(h);
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: dlgFin = 1; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static int pedir_nombre(HWND dueno, const wchar_t *titulo, wchar_t *texto) {
    static int registrada = 0;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registrada) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.lpfnWndProc = ventana_nombre;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"AudioWireNombre";
        RegisterClassExW(&wc);
        registrada = 1;
    }
    DWORD estilo = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT r = {0, 0, S(320), S(118)};
    AdjustWindowRect(&r, estilo, FALSE);
    int an = r.right - r.left, al = r.bottom - r.top;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AudioWireNombre", titulo, estilo,
                               (GetSystemMetrics(SM_CXSCREEN) - an) / 2, (GetSystemMetrics(SM_CYSCREEN) - al) / 2,
                               an, al, IsWindowVisible(dueno) ? dueno : NULL, NULL, inst, NULL);
    HWND et = CreateWindowExW(0, L"STATIC", L"Nombre para este dispositivo (vacío = el original):", WS_CHILD | WS_VISIBLE,
                              S(14), S(12), S(292), S(20), dlg, NULL, inst, NULL);
    HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", texto, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                              S(14), S(36), S(292), S(26), dlg, (HMENU)10, inst, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"Guardar", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              S(122), S(76), S(88), S(28), dlg, (HMENU)IDOK, inst, NULL);
    HWND no = CreateWindowExW(0, L"BUTTON", L"Cancelar", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              S(218), S(76), S(88), S(28), dlg, (HMENU)IDCANCEL, inst, NULL);
    HFONT f = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HWND hijos[] = {et, ed, ok, no};
    for (int i = 0; i < 4; i++) SendMessageW(hijos[i], WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(ed, EM_LIMITTEXT, 47, 0);
    SendMessageW(ed, EM_SETSEL, 0, -1);

    dlgOk = 0; dlgFin = 0;
    if (IsWindowVisible(dueno)) EnableWindow(dueno, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetFocus(ed);
    MSG m; int salir = 0;
    while (!dlgFin) {
        if (GetMessageW(&m, NULL, 0, 0) <= 0) { salir = 1; break; }
        if (!IsDialogMessageW(dlg, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    EnableWindow(dueno, TRUE);
    if (IsWindowVisible(dueno)) SetForegroundWindow(dueno);
    DeleteObject(f);
    if (salir) PostQuitMessage((int)m.wParam);
    if (dlgOk) { wcsncpy(texto, dlgTexto, 47); texto[47] = 0; }
    return dlgOk;
}

static void renombrar(HWND h, ULONG ip) {
    wchar_t clave[24], actual[48], ipt[16], titulo[64];
    EnterCriticalSection(&cs);
    const Cliente *c = cliente_de(ip);
    clave_de(c ? c->id : NULL, ip, clave);
    etiqueta(ip, actual, 48);
    LeaveCriticalSection(&cs);
    ip_texto(ip, ipt);
    swprintf(titulo, 64, L"Renombrar %ls", ipt);
    if (!pedir_nombre(h, titulo, actual)) return;
    wchar_t *t = actual; while (*t == L' ') t++;  /* sin espacios en los extremos */
    size_t l = wcslen(t); while (l && t[l - 1] == L' ') t[--l] = 0;
    EnterCriticalSection(&cs);
    alias_poner(clave, t);
    LeaveCriticalSection(&cs);
    alias_guardar();
}

static void dispositivo_accion(HWND h, int k, int accion) {
    EnterCriticalSection(&cs);
    if (k >= nmenuIPs) { LeaveCriticalSection(&cs); return; }
    ULONG ip = menuIPs[k];
    wchar_t ipt[16], nom[48], quien[96];
    ip_texto(ip, ipt);
    etiqueta(ip, nom, 48);
    if (nom[0]) swprintf(quien, 96, L"%ls (%ls)", nom, ipt); else wcscpy(quien, ipt);
    if (accion == 0) {
        if (en_lista(pausados, npausados, ip) >= 0) quitar_de(pausados, &npausados, ip);
        else agregar_a(pausados, &npausados, ip);
        LeaveCriticalSection(&cs);
    } else if (accion == 1) {
        LeaveCriticalSection(&cs);
        renombrar(h, ip);
    } else {
        LeaveCriticalSection(&cs);
        wchar_t msg[200];
        swprintf(msg, 200, L"¿Bloquear %ls?\n\nNo va a poder conectarse hasta que lo desbloquees desde Dispositivos.", quien);
        if (MessageBoxW(h, msg, L"AudioWire", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        EnterCriticalSection(&cs);
        /* Así la lista de bloqueados sigue mostrando su nombre aunque ya no esté conectado. */
        wchar_t clave[24]; clave_de(NULL, ip, clave);
        if (nom[0] && !alias_de(clave)) alias_poner(clave, nom);
        agregar_a(bloqueados, &nbloqueados, ip);
        quitar_de(pausados, &npausados, ip);
        for (int i = 0; i < nclientes;)
            if (clientes[i].dir.sin_addr.s_addr == ip) clientes[i] = clientes[--nclientes]; else i++;
        LeaveCriticalSection(&cs);
        bloqueados_guardar();
        alias_guardar();
    }
    actualizar_estado(h);
}

static void desbloquear(HWND h, int j) {
    EnterCriticalSection(&cs);
    if (j < nmenuBloq) quitar_de(bloqueados, &nbloqueados, menuBloq[j]);
    LeaveCriticalSection(&cs);
    bloqueados_guardar();
    actualizar_estado(h);
}

static void menu_bandeja(HWND h) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_ABRIR, L"Abrir AudioWire");
    AppendMenuW(m, MF_STRING | (g_vol == 0 ? MF_CHECKED : 0), CMD_SILENCIAR, L"Silenciar");
    HMENU d = CreatePopupMenu();
    menu_dispositivos(d);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)d, L"Dispositivos");
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
    purgar(ahora);
    int n = nclientes;
    int sonando = n > 0 && ahora - g_ultimoEnvio < 1000;
    if (g_rate && wcsncmp(g_disp, L"No ", 3) != 0)
        swprintf(disp, 300, L"%ls  ·  %ld kHz", g_disp, (long)(g_rate / 1000));
    else wcscpy(disp, g_disp);
    int errRed = g_errRed[0] != 0;
    if (errRed) swprintf(est, 96, L"●  %ls", g_errRed);
    int hayBloq = nbloqueados > 0;
    LeaveCriticalSection(&cs);

    wchar_t moviles[24];
    if (n == 1) wcscpy(moviles, L"1 móvil"); else swprintf(moviles, 24, L"%d móviles", n);
    if (errRed) col = COL_ERROR;
    else if (sonando) { swprintf(est, 96, L"●  Transmitiendo · %ls", moviles); col = COL_ACENTO; }
    else if (n) { swprintf(est, 96, L"●  Conectado · %ls", moviles); col = COL_TEXTO; }
    else { wcscpy(est, L"●  Esperando"); col = COL_SUAVE; }
    estadoClic = !errRed && (n || hayBloq);
    if (estadoClic) wcscat(est, L"  ▾");

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
        else if (en_estado(h, x, y)) menu_ventana(h);
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
            SetCursor(LoadCursor(NULL, en_volumen(h, p.x, p.y) || en_icono(p.x, p.y) || en_estado(h, p.x, p.y) || arrastrando ? IDC_HAND : IDC_ARROW));
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
        } else if (LOWORD(wp) >= CMD_DISP && LOWORD(wp) < CMD_DISP + MAX_CLIENTES * 3)
            dispositivo_accion(h, (LOWORD(wp) - CMD_DISP) / 3, (LOWORD(wp) - CMD_DISP) % 3);
        else if (LOWORD(wp) >= CMD_DESBLOQ && LOWORD(wp) < CMD_DESBLOQ + MAX_LISTA)
            desbloquear(h, LOWORD(wp) - CMD_DESBLOQ);
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
    bloqueados_cargar();
    alias_cargar();
    timeBeginPeriod(1);
    CreateThread(NULL, 0, hilo_red, NULL, 0, NULL);
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
