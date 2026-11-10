CXXFLAGS   := -O2 -Wall
CFLAGS     := -O2 -Wall
FRAMEWORKS := -framework IOKit -framework CoreFoundation

# start   opens the card, runs the cold init, reads the isochronous stream
# diag    read-only register dump, first thing to run when the picture is black
# status  live input status: resolution, frame rate, whether a source is locked
DRIVER := start diag status

all: $(DRIVER) demux

$(DRIVER): %: src/%.cpp $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) -o $@ $< $(FRAMEWORKS)

# demux splits the card's single endpoint into video frames and audio blocks
demux: src/demux.c src/hd60s_ipc.h
	$(CC) $(CFLAGS) -o $@ src/demux.c

# OBS source plugin. libobs's public headers are not shipped inside OBS.app, so
# point OBS_SRC at an obs-studio checkout of the version you run:
#
#   git clone --depth 1 -b 31.1.2 https://github.com/obsproject/obs-studio
#   make obs-plugin OBS_SRC=obs-studio
OBS_SRC ?= obs-studio
OBS_APP ?= /Applications/OBS.app
# libobs's headers include <simde/x86/sse2.h> on arm64: brew install simde
SIMDE   ?= $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)/opt/simde/include
PLUGIN  := $(HOME)/Library/Application Support/obs-studio/plugins/hd60s.plugin

obs-plugin:
	@test -f "$(OBS_SRC)/libobs/obs-module.h" || \
	  { echo "set OBS_SRC to an obs-studio checkout"; exit 1; }
	@test -d "$(OBS_APP)/Contents/Frameworks/libobs.framework" || \
	  { echo "set OBS_APP to your OBS.app (tried $(OBS_APP))"; exit 1; }
	@test -f "$(SIMDE)/simde/x86/sse2.h" || \
	  { echo "simde not found in $(SIMDE). Run: brew install simde"; exit 1; }
	mkdir -p "$(PLUGIN)/Contents/MacOS"
	cp obs-plugin/Info.plist "$(PLUGIN)/Contents/Info.plist"
	$(CC) -O2 -Wall -bundle -o "$(PLUGIN)/Contents/MacOS/hd60s" \
	  -I$(OBS_SRC)/libobs -I"$(SIMDE)" -Iobs-plugin/stub obs-plugin/hd60s-source.c \
	  -DHD60S_CAPTURE_CMD='"$(CURDIR)/hd60s-obs"' \
	  -F$(OBS_APP)/Contents/Frameworks -framework libobs
	codesign --force --sign - "$(PLUGIN)"
	@echo "installed to $(PLUGIN)"

# Hardware-free checks for the tint-bisect stop map / seq parsing.
test-tint-bisect:
	cd tools && python3 test_tint_bisect_map.py

clean:
	rm -f $(DRIVER) demux

.PHONY: all clean obs-plugin test-tint-bisect
