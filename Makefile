CC = gcc
CFLAGS = -Wall -Wextra -O2 -Wunused-function
LDFLAGS = -lraylib -lGL -lm -lpthread -ldl -lrt

all: chip8

chip8: chip8.c chip8.h
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
