CC ?= i686-w64-mingw32-gcc
CFLAGS ?= -O2 -s -mwindows -Wall -Wextra -Wno-unused-parameter \
          -D_WIN32_WINNT=0x0501 -Isrc
LDFLAGS ?= -no-pie -static -static-libgcc \
           -Wl,--major-subsystem-version=5,--minor-subsystem-version=1 \
           -lws2_32 -lwinmm -lgdi32 -luser32 -ladvapi32 -lshell32

BIN_DIR := bin
SRC_DIR := src
SOURCES := $(wildcard $(SRC_DIR)/*.c)
TARGET  := $(BIN_DIR)/xpdash-agent.exe

all: $(TARGET)

$(TARGET): $(SOURCES) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(BIN_DIR)

.PHONY: all clean
