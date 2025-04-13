CC = gcc
CFLAGS = -Wall -g $(shell pkg-config --cflags gtk4)
LIBS = $(shell pkg-config --libs gtk4) -lavformat -lavcodec -lavutil -lswresample -lpthread -lm

# Add platform-specific libraries
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS
    LIBS += -framework AudioToolbox -framework CoreFoundation
else ifeq ($(UNAME_S),Linux)
    # Linux
    LIBS += -lasound
else ifeq ($(OS),Windows_NT)
    # Windows
    LIBS += -lole32 -loleaut32 -luuid -lwinmm
endif

all: A9

A9: A9.c
	$(CC) $(CFLAGS) -o A9 A9.c $(LIBS)

clean:
	rm -f A9

# Example run target - replace with your own audio file
run: A9
	./A9 audio.wav

.PHONY: all clean run