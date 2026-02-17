CC = cc
CFLAGS = -Wall -Wextra -std=c11 -Iinclude -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lcurl -lpthread -lm

SRC_UTIL = $(wildcard src/util/*.c)
SRC_LLM  = $(wildcard src/llm/*.c)
SRC_AGENT = $(wildcard src/agent/*.c)
SRC_ATTR = $(wildcard src/attractor/*.c)
SRC_ALL  = $(SRC_UTIL) $(SRC_LLM) $(SRC_AGENT) $(SRC_ATTR)

OBJ_ALL = $(SRC_ALL:.c=.o)

LIB = libattractor.a
BIN = attractor

all: $(LIB) $(BIN)

$(LIB): $(OBJ_ALL)
	ar rcs $@ $^

$(BIN): src/main.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lattractor $(LDFLAGS)

src/main.o: src/main.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ_ALL) src/main.o $(LIB) $(BIN)

.PHONY: all clean
