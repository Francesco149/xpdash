CC ?= i686-w64-mingw32-gcc
CFLAGS ?= -O2 -s -Wall -Wextra -D_WIN32_WINNT=0x0501 -no-pie -static -static-libgcc -Wl,--major-subsystem-version=5,--minor-subsystem-version=1
LDFLAGS ?= -ld3d8 -lgdi32 -luser32 -lwinmm

all: bin/test-d3d8.exe

bin/test-d3d8.exe: src/main.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -rf bin

.PHONY: all clean
