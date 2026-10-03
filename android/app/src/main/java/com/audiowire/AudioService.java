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

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicBoolean;

public class AudioService extends Service {

    public static final String EXTRA_IP = "ip";
    public static final int PUERTO = 5005;
    private static final String CANAL_ID = "audiowire";
    private static final int MAX_RETRASO_MS = 120;

    public static volatile String estado = "Desconectado";
    public static volatile boolean activo = false;

    private AtomicBoolean vivo;
    private Thread hilo;
    private volatile Socket socket;
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
        iniciarPrimerPlano(ip);
        adquirirLocks();
        detenerHilo();

        final AtomicBoolean flag = new AtomicBoolean(true);
        vivo = flag;
        activo = true;
        hilo = new Thread(new Runnable() {
            @Override
            public void run() {
                bucle(ip, flag);
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
        Socket s = socket;
        if (s != null) {
            try { s.close(); } catch (IOException ignored) { }
        }
        if (hilo != null) {
            hilo.interrupt();
            try { hilo.join(500); } catch (InterruptedException ignored) { }
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
                .setContentText("Recibiendo audio de " + ip)
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

    private void bucle(String ip, AtomicBoolean vivo) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
        while (vivo.get()) {
            Socket s = new Socket();
            socket = s;
            try {
                estado = "Conectando a " + ip + "…";
                s.setTcpNoDelay(true);
                s.connect(new InetSocketAddress(ip, PUERTO), 3000);
                DataInputStream entrada = new DataInputStream(new BufferedInputStream(s.getInputStream(), 32 * 1024));

                byte[] cab = new byte[10];
                entrada.readFully(cab);
                if (cab[0] != 'S' || cab[1] != 'W' || cab[2] != 'X' || cab[3] != '1') {
                    throw new IOException("servidor no compatible");
                }
                ByteBuffer bb = ByteBuffer.wrap(cab).order(ByteOrder.LITTLE_ENDIAN);
                int rate = bb.getInt(4);
                int canales = bb.getShort(8) & 0xFFFF;
                reproducir(entrada, rate, canales, ip, vivo);
            } catch (Exception e) {
                if (!vivo.get()) break;
                estado = "Sin conexión (" + (e.getMessage() != null ? e.getMessage() : "error") + "). Reintentando…";
                try { Thread.sleep(1500); } catch (InterruptedException ie) { break; }
            } finally {
                try { s.close(); } catch (IOException ignored) { }
                socket = null;
            }
        }
    }

    private void reproducir(DataInputStream entrada, int rate, int canales, String ip, AtomicBoolean vivo)
            throws IOException {
        int canalesSalida = canales == 1 ? 1 : 2;
        int mascara = canalesSalida == 1 ? AudioFormat.CHANNEL_OUT_MONO : AudioFormat.CHANNEL_OUT_STEREO;
        int minBuf = AudioTrack.getMinBufferSize(rate, mascara, AudioFormat.ENCODING_PCM_16BIT);

        AudioTrack.Builder builder = new AudioTrack.Builder()
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
                .setBufferSizeInBytes(minBuf * 2);
        if (Build.VERSION.SDK_INT >= 26) builder.setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY);
        AudioTrack track = builder.build();

        int bytesFrameEntrada = canales * 2;
        int bytesFrameSalida = canalesSalida * 2;
        int framesBloque = rate / 100;
        byte[] bufEntrada = new byte[framesBloque * bytesFrameEntrada];
        byte[] bufSalida = canales > 2 ? new byte[framesBloque * bytesFrameSalida] : bufEntrada;
        byte[] tmp = new byte[8192];
        int maxRetraso = rate * bytesFrameEntrada / 1000 * MAX_RETRASO_MS;

        track.play();
        estado = "Reproduciendo desde " + ip + " · " + rate + " Hz";
        try {
            while (vivo.get()) {
                entrada.readFully(bufEntrada);

                int pendiente = entrada.available();
                if (pendiente > maxRetraso) {
                    int sobra = (pendiente - maxRetraso / 2) / bytesFrameEntrada * bytesFrameEntrada;
                    int bloque = tmp.length / bytesFrameEntrada * bytesFrameEntrada;
                    while (sobra > 0) {
                        int n = Math.min(sobra, bloque);
                        entrada.readFully(tmp, 0, n);
                        sobra -= n;
                    }
                }

                if (canales > 2) {
                    for (int f = 0; f < framesBloque; f++) {
                        System.arraycopy(bufEntrada, f * bytesFrameEntrada, bufSalida, f * 4, 4);
                    }
                }
                track.write(bufSalida, 0, bufSalida.length);
            }
        } finally {
            try { track.stop(); } catch (IllegalStateException ignored) { }
            track.release();
        }
    }
}
