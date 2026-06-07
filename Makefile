CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -Iprerequisites

hangman-server: hangman-server.c prerequisites/game.c prerequisites/game.h
	$(CC) $(CFLAGS) -o $@ hangman-server.c prerequisites/game.c -lpthread

hangman-client: hangman-client.c
	$(CC) $(CFLAGS) -o $@ hangman-client.c

clean:
	rm -f hangman-server hangman-client

.PHONY: clean
