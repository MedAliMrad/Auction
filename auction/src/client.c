/*
 * client.c — Client d'enchères en temps réel (version complète)
 *
 * Supporte : salles multiples, solde, historique, timer étendu, etc.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <pthread.h>
#include <signal.h>
#include "../include/common.h"

/* ─── Shared state ─────── */
static int  sockfd  = -1;
static int  running = 1;
static pthread_mutex_t running_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t current_price = 0;
static uint32_t my_balance    = STARTING_BALANCE;
static char     current_leader[MAX_NAME_LEN] = "aucun";
static pthread_mutex_t price_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ══════════════════════════════════════════════════
   Display a message based on type
   ══════════════════════════════════════════════════ */
static void display_message(const Message *m) {
    switch (m->type) {

    case MSG_AUCTION_INFO:
        printf("\n\033[1;36m╔══════════════════════════════════════════════╗\033[0m\n");
        printf(  "\033[1;36m║  ENCHERE EN COURS                              ║\033[0m\n");
        printf(  "\033[1;36m╚══════════════════════════════════════════════╝\033[0m\n");
        printf("  %s\n\n", m->text);
        pthread_mutex_lock(&price_mutex);
        current_price = m->amount;
        pthread_mutex_unlock(&price_mutex);
        break;

    case MSG_UPDATE:
        printf("\n  \033[1;33m** %s\033[0m\n", m->text);
        pthread_mutex_lock(&price_mutex);
        current_price = m->amount;
        strncpy(current_leader, m->name, MAX_NAME_LEN - 1);
        pthread_mutex_unlock(&price_mutex);
        printf("  \033[90m[Meneur : %s — %u DT]\033[0m\n\n", m->name, m->amount);
        break;

    case MSG_BID_OK:
        printf("  \033[1;32m[OK] %s\033[0m\n", m->text);
        break;

    case MSG_BID_REJECT:
        printf("  \033[1;31m[X]  %s\033[0m\n", m->text);
        break;

    case MSG_TIMER:
        if (m->amount <= 5 && m->amount > 0)
            printf("  \033[1;31m[%02us] %s\033[0m\n", m->amount, m->text);
        else if (m->amount <= 10)
            printf("  \033[33m[%02us] %s\033[0m\n", m->amount, m->text);
        else
            printf("  \033[90m[%02us] %s\033[0m\n", m->amount, m->text);
        break;

    case MSG_WINNER:
        printf("\n\033[1;35m╔══════════════════════════════════════════════╗\033[0m\n");
        printf(  "\033[1;35m║  FIN DE L'ENCHERE                              ║\033[0m\n");
        printf(  "\033[1;35m╚══════════════════════════════════════════════╝\033[0m\n");
        printf("  \033[1;35m%s\033[0m\n\n", m->text);
        break;

    case MSG_CHAT:
        printf("  \033[90m%s\033[0m\n", m->text);
        break;

    case MSG_CONNECTED:
        printf("  \033[36m[%d connecte(s) dans cette salle]\033[0m\n", m->amount);
        break;

    case MSG_HISTORY:
        printf("\n  \033[1;34m--- Historique des mises ---\033[0m\n");
        printf("  %s", m->text);
        printf("  \033[1;34m----------------------------\033[0m\n\n");
        break;

    case MSG_BALANCE:
        pthread_mutex_lock(&price_mutex);
        my_balance = m->balance;
        pthread_mutex_unlock(&price_mutex);
        printf("  \033[1;32m[Solde] %s\033[0m\n", m->text);
        break;

    case MSG_NEXT_AUCTION:
        printf("\n  \033[1;33m>>> %s\033[0m\n\n", m->text);
        break;

    case MSG_ROOM_LIST:
        printf("  %s\n", m->text);
        break;

    default:
        break;
    }
    fflush(stdout);
}

/* ══════════════════════════════════════════════════
   Receive thread
   ══════════════════════════════════════════════════ */
static void *recv_thread(void *arg) {
    (void)arg;
    Message m;

    while (1) {
        pthread_mutex_lock(&running_mutex);
        int go = running;
        pthread_mutex_unlock(&running_mutex);
        if (!go) break;

        ssize_t n = recv(sockfd, &m, sizeof(m), 0);
        if (n <= 0) {
            printf("\n  \033[1;31mConnexion au serveur perdue.\033[0m\n");
            pthread_mutex_lock(&running_mutex);
            running = 0;
            pthread_mutex_unlock(&running_mutex);
            break;
        }
        display_message(&m);
    }
    return NULL;
}

/* ══════════════════════════════════════════════════
   Send thread
   ══════════════════════════════════════════════════ */
static void *send_thread(void *arg) {
    char *my_name = (char *)arg;
    char  buf[64];
    Message m;

    printf("\n  \033[1mCommandes :\033[0m\n");
    printf("    <nombre>  → placer une mise\n");
    printf("    q         → quitter\n\n");

    while (1) {
        pthread_mutex_lock(&running_mutex);
        int go = running;
        pthread_mutex_unlock(&running_mutex);
        if (!go) break;

        pthread_mutex_lock(&price_mutex);
        uint32_t cur = current_price;
        uint32_t bal = my_balance;
        pthread_mutex_unlock(&price_mutex);

        printf("  \033[1mMise\033[0m (prix: %u DT | min: %u | solde: %u) > ",
               cur, cur + MIN_BID_INCREMENT, bal);
        fflush(stdout);

        if (!fgets(buf, sizeof(buf), stdin)) break;
        buf[strcspn(buf, "\n")] = '\0';

        if (buf[0] == 'q' || buf[0] == 'Q') {
            memset(&m, 0, sizeof(m));
            m.type = MSG_BYE;
            strncpy(m.name, my_name, MAX_NAME_LEN - 1);
            send(sockfd, &m, sizeof(m), MSG_NOSIGNAL);
            pthread_mutex_lock(&running_mutex);
            running = 0;
            pthread_mutex_unlock(&running_mutex);
            break;
        }

        long val = atol(buf);
        if (val <= 0) {
            printf("  \033[31mEntree invalide. Entrez un nombre positif.\033[0m\n");
            continue;
        }

        memset(&m, 0, sizeof(m));
        m.type   = MSG_BID;
        m.amount = (uint32_t)val;
        strncpy(m.name, my_name, MAX_NAME_LEN - 1);

        if (send(sockfd, &m, sizeof(m), MSG_NOSIGNAL) < 0) {
            printf("  \033[31mErreur d'envoi.\033[0m\n");
            pthread_mutex_lock(&running_mutex);
            running = 0;
            pthread_mutex_unlock(&running_mutex);
            break;
        }
    }
    return NULL;
}

/* ══════════════════════════════════════════════════
   SIGINT
   ══════════════════════════════════════════════════ */
static void handle_sigint(int sig) {
    (void)sig;
    printf("\n  Au revoir !\n");
    pthread_mutex_lock(&running_mutex);
    running = 0;
    pthread_mutex_unlock(&running_mutex);
    close(sockfd);
    exit(0);
}

/* ══════════════════════════════════════════════════
   Main
   ══════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    char server_ip[64];
    int  port = DEFAULT_PORT;
    char name[MAX_NAME_LEN];
    struct sockaddr_in saddr;
    pthread_t rtid, stid;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <IP_SERVEUR> [port]\n", argv[0]);
        fprintf(stderr, "  ex : %s 192.168.1.10\n", argv[0]);
        fprintf(stderr, "  ex : %s 10.147.17.5 8080\n", argv[0]);
        return 1;
    }
    strncpy(server_ip, argv[1], sizeof(server_ip) - 1);
    if (argc >= 3) port = atoi(argv[2]);

    if (platform_network_init() != 0) {
        fprintf(stderr, "network init failed\n");
        return 1;
    }
    atexit(platform_network_cleanup);

    signal(SIGINT, handle_sigint);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    /* Pseudo */
    printf("\n");
    printf("  ╔══════════════════════════════════════════════╗\n");
    printf("  ║   SYSTEME D'ENCHERES EN TEMPS REEL v2.0      ║\n");
    printf("  ╠══════════════════════════════════════════════╣\n");
    printf("  ║  Solde de depart : %4d DT                    ║\n", STARTING_BALANCE);
    printf("  ║  Mise minimum    : +%d DT                     ║\n", MIN_BID_INCREMENT);
    printf("  ║  Salles          : %d                          ║\n", NUM_ROOMS);
    printf("  ╚══════════════════════════════════════════════╝\n\n");
    printf("  Serveur : %s:%d\n\n", server_ip, port);

    printf("  Votre pseudo : ");
    fflush(stdout);
    if (!fgets(name, sizeof(name), stdin)) return 1;
    name[strcspn(name, "\n")] = '\0';
    if (strlen(name) == 0) strncpy(name, "Anonyme", MAX_NAME_LEN - 1);

    /* Room selection */
    int room = 0;
    printf("  Salle (1-%d, defaut=1) : ", NUM_ROOMS);
    fflush(stdout);
    char rbuf[8];
    if (fgets(rbuf, sizeof(rbuf), stdin)) {
        int r = atoi(rbuf);
        if (r >= 1 && r <= NUM_ROOMS) room = r - 1;
    }

    /* TCP connection */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port   = htons(port);

    if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) <= 0) {
        fprintf(stderr, "  Adresse invalide : %s\n", server_ip);
        close(sockfd);
        return 1;
    }

    printf("\n  Connexion en cours...\n");
    if (connect(sockfd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        perror("  connect");
        close(sockfd);
        return 1;
    }
    printf("  \033[1;32mConnecte !\033[0m Bienvenue \033[1m%s\033[0m (salle %d).\n",
           name, room + 1);

    /* Send MSG_JOIN with room */
    Message join = {0};
    join.type = MSG_JOIN;
    join.room = (uint8_t)room;
    strncpy(join.name, name, MAX_NAME_LEN - 1);
    send(sockfd, &join, sizeof(join), 0);

    /* Start threads */
    pthread_create(&rtid, NULL, recv_thread, NULL);
    pthread_create(&stid, NULL, send_thread, name);

    pthread_join(rtid, NULL);
    pthread_join(stid, NULL);

    close(sockfd);
    printf("\n  \033[90mDeconnecte. A bientot !\033[0m\n\n");
    return 0;
}
