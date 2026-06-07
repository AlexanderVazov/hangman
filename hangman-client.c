/*
 * hangman-client.c
 *
 * Usage: ./hangman-client <host> <port> <opponent-word>
 *
 * Connects to a hangman-server, submits <opponent-word> (the word the
 * opponent will have to guess), then plays the game by guessing the word
 * that the opponent submitted.
 *
 * Protocol details are in hangman-server.c.  The client side is:
 *   1. Send "<opponent-word>\n".
 *   2. Receive "OK\n" (or "ERROR\n" → exit).
 *   3. Loop:
 *        a. Read a WORD / INCORRECT / STATUS triplet from the server.
 *        b. Display "Word: ..." and "Incorrect guesses: ...".
 *        c. If STATUS is ONGOING  → read one letter from stdin, send it.
 *           If STATUS is SOLVED   → wait; the server will send RESULT next.
 *        d. When RESULT arrives, print outcome and quit.
 */

#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUF_SIZE 1024

/* ── Utility: read one newline-terminated line from fd ───────────────────── */
/* Returns 1 on success, 0 on EOF/error.  buf is always null-terminated. */
static int read_line(int fd, char *buf, size_t max_len)
{
    size_t i = 0;
    char c;
    buf[0] = '\0';
    while (i < max_len - 1) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return 0;
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return 1;
}

/* ── Utility: send the full message to fd ────────────────────────────────── */
static int send_all(int fd, const char *msg)
{
    size_t len  = strlen(msg);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, msg + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <host> <port> <opponent-word>\n", argv[0]);
        return 1;
    }

    const char *host     = argv[1];
    int         port     = atoi(argv[2]);
    const char *opp_word = argv[3];

    /* Connect to server. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid host address: %s\n", host);
        close(fd);
        return 1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    /* ── Step 1: send our word (the word the opponent must guess) ── */
    char send_buf[BUF_SIZE];
    snprintf(send_buf, sizeof(send_buf), "%s\n", opp_word);
    if (send_all(fd, send_buf) < 0) {
        perror("send");
        close(fd);
        return 1;
    }

    /* ── Step 2: wait for OK or ERROR ── */
    char line[BUF_SIZE];
    if (!read_line(fd, line, sizeof(line))) {
        fprintf(stderr, "Server closed connection unexpectedly\n");
        close(fd);
        return 1;
    }
    if (strcmp(line, "OK") != 0) {
        if (strcmp(line, "ERROR") == 0)
            fprintf(stderr, "Server rejected word: invalid\n");
        else
            fprintf(stderr, "Unexpected server response: %s\n", line);
        close(fd);
        return 1;
    }

    /* ── Step 3: main game loop ── */
    while (1) {
        /* Each iteration begins by reading the next server message. */
        if (!read_line(fd, line, sizeof(line))) break;

        /* ── RESULT: game over ── */
        if (strncmp(line, "RESULT ", 7) == 0) {
            char *result_type = line + 7;   /* "WIN", "LOSE", or "TIE" */

            char my_line[BUF_SIZE], opp_line[BUF_SIZE];
            if (!read_line(fd, my_line,  sizeof(my_line)))  break;
            if (!read_line(fd, opp_line, sizeof(opp_line))) break;

            const char *outcome;
            if (strcmp(result_type, "WIN") == 0)
                outcome = "YOU WIN! :)";
            else if (strcmp(result_type, "LOSE") == 0)
                outcome = "You Lose! :(";
            else
                outcome = "Tie :/";

            /* "MY_INCORRECT " is 13 chars; "OPP_INCORRECT " is 14 chars. */
            char *my_letters =
                (strncmp(my_line,  "MY_INCORRECT ",  13) == 0)
                ? my_line  + 13 : "";
            char *opp_letters =
                (strncmp(opp_line, "OPP_INCORRECT ", 14) == 0)
                ? opp_line + 14 : "";

            printf("%s\n", outcome);
            printf("Your incorrect guesses: %s\n",     my_letters);
            printf("Opponent's incorrect guesses: %s\n", opp_letters);
            break;
        }

        /* ── WORD <masked>: game state frame ── */
        if (strncmp(line, "WORD ", 5) != 0) continue; /* unexpected line */
        char *masked = line + 5;

        /* Read the INCORRECT line. */
        char inc_line[BUF_SIZE];
        if (!read_line(fd, inc_line, sizeof(inc_line))) break;
        /* "INCORRECT " is 10 chars. */
        char *incorrect =
            (strncmp(inc_line, "INCORRECT ", 10) == 0)
            ? inc_line + 10 : "";

        /* Read the STATUS line. */
        char status_line[BUF_SIZE];
        if (!read_line(fd, status_line, sizeof(status_line))) break;

        /* Display current state. */
        printf("Word: %s\n", masked);
        printf("Incorrect guesses: %s\n", incorrect);
        fflush(stdout);

        /* If the word is solved, wait for the RESULT (next loop iteration). */
        if (strcmp(status_line, "STATUS SOLVED") == 0) continue;

        /* ── Read one guess from stdin ── */
        char input[256];
        char guess = '\0';
        while (fgets(input, sizeof(input), stdin) != NULL) {
            char c = input[0];
            if (c != '\n' && c != '\r' && c != '\0') {
                guess = c;
                break;
            }
            /* Empty line: re-prompt silently (server state unchanged). */
        }

        if (guess == '\0') break; /* stdin closed */

        /* Send the guess to the server. */
        char guess_buf[4];
        snprintf(guess_buf, sizeof(guess_buf), "%c\n", guess);
        if (send_all(fd, guess_buf) < 0) break;
    }

    close(fd);
    return 0;
}
