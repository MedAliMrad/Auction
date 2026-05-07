/*
 * bot.c — Client robot d'enchères automatique
 *
 * Se connecte comme un client normal mais mise automatiquement
 * jusqu'à un plafond défini. Utile pour tester sans 4 personnes.
 *
 * Usage: ./bin/bot <IP> [port] [plafond] [room]
 *   plafond = montant maximum que le bot est prêt à miser (défaut: 500)
 *   room    = salle à rejoindre (1-4, défaut: 1)
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <pthread.h>
#include <signal.h>
#include "../include/common.h"

static int      sockfd      = -1;
static int      running     = 1;
static uint32_t max_bid     = 500;
static uint32_t cur_price   = 0;
static char     cur_leader[MAX_NAME_LEN] = "";
static char     bot_name[MAX_NAME_LEN]   = "RoBot";
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

static void place_bid(uint32_t amount) {
    Message m = {0};
    m.type   = MSG_BID;
    m.amount = amount;
    strncpy(m.name, bot_name, MAX_NAME_LEN - 1);
    send(sockfd, &m, sizeof(m), MSG_NOSIGNAL);
    printf("  [BOT] Mise envoyee : %u DT\n", amount);
}

static void *recv_thread(void *arg) {
    (void)arg;
    Message m;

    while (running) {
        ssize_t n = recv(sockfd, &m, sizeof(m), 0);
        if (n <= 0) {
            running = 0;
            break;
        }

        switch (m.type) {
        case MSG_AUCTION_INFO:
            pthread_mutex_lock(&state_mutex);
            cur_price = m.amount;
            pthread_mutex_unlock(&state_mutex);
            printf("  [BOT] Enchere: %s — prix: %u DT\n", m.item, m.amount);

            /* Initial bid after short delay */
            {
                uint32_t bid = m.amount + MIN_BID_INCREMENT;
                if (bid <= max_bid && strcmp(cur_leader, bot_name) != 0) {
                    sleep(2 + (rand() % 3));
                    place_bid(bid);
                }
            }
            break;

        case MSG_UPDATE:
            pthread_mutex_lock(&state_mutex);
            cur_price = m.amount;
            strncpy(cur_leader, m.name, MAX_NAME_LEN - 1);
            pthread_mutex_unlock(&state_mutex);

            printf("  [BOT] Mise de %s : %u DT\n", m.name, m.amount);

            /* Counter-bid if we're not the leader and within budget */
            if (strcmp(m.name, bot_name) != 0) {
                uint32_t counter = m.amount + MIN_BID_INCREMENT + (rand() % 20);
                if (counter <= max_bid) {
                    int delay = 3 + (rand() % 5);
                    printf("  [BOT] Reaction dans %d sec...\n", delay);
                    sleep(delay);

                    /* Re-check state after delay */
                    pthread_mutex_lock(&state_mutex);
                    int still_behind = (strcmp(cur_leader, bot_name) != 0);
                    uint32_t new_bid = cur_price + MIN_BID_INCREMENT + (rand() % 15);
                    pthread_mutex_unlock(&state_mutex);

                    if (still_behind && new_bid <= max_bid && running) {
                        place_bid(new_bid);
                    }
                } else {
                    printf("  [BOT] Plafond atteint (%u DT). J'abandonne.\n", max_bid);
                }
            }
            break;

        case MSG_BID_OK:
            printf("  [BOT] \033[32mMise acceptee !\033[0m %s\n", m.text);
            break;

        case MSG_BID_REJECT:
            printf("  [BOT] \033[31mMise refusee:\033[0m %s\n", m.text);
            break;

        case MSG_TIMER:
            if (m.amount <= 10 && m.amount > 0) {
                /* Last-second bid if we're losing */
                pthread_mutex_lock(&state_mutex);
                int losing = (strcmp(cur_leader, bot_name) != 0);
                uint32_t bid = cur_price + MIN_BID_INCREMENT;
                pthread_mutex_unlock(&state_mutex);

                if (losing && bid <= max_bid && (rand() % 3 == 0)) {
                    sleep(1);
                    place_bid(bid);
                }
            }
            break;

        case MSG_WINNER:
            printf("  [BOT] \033[35m%s\033[0m\n", m.text);
            if (strcmp(m.name, bot_name) == 0) {
                printf("  [BOT] J'ai gagne !\n");
            }
            /* Reset for next auction */
            pthread_mutex_lock(&state_mutex);
            cur_price = 0;
            memset(cur_leader, 0, MAX_NAME_LEN);
            pthread_mutex_unlock(&state_mutex);
            break;

        case MSG_NEXT_AUCTION:
            printf("  [BOT] %s\n", m.text);
            break;

        case MSG_BALANCE:
            printf("  [BOT] Solde: %u DT\n", m.balance);
            if (m.balance < MIN_BID_INCREMENT) {
                printf("  [BOT] Plus de solde. Deconnexion.\n");
                running = 0;
            }
            break;

        case MSG_CHAT:
            printf("  [BOT] %s\n", m.text);
            break;

        default:
            break;
        }
    }
    return NULL;
}

static void handle_sigint(int sig) {
    (void)sig;
    printf("\n  [BOT] Arret.\n");
    running = 0;
    close(sockfd);
    exit(0);
}

int main(int argc, char *argv[]) {
    srand(time(NULL));

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <IP> [port] [plafond] [room]\n", argv[0]);
        fprintf(stderr, "  plafond = mise max (defaut: 500)\n");
        fprintf(stderr, "  room    = salle 1-%d (defaut: 1)\n", NUM_ROOMS);
        return 1;
    }

    char server_ip[64] = {0};
    int port = DEFAULT_PORT;
    int room = 0;

    strncpy(server_ip, argv[1], sizeof(server_ip) - 1);
    if (argc >= 3) port    = atoi(argv[2]);
    if (argc >= 4) max_bid = (uint32_t)atoi(argv[3]);
    if (argc >= 5) { room = atoi(argv[4]) - 1; if (room < 0) room = 0; }

    snprintf(bot_name, MAX_NAME_LEN, "Bot_%d", (int)(time(NULL) % 1000));

    if (platform_network_init() != 0) return 1;
    atexit(platform_network_cleanup);

    signal(SIGINT, handle_sigint);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in saddr = {0};
    saddr.sin_family = AF_INET;
    saddr.sin_port   = htons(port);

    if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) <= 0) {
        fprintf(stderr, "Adresse invalide: %s\n", server_ip);
        return 1;
    }

    printf("  [BOT] %s — Plafond: %u DT — Salle: %d\n", bot_name, max_bid, room + 1);
    printf("  [BOT] Connexion a %s:%d...\n", server_ip, port);

    if (connect(sockfd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        perror("connect");
        return 1;
    }
    printf("  [BOT] Connecte !\n");

    /* JOIN */
    Message join = {0};
    join.type = MSG_JOIN;
    join.room = (uint8_t)room;
    strncpy(join.name, bot_name, MAX_NAME_LEN - 1);
    send(sockfd, &join, sizeof(join), 0);

    /* Single-threaded receive loop */
    pthread_t tid;
    pthread_create(&tid, NULL, recv_thread, NULL);

    /* Keep alive until done */
    while (running) {
        sleep(1);
    }

    close(sockfd);
    printf("  [BOT] Termine.\n");
    return 0;
}
