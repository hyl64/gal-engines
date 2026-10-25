# T4 mpvdev harness — standalone build of the libmpv fallback backend.
#
# NO libmpv at compile OR link time: video_player_mpv.cpp resolves the
# library via dlopen() at run time (candidates /opt/homebrew/lib,
# /usr/local/lib, ...). What we DO link for the harness: SDL2 only.
#
#   make            → ./mpvdev_harness
#   make probe      → dlopen availability check against the real system lib
#   make fixtures   → ffmpeg-generated test media (h264 mp4 + wmv2 avi + 1080p)
#   make test       → probe + play both fixtures 3s, save ppm frames
#   make clean
#
# Vendored headers: client.h / render.h (libmpv 2.x API, ISC notice kept in
# client.h header). They exist for ABI cross-checks and future maintainers;
# video_player_mpv.cpp does NOT include them (that would be a compile-time
# dependency — we mirror the ABI inline instead).

CXX      ?= c++
SDL2_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || echo -I/opt/homebrew/include/SDL2 -D_THREAD_SAFE)
SDL2_LIBS   := $(shell sdl2-config --libs 2>/dev/null || echo -L/opt/homebrew/lib -lSDL2main -lSDL2 -Wl,-framework,Cocoa)
# code uses <SDL2/SDL.h> form (like ONScripter.h) → need the PARENT dir too
HOMEBREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -I../src/onsyuri -I$(HOMEBREW_PREFIX)/include $(SDL2_CFLAGS)
LDFLAGS  := $(SDL2_LIBS)

BACKEND_SRC := ../src/onsyuri/video_player_mpv.cpp
HARNESS     := mpvdev_harness

.PHONY: all probe fixtures test clean

all: $(HARNESS)

$(HARNESS): main.cpp $(BACKEND_SRC) ../src/onsyuri/video_player_mpv.h
	$(CXX) $(CXXFLAGS) main.cpp $(BACKEND_SRC) $(LDFLAGS) -o $@

# dlopen probe: prints mpvIsAvailable and exits 0/1
probe: $(HARNESS)
	./$(HARNESS) probe

# Test media (testdata dir under mpvdev/)
fixtures:
	mkdir -p fixtures
	# 1) h264 mp4 (main path container), 6s 640x480 @30, tone audio
	ffmpeg -y -f lavfi -i "testsrc=size=640x480:rate=30:duration=6" \
	       -f lavfi -i "sine=frequency=440:duration=6" \
	       -c:v libx264 -pix_fmt yuv420p -c:a aac fixtures/h264.mp4
	# 2) WMV2 (wmv3 decoder fourcc) AVI — AVFoundation CANNOT play this;
	#    the whole point of the libmpv fallback.
	ffmpeg -y -f lavfi -i "smptebars=size=640x480:rate=25:duration=5" \
	       -c:v wmv2 -pix_fmt yuv420p -an fixtures/wmv2.avi
	# 3) 1080p60 h264 — efficiency measurement of the SW fallback path
	ffmpeg -y -f lavfi -i "testsrc2=size=1920x1080:rate=60:duration=5" \
	       -c:v libx264 -preset ultrafast -pix_fmt yuv420p -an fixtures/h264-1080p60.mp4

test: $(HARNESS) fixtures
	mkdir -p out
	@echo "== probe ==";              ./$(HARNESS) probe
	@echo "== h264.mp4 (3s) ==";      ./$(HARNESS) play fixtures/h264.mp4 3 --out out/h264
	@echo "== wmv2.avi (3s) ==";      ./$(HARNESS) play fixtures/wmv2.avi 3 --out out/wmv2
	@echo "== loop+click stop ==";    ./$(HARNESS) play fixtures/h264.mp4 10 --loop --click-stop 1500 --out out/loop
	@echo "== 1080p60 capped-720 =="; ./$(HARNESS) play fixtures/h264-1080p60.mp4 3 --out out/p1080c
	@echo "== 1080p60 native-1080 =="; ONSYURI_MPV_MAXW=1920 ONSYURI_MPV_MAXH=1080 ./$(HARNESS) play fixtures/h264-1080p60.mp4 3 --out out/p1080n
	@echo "ppm frames in out/ — convert with: sips -s format png out/h264.ppm --out out/h264.png"

# missing-file + disabled-backend negative paths
test-negative: $(HARNESS)
	./$(HARNESS) play /nonexistent/x.mp4 1 || echo "OK: missing file -> false"
	ONSYURI_MPV_DISABLE=1 ./$(HARNESS) probe | grep -q mpvIsAvailable=0 && echo "OK: kill switch"
	ONSYURI_MPV_LIB=/no/such/lib.dylib ./$(HARNESS) probe || echo "OK: bad lib path -> false"

out:
	mkdir -p out

clean:
	rm -f $(HARNESS) out/*.ppm
