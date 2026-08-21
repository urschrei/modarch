CC      = i686-w64-mingw32-gcc
VERSION ?= $(shell git describe --always --dirty 2>/dev/null || echo unknown)
# __USE_MINGW_ANSI_STDIO: msvcrt.dll's own printf does not support %llu;
# make the MinGW implementation explicit rather than relying on -std=c99
# to imply it.
CFLAGS  = -O2 -Wall -Wextra -std=c99 -D_WIN32_WINNT=0x0600 \
          -D__USE_MINGW_ANSI_STDIO=1 -DIMAGER_VERSION=\"$(VERSION)\"
LDFLAGS = -static -s

imager.exe: imager.c sha256.c sha256.h
	$(CC) $(CFLAGS) -o $@ imager.c sha256.c $(LDFLAGS)

clean:
	rm -f imager.exe

.PHONY: clean check-vista

check-vista: imager.exe
	@i686-w64-mingw32-objdump -p $< | grep -q 'api-ms-win-crt' \
	  && { echo "FAIL: UCRT-linked, will not run on Vista"; exit 1; } \
	  || echo "OK: msvcrt-linked"
