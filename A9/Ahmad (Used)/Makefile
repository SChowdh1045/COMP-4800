CC = gcc
CFLAGS = -Wall -g $(shell pkg-config --cflags gtk+-3.0)
LIBS = $(shell pkg-config --libs gtk+-3.0) -lavformat -lavcodec -lavutil -lswresample -lpthread -lm

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
    LIBS += -lwinmm
endif

all: A9

A9: A9.c
	$(CC) $(CFLAGS) -o A9 A9.c $(LIBS)

clean:
	rm -f A9

# Example run target - replace with your own audio file
run: A9
	./A9 pencil.wav

.PHONY: all clean run