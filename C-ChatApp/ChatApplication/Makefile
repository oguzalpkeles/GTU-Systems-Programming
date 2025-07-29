CC = gcc
CFLAGS = -Wall -pthread

all: chatserver chatclient

chatserver: server/chatserver.c
	$(CC) $(CFLAGS) -o chatserver server/chatserver.c -lpthread

chatclient: client/chatclient.c
	$(CC) $(CFLAGS) -o chatclient client/chatclient.c -lpthread

clean:
	rm -f chatserver chatclient example_log.txt

