/*
 * serveur.c — Serveur d'enchères en temps réel
 *
 * Architecture :
 *   main()           → socket TCP, accept() en boucle
 *   handle_client()  → 1 thread par client (reçoit les mises)
 *   timer_thread()   → compte à rebours, ferme l'enchère
 *   broadcast()      → envoie un message à TOUS les clients connectés
 *
 * Synchronisation :
 *   clients_mutex  → protège la liste des clients
 *   auction_mutex  → protège le prix courant + gagnant actuel
 *
 * Compilation : gcc -Wall -o bin/serveur src/serveur.c -lpthread
 * Lancement   : ./bin/serveur [port]   (défaut : 8080)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "../include/common.h"

/* ══════════════════════════════════════════════════
   Structure client (un slot dans le tableau global)
   ══════════════════════════════════════════════════ */
typedef struct {
    int    fd;                    /* socket de ce client          */
    int    active;                /* 1 = connecté                 */
    char   name[MAX_NAME_LEN];    /* pseudo choisi à la connexion */
    char   ip[INET_ADDRSTRLEN];   /* IP distante (pour les logs)  */
} Client;

/* ══════════════════════════════════════════════════
   État global de l'enchère
   ══════════════════════════════════════════════════ */
typedef struct {
    char     item[MAX_ITEM_LEN];  /* article en vente             */
    uint32_t start_price;         /* prix de départ               */
    uint32_t current_price;       /* meilleure mise courante      */
    char     best_bidder[MAX_NAME_LEN]; /* nom du meneur          */
    int      running;             /* enchère en cours ?           */
    int      time_left;           /* secondes restantes           */
} Auction;

/* ─── Données partagées ──────────────────────────── */
static Client  clients[MAX_CLIENTS];
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static Auction auction;
static pthread_mutex_t auction_mutex = PTHREAD_MUTEX_INITIALIZER;

static int listen_fd   = -1;
static int server_port = DEFAULT_PORT;

/* ══════════════════════════════════════════════════
   broadcast() — envoie msg à tous les clients actifs
   DOIT être appelé avec clients_mutex déjà acquis
   ══════════════════════════════════════════════════ */
static void broadcast(const Message *msg) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            if (send(clients[i].fd, msg, sizeof(*msg), MSG_NOSIGNAL) < 0) {
                /* client perdu, on le marque inactif */
                clients[i].active = 0;
                close(clients[i].fd);
            }
        }
    }
}

/* ══════════════════════════════════════════════════
   send_to() — envoie uniquement à un client
   ══════════════════════════════════════════════════ */
static void send_to(int fd, const Message *msg) {
    send(fd, msg, sizeof(*msg), MSG_NOSIGNAL);
}

/* ══════════════════════════════════════════════════
   announce_auction() — informe tous les connectés
   ══════════════════════════════════════════════════ */
static void announce_auction(void) {
    Message m = {0};
    m.type = MSG_AUCTION_INFO;
    strncpy(m.item, auction.item, MAX_ITEM_LEN - 1);
    m.amount = auction.start_price;
    snprintf(m.text, BUFFER_SIZE,
             "=== NOUVELLE ENCHERE === Article : %s | Prix depart : %u DT | Duree : %d sec",
             auction.item, auction.start_price, AUCTION_DURATION);

    pthread_mutex_lock(&clients_mutex);
    broadcast(&m);
    pthread_mutex_unlock(&clients_mutex);
}

/* ══════════════════════════════════════════════════
   Thread TIMER — compte à rebours de l'enchère
   ══════════════════════════════════════════════════ */
static void *timer_thread(void *arg) {
    (void)arg;
    log_msg("TIMER", "Compte a rebours demarre (%d secondes)", AUCTION_DURATION);

    for (int t = AUCTION_DURATION; t >= 0; t--) {
        /* mise à jour du temps restant */
        pthread_mutex_lock(&auction_mutex);
        auction.time_left = t;
        pthread_mutex_unlock(&auction_mutex);

        /* diffusion du timer toutes les 10 secondes + dernières 5 sec */
        if (t % 10 == 0 || t <= 5) {
            Message tm = {0};
            tm.type   = MSG_TIMER;
            tm.amount = (uint32_t)t;
            snprintf(tm.text, BUFFER_SIZE, "Temps restant : %d secondes", t);
            pthread_mutex_lock(&clients_mutex);
            broadcast(&tm);
            pthread_mutex_unlock(&clients_mutex);

            if (t > 0)
                log_msg("TIMER", "%d secondes restantes", t);
        }

        if (t == 0) break;
        sleep(1);
    }

    /* ── Fermeture de l'enchère ── */
    pthread_mutex_lock(&auction_mutex);
    auction.running = 0;
    char winner_name[MAX_NAME_LEN];
    uint32_t winning_price;
    strncpy(winner_name, auction.best_bidder, MAX_NAME_LEN - 1);
    winning_price = auction.current_price;
    pthread_mutex_unlock(&auction_mutex);

    Message win = {0};
    win.type   = MSG_WINNER;
    win.amount = winning_price;

    if (strlen(winner_name) == 0 || winning_price == 0) {
        strncpy(win.name, "Personne", MAX_NAME_LEN - 1);
        snprintf(win.text, BUFFER_SIZE,
                 "ENCHERE TERMINEE - Aucune mise valide. Article invendu.");
        log_msg("TIMER", "Aucune mise. Article invendu.");
    } else {
        strncpy(win.name, winner_name, MAX_NAME_LEN - 1);
        snprintf(win.text, BUFFER_SIZE,
                 "GAGNANT : %s - Prix final : %u DT  FELICITATIONS !",
                 winner_name, winning_price);
        log_msg("TIMER", "GAGNANT : %s avec %u DT", winner_name, winning_price);
    }

    pthread_mutex_lock(&clients_mutex);
    broadcast(&win);
    pthread_mutex_unlock(&clients_mutex);

    return NULL;
}

/* ══════════════════════════════════════════════════
   Thread par CLIENT — reçoit et traite les messages
   ══════════════════════════════════════════════════ */
typedef struct { int slot; } ClientArg;

static void *handle_client(void *arg) {
    ClientArg *ca   = (ClientArg *)arg;
    int        slot = ca->slot;
    free(ca);

    int fd = clients[slot].fd;
    Message msg;

    /* ── Attente du message MSG_JOIN (pseudo) ── */
    ssize_t n = recv(fd, &msg, sizeof(msg), 0);
    if (n <= 0 || msg.type != MSG_JOIN) {
        goto disconnect;
    }

    /* enregistrement du pseudo */
    pthread_mutex_lock(&clients_mutex);
    strncpy(clients[slot].name, msg.name, MAX_NAME_LEN - 1);
    pthread_mutex_unlock(&clients_mutex);

    log_msg("JOIN", "%s (%s) a rejoint l'enchere", msg.name, clients[slot].ip);

    /* informer tout le monde de l'arrivée */
    {
        Message info = {0};
        info.type = MSG_CHAT;
        snprintf(info.text, BUFFER_SIZE, ">>> %s a rejoint l'enchere !", msg.name);
        pthread_mutex_lock(&clients_mutex);
        broadcast(&info);
        pthread_mutex_unlock(&clients_mutex);
    }

    /* envoyer l'état actuel de l'enchère au nouveau connecté */
    {
        Message info = {0};
        info.type = MSG_AUCTION_INFO;
        pthread_mutex_lock(&auction_mutex);
        strncpy(info.item, auction.item, MAX_ITEM_LEN - 1);
        info.amount = auction.current_price > 0
                      ? auction.current_price : auction.start_price;
        snprintf(info.text, BUFFER_SIZE,
                 "Article : %s | Meilleure mise : %u DT par [%s] | Temps restant : %d sec",
                 auction.item,
                 info.amount,
                 strlen(auction.best_bidder) ? auction.best_bidder : "aucun",
                 auction.time_left);
        pthread_mutex_unlock(&auction_mutex);
        send_to(fd, &info);
    }

    /* ── Boucle de réception des mises ── */
    while (1) {
        n = recv(fd, &msg, sizeof(msg), 0);
        if (n <= 0) break;

        if (msg.type == MSG_BYE) {
            log_msg("BYE", "%s s'est deconnecte proprement", clients[slot].name);
            break;
        }

        if (msg.type == MSG_BID) {
            uint32_t offered = msg.amount;
            Message  reply   = {0};

            pthread_mutex_lock(&auction_mutex);

            int valid = auction.running
                     && offered > auction.current_price
                     && offered > auction.start_price;

            if (valid) {
                auction.current_price = offered;
                strncpy(auction.best_bidder, clients[slot].name, MAX_NAME_LEN - 1);

                /* MSG_BID_OK → uniquement à l'expéditeur */
                reply.type   = MSG_BID_OK;
                reply.amount = offered;
                snprintf(reply.text, BUFFER_SIZE,
                         "Votre mise de %u DT est acceptee ! Vous etes le meneur.", offered);
                send_to(fd, &reply);

                /* MSG_UPDATE → diffusion à tous */
                Message upd = {0};
                upd.type   = MSG_UPDATE;
                upd.amount = offered;
                strncpy(upd.name, clients[slot].name, MAX_NAME_LEN - 1);
                snprintf(upd.text, BUFFER_SIZE,
                         "NOUVELLE MISE : %s --> %u DT", clients[slot].name, offered);
                pthread_mutex_unlock(&auction_mutex);

                log_msg("BID", "%s mise %u DT", clients[slot].name, offered);

                pthread_mutex_lock(&clients_mutex);
                broadcast(&upd);
                pthread_mutex_unlock(&clients_mutex);

            } else {
                /* mise refusée */
                reply.type   = MSG_BID_REJECT;
                reply.amount = auction.current_price;
                if (!auction.running) {
                    snprintf(reply.text, BUFFER_SIZE,
                             "Enchere terminee, aucune mise acceptee.");
                } else {
                    snprintf(reply.text, BUFFER_SIZE,
                             "Mise refusee. Vous devez depasser %u DT.",
                             auction.current_price > 0
                             ? auction.current_price : auction.start_price);
                }
                pthread_mutex_unlock(&auction_mutex);
                send_to(fd, &reply);
            }
        }
    }

disconnect:
    /* nettoyage du slot */
    {
        char name_copy[MAX_NAME_LEN];
        pthread_mutex_lock(&clients_mutex);
        strncpy(name_copy, clients[slot].name, MAX_NAME_LEN - 1);
        clients[slot].active = 0;
        close(clients[slot].fd);
        pthread_mutex_unlock(&clients_mutex);

        if (strlen(name_copy)) {
            Message bye_msg = {0};
            bye_msg.type = MSG_CHAT;
            snprintf(bye_msg.text, BUFFER_SIZE, "<<< %s a quitte l'enchere.", name_copy);
            pthread_mutex_lock(&clients_mutex);
            broadcast(&bye_msg);
            pthread_mutex_unlock(&clients_mutex);
        }
    }
    return NULL;
}

/* ══════════════════════════════════════════════════
   Gestion SIGINT propre
   ══════════════════════════════════════════════════ */
static void handle_sigint(int sig) {
    (void)sig;
    printf("\n");
    log_msg("SERVER", "Arret du serveur...");
    if (listen_fd >= 0) close(listen_fd);
    exit(0);
}

/* ══════════════════════════════════════════════════
   Saisie de l'article (depuis le terminal du serveur)
   ══════════════════════════════════════════════════ */
static void setup_auction(void) {
    char buf[MAX_ITEM_LEN];
    uint32_t price;

    printf("\n");
    printf("╔══════════════════════════════════════╗\n");
    printf("║   SERVEUR D'ENCHERES EN TEMPS REEL   ║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    printf("  Article a vendre : ");
    fflush(stdout);
    if (fgets(buf, sizeof(buf), stdin)) {
        buf[strcspn(buf, "\n")] = '\0';
        strncpy(auction.item, buf, MAX_ITEM_LEN - 1);
    }

    printf("  Prix de depart (DT) : ");
    fflush(stdout);
    if (scanf("%u", &price) == 1)
        auction.start_price = price;
    else
        auction.start_price = 1;
    while (getchar() != '\n'); /* vider le buffer */

    auction.current_price = 0;
    memset(auction.best_bidder, 0, MAX_NAME_LEN);
    auction.running   = 1;
    auction.time_left = AUCTION_DURATION;

    printf("\n");
    log_msg("SERVER", "Enchere configuree : \"%s\" | Depart : %u DT | Duree : %ds",
            auction.item, auction.start_price, AUCTION_DURATION);
}

/* ══════════════════════════════════════════════════
   Main
   ══════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    struct sockaddr_in addr;
    int opt = 1;

    if (argc >= 2) server_port = atoi(argv[1]);

    if (platform_network_init() != 0) {
        fprintf(stderr, "network init failed\n");
        return 1;
    }
    atexit(platform_network_cleanup);

#ifdef _WIN32
    platform_mutex_init(&clients_mutex);
    platform_mutex_init(&auction_mutex);
#endif

    signal(SIGINT,  handle_sigint);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);      /* évite le crash sur client déconnecté */
#endif

    memset(clients, 0, sizeof(clients));

    /* ── Saisie de l'article ── */
    setup_auction();

    /* ── Socket TCP ── */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(server_port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(listen_fd, 16);
    log_msg("SERVER", "En ecoute sur le port %d — En attente de clients...", server_port);
    log_msg("SERVER", "Lancez les clients avec : ./bin/client <IP_SERVEUR> %d", server_port);

    /* ── Thread timer ── */
    pthread_t timer_tid;
    pthread_create(&timer_tid, NULL, timer_thread, NULL);
    pthread_detach(timer_tid);

    /* ── Boucle accept ── */
    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) break;

        /* trouver un slot libre */
        pthread_mutex_lock(&clients_mutex);
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active) { slot = i; break; }
        }
        if (slot < 0) {
            pthread_mutex_unlock(&clients_mutex);
            close(cfd);
            log_msg("SERVER", "Connexion refusee : trop de clients");
            continue;
        }
        clients[slot].fd     = cfd;
        clients[slot].active = 1;
        memset(clients[slot].name, 0, MAX_NAME_LEN);
        inet_ntop(AF_INET, &caddr.sin_addr, clients[slot].ip, INET_ADDRSTRLEN);
        pthread_mutex_unlock(&clients_mutex);

        log_msg("SERVER", "Nouvelle connexion depuis %s (slot %d)",
                clients[slot].ip, slot);

        /* annoncer l'enchère au nouveau connecté (sera affiné dans handle_client) */
        announce_auction();

        /* thread dédié */
        ClientArg *ca = malloc(sizeof(ClientArg));
        ca->slot = slot;
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, ca);
        pthread_detach(tid);
    }

    close(listen_fd);
    return 0;
}