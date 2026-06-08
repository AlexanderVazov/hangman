# Hangman Game

A two-player online word guessing race implemented in C, using TCP sockets and POSIX threads.

## Description

Two players connect to a server and each submit a secret word for their opponent to guess. Both players then race simultaneously to reveal their opponent's word one letter at a time. The player who finishes with fewer incorrect guesses wins. If both finish with the same number of incorrect guesses, the result is a tie.

## Getting Started

### Prerequisites

- GCC
- GNU Make
- A POSIX-compatible system (Linux / WSL)

### Building

Build both executables with:

```bash
make
```

Or build them individually:

```bash
make hangman-server
make hangman-client
```

To remove compiled binaries:

```bash
make clean
```

## Usage

### Server

Start the server by specifying the port to listen on:

```bash
./hangman-server <port>
```

Example:

```bash
./hangman-server 8080
```

The server prints `Listening on 8080...` when ready. It accepts exactly two clients before starting the game and stops accepting connections after that. If a client submits an invalid word (containing non-letter characters), the server responds with an error and waits for the next client instead.

### Client

```bash
./hangman-client <host> <port> <opponent-word>
```

- `<host>` — IP address of the server (e.g. `127.0.0.1`)
- `<port>` — port the server is listening on
- `<opponent-word>` — the word **your opponent** will have to guess (letters only)

Example (two terminals on the same machine):

```bash
# Terminal 1
./hangman-client 127.0.0.1 8080 elephant

# Terminal 2
./hangman-client 127.0.0.1 8080 castle
```

### Gameplay

Once both clients connect the game begins. On each turn the current state of the word is shown and you are prompted to enter a letter:

```
Word: _l_ph_nt
Incorrect guesses: b, d, z
```

Type a single letter and press Enter to guess. The game continues until you reveal every letter in your opponent's word. You then wait for your opponent to finish, after which the result is displayed:

```
YOU WIN! :)
Your incorrect guesses: b, d, z
Opponent's incorrect guesses: c, d, e, f, g
```

Possible outcomes: `YOU WIN! :)`, `You Lose! :(`, `Tie :/`

## Project Structure

```
.
├── Makefile
├── hangman-server.c       # Server implementation
├── hangman-client.c       # Client implementation
└── prerequisites/
    ├── game.h             # Game logic API
    └── game.c             # Game logic implementation (word state, guessing)
```

