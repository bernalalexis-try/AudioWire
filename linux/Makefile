CFLAGS ?= -O2 -Wall
PAQUETES = gtk+-3.0 libpulse libpulse-simple

audiowire: audiowire.c sprites.h
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -Wl,--as-needed -o $@ audiowire.c $$(pkg-config --cflags --libs $(PAQUETES)) -lm -lpthread
	strip $@

install: audiowire
	install -Dm755 audiowire $(HOME)/.local/bin/audiowire
	install -Dm644 audiowire.png $(HOME)/.local/share/icons/hicolor/128x128/apps/audiowire.png
	install -Dm644 audiowire.desktop $(HOME)/.local/share/applications/audiowire.desktop

clean:
	rm -f audiowire

.PHONY: install clean
