#!/bin/sh
set -e
x86_64-w64-mingw32-windres recursos.rc -O coff -o recursos.o
# libopus estática para mingw, en pc/opus (ver .github/workflows/compilar.yml)
OPUS=${OPUS:-opus}
x86_64-w64-mingw32-gcc -O2 -s -municode -mwindows -static -Wall -I"$OPUS/include" \
  audiowire.c recursos.o -o AudioWire.exe -L"$OPUS/lib" -lopus \
  -lole32 -lws2_32 -lmsimg32 -lgdi32 -luser32 -lwinmm -ladvapi32 -lshell32
rm -f recursos.o
echo "Listo: AudioWire.exe"
