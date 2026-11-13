CC        ?= gcc
PREFIX    ?= /usr
DESTDIR   ?=

SDL2_CFLAGS ?= $(shell sdl2-config --cflags 2>/dev/null || pkg-config --cflags sdl2 2>/dev/null || echo "-I/usr/include/SDL2")
SDL2_LIBS   ?= $(shell sdl2-config --libs 2>/dev/null || pkg-config --libs sdl2 2>/dev/null || echo "-lSDL2")

CFLAGS    := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
             -Isrc -Ithird_party $(SDL2_CFLAGS) $(EXTRA_CFLAGS)
LDFLAGS   := -Wl,-rpath,'$$ORIGIN/../lib' $(EXTRA_LDFLAGS)
LDLIBS    := $(SDL2_LIBS) -ldl -lm -lpthread

SRCS      := src/main.c src/gl_loader.c src/mpv_engine.c src/ui_renderer.c src/player_ui.c
BIN       := build/vixglitcher

.PHONY: all clean install

all: $(BIN)

$(BIN): $(SRCS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) -o $(BIN) $(LDFLAGS) $(LDLIBS)

install: $(BIN)
	install -Dm755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/vixglitcher"
	install -Dm644 packaging/vixglitcher.desktop "$(DESTDIR)$(PREFIX)/share/applications/vixglitcher.desktop"
	install -Dm644 packaging/io.github.vixalt.vixglitcher.metainfo.xml "$(DESTDIR)$(PREFIX)/share/metainfo/io.github.vixalt.vixglitcher.metainfo.xml"
	install -Dm644 assets/icons/vixglitcher-512.png "$(DESTDIR)$(PREFIX)/share/icons/hicolor/512x512/apps/vixglitcher.png"
	install -Dm644 assets/icons/vixglitcher-256.png "$(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/vixglitcher.png"
	install -Dm644 assets/icons/vixglitcher-128.png "$(DESTDIR)$(PREFIX)/share/icons/hicolor/128x128/apps/vixglitcher.png"
	install -Dm644 assets/icons/vixglitcher-64.png  "$(DESTDIR)$(PREFIX)/share/icons/hicolor/64x64/apps/vixglitcher.png"
	install -Dm644 assets/icons/vixglitcher-256.png "$(DESTDIR)$(PREFIX)/share/vixglitcher/icons/vixglitcher-256.png"
	install -Dm644 assets/fonts/JetBrainsMono-Medium.ttf "$(DESTDIR)$(PREFIX)/share/vixglitcher/fonts/JetBrainsMono-Medium.ttf"
	install -Dm644 assets/fonts/JetBrainsMono-Bold.ttf   "$(DESTDIR)$(PREFIX)/share/vixglitcher/fonts/JetBrainsMono-Bold.ttf"
	install -Dm644 LICENSE "$(DESTDIR)$(PREFIX)/share/licenses/vixglitcher/LICENSE"

clean:
	rm -rf build AppDir
