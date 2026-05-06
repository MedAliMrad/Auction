/*
 * client.c — Client d'enchères en temps réel
 *
 * Architecture :
 *   main()          → connexion TCP, saisie du pseudo, démarre les 2 threads
 *   recv_thread()   → reçoit et affiche tous les messages du serveur
 *   send_thread()   → lit les mises au clavier et les envoie
 *
 * Synchronisation :
 *   running_mutex   → flag partagé d'arrêt entre les 2 threads
 *
 * Compilation : gcc -Wall -o bin/client src/client.c -lpthread
 * Lancement   : ./bin/client <IP_SERVEUR> [port]
 *
 * Depuis Termux (téléphone) ou autre PC : même commande
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "../include/common.h"

/* ─── État partagé entre les deux threads ─────── */
static int  sockfd  = -1;
static int  running = 1;
static pthread_mutex_t running_mutex = PTHREAD_MUTEX_INITIALIZER;

/* prix courant conn
u par le client (pour affichage) */
static uint32_t current_price = 0;
static char     current_leader[MAX_NAME_LEN] = "aucun";
static pthread_mutex_t price_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ══════════════════════════════════════════════════
   Affichage coloré selon le type de message
   ══════════════════════════════════════════════════ */
static void display_message(const Message *m) {
    switch (m->type) {

    case MSG_AUCTION_INFO:
        printf("\n\033[1;36m╔══════════════════════════════════════════╗\033[0m\n");
        printf(  "\033[1;36m║  ENCHERE EN COURS                        ║\033[0m\n");
        printf(  "\033[1;36m╚══════════════════════════════════════════╝\033[0m\n");
        printf("  %s\n\n", m->text);
        pthread_mutex_lock(&price_mutex);
        current_price = m->amount;
        pthread_mutex_unlock(&price_mutex);
        break;

    case MSG_UPDATE:
        printf("\n  \033[1;33m🔔  %s\033[0m\n", m->text);
        pthread_mutex_lock(&price_mutex);
        current_price = m->amount;
        strncpy(current_leader, m->name, MAX_NAME_LEN - 1);
        pthread_mutex_unlock(&price_mutex);
        printf("  \033[90m[Meilleure mise : %u DT par %s]\033[0m\n\n",
               m->amount, m->name);
        break;

    case MSG_BID_OK:
        printf("  \033[1;32m✔  %s\033[0m\n", m->text);
        break;

    case MSG_BID_REJECT:
        printf("  \033[1;31m✘  %s\033[0m\n", m->text);
        break;

    case MSG_TIMER:
        if (m->amount <= 5 && m->amount > 0)
            printf("  \033[1;31m⏱  %s\033[0m\n", m->text);
        else if (m->amount <= 10)
            printf("  \033[33m⏱  %s\033[0m\n", m->text);
        else
            printf("  \033[90m⏱  %s\033[0m\n", m->text);
        break;

    case MSG_WINNER:
        printf("\n\033[1;35m╔══════════════════════════════════════════╗\033[0m\n");
        printf(  "\033[1;35m║  FIN DE L'ENCHERE                        ║\033[0m\n");
        printf(  "\033[1;35m╚══════════════════════════════════════════╝\033[0m\n");
        printf("  \033[1;35m%s\033[0m\n\n", m->text);
        /* signaler la fin aux deux threads */
        pthread_mutex_lock(&running_mutex);
        running = 0;
        pthread_mutex_unlock(&running_mutex);
        break;

    case MSG_CHAT:
        printf("  \033[90m%s\033[0m\n", m->text);
        break;

    default:
        break;
    }
    fflush(stdout);
}

/* ══════════════════════════════════════════════════
   Thread RÉCEPTION — écoute le serveur en continu
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
   Thread ENVOI — lit les mises au clavier
   ══════════════════════════════════════════════════ */
static void *send_thread(void *arg) {
    char *my_name = (char *)arg;
    char  buf[64];
    Message m;

    printf("\n  \033[1mTapez votre mise (nombre entier en DT) puis Entrée.\033[0m\n");
    printf("  Tapez \033[1mq\033[0m pour quitter.\n\n");

    while (1) {
        pthread_mutex_lock(&running_mutex);
        int go = running;
        pthread_mutex_unlock(&running_mutex);
        if (!go) break;

        pthread_mutex_lock(&price_mutex);
        uint32_t cur = current_price;
        pthread_mutex_unlock(&price_mutex);

        printf("  \033[1mVotre mise\033[0m (actuelle : %u DT) > ", cur);
        fflush(stdout);

        if (!fgets(buf, sizeof(buf), stdin)) break;
        buf[strcspn(buf, "\n")] = '\0';

        if (buf[0] == 'q' || buf[0] == 'Q') {
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
            printf("  \033[31mEntrée invalide. Entrez un nombre entier positif.\033[0m\n");
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
   Gestion SIGINT
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
        fprintf(stderr, "  ex : %s 0.tcp.ngrok.io 12345\n", argv[0]);
        return 1;
    }
    strncpy(server_ip, argv[1], sizeof(server_ip) - 1);
    if (argc >= 3) port = atoi(argv[2]);

    if (platform_network_init() != 0) {
        fprintf(stderr, "network init failed\n");
        return 1;
    }
    atexit(platform_network_cleanup);

#ifdef _WIN32
    platform_mutex_init(&running_mutex);
    platform_mutex_init(&price_mutex);
#endif

    signal(SIGINT,  handle_sigint);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    /* ── Saisie du pseudo ── */
    printf("\n");
    printf("  ╔══════════════════════════════════════╗\n");
    printf("  ║   SYSTEME D'ENCHERES EN TEMPS REEL   ║\n");
    printf("  ╚══════════════════════════════════════╝\n\n");
    printf("  Serveur : %s:%d\n\n", server_ip, port);
    printf("  Votre pseudo : ");
    fflush(stdout);
    if (!fgets(name, sizeof(name), stdin)) return 1;
    name[strcspn(name, "\n")] = '\0';
    if (strlen(name) == 0) strncpy(name, "Anonyme", MAX_NAME_LEN - 1);

    /* ── Connexion TCP ── */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port   = htons(port);

    if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) <= 0) {
        /* essai résolution hostname (ngrok) */
        fprintf(stderr, "  Adresse invalide : %s\n", server_ip);
        close(sockfd);
        return 1;
    }

    printf("\n  Connexion au serveur en cours...\n");
    if (connect(sockfd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        perror("  connect");
        close(sockfd);
        return 1;
    }
    printf("  \033[1;32mConnecte !\033[0m Bienvenue, \033[1m%s\033[0m.\n", name);

    /* ── Envoi du pseudo (MSG_JOIN) ── */
    Message join = {0};
    join.type = MSG_JOIN;
    strncpy(join.name, name, MAX_NAME_LEN - 1);
    send(sockfd, &join, sizeof(join), 0);

    /* ── Démarrage des 2 threads ── */
    pthread_create(&rtid, NULL, recv_thread, NULL);
    pthread_create(&stid, NULL, send_thread, name);

    /* attendre la fin des deux threads */
    pthread_join(rtid, NULL);
    pthread_join(stid, NULL);

    close(sockfd);
    printf("\n  \033[90mDéconnecté. A bientot !\033[0m\n\n");
    return 0;
}
