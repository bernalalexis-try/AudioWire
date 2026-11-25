package com.audiowire;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.Process;
import android.provider.Settings;

import android.os.SystemClock;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.SocketTimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;

public class AudioService extends Service {

    public static final String EXTRA_IP = "ip";
    public static final int PUERTO = 5005;
    private static final String CANAL_ID = "audiowire";
    private static final int RETRASO_MS = 60;
    private static final byte[] ADIOS = {'S', 'W', 'X', 'B'};
    /* Le avisa al PC que este móvil decodifica Opus (los PCs viejos lo ignoran). */
    private static final byte[] OPUS = {'S', 'W', 'X', 'O'};

    public static volatile String estado = "Desconectado";
    public static volatile boolean activo = false;
    public static volatile String nombrePc = null;

    private AtomicBoolean vivo;
    private Thread hilo;
    private WifiManager.WifiLock wifiLock;
    private PowerManager.WakeLock wakeLock;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        final String ip = intent != null ? intent.getStringExtra(EXTRA_IP) : null;
        if (ip == null) {
            stopSelf();
            return START_NOT_STICKY;
        }
        String quien = nombrePc != null ? nombrePc : ip;
        iniciarPrimerPlano(quien);
        adquirirLocks();
        detenerHilo();

        final AtomicBoolean flag = new AtomicBoolean(true);
        vivo = flag;
        activo = true;
        hilo = new Thread(new Runnable() {
            @Override
            public void run() {
                bucle(ip, quien, flag);
            }
        }, "AudioWire");
        hilo.start();
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        detenerHilo();
        activo = false;
        estado = "Desconectado";
        if (wifiLock != null && wifiLock.isHeld()) wifiLock.release();
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        super.onDestroy();
    }

    private void detenerHilo() {
        if (vivo != null) vivo.set(false);
        if (hilo != null) {
            try { hilo.join(800); } catch (InterruptedException ignored) { }
            hilo = null;
        }
    }

    @SuppressWarnings("deprecation")
    private void adquirirLocks() {
        if (wifiLock == null) {
            WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            int modo = Build.VERSION.SDK_INT >= 29 ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                                                   : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
            wifiLock = wm.createWifiLock(modo, "AudioWire:wifi");
            wifiLock.setReferenceCounted(false);
            wifiLock.acquire();
        }
        if (wakeLock == null) {
            PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "AudioWire:cpu");
            wakeLock.setReferenceCounted(false);
            wakeLock.acquire();
        }
    }

    @SuppressWarnings("deprecation")
    private void iniciarPrimerPlano(String ip) {
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(new NotificationChannel(CANAL_ID, "Reproducción",
                    NotificationManager.IMPORTANCE_LOW));
            b = new Notification.Builder(this, CANAL_ID);
        } else {
            b = new Notification.Builder(this);
        }
        PendingIntent pi = PendingIntent.getActivity(this, 0,
                new Intent(this, MainActivity.class), PendingIntent.FLAG_IMMUTABLE);
        Notification n = b.setContentTitle("AudioWire")
                .setContentText("Conectado a " + ip)
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setContentIntent(pi)
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        } else {
            startForeground(1, n);
        }
    }

    /* El nombre del teléfono: el de Ajustes y, si no hay, la marca y el modelo. */
    private String nombreDispositivo() {
        String n = null;
        try {
            if (Build.VERSION.SDK_INT >= 25)
                n = Settings.Global.getString(getContentResolver(), Settings.Global.DEVICE_NAME);
            if (n == null || n.trim().isEmpty())
                n = Settings.Secure.getString(getContentResolver(), "bluetooth_name");
        } catch (RuntimeException ignored) { }
        if (n == null || n.trim().isEmpty()) {
            String marca = Build.MANUFACTURER == null ? "" : Build.MANUFACTURER;
            String modelo = Build.MODEL == null ? "" : Build.MODEL;
            n = modelo.toLowerCase().startsWith(marca.toLowerCase()) ? modelo : marca + " " + modelo;
        }
        return n.trim();
    }

    /* "SWXH" + id del teléfono + "\n" + nombre. Cabe en 64 bytes, que es lo que lee el PC
       (uno viejo descarta un saludo más largo); un PC viejo ignora lo que sigue a "SWXH". */
    private byte[] saludo() {
        String id = null;
        try { id = Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID); }
        catch (RuntimeException ignored) { }
        if (id == null) id = "";
        if (id.length() > 16) id = id.substring(0, 16);
        byte[] cab = ("SWXH" + id + "\n").getBytes(StandardCharsets.US_ASCII);
        String nombre = nombreDispositivo();
        byte[] nb = nombre.getBytes(StandardCharsets.UTF_8);
        int lugar = 64 - cab.length;
        while (nb.length > lugar && nombre.length() > 0) {
            nombre = nombre.substring(0, nombre.offsetByCodePoints(nombre.length(), -1));
            nb = nombre.getBytes(StandardCharsets.UTF_8);
        }
        byte[] out = new byte[cab.length + nb.length];
        System.arraycopy(cab, 0, out, 0, cab.length);
        System.arraycopy(nb, 0, out, cab.length, nb.length);
        return out;
    }

    private static int leer32(byte[] b, int p) {
        return (b[p] & 0xFF) | (b[p + 1] & 0xFF) << 8 | (b[p + 2] & 0xFF) << 16 | (b[p + 3] & 0xFF) << 24;
    }

    private AudioTrack crearPista(int rate, int canales) {
        int mascara = canales == 1 ? AudioFormat.CHANNEL_OUT_MONO : AudioFormat.CHANNEL_OUT_STEREO;
        int minBuf = AudioTrack.getMinBufferSize(rate, mascara, AudioFormat.ENCODING_PCM_16BIT);
        int deseado = rate * canales * 2 * RETRASO_MS / 1000;
        AudioTrack.Builder b = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build())
                .setAudioFormat(new AudioFormat.Builder()
                        .setSampleRate(rate)
                        .setChannelMask(mascara)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .build())
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setBufferSizeInBytes(Math.max(minBuf, deseado));
        if (Build.VERSION.SDK_INT >= 26) b.setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY);
        AudioTrack t = b.build();
        t.play();
        return t;
    }

    private static boolean tieneOpus() {
        try {
            for (MediaCodecInfo i : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
                if (i.isEncoder()) continue;
                for (String t : i.getSupportedTypes())
                    if (t.equalsIgnoreCase(MediaFormat.MIMETYPE_AUDIO_OPUS)) return true;
            }
        } catch (RuntimeException ignored) { }
        return false;
    }

    private static ByteBuffer largo64(long v) {
        ByteBuffer b = ByteBuffer.allocate(8).order(ByteOrder.nativeOrder());
        b.putLong(v).flip();
        return b;
    }

    private static MediaCodec crearDecoder(int rate, int canales) throws IOException {
        ByteBuffer cab = ByteBuffer.allocate(19).order(ByteOrder.LITTLE_ENDIAN);
        cab.put("OpusHead".getBytes(StandardCharsets.US_ASCII)).put((byte) 1).put((byte) canales)
                .putShort((short) 0).putInt(rate).putShort((short) 0).put((byte) 0).flip();
        MediaFormat f = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, rate, canales);
        f.setByteBuffer("csd-0", cab);
        f.setByteBuffer("csd-1", largo64(0));
        f.setByteBuffer("csd-2", largo64(80_000_000L));
        MediaCodec d = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS);
        d.configure(f, null, null, 0);
        d.start();
        return d;
    }

    private final MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
    private byte[] pcm = new byte[8192];
    private long pts = 0;

    /* Pasa un paquete Opus al decoder y escribe en la pista lo que ya salió, sin pasar de "libre" frames.
       Devuelve los frames escritos. */
    private int decodificar(MediaCodec d, byte[] b, int off, int len, AudioTrack pista, int libre, int bytesFrame) {
        int in = d.dequeueInputBuffer(5000);
        if (in >= 0) {
            ByteBuffer ib = d.getInputBuffer(in);
            ib.clear();
            ib.put(b, off, len);
            d.queueInputBuffer(in, 0, len, pts, 0);
            pts += 10_000;
        }
        int escritos = 0;
        for (;;) {
            int o = d.dequeueOutputBuffer(info, 0);
            if (o == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED || o == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) continue;
            if (o < 0) break;
            ByteBuffer ob = d.getOutputBuffer(o);
            int tam = info.size;
            if (ob != null && tam > 0) {
                if (pcm.length < tam) pcm = new byte[tam];
                ob.position(info.offset);
                ob.get(pcm, 0, tam);
                int f = tam / bytesFrame;
                if (escritos + f <= libre) {
                    int w = pista.write(pcm, 0, f * bytesFrame);
                    if (w > 0) escritos += w / bytesFrame;
                }
            }
            d.releaseOutputBuffer(o, false);
        }
        return escritos;
    }

    private void bucle(String ip, String quien, AtomicBoolean vivo) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
        estado = "Conectando a " + quien + "…";
        DatagramSocket s = null;
        AudioTrack pista = null;
        MediaCodec dec = null;
        try {
            InetAddress dir = InetAddress.getByName(ip);
            s = new DatagramSocket();
            s.setSoTimeout(200);
            byte[] saludo = saludo();
            DatagramPacket hola = new DatagramPacket(saludo, saludo.length, dir, PUERTO);
            DatagramPacket conOpus = tieneOpus() ? new DatagramPacket(OPUS, OPUS.length, dir, PUERTO) : null;
            boolean esOpus = false;
            byte[] buf = new byte[2048];
            byte[] silencio = new byte[2048];
            DatagramPacket p = new DatagramPacket(buf, buf.length);

            long ultimoHola = 0, ultimaRespuesta = 0, ultimoAudio = 0, ultimoEstado = 0, ultimaEspera = 0;
            int rate = 0, canales = 0, seqEsperado = 0, escritos = 0, capacidad = 0;
            boolean haySeq = false;

            while (vivo.get()) {
                long ahora = SystemClock.elapsedRealtime();
                if (ahora - ultimoHola >= 1000) {
                    try {
                        s.send(hola);
                        if (conOpus != null) s.send(conOpus);
                    } catch (IOException ignored) { }
                    ultimoHola = ahora;
                }
                if (ahora - ultimoEstado >= 250) {
                    ultimoEstado = ahora;
                    if (ultimaRespuesta == 0) estado = "Conectando a " + quien + "…";
                    else if (ahora - ultimaRespuesta > 3000) estado = "Sin respuesta de " + quien + ". Reintentando…";
                    else if (ahora - ultimaEspera < 3000) estado = "Esperando que aceptes este móvil en " + quien + "…";
                    else if (ahora - ultimoAudio < 1000) estado = "Reproduciendo desde " + quien;
                    else estado = "Conectado a " + quien + " · sin sonido";
                }

                p.setLength(buf.length);
                try {
                    s.receive(p);
                } catch (SocketTimeoutException e) {
                    continue;
                } catch (IOException e) {
                    try { Thread.sleep(200); } catch (InterruptedException ie) { break; }
                    continue;
                }
                if (!dir.equals(p.getAddress())) continue;
                int n = p.getLength();
                if (n < 4 || buf[0] != 'S' || buf[1] != 'W') continue;
                if (buf[2] == 'X' && buf[3] == 'P') {
                    ultimaRespuesta = ahora;
                    ultimaEspera = 0;
                    continue;
                }
                if (buf[2] == 'X' && buf[3] == 'W') {
                    ultimaRespuesta = ahora;
                    ultimaEspera = ahora;
                    continue;
                }
                boolean opus = buf[2] == 'O';
                if ((buf[2] != 'A' && !opus) || n < 12) continue;

                int ch = buf[3] & 0xFF, r = leer32(buf, 4), seq = leer32(buf, 8);
                if (ch < 1 || ch > 2 || r < 8000 || r > 192000) continue;
                if (opus && (conOpus == null || r != 48000)) continue;
                ultimaRespuesta = ahora;
                ultimoAudio = ahora;

                if (pista == null || r != rate || ch != canales || opus != esOpus) {
                    if (pista != null) pista.release();
                    if (dec != null) { dec.release(); dec = null; }
                    pista = crearPista(r, ch);
                    capacidad = pista.getBufferSizeInFrames();
                    rate = r;
                    canales = ch;
                    escritos = 0;
                    haySeq = false;
                    esOpus = opus;
                    if (opus) dec = crearDecoder(r, ch);
                }

                int bytesFrame = canales * 2;
                /* Un paquete Opus trae 10 ms de audio. */
                int frames = opus ? r / 100 : (n - 12) / bytesFrame;
                if (haySeq) {
                    int dif = seq - seqEsperado;
                    if (dif < 0 && dif > -1000) continue;
                    if (dif > 1000) haySeq = false;
                }
                int cola = escritos - pista.getPlaybackHeadPosition();
                if (haySeq) {
                    int perdidos = seq - seqEsperado;
                    for (int k = 0; k < perdidos && k < 4 && cola + 2 * frames <= capacidad; k++) {
                        pista.write(silencio, 0, frames * bytesFrame);
                        escritos += frames;
                        cola += frames;
                    }
                }
                seqEsperado = seq + 1;
                haySeq = true;
                if (opus) {
                    /* Se decodifica siempre para no romper el decoder; si la cola está llena, se descarta. */
                    escritos += decodificar(dec, buf, 12, n - 12, pista, capacidad - cola, bytesFrame);
                    continue;
                }
                if (cola + frames > capacidad) continue;
                int w = pista.write(buf, 12, frames * bytesFrame);
                if (w > 0) escritos += w / bytesFrame;
            }
        } catch (Exception e) {
            estado = "Error: " + (e.getMessage() != null ? e.getMessage() : "desconocido");
        } finally {
            if (s != null) {
                try {
                    s.send(new DatagramPacket(ADIOS, ADIOS.length, InetAddress.getByName(ip), PUERTO));
                } catch (Exception ignored) { }
                s.close();
            }
            if (pista != null) {
                try { pista.stop(); } catch (IllegalStateException ignored) { }
                pista.release();
            }
            if (dec != null) {
                try { dec.stop(); } catch (IllegalStateException ignored) { }
                dec.release();
            }
        }
    }
}
