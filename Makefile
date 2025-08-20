CC:=gcc
CFLAGS:=-Wall -Wextra -Wno-unused-function
all: dim
dim: src/main.c
	$(CC) $(CFLAGS) $< -o $@
