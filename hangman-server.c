/*
 * hangman-server.c
 *
 * Two-player online hangman: the server coordinates two clients racing to
 * guess each other's submitted word.  The player with fewer incorrect
 * guesses when both words are solved wins.
 *
 * Protocol (newline-terminated text):
 *   Client → Server:
 *       <word>\n            — first message: the word the opponent must guess
 *       <letter>\n          — each subsequent guess
 *
 *   Server → Client (word validation):
 *       OK\n                — word accepted
 *       ERROR\n             — invalid word (non-letters); connection closed
 *
 *   Server → Client (each game state, initial and after every guess):
 *       WORD <masked>\n
 *       INCORRECT <a, b, c or empty>\n
 *       STATUS ONGOING\n | STATUS SOLVED\n
 *
 *   Server → Client (after both players finish):
 *       RESULT WIN|LOSE|TIE\n
 *       MY_INCORRECT <letters>\n
 *       OPP_INCORRECT <letters>\n
 */

#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include "prerequisites/game.h"

#define MAX_WORD_LEN 256
#define BUF_SIZE     1024

/* Shared state between the two game threads. */
typedef struct {
    secret_word_t   words[2];     /* words[i] = the word player i is guessing  */
    int             fds[2];       /* socket fd for each player                  */
    int             finish_count; /* how many players have finished (0-2)       */
    int             solved[2];    /* 1 if player i solved their word            */
    pthread_mutex_t mutex;
    pthread_cond_t  all_done;
} game_t;

static game_t g;

/* ── Utility: read one newline-terminated line from fd ───────────────────── */
/* Returns 1 on success (buf is null-terminated, '\n' stripped),
   0 on EOF or error.  '\r' is silently ignored.  buf is always set. */
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

/* ── Build helper: masked representation of the secret word ─────────────── */
static void build_masked(const secret_word_t *sw, char *buf)
{
    for (size_t i = 0; i < sw->word_length; i++) {
        char c;
        if (secret_word_letter_at(sw, i, &c) == SECRET_WORD_LETTER_REVEALED)
            buf[i] = c;
        else
            buf[i] = '_';
    }
    buf[sw->word_length] = '\0';
}

/* ── Build helper: "a, b, c" string from a letter_set ───────────────────── */
static void build_incorrect_str(letter_set_t incorrect,
                                char *buf, size_t buf_size)
{
    size_t pos   = 0;
    int    first = 1;
    for (char c = 'a'; c <= 'z'; c++) {
        if (!letter_set_contains(incorrect, c)) continue;
        if (!first) {
            if (pos + 2 >= buf_size) break;
            buf[pos++] = ',';
            buf[pos++] = ' ';
        }
        if (pos >= buf_size - 1) break;
        buf[pos++] = c;
        first = 0;
    }
    buf[pos] = '\0';
}

/* ── Send the current game state to the client ───────────────────────────── */
static int send_state(int fd, const secret_word_t *sw, int solved)
{
    char masked[MAX_WORD_LEN + 1];
    char incorrect[200];
    char msg[BUF_SIZE];

    build_masked(sw, masked);
    build_incorrect_str(sw->incorrect_guesses, incorrect, sizeof(incorrect));

    snprintf(msg, sizeof(msg), "WORD %s\nINCORRECT %s\nSTATUS %s\n",
             masked, incorrect, solved ? "SOLVED" : "ONGOING");
    return send_all(fd, msg);
}

/* ── Thread: manages one player's game loop ─────────────────────────────── */
static void *player_thread(void *arg)
{
    int            idx     = (int)(intptr_t)arg;
    int            opp_idx = 1 - idx;
    int            fd      = g.fds[idx];
    secret_word_t *sw      = &g.words[idx];
    int            game_solved = 0;

    /* Send the initial (all-hidden) game state. */
    if (send_state(fd, sw, 0) < 0) goto finish;

    /* Main game loop: receive guesses, update state, reply. */
    while (1) {
        char line[32];
        if (!read_line(fd, line, sizeof(line))) goto finish;

        /* Empty line: re-send current state so client can re-prompt. */
        if (line[0] == '\0') {
            if (send_state(fd, sw, 0) < 0) goto finish;
            continue;
        }

        /* Process the guess (invalid / already-guessed: state unchanged). */
        secret_word_guess(sw, line[0]);

        int solved = secret_word_is_solved(sw);
        if (send_state(fd, sw, solved) < 0) goto finish;

        if (solved) {
            game_solved = 1;
            break;
        }
    }

finish:
    /* Synchronise: wait until both threads have finished. */
    pthread_mutex_lock(&g.mutex);
    g.solved[idx] = game_solved;
    g.finish_count++;
    if (g.finish_count == 2)
        pthread_cond_broadcast(&g.all_done);
    while (g.finish_count < 2)
        pthread_cond_wait(&g.all_done, &g.mutex);
    pthread_mutex_unlock(&g.mutex);

    /* If we disconnected before solving, just close and exit. */
    if (!game_solved) {
        close(fd);
        return NULL;
    }

    /* Determine result: fewer incorrect guesses wins. */
    size_t my_inc  = letter_set_size(g.words[idx].incorrect_guesses);
    size_t opp_inc = letter_set_size(g.words[opp_idx].incorrect_guesses);

    const char *result;
    if (!g.solved[opp_idx])
        result = "WIN";          /* opponent disconnected */
    else if (my_inc < opp_inc)
        result = "WIN";
    else if (my_inc > opp_inc)
        result = "LOSE";
    else
        result = "TIE";

    char my_inc_str[200], opp_inc_str[200];
    build_incorrect_str(g.words[idx].incorrect_guesses,
                        my_inc_str,  sizeof(my_inc_str));
    build_incorrect_str(g.words[opp_idx].incorrect_guesses,
                        opp_inc_str, sizeof(opp_inc_str));

    char msg[BUF_SIZE];
    snprintf(msg, sizeof(msg),
             "RESULT %s\nMY_INCORRECT %s\nOPP_INCORRECT %s\n",
             result, my_inc_str, opp_inc_str);
    send_all(fd, msg);
    close(fd);
    return NULL;
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);

    /* Create listening socket. */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;   /* 0.0.0.0 */
    addr.sin_port        = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 2) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("Listening on %d...\n", port);
    fflush(stdout);

    /*
     * Accept clients one at a time.  If a client submits an invalid word the
     * connection is closed with an error message and we wait for the next one.
     */
    char player_words[2][MAX_WORD_LEN + 1];

    for (int i = 0; i < 2; ) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int cfd = accept(server_fd,
                         (struct sockaddr *)&client_addr, &client_len);
        if (cfd < 0) continue;

        char word_buf[MAX_WORD_LEN + 2];
        if (!read_line(cfd, word_buf, sizeof(word_buf))) {
            close(cfd);
            continue;
        }

        /* Validate: word must consist entirely of letters. */
        secret_word_t dummy;
        if (!secret_word_init_from_c_string(&dummy, word_buf)) {
            send_all(cfd, "ERROR\n");
            close(cfd);
            continue;
        }
        secret_word_free(&dummy);

        send_all(cfd, "OK\n");
        g.fds[i] = cfd;
        strncpy(player_words[i], word_buf, MAX_WORD_LEN);
        player_words[i][MAX_WORD_LEN] = '\0';
        i++;
    }

    /* No more connections accepted. */
    close(server_fd);

    /*
     * Cross-assign words:
     *   Player 0 guesses player 1's word.
     *   Player 1 guesses player 0's word.
     */
    secret_word_init_from_c_string(&g.words[0], player_words[1]);
    secret_word_init_from_c_string(&g.words[1], player_words[0]);

    pthread_mutex_init(&g.mutex,    NULL);
    pthread_cond_init(&g.all_done,  NULL);

    pthread_t threads[2];
    if (pthread_create(&threads[0], NULL,
                       player_thread, (void *)(intptr_t)0) != 0) {
        perror("pthread_create");
        return 1;
    }
    if (pthread_create(&threads[1], NULL,
                       player_thread, (void *)(intptr_t)1) != 0) {
        perror("pthread_create");
        return 1;
    }

    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);

    secret_word_free(&g.words[0]);
    secret_word_free(&g.words[1]);
    pthread_mutex_destroy(&g.mutex);
    pthread_cond_destroy(&g.all_done);

    return 0;
}
