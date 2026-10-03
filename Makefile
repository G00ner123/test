# Makefile für den Hyprland Ultimate Cyber-Visualizer Pro (C/SDL2)
#
# Verwendung:
#   make          -> baut das Programm als "./visualizer"
#   make run      -> baut und startet es direkt
#   make clean    -> entfernt die gebaute Binary

CC      := gcc
CFLAGS  := -std=c11 -O3 -march=native -flto -Wall -Wextra
LDFLAGS := -flto
PKGS    := sdl2 SDL2_ttf libpulse-simple libpulse
LIBS    := $(shell pkg-config --libs $(PKGS)) -lm
INCLUDES:= $(shell pkg-config --cflags $(PKGS))

TARGET  := visualizer
SRC     := visualizer.c

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(TARGET) $(SRC) $(LIBS) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
