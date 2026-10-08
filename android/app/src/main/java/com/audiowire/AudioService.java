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
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.Process;

import android.os.SystemClock;

import java.io.IOException;
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
    private static final byte[] HOLA = {'S', 'W', 'X', 'H'};
    private static final byte[] ADIOS = {'S', 'W', 'X', 'B'};

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

    private void bucle(String ip, String quien, AtomicBoolean vivo) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
        estado = "Conectando a " + quien + "…";
        DatagramSocket s = null;
        AudioTrack pista = null;
        try {
            InetAddress dir = InetAddress.getByName(ip);
            s = new DatagramSocket();
            s.setSoTimeout(200);
            DatagramPacket hola = new DatagramPacket(HOLA, HOLA.length, dir, PUERTO);
            byte[] buf = new byte[2048];
            byte[] silencio = new byte[2048];
            DatagramPacket p = new DatagramPacket(buf, buf.length);

            long ultimoHola = 0, ultimaRespuesta = 0, ultimoAudio = 0, ultimoEstado = 0;
            int rate = 0, canales = 0, seqEsperado = 0, escritos = 0, capacidad = 0;
            boolean haySeq = false;

            while (vivo.get()) {
                long ahora = SystemClock.elapsedRealtime();
                if (ahora - ultimoHola >= 1000) {
                    try { s.send(hola); } catch (IOException ignored) { }
                    ultimoHola = ahora;
                }
                if (ahora - ultimoEstado >= 250) {
                    ultimoEstado = ahora;
                    if (ultimaRespuesta == 0) estado = "Conectando a " + quien + "…";
                    else if (ahora - ultimaRespuesta > 3000) estado = "Sin respuesta de " + quien + ". Reintentando…";
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
                    continue;
                }
                if (buf[2] != 'A' || n < 12) continue;

                int ch = buf[3] & 0xFF, r = leer32(buf, 4), seq = leer32(buf, 8);
                if (ch < 1 || ch > 2 || r < 8000 || r > 192000) continue;
                ultimaRespuesta = ahora;
                ultimoAudio = ahora;

                if (pista == null || r != rate || ch != canales) {
                    if (pista != null) pista.release();
                    pista = crearPista(r, ch);
                    capacidad = pista.getBufferSizeInFrames();
                    rate = r;
                    canales = ch;
                    escritos = 0;
                    haySeq = false;
                }

                int bytesFrame = canales * 2;
                int frames = (n - 12) / bytesFrame;
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
        }
    }
}
