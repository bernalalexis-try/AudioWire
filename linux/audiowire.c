#include <gtk/gtk.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include "sprites.h"

#define PUERTO 5005
#define MAX_CLIENTES 16
#define RATE 48000
#define CANALES 2
#define BLOQUE 480
#define VOL_MAX 150

#define ANCHO 340
#define ESCALA 2
#define SPR_Y 18
#define Y_BASE (SPR_Y + SPR_H * ESCALA + 8)
#define VOL_Y (Y_BASE + 112)
#define ALTO (Y_BASE + 138)
#define VOL_IZQ 54
#define VOL_DER (ANCHO - 72)

#define COL_FONDO  0xfaf7f2
#define COL_TEXTO  0x2b2622
#define COL_SUAVE  0x9a918a
#define COL_ACENTO 0xf0651e
#define COL_FUERTE 0xd64514
#define COL_ERROR  0xc0392b
#define COL_PISTA  0xe5ddd2
#define COL_MARCA  0xcdc4ba
#define COL_SOMBRA 0xece4da

#define MAX_DATOS 1200
#define CADUCIDAD 3500

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
typedef struct { struct sockaddr_in dir; long long visto; } Cliente;
static Cliente clientes[MAX_CLIENTES];
static int nclientes = 0;
static int g_sock = -1;
static uint32_t g_seq = 0;
static atomic_int g_vol = 100;
static atomic_llong g_ultimoSonido = 0;
static char g_sink[256] = "";
static int g_sinkGen = 0;
static char g_disp[256] = "Iniciando…";
static char g_errAudio[128] = "";
static char g_errRed[64] = "";

static long long ahora_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void purgar(long long ahora) {
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

static void poner32(unsigned char *p, uint32_t v) {
    p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255;
}

static void enviar(const int16_t *pcm, int frames, int sonido) {
    pthread_mutex_lock(&mx);
    long long ahora = ahora_ms();
    purgar(ahora);
    if (nclientes && g_sock >= 0) {
        if (sonido) g_ultimoSonido = ahora;
        int maxF = MAX_DATOS / (CANALES * 2);
        unsigned char paq[12 + MAX_DATOS] = {'S', 'W', 'A', CANALES};
        poner32(paq + 4, RATE);
        for (int off = 0; off < frames; off += maxF) {
            int f = frames - off < maxF ? frames - off : maxF;
            poner32(paq + 8, g_seq++);
            memcpy(paq + 12, pcm + (size_t)off * CANALES, (size_t)f * CANALES * 2);
            for (int i = 0; i < nclientes; i++)
                sendto(g_sock, paq, 12 + f * CANALES * 2, MSG_DONTWAIT, (struct sockaddr *)&clientes[i].dir,
                       sizeof clientes[i].dir);
        }
    }
    pthread_mutex_unlock(&mx);
}

static void *hilo_red(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(PUERTO);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&a, sizeof a)) {
        pthread_mutex_lock(&mx);
        snprintf(g_errRed, sizeof g_errRed, "Puerto %d ocupado", PUERTO);
        pthread_mutex_unlock(&mx);
        return NULL;
    }
    int tam = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &tam, sizeof tam);
    g_sock = s;
    for (;;) {
        char b[64];
        struct sockaddr_in de;
        socklen_t l = sizeof de;
        ssize_t n = recvfrom(s, b, sizeof b, 0, (struct sockaddr *)&de, &l);
        if (n < 4 || b[0] != 'S' || b[1] != 'W' || b[2] != 'X') {
            if (n < 0) usleep(10000);
            continue;
        }
        pthread_mutex_lock(&mx);
        long long ahora = ahora_ms();
        int i = buscar(&de);
        if (b[3] == 'H') {
            if (i >= 0) clientes[i].visto = ahora;
            else if (nclientes < MAX_CLIENTES) {
                clientes[nclientes].dir = de;
                clientes[nclientes].visto = ahora;
                nclientes++;
            }
            sendto(s, "SWXP", 4, MSG_DONTWAIT, (struct sockaddr *)&de, sizeof de);
        } else if (b[3] == 'B' && i >= 0) clientes[i] = clientes[--nclientes];
        pthread_mutex_unlock(&mx);
        if (b[3] == 'D') {
            char r[80] = "SWXI";
            if (gethostname(r + 4, sizeof r - 5) != 0) snprintf(r + 4, sizeof r - 4, "PC");
            r[sizeof r - 1] = 0;
            sendto(s, r, strlen(r), MSG_DONTWAIT, (struct sockaddr *)&de, sizeof de);
        }
    }
    return NULL;
}

static void ip_local(char *out, size_t n) {
    snprintf(out, n, "Sin red");
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(1);
    a.sin_addr.s_addr = inet_addr("10.255.255.255");
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        struct sockaddr_in yo;
        socklen_t len = sizeof yo;
        if (getsockname(s, (struct sockaddr *)&yo, &len) == 0) inet_ntop(AF_INET, &yo.sin_addr, out, (socklen_t)n);
    }
    close(s);
}

static void cb_servidor(pa_context *c, const pa_server_info *i, void *u) {
    (void)c;
    if (i && i->default_sink_name) snprintf((char *)u, 256, "%s", i->default_sink_name);
}

static void cb_sink(pa_context *c, const pa_sink_info *i, int eol, void *u) {
    (void)c;
    if (eol == 0 && i) snprintf((char *)u, 256, "%s", i->description ? i->description : i->name);
}

static int esperar_op(pa_mainloop *m, pa_operation *op) {
    if (!op) return 0;
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        if (pa_mainloop_iterate(m, 1, NULL) < 0) {
            pa_operation_unref(op);
            return 0;
        }
    }
    pa_operation_unref(op);
    return 1;
}

static void *hilo_info(void *arg) {
    (void)arg;
    for (;;) {
        pa_mainloop *m = pa_mainloop_new();
        pa_context *c = pa_context_new(pa_mainloop_get_api(m), "AudioWire");
        int ok = c && pa_context_connect(c, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) >= 0;
        while (ok) {
            pa_context_state_t st = pa_context_get_state(c);
            if (st == PA_CONTEXT_READY) break;
            if (!PA_CONTEXT_IS_GOOD(st) || pa_mainloop_iterate(m, 1, NULL) < 0) ok = 0;
        }
        if (!ok) {
            pthread_mutex_lock(&mx);
            snprintf(g_disp, sizeof g_disp, "No se encontró el servidor de sonido");
            pthread_mutex_unlock(&mx);
        }
        while (ok) {
            char sink[256] = "", desc[256] = "";
            if (!esperar_op(m, pa_context_get_server_info(c, cb_servidor, sink))) break;
            if (sink[0]) esperar_op(m, pa_context_get_sink_info_by_name(c, sink, cb_sink, desc));
            pthread_mutex_lock(&mx);
            if (sink[0] && strcmp(sink, g_sink) != 0) {
                snprintf(g_sink, sizeof g_sink, "%s", sink);
                g_sinkGen++;
            }
            if (desc[0]) snprintf(g_disp, sizeof g_disp, "%s", desc);
            pthread_mutex_unlock(&mx);
            for (int i = 0; i < 20 && pa_context_get_state(c) == PA_CONTEXT_READY; i++) {
                pa_mainloop_iterate(m, 0, NULL);
                usleep(100000);
            }
            if (pa_context_get_state(c) != PA_CONTEXT_READY) break;
        }
        if (c) {
            pa_context_disconnect(c);
            pa_context_unref(c);
        }
        pa_mainloop_free(m);
        sleep(2);
    }
    return NULL;
}

static void *hilo_captura(void *arg) {
    (void)arg;
    int16_t buf[BLOQUE * CANALES];
    for (;;) {
        char dev[300] = "";
        int gen = 0;
        for (int i = 0; i < 30; i++) {
            pthread_mutex_lock(&mx);
            gen = g_sinkGen;
            if (g_sink[0]) snprintf(dev, sizeof dev, "%s.monitor", g_sink);
            pthread_mutex_unlock(&mx);
            if (dev[0]) break;
            usleep(100000);
        }
        pa_sample_spec ss = {PA_SAMPLE_S16LE, RATE, CANALES};
        pa_buffer_attr at = {(uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, sizeof buf};
        int err = 0;
        pa_simple *s = pa_simple_new(NULL, "AudioWire", PA_STREAM_RECORD, dev[0] ? dev : "@DEFAULT_MONITOR@",
                                     "Captura", &ss, NULL, &at, &err);
        pthread_mutex_lock(&mx);
        if (s) g_errAudio[0] = 0;
        else snprintf(g_errAudio, sizeof g_errAudio, "No se pudo capturar el audio");
        pthread_mutex_unlock(&mx);
        if (!s) {
            sleep(1);
            continue;
        }
        for (;;) {
            if (pa_simple_read(s, buf, sizeof buf, &err) < 0) break;
            float g = g_vol / 100.0f;
            int sonido = 0;
            for (int i = 0; i < BLOQUE * CANALES; i++) {
                int v = buf[i];
                if (v > 64 || v < -64) sonido = 1;
                float f = v * g;
                buf[i] = (int16_t)(f > 32767.f ? 32767 : f < -32768.f ? -32768 : (int)f);
            }
            enviar(buf, BLOQUE, sonido && g_vol > 0);
            pthread_mutex_lock(&mx);
            int cambio = g_sinkGen != gen;
            pthread_mutex_unlock(&mx);
            if (cambio) break;
        }
        pa_simple_free(s);
    }
    return NULL;
}

static GtkWidget *ventana, *lienzo;
static GtkStatusIcon *icono;
static cairo_surface_t *superficies[2][SPR_N];
static GdkPixbuf *pbIdle, *pbActivo;
static int modo = 0, cuadro = 0, minimizada = 0, arrastrando = 0, manoPuesta = 0;
static int volPrevio = 100;
static guint idAnim = 0;
static char txtIP[64] = "", txtEstado[160] = "", txtDisp[300] = "";
static unsigned colEstado = COL_SUAVE;
static long long tIP = -100000;

static void color(cairo_t *cr, unsigned c) {
    cairo_set_source_rgb(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
}

static char *ruta_config(void) {
    return g_build_filename(g_get_user_config_dir(), "audiowire", "volumen", NULL);
}

static void vol_cargar(void) {
    char *ruta = ruta_config(), *txt = NULL;
    if (g_file_get_contents(ruta, &txt, NULL, NULL)) {
        int v = atoi(txt);
        if (v >= 0 && v <= VOL_MAX) g_vol = v;
        g_free(txt);
    }
    g_free(ruta);
}

static void vol_guardar(void) {
    char *ruta = ruta_config(), *dir = g_path_get_dirname(ruta), txt[16];
    g_mkdir_with_parents(dir, 0755);
    snprintf(txt, sizeof txt, "%d\n", (int)g_vol);
    g_file_set_contents(ruta, txt, -1, NULL);
    g_free(dir);
    g_free(ruta);
}

static GdkPixbuf *pixbuf_cuadrado(int m) {
    const unsigned *p = SPR[m][0];
    int x0 = SPR_W, y0 = SPR_H, x1 = -1, y1 = -1;
    for (int y = 0; y < SPR_H; y++)
        for (int x = 0; x < SPR_W; x++)
            if (p[y * SPR_W + x] >> 24) {
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
    int w = x1 - x0 + 1, h = y1 - y0 + 1, lado = w > h ? w : h;
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, lado, lado);
    gdk_pixbuf_fill(pb, 0);
    guchar *px = gdk_pixbuf_get_pixels(pb);
    int rs = gdk_pixbuf_get_rowstride(pb), ox = (lado - w) / 2, oy = (lado - h) / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned v = p[(y + y0) * SPR_W + x + x0], a = v >> 24;
            guchar *d = px + (y + oy) * rs + (x + ox) * 4;
            if (a) {
                d[0] = (guchar)(((v >> 16) & 255) * 255 / a);
                d[1] = (guchar)(((v >> 8) & 255) * 255 / a);
                d[2] = (guchar)((v & 255) * 255 / a);
            }
            d[3] = (guchar)a;
        }
    GdkPixbuf *grande = gdk_pixbuf_scale_simple(pb, lado * 4, lado * 4, GDK_INTERP_NEAREST);
    g_object_unref(pb);
    return grande;
}

static void texto(cairo_t *cr, const char *t, const char *fuente, unsigned col, int y, int alto, int x1, int x2,
                  PangoAlignment al) {
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(fuente);
    pango_layout_set_font_description(l, fd);
    pango_layout_set_text(l, t, -1);
    if (al != PANGO_ALIGN_RIGHT) {
        pango_layout_set_width(l, (x2 - x1) * PANGO_SCALE);
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
        pango_layout_set_alignment(l, al);
    }
    int lw, lh;
    pango_layout_get_pixel_size(l, &lw, &lh);
    color(cr, col);
    cairo_move_to(cr, al == PANGO_ALIGN_RIGHT ? x2 - lw : x1, y + (alto - lh) / 2.0);
    pango_cairo_show_layout(cr, l);
    pango_font_description_free(fd);
    g_object_unref(l);
}

static void altavoz(cairo_t *cr, double cx, double cy, double t, int ondas, unsigned col) {
    double u = t / 24.0, ox = cx - t / 2, oy = cy - t / 2;
    color(cr, col);
    cairo_move_to(cr, ox + 3 * u, oy + 9 * u);
    cairo_line_to(cr, ox + 7 * u, oy + 9 * u);
    cairo_line_to(cr, ox + 12 * u, oy + 4.5 * u);
    cairo_line_to(cr, ox + 12 * u, oy + 19.5 * u);
    cairo_line_to(cr, ox + 7 * u, oy + 15 * u);
    cairo_line_to(cr, ox + 3 * u, oy + 15 * u);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_set_line_width(cr, 2.2 * u);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (ondas == 0) {
        cairo_move_to(cr, ox + 15.5 * u, oy + 9 * u);
        cairo_line_to(cr, ox + 21.5 * u, oy + 15 * u);
        cairo_move_to(cr, ox + 21.5 * u, oy + 9 * u);
        cairo_line_to(cr, ox + 15.5 * u, oy + 15 * u);
        cairo_stroke(cr);
    }
    for (int w = 1; w <= ondas; w++) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, ox + 12 * u, oy + 12 * u, (w == 1 ? 4.5 : 8.5) * u, -0.72, 0.72);
        cairo_stroke(cr);
    }
}

static void circulo(cairo_t *cr, double x, double y, double r, unsigned col) {
    color(cr, col);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x, y, r, 0, 2 * G_PI);
    cairo_fill(cr);
}

static void linea(cairo_t *cr, double x1, double x2, double y, double grosor, unsigned col) {
    if (x2 < x1) return;
    color(cr, col);
    cairo_set_line_width(cr, grosor);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, x1, y);
    cairo_line_to(cr, x2, y);
    cairo_stroke(cr);
}

static void pintar_volumen(cairo_t *cr) {
    int v = g_vol;
    double y = VOL_Y + 0.5;
    double xv = VOL_IZQ + (VOL_DER - VOL_IZQ) * (double)v / VOL_MAX;
    double x100 = VOL_IZQ + (VOL_DER - VOL_IZQ) * 100.0 / VOL_MAX;
    unsigned acento = v > 100 ? COL_FUERTE : COL_ACENTO;

    altavoz(cr, VOL_IZQ - 22, y, 22, v == 0 ? 0 : v < 50 ? 1 : 2, v ? COL_TEXTO : COL_SUAVE);
    linea(cr, VOL_IZQ, VOL_DER, y, 4, COL_PISTA);
    color(cr, COL_MARCA);
    cairo_rectangle(cr, floor(x100), VOL_Y - 7, 1, 3);
    cairo_fill(cr);
    linea(cr, VOL_IZQ, xv, y, 4, acento);

    double r = arrastrando ? 9 : 8;
    circulo(cr, xv, y + 0.5, r + 1, COL_SOMBRA);
    circulo(cr, xv, y, r, acento);
    circulo(cr, xv, y, r - 3, 0xffffff);

    char t[16];
    snprintf(t, sizeof t, "%d %%", v);
    texto(cr, t, "Sans 11", COL_TEXTO, VOL_Y - 12, 24, VOL_DER + 14, ANCHO - 14, PANGO_ALIGN_RIGHT);
}

static gboolean al_dibujar(GtkWidget *w, cairo_t *cr, gpointer u) {
    (void)w; (void)u;
    color(cr, COL_FONDO);
    cairo_paint(cr);

    cairo_save(cr);
    cairo_translate(cr, (ANCHO - SPR_W * ESCALA) / 2, SPR_Y);
    cairo_scale(cr, ESCALA, ESCALA);
    cairo_set_source_surface(cr, superficies[modo][cuadro], 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);

    texto(cr, txtIP, "Sans Bold 24", COL_TEXTO, Y_BASE, 44, 16, ANCHO - 16, PANGO_ALIGN_CENTER);
    texto(cr, txtEstado, "Sans 11", colEstado, Y_BASE + 46, 24, 16, ANCHO - 16, PANGO_ALIGN_CENTER);
    texto(cr, txtDisp, "Sans 8", COL_SUAVE, Y_BASE + 72, 20, 16, ANCHO - 16, PANGO_ALIGN_CENTER);
    pintar_volumen(cr);
    return TRUE;
}

static gboolean animar(gpointer u) {
    (void)u;
    if (minimizada || !gtk_widget_get_visible(ventana)) return G_SOURCE_CONTINUE;
    cuadro = (cuadro + 1) % SPR_N;
    gtk_widget_queue_draw_area(lienzo, (ANCHO - SPR_W * ESCALA) / 2, SPR_Y, SPR_W * ESCALA, SPR_H * ESCALA);
    return G_SOURCE_CONTINUE;
}

static void actualizar_estado(void) {
    long long ahora = ahora_ms();
    char ip[64], est[160], disp[300];
    unsigned col;

    if (ahora - tIP > 5000) {
        ip_local(ip, sizeof ip);
        tIP = ahora;
    } else snprintf(ip, sizeof ip, "%s", txtIP);

    pthread_mutex_lock(&mx);
    purgar(ahora);
    int n = nclientes;
    int sonando = n > 0 && ahora - g_ultimoSonido < 1000;
    int errRed = g_errRed[0] != 0;
    if (g_errAudio[0]) snprintf(disp, sizeof disp, "%s", g_errAudio);
    else if (g_sink[0]) snprintf(disp, sizeof disp, "%s  ·  %d kHz", g_disp, RATE / 1000);
    else snprintf(disp, sizeof disp, "%s", g_disp);
    if (errRed) snprintf(est, sizeof est, "●  %s", g_errRed);
    pthread_mutex_unlock(&mx);

    char moviles[32];
    if (n == 1) snprintf(moviles, sizeof moviles, "1 móvil");
    else snprintf(moviles, sizeof moviles, "%d móviles", n);
    if (errRed) col = COL_ERROR;
    else if (sonando) { snprintf(est, sizeof est, "●  Transmitiendo · %s", moviles); col = COL_ACENTO; }
    else if (n) { snprintf(est, sizeof est, "●  Conectado · %s", moviles); col = COL_TEXTO; }
    else { snprintf(est, sizeof est, "●  Esperando"); col = COL_SUAVE; }

    int nuevo = sonando ? 1 : 0;
    if (nuevo != modo) {
        modo = nuevo;
        cuadro = 0;
        if (idAnim) g_source_remove(idAnim);
        idAnim = g_timeout_add(modo ? 90 : 140, animar, NULL);
        gtk_status_icon_set_from_pixbuf(icono, modo ? pbActivo : pbIdle);
    }

    if (strcmp(ip, txtIP) || strcmp(est, txtEstado) || strcmp(disp, txtDisp) || col != colEstado) {
        snprintf(txtIP, sizeof txtIP, "%s", ip);
        snprintf(txtEstado, sizeof txtEstado, "%s", est);
        snprintf(txtDisp, sizeof txtDisp, "%s", disp);
        colEstado = col;
        char tip[256];
        snprintf(tip, sizeof tip, "AudioWire · %s\n%s", strncmp(est, "●  ", 5) == 0 ? est + 5 : est, ip);
        gtk_status_icon_set_tooltip_text(icono, tip);
        gtk_widget_queue_draw(lienzo);
    }
}

static gboolean al_temporizador(gpointer u) {
    (void)u;
    actualizar_estado();
    return G_SOURCE_CONTINUE;
}

static void redibujar(void) {
    gtk_widget_queue_draw(lienzo);
}

static void vol_poner(int v, int guardar) {
    v = v < 0 ? 0 : v > VOL_MAX ? VOL_MAX : v;
    if (v != g_vol) {
        g_vol = v;
        redibujar();
    }
    if (guardar) vol_guardar();
}

static void alternar_silencio(void) {
    if (g_vol) {
        volPrevio = g_vol;
        vol_poner(0, 1);
    } else vol_poner(volPrevio ? volPrevio : 100, 1);
}

static int en_icono(double x, double y) {
    return fabs(x - (VOL_IZQ - 22)) <= 14 && fabs(y - VOL_Y) <= 14;
}

static int en_volumen(double x, double y) {
    return x >= VOL_IZQ - 12 && x <= VOL_DER + 12 && fabs(y - VOL_Y) <= 16;
}

static void vol_desde_x(double x) {
    int v = (int)lround((x - VOL_IZQ) * VOL_MAX / (VOL_DER - VOL_IZQ));
    if (v > 95 && v < 105) v = 100;
    vol_poner(v, 0);
}

static gboolean al_pulsar(GtkWidget *w, GdkEventButton *e, gpointer u) {
    (void)w; (void)u;
    if (e->button != 1) return FALSE;
    if (e->type == GDK_2BUTTON_PRESS) {
        if (en_volumen(e->x, e->y)) vol_poner(100, 1);
        return TRUE;
    }
    if (e->type != GDK_BUTTON_PRESS) return TRUE;
    if (en_icono(e->x, e->y)) alternar_silencio();
    else if (en_volumen(e->x, e->y)) {
        arrastrando = 1;
        vol_desde_x(e->x);
        redibujar();
    }
    return TRUE;
}

static gboolean al_soltar(GtkWidget *w, GdkEventButton *e, gpointer u) {
    (void)w; (void)u;
    if (e->button == 1 && arrastrando) {
        arrastrando = 0;
        vol_guardar();
        redibujar();
    }
    return TRUE;
}

static gboolean al_mover(GtkWidget *w, GdkEventMotion *e, gpointer u) {
    (void)u;
    if (arrastrando) vol_desde_x(e->x);
    int mano = arrastrando || en_volumen(e->x, e->y) || en_icono(e->x, e->y);
    if (mano != manoPuesta) {
        manoPuesta = mano;
        GdkWindow *gw = gtk_widget_get_window(w);
        GdkCursor *c = mano ? gdk_cursor_new_from_name(gdk_window_get_display(gw), "pointer") : NULL;
        gdk_window_set_cursor(gw, c);
        if (c) g_object_unref(c);
    }
    return TRUE;
}

static gboolean al_rueda(GtkWidget *w, GdkEventScroll *e, gpointer u) {
    (void)w; (void)u;
    int d = 0;
    if (e->direction == GDK_SCROLL_UP) d = 5;
    else if (e->direction == GDK_SCROLL_DOWN) d = -5;
    else if (e->direction == GDK_SCROLL_SMOOTH && e->delta_y != 0) d = e->delta_y < 0 ? 5 : -5;
    if (d) vol_poner(g_vol + d, 1);
    return TRUE;
}

static gboolean al_tecla(GtkWidget *w, GdkEventKey *e, gpointer u) {
    (void)w; (void)u;
    if (e->keyval == GDK_KEY_Right || e->keyval == GDK_KEY_Up) vol_poner(g_vol + 5, 1);
    else if (e->keyval == GDK_KEY_Left || e->keyval == GDK_KEY_Down) vol_poner(g_vol - 5, 1);
    else return FALSE;
    return TRUE;
}

static void mostrar(void) {
    gtk_widget_show(ventana);
    gtk_window_deiconify(GTK_WINDOW(ventana));
    gtk_window_present(GTK_WINDOW(ventana));
}

static gboolean al_cambiar_estado(GtkWidget *w, GdkEventWindowState *e, gpointer u) {
    (void)u;
    minimizada = (e->new_window_state & GDK_WINDOW_STATE_ICONIFIED) != 0;
    if (minimizada && gtk_status_icon_is_embedded(icono)) gtk_widget_hide(w);
    return FALSE;
}

static void menu_abrir(GtkMenuItem *i, gpointer u) { (void)i; (void)u; mostrar(); }
static void menu_silenciar(GtkMenuItem *i, gpointer u) { (void)i; (void)u; alternar_silencio(); }
static void menu_salir(GtkMenuItem *i, gpointer app) { (void)i; g_application_quit(G_APPLICATION(app)); }

static void al_activar_icono(GtkStatusIcon *i, gpointer u) { (void)i; (void)u; mostrar(); }

static void al_menu_icono(GtkStatusIcon *i, guint boton, guint tiempo, gpointer app) {
    GtkWidget *m = gtk_menu_new();
    GtkWidget *abrir = gtk_menu_item_new_with_label("Abrir AudioWire");
    GtkWidget *silenciar = gtk_check_menu_item_new_with_label("Silenciar");
    GtkWidget *salir = gtk_menu_item_new_with_label("Salir");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(silenciar), g_vol == 0);
    g_signal_connect(abrir, "activate", G_CALLBACK(menu_abrir), NULL);
    g_signal_connect(silenciar, "activate", G_CALLBACK(menu_silenciar), NULL);
    g_signal_connect(salir, "activate", G_CALLBACK(menu_salir), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), abrir);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), silenciar);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(m), salir);
    gtk_widget_show_all(m);
    gtk_menu_popup(GTK_MENU(m), NULL, NULL, gtk_status_icon_position_menu, i, boton, tiempo);
}

static void al_activar(GtkApplication *app, gpointer u) {
    (void)u;
    if (ventana) {
        mostrar();
        return;
    }

    pthread_t t;
    pthread_create(&t, NULL, hilo_red, NULL);
    pthread_create(&t, NULL, hilo_info, NULL);
    pthread_create(&t, NULL, hilo_captura, NULL);

    for (int m = 0; m < 2; m++)
        for (int f = 0; f < SPR_N; f++)
            superficies[m][f] = cairo_image_surface_create_for_data((unsigned char *)SPR[m][f], CAIRO_FORMAT_ARGB32,
                                                                    SPR_W, SPR_H, SPR_W * 4);
    pbIdle = pixbuf_cuadrado(0);
    pbActivo = pixbuf_cuadrado(1);
    vol_cargar();

    ventana = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ventana), "AudioWire");
    gtk_window_set_resizable(GTK_WINDOW(ventana), FALSE);
    gtk_window_set_icon(GTK_WINDOW(ventana), pbIdle);
    gtk_window_set_position(GTK_WINDOW(ventana), GTK_WIN_POS_CENTER);

    lienzo = gtk_drawing_area_new();
    gtk_widget_set_size_request(lienzo, ANCHO, ALTO);
    gtk_widget_add_events(lienzo, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                                      GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_signal_connect(lienzo, "draw", G_CALLBACK(al_dibujar), NULL);
    g_signal_connect(lienzo, "button-press-event", G_CALLBACK(al_pulsar), NULL);
    g_signal_connect(lienzo, "button-release-event", G_CALLBACK(al_soltar), NULL);
    g_signal_connect(lienzo, "motion-notify-event", G_CALLBACK(al_mover), NULL);
    g_signal_connect(lienzo, "scroll-event", G_CALLBACK(al_rueda), NULL);
    g_signal_connect(ventana, "key-press-event", G_CALLBACK(al_tecla), NULL);
    g_signal_connect(ventana, "window-state-event", G_CALLBACK(al_cambiar_estado), NULL);
    gtk_container_add(GTK_CONTAINER(ventana), lienzo);

    icono = gtk_status_icon_new_from_pixbuf(pbIdle);
    gtk_status_icon_set_title(icono, "AudioWire");
    gtk_status_icon_set_tooltip_text(icono, "AudioWire");
    g_signal_connect(icono, "activate", G_CALLBACK(al_activar_icono), NULL);
    g_signal_connect(icono, "popup-menu", G_CALLBACK(al_menu_icono), app);

    actualizar_estado();
    idAnim = g_timeout_add(140, animar, NULL);
    g_timeout_add(500, al_temporizador, NULL);
    gtk_widget_show_all(ventana);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    GtkApplication *app = gtk_application_new("io.github.audiowire.AudioWire", 0);
    g_signal_connect(app, "activate", G_CALLBACK(al_activar), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}
