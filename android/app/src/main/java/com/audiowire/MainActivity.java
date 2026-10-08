package com.audiowire;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class MainActivity extends Activity {

    private static final byte[] BUSCAR = {'S', 'W', 'X', 'D'};

    private Button boton;
    private TextView textoEstado, textoBusqueda;
    private LinearLayout listaPcs, filaManual;
    private EditText campoIp;
    private SharedPreferences prefs;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private final Map<String, String> encontrados = Collections.synchronizedMap(new LinkedHashMap<String, String>());
    private final Map<String, Long> vistos = Collections.synchronizedMap(new LinkedHashMap<String, Long>());
    private volatile boolean buscando = false;
    private Thread hiloBusqueda;
    private boolean desconectadoAMano = false;
    private long inicioBusqueda = 0;
    private String firmaLista = "";

    private final Runnable refresco = new Runnable() {
        @Override
        public void run() {
            actualizar();
            handler.postDelayed(this, 400);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences("config", MODE_PRIVATE);
        int pad = dp(24);

        ScrollView scroll = new ScrollView(this);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(pad, pad, pad, pad);
        scroll.addView(layout);

        textoEstado = new TextView(this);
        textoEstado.setTextSize(17f);
        layout.addView(textoEstado);

        boton = new Button(this);
        boton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                desconectar();
            }
        });
        layout.addView(boton);

        textoBusqueda = new TextView(this);
        textoBusqueda.setTextSize(15f);
        textoBusqueda.setPadding(0, dp(8), 0, dp(4));
        layout.addView(textoBusqueda);

        listaPcs = new LinearLayout(this);
        listaPcs.setOrientation(LinearLayout.VERTICAL);
        layout.addView(listaPcs);

        TextView manual = new TextView(this);
        manual.setText("Conectar por IP");
        manual.setTextSize(14f);
        manual.setPadding(0, dp(28), 0, 0);
        layout.addView(manual);

        filaManual = new LinearLayout(this);
        filaManual.setOrientation(LinearLayout.HORIZONTAL);
        filaManual.setGravity(Gravity.CENTER_VERTICAL);
        campoIp = new EditText(this);
        campoIp.setHint("192.168.1.50");
        campoIp.setSingleLine(true);
        campoIp.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        campoIp.setText(prefs.getString("ip", ""));
        filaManual.addView(campoIp, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        Button ir = new Button(this);
        ir.setText("Conectar");
        ir.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                String ip = campoIp.getText().toString().trim();
                if (ip.isEmpty()) {
                    Toast.makeText(MainActivity.this, "Escribe la IP del PC", Toast.LENGTH_SHORT).show();
                    return;
                }
                conectar(ip, null);
            }
        });
        filaManual.addView(ir);
        layout.addView(filaManual);

        setContentView(scroll);

        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 1);
        }
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density);
    }

    private void conectar(String ip, String nombre) {
        SharedPreferences.Editor e = prefs.edit().putString("ip", ip);
        if (nombre != null) e.putString("nombre", nombre);
        e.apply();
        campoIp.setText(ip);
        AudioService.nombrePc = nombre != null ? nombre : ip;
        Intent intent = new Intent(this, AudioService.class).putExtra(AudioService.EXTRA_IP, ip);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(intent);
        else startService(intent);
        detenerBusqueda();
        handler.postDelayed(new Runnable() {
            @Override
            public void run() {
                actualizar();
            }
        }, 150);
    }

    private void desconectar() {
        desconectadoAMano = true;
        stopService(new Intent(this, AudioService.class));
        handler.postDelayed(new Runnable() {
            @Override
            public void run() {
                iniciarBusqueda();
                actualizar();
            }
        }, 300);
    }

    private static List<InetAddress> destinos() {
        List<InetAddress> r = new ArrayList<>();
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress b = ia.getBroadcast();
                    if (b != null && !r.contains(b)) r.add(b);
                }
            }
        } catch (Exception ignored) { }
        try {
            r.add(InetAddress.getByName("255.255.255.255"));
        } catch (Exception ignored) { }
        return r;
    }

    private void iniciarBusqueda() {
        if (buscando || AudioService.activo) return;
        buscando = true;
        inicioBusqueda = SystemClock.elapsedRealtime();
        encontrados.clear();
        vistos.clear();
        hiloBusqueda = new Thread(new Runnable() {
            @Override
            public void run() {
                buscar();
            }
        }, "Busqueda");
        hiloBusqueda.start();
    }

    private void detenerBusqueda() {
        buscando = false;
    }

    private void buscar() {
        DatagramSocket s = null;
        try {
            s = new DatagramSocket();
            s.setBroadcast(true);
            s.setSoTimeout(250);
            byte[] buf = new byte[256];
            DatagramPacket p = new DatagramPacket(buf, buf.length);
            long ultimoEnvio = 0;
            List<InetAddress> dest = destinos();
            while (buscando) {
                long ahora = SystemClock.elapsedRealtime();
                if (ahora - ultimoEnvio >= 1000) {
                    if (ahora - ultimoEnvio >= 5000) dest = destinos();
                    for (InetAddress d : dest) {
                        try {
                            s.send(new DatagramPacket(BUSCAR, BUSCAR.length, d, AudioService.PUERTO));
                        } catch (Exception ignored) { }
                    }
                    ultimoEnvio = ahora;
                    synchronized (vistos) {
                        for (String ip : new ArrayList<>(vistos.keySet())) {
                            if (ahora - vistos.get(ip) > 4000) {
                                vistos.remove(ip);
                                encontrados.remove(ip);
                            }
                        }
                    }
                }
                p.setLength(buf.length);
                try {
                    s.receive(p);
                } catch (SocketTimeoutException e) {
                    continue;
                }
                int n = p.getLength();
                if (n < 4 || buf[0] != 'S' || buf[1] != 'W' || buf[2] != 'X' || buf[3] != 'I') continue;
                String nombre = n > 4 ? new String(buf, 4, n - 4, StandardCharsets.UTF_8).trim() : "";
                String ip = p.getAddress().getHostAddress();
                if (nombre.isEmpty()) nombre = ip;
                encontrados.put(ip, nombre);
                vistos.put(ip, SystemClock.elapsedRealtime());
            }
        } catch (Exception ignored) {
        } finally {
            if (s != null) s.close();
        }
    }

    private void intentarAutoConexion() {
        if (AudioService.activo || desconectadoAMano) return;
        Map<String, String> copia;
        synchronized (encontrados) {
            copia = new LinkedHashMap<>(encontrados);
        }
        if (copia.isEmpty()) return;
        String ipGuardada = prefs.getString("ip", ""), nombreGuardado = prefs.getString("nombre", "");
        for (Map.Entry<String, String> e : copia.entrySet()) {
            if (!nombreGuardado.isEmpty() && nombreGuardado.equals(e.getValue())) {
                conectar(e.getKey(), e.getValue());
                return;
            }
        }
        if (copia.containsKey(ipGuardada)) {
            conectar(ipGuardada, copia.get(ipGuardada));
            return;
        }
        if (copia.size() == 1 && SystemClock.elapsedRealtime() - inicioBusqueda > 1500) {
            Map.Entry<String, String> e = copia.entrySet().iterator().next();
            conectar(e.getKey(), e.getValue());
        }
    }

    private void pintarLista() {
        Map<String, String> copia;
        synchronized (encontrados) {
            copia = new LinkedHashMap<>(encontrados);
        }
        String firma = copia.toString() + AudioService.activo;
        if (firma.equals(firmaLista)) return;
        firmaLista = firma;
        listaPcs.removeAllViews();
        if (AudioService.activo) return;
        for (final Map.Entry<String, String> e : copia.entrySet()) {
            Button b = new Button(this);
            b.setAllCaps(false);
            b.setText(e.getValue() + "   ·   " + e.getKey());
            b.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    conectar(e.getKey(), e.getValue());
                }
            });
            listaPcs.addView(b);
        }
    }

    private void actualizar() {
        boolean activo = AudioService.activo;
        if (activo) {
            detenerBusqueda();
            textoEstado.setText(AudioService.estado);
            boton.setText("Desconectar");
            boton.setVisibility(View.VISIBLE);
            textoBusqueda.setVisibility(View.GONE);
        } else {
            iniciarBusqueda();
            intentarAutoConexion();
            if (AudioService.activo) return;
            textoEstado.setText("Desconectado");
            boton.setVisibility(View.GONE);
            textoBusqueda.setVisibility(View.VISIBLE);
            int n = encontrados.size();
            if (n == 0) textoBusqueda.setText("Buscando AudioWire en la red…");
            else if (desconectadoAMano || n > 1) textoBusqueda.setText("Toca un PC para conectar:");
            else textoBusqueda.setText("PC encontrado, conectando…");
        }
        filaManual.setEnabled(!activo);
        campoIp.setEnabled(!activo);
        pintarLista();
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.post(refresco);
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(refresco);
        detenerBusqueda();
        super.onPause();
    }
}
