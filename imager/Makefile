CC      = i686-w64-mingw32-gcc
CFLAGS  = -O2 -Wall -Wextra -std=c99 -D_WIN32_WINNT=0x0600
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
