<div align="center">

<img src="docs/icono.png" width="112" alt="AudioWire">

# AudioWire

**[English](README.md)** · Español

Envía el audio de tu PC con Windows o Linux a tu móvil Android por Wi-Fi.

[![Descargar para Windows](https://img.shields.io/badge/Windows-Descargar_.exe-F0651E?style=for-the-badge&logo=windows&logoColor=white)](https://github.com/bernalalexis-try/AudioWire/releases/latest/download/AudioWire.exe)
[![Descargar para Linux](https://img.shields.io/badge/Linux-Descargar-F0651E?style=for-the-badge&logo=linux&logoColor=white)](https://github.com/bernalalexis-try/AudioWire/releases/latest/download/audiowire)
[![Descargar para Android](https://img.shields.io/badge/Android-Descargar_.apk-F0651E?style=for-the-badge&logo=android&logoColor=white)](https://github.com/bernalalexis-try/AudioWire/releases/latest/download/AudioWire.apk)

<img src="docs/captura.png" width="396" alt="AudioWire en Windows transmitiendo a un móvil">

</div>

## Uso

1. Abre AudioWire en el PC.
   - **Windows:** cuando pregunte por el firewall, permite **redes privadas**.
   - **Linux:** dale permiso de ejecución con `chmod +x audiowire` y ábrelo.
2. Instala el APK en el móvil (acepta "orígenes desconocidos").
3. Escribe en el móvil la IP que muestra el PC y pulsa **Conectar**.

Si se corta, el móvil se reconecta solo.

## Requisitos

- PC con Windows, o con Linux con GTK 3 y PulseAudio o PipeWire (Ubuntu 22.04, Debian 12, Fedora 35 o más nuevos).
- Móvil con Android 7.0 o superior.
- Los dos conectados a la misma red Wi-Fi.
- El puerto TCP 5005 permitido en el firewall del PC.

## Estructura

- `pc/` → app de Windows en C nativo (Win32 + WASAPI), un solo `.exe` sin dependencias.
- `linux/` → app de Linux en C (GTK 3 + PulseAudio/PipeWire).
- `android/` → app del móvil, que reproduce el audio.
