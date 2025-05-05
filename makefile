# Makefile
CC=gcc
CFLAGS=-Wall -Wextra -pthread
LDFLAGS=-pthread

all: client server

common.o: common.c common.h
	$(CC) $(CFLAGS) -c common.c -o common.o

client.o: client.c client.h common.h
	$(CC) $(CFLAGS) -c client.c -o client.o

server.o: server.c server.h common.h
	$(CC) $(CFLAGS) -c server.c -o server.o

client: client.o common.o
	$(CC) $(LDFLAGS) client.o common.o -o client

server: server.o common.o
	$(CC) $(LDFLAGS) server.o common.o -o server

clean:
	rm -f *.o client server

.PHONY: all clean