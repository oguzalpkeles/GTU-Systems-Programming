# Makefile for AdaBank Server and Client

CC = gcc

# Targets to build
TARGETS = server client

# Default rule: make or make all
all: $(TARGETS)

server: server.c
	$(CC) $(CFLAGS) server.c -o server

client: client.c
	$(CC) $(CFLAGS) client.c -o client

# Clean rule: remove generated binaries
clean:
	rm -f $(TARGETS)
