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
import android.text.InputType;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

public class MainActivity extends Activity {

    private EditText campoIp;
    private Button boton;
    private TextView textoEstado;
    private SharedPreferences prefs;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private final Runnable refresco = new Runnable() {
        @Override
        public void run() {
            actualizar();
            handler.postDelayed(this, 500);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences("config", MODE_PRIVATE);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);

        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(pad, pad, pad, pad);

        TextView titulo = new TextView(this);
        titulo.setText("IP del PC");
        titulo.setTextSize(16f);
        layout.addView(titulo);

        campoIp = new EditText(this);
        campoIp.setHint("192.168.1.50");
        campoIp.setSingleLine(true);
        campoIp.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        campoIp.setText(prefs.getString("ip", ""));
        layout.addView(campoIp);

        boton = new Button(this);
        boton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                alPulsar();
            }
        });
        layout.addView(boton);

        textoEstado = new TextView(this);
        textoEstado.setTextSize(15f);
        textoEstado.setPadding(0, pad / 2, 0, 0);
        layout.addView(textoEstado);

        setContentView(layout);

        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 1);
        }
    }

    private void alPulsar() {
        if (AudioService.activo) {
            stopService(new Intent(this, AudioService.class));
        } else {
            String ip = campoIp.getText().toString().trim();
            if (ip.isEmpty()) {
                Toast.makeText(this, "Escribe la IP del PC", Toast.LENGTH_SHORT).show();
                return;
            }
            prefs.edit().putString("ip", ip).apply();
            Intent intent = new Intent(this, AudioService.class).putExtra(AudioService.EXTRA_IP, ip);
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(intent);
            else startService(intent);
        }
        handler.postDelayed(new Runnable() {
            @Override
            public void run() {
                actualizar();
            }
        }, 150);
    }

    private void actualizar() {
        boton.setText(AudioService.activo ? "Desconectar" : "Conectar");
        campoIp.setEnabled(!AudioService.activo);
        textoEstado.setText(AudioService.estado);
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.post(refresco);
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(refresco);
        super.onPause();
    }
}
