CC ?= i686-w64-mingw32-gcc
CFLAGS ?= -O2 -s -Wall -Wextra -D_WIN32_WINNT=0x0501
LDFLAGS ?= -no-pie -static -static-libgcc \
           -Wl,--major-subsystem-version=5,--minor-subsystem-version=1 \
           -lwinmm

BIN_DIR := bin
SRC_DIR := src
TARGET := $(BIN_DIR)/eax-test.exe

all: $(TARGET)

$(TARGET): $(SRC_DIR)/main.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(BIN_DIR)

.PHONY: all clean
