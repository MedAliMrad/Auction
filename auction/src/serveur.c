/*
 * serveur.c — Serveur d'enchères en temps réel (version complète)
 *
 * Fonctionnalités :
 *   - Plusieurs articles enchaînés automatiquement
 *   - Nombre de connectés affiché en temps réel
 *   - Mise minimum automatique (+10 DT)
 *   - Historique des mises horodaté
 *   - Système de solde (1000 DT par client)
 *   - Extension du timer si mise dans les 10 dernières secondes
 *   - Salles d'enchères multiples
 *   - Sauvegarde des résultats dans resultats.txt
 */
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

/* ══════════════════════════════════════════════════
   Structures
   ══════════════════════════════════════════════════ */
typedef struct {
    int      fd;
    int      active;
    char     name[MAX_NAME_LEN];
    char     ip[INET_ADDRSTRLEN];
    uint32_t balance;
    uint8_t  room;
} Client;

typedef struct {
    char     bidder[MAX_NAME_LEN];
    uint32_t amount;
    char     timestamp[10];
} BidRecord;

typedef struct {
    char       item[MAX_ITEM_LEN];
    uint32_t   start_price;
    uint32_t   current_price;
    char       best_bidder[MAX_NAME_LEN];
    int        running;
    int        time_left;
    BidRecord  history[MAX_HISTORY];
    int        history_count;
} Auction;

typedef struct {
    char     name[MAX_ITEM_LEN];
    uint32_t start_price;
} ItemDef;

/* ─── Données partagées ──────────────────────────── */
static Client  clients[MAX_CLIENTS];
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static Auction rooms[NUM_ROOMS];
static pthread_mutex_t room_mutex[NUM_ROOMS];

static int listen_fd   = -1;
static int server_port = DEFAULT_PORT;

static ItemDef item_list[MAX_ITEMS];
static int     item_count = 0;
static int     current_item_index[NUM_ROOMS];

/* ══════════════════════════════════════════════════
   Helpers
   ══════════════════════════════════════════════════ */
static int count_connected(int room) {
    int c = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active && clients[i].room == room)
            c++;
    return c;
}

static int count_all_connected(void) {
    int c = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active) c++;
    return c;
}

static void broadcast_room(int room, const Message *msg) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].room == room) {
            if (send(clients[i].fd, msg, sizeof(*msg), MSG_NOSIGNAL) < 0) {
                clients[i].active = 0;
                close(clients[i].fd);
            }
        }
    }
}

__attribute__((unused))
static void broadcast_all(const Message *msg) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            if (send(clients[i].fd, msg, sizeof(*msg), MSG_NOSIGNAL) < 0) {
                clients[i].active = 0;
                close(clients[i].fd);
            }
        }
    }
}

static void send_to(int fd, const Message *msg) {
    send(fd, msg, sizeof(*msg), MSG_NOSIGNAL);
}

static void broadcast_connected_count(int room) {
    Message m = {0};
    m.type   = MSG_CONNECTED;
    m.room   = (uint8_t)room;
    m.amount = (uint32_t)count_connected(room);
    snprintf(m.text, BUFFER_SIZE, "%d encherisseur(s) connecte(s) dans la salle %d",
             m.amount, room + 1);
    broadcast_room(room, &m);
}

static void save_result(int room) {
    Auction *a = &rooms[room];
    FILE *f = fopen(RESULTS_FILE, "a");
    if (!f) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

    fprintf(f, "══════════════════════════════════════════\n");
    fprintf(f, "Date       : %s\n", ts);
    fprintf(f, "Salle      : %d\n", room + 1);
    fprintf(f, "Article    : %s\n", a->item);
    fprintf(f, "Prix depart: %u DT\n", a->start_price);

    if (strlen(a->best_bidder) > 0 && a->current_price > 0) {
        fprintf(f, "Gagnant    : %s\n", a->best_bidder);
        fprintf(f, "Prix final : %u DT\n", a->current_price);
    } else {
        fprintf(f, "Resultat   : Invendu\n");
    }

    fprintf(f, "\nHistorique des mises :\n");
    for (int i = 0; i < a->history_count; i++) {
        fprintf(f, "  %s  %-12s  %u DT\n",
                a->history[i].timestamp,
                a->history[i].bidder,
                a->history[i].amount);
    }
    fprintf(f, "══════════════════════════════════════════\n\n");
    fclose(f);
}

static void send_history(int fd, int room) {
    Auction *a = &rooms[room];
    Message m = {0};
    m.type = MSG_HISTORY;
    m.room = (uint8_t)room;

    if (a->history_count == 0) {
        snprintf(m.text, BUFFER_SIZE, "Aucune mise pour le moment.");
        send_to(fd, &m);
        return;
    }

    int start = a->history_count > 10 ? a->history_count - 10 : 0;
    m.text[0] = '\0';
    for (int i = start; i < a->history_count; i++) {
        char line[80];
        snprintf(line, sizeof(line), "%s  %-10s -> %u DT\n",
                 a->history[i].timestamp,
                 a->history[i].bidder,
                 a->history[i].amount);
        strncat(m.text, line, BUFFER_SIZE - strlen(m.text) - 1);
    }
    send_to(fd, &m);
}

/* ══════════════════════════════════════════════════
   Announce auction state to a room
   ══════════════════════════════════════════════════ */
static void announce_auction(int room) {
    Auction *a = &rooms[room];
    Message m = {0};
    m.type   = MSG_AUCTION_INFO;
    m.room   = (uint8_t)room;
    strncpy(m.item, a->item, MAX_ITEM_LEN - 1);
    m.amount = a->start_price;
    snprintf(m.text, BUFFER_SIZE,
             "=== SALLE %d === Article : %s | Prix depart : %u DT | Mise min +%d DT | Duree : %d sec",
             room + 1, a->item, a->start_price, MIN_BID_INCREMENT, AUCTION_DURATION);

    pthread_mutex_lock(&clients_mutex);
    broadcast_room(room, &m);
    pthread_mutex_unlock(&clients_mutex);
}

/* ══════════════════════════════════════════════════
   Start next auction in a room
   ══════════════════════════════════════════════════ */
static int start_next_auction(int room) {
    int idx = current_item_index[room];
    if (idx >= item_count) return 0;

    Auction *a = &rooms[room];
    strncpy(a->item, item_list[idx].name, MAX_ITEM_LEN - 1);
    a->start_price   = item_list[idx].start_price;
    a->current_price = 0;
    memset(a->best_bidder, 0, MAX_NAME_LEN);
    a->running       = 1;
    a->time_left     = AUCTION_DURATION;
    a->history_count = 0;
    memset(a->history, 0, sizeof(a->history));

    current_item_index[room]++;
    return 1;
}

/* ══════════════════════════════════════════════════
   Timer thread (one per room)
   ══════════════════════════════════════════════════ */
typedef struct { int room; } TimerArg;

static void *timer_thread(void *arg) {
    TimerArg *ta = (TimerArg *)arg;
    int room = ta->room;
    free(ta);

    while (1) {
        Auction *a = &rooms[room];

        pthread_mutex_lock(&room_mutex[room]);
        if (!a->running) {
            pthread_mutex_unlock(&room_mutex[room]);
            break;
        }
        pthread_mutex_unlock(&room_mutex[room]);

        log_msg("TIMER", "Salle %d: Compte a rebours (%d sec) — Article: %s",
                room + 1, AUCTION_DURATION, a->item);

        announce_auction(room);

        while (1) {
            pthread_mutex_lock(&room_mutex[room]);
            int t = a->time_left;
            pthread_mutex_unlock(&room_mutex[room]);

            if (t <= 0) break;

            if (t % 10 == 0 || t <= 5) {
                Message tm = {0};
                tm.type   = MSG_TIMER;
                tm.room   = (uint8_t)room;
                tm.amount = (uint32_t)t;
                snprintf(tm.text, BUFFER_SIZE, "Temps restant : %d secondes", t);
                pthread_mutex_lock(&clients_mutex);
                broadcast_room(room, &tm);
                pthread_mutex_unlock(&clients_mutex);

                if (t > 0)
                    log_msg("TIMER", "Salle %d: %d sec restantes", room + 1, t);
            }

            sleep(1);

            pthread_mutex_lock(&room_mutex[room]);
            a->time_left--;
            pthread_mutex_unlock(&room_mutex[room]);
        }

        /* Auction ended */
        pthread_mutex_lock(&room_mutex[room]);
        a->running = 0;
        char winner_name[MAX_NAME_LEN] = {0};
        uint32_t winning_price = a->current_price;
        strncpy(winner_name, a->best_bidder, MAX_NAME_LEN - 1);
        pthread_mutex_unlock(&room_mutex[room]);

        /* Deduct balance from winner */
        if (strlen(winner_name) > 0 && winning_price > 0) {
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].active && strcmp(clients[i].name, winner_name) == 0) {
                    if (clients[i].balance >= winning_price)
                        clients[i].balance -= winning_price;
                    else
                        clients[i].balance = 0;

                    Message bal = {0};
                    bal.type    = MSG_BALANCE;
                    bal.balance = clients[i].balance;
                    snprintf(bal.text, BUFFER_SIZE,
                             "Vous avez remporte l'enchere ! Solde restant : %u DT",
                             clients[i].balance);
                    send_to(clients[i].fd, &bal);
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);
        }

        /* Broadcast winner */
        Message win = {0};
        win.type   = MSG_WINNER;
        win.room   = (uint8_t)room;
        win.amount = winning_price;

        if (strlen(winner_name) == 0 || winning_price == 0) {
            strncpy(win.name, "Personne", MAX_NAME_LEN - 1);
            snprintf(win.text, BUFFER_SIZE,
                     "ENCHERE TERMINEE - Aucune mise. Article \"%s\" invendu.", a->item);
            log_msg("FIN", "Salle %d: Article invendu.", room + 1);
        } else {
            strncpy(win.name, winner_name, MAX_NAME_LEN - 1);
            snprintf(win.text, BUFFER_SIZE,
                     "GAGNANT : %s — Prix final : %u DT — FELICITATIONS !",
                     winner_name, winning_price);
            log_msg("FIN", "Salle %d: GAGNANT %s avec %u DT", room + 1, winner_name, winning_price);
        }

        pthread_mutex_lock(&clients_mutex);
        broadcast_room(room, &win);
        pthread_mutex_unlock(&clients_mutex);

        /* Save to file */
        save_result(room);

        /* Start next auction or end */
        sleep(5);

        pthread_mutex_lock(&room_mutex[room]);
        int has_next = start_next_auction(room);
        pthread_mutex_unlock(&room_mutex[room]);

        if (!has_next) {
            Message end = {0};
            end.type = MSG_CHAT;
            end.room = (uint8_t)room;
            snprintf(end.text, BUFFER_SIZE,
                     "=== Toutes les encheres de la salle %d sont terminees ! ===", room + 1);
            pthread_mutex_lock(&clients_mutex);
            broadcast_room(room, &end);
            pthread_mutex_unlock(&clients_mutex);
            break;
        } else {
            Message next = {0};
            next.type = MSG_NEXT_AUCTION;
            next.room = (uint8_t)room;
            snprintf(next.text, BUFFER_SIZE,
                     ">>> Prochaine enchere dans 5 secondes... Article : %s",
                     rooms[room].item);
            pthread_mutex_lock(&clients_mutex);
            broadcast_room(room, &next);
            pthread_mutex_unlock(&clients_mutex);

            log_msg("NEXT", "Salle %d: Prochain article — %s", room + 1, rooms[room].item);
            sleep(3);
        }
    }

    return NULL;
}

/* ══════════════════════════════════════════════════
   Handle a client connection
   ══════════════════════════════════════════════════ */
typedef struct { int slot; } ClientArg;

static void *handle_client(void *arg) {
    ClientArg *ca   = (ClientArg *)arg;
    int        slot = ca->slot;
    free(ca);

    int fd = clients[slot].fd;
    Message msg;

    /* Wait for MSG_JOIN */
    ssize_t n = recv(fd, &msg, sizeof(msg), 0);
    if (n <= 0 || msg.type != MSG_JOIN) {
        goto disconnect;
    }

    pthread_mutex_lock(&clients_mutex);
    strncpy(clients[slot].name, msg.name, MAX_NAME_LEN - 1);
    clients[slot].balance = STARTING_BALANCE;
    clients[slot].room    = msg.room < NUM_ROOMS ? msg.room : 0;
    pthread_mutex_unlock(&clients_mutex);

    int room = clients[slot].room;

    log_msg("JOIN", "%s (%s) → salle %d [solde: %u DT]",
            msg.name, clients[slot].ip, room + 1, STARTING_BALANCE);

    /* Announce join to room */
    {
        Message info = {0};
        info.type = MSG_CHAT;
        info.room = (uint8_t)room;
        snprintf(info.text, BUFFER_SIZE, ">>> %s a rejoint la salle %d !", msg.name, room + 1);
        pthread_mutex_lock(&clients_mutex);
        broadcast_room(room, &info);
        broadcast_connected_count(room);
        pthread_mutex_unlock(&clients_mutex);
    }

    /* Send balance */
    {
        Message bal = {0};
        bal.type    = MSG_BALANCE;
        bal.balance = STARTING_BALANCE;
        snprintf(bal.text, BUFFER_SIZE, "Votre solde : %u DT", STARTING_BALANCE);
        send_to(fd, &bal);
    }

    /* Send current auction state */
    {
        Message info = {0};
        info.type = MSG_AUCTION_INFO;
        info.room = (uint8_t)room;
        pthread_mutex_lock(&room_mutex[room]);
        Auction *a = &rooms[room];
        strncpy(info.item, a->item, MAX_ITEM_LEN - 1);
        info.amount = a->current_price > 0 ? a->current_price : a->start_price;
        snprintf(info.text, BUFFER_SIZE,
                 "Article : %s | Mise actuelle : %u DT par [%s] | Temps : %d sec | Min +%d DT",
                 a->item, info.amount,
                 strlen(a->best_bidder) ? a->best_bidder : "aucun",
                 a->time_left, MIN_BID_INCREMENT);
        pthread_mutex_unlock(&room_mutex[room]);
        send_to(fd, &info);
    }

    /* Send bid history */
    pthread_mutex_lock(&room_mutex[room]);
    send_history(fd, room);
    pthread_mutex_unlock(&room_mutex[room]);

    /* ── Bid reception loop ── */
    while (1) {
        n = recv(fd, &msg, sizeof(msg), 0);
        if (n <= 0) break;

        if (msg.type == MSG_BYE) {
            log_msg("BYE", "%s deconnecte", clients[slot].name);
            break;
        }

        if (msg.type == MSG_BID) {
            uint32_t offered = msg.amount;
            Message  reply   = {0};

            pthread_mutex_lock(&room_mutex[room]);
            Auction *a = &rooms[room];

            uint32_t min_required = a->current_price > 0
                ? a->current_price + MIN_BID_INCREMENT
                : a->start_price + MIN_BID_INCREMENT;

            int valid = a->running && offered >= min_required;

            /* Check balance */
            pthread_mutex_lock(&clients_mutex);
            uint32_t bal = clients[slot].balance;
            pthread_mutex_unlock(&clients_mutex);

            if (valid && offered > bal) {
                valid = 0;
                reply.type = MSG_BID_REJECT;
                reply.amount = a->current_price;
                reply.balance = bal;
                snprintf(reply.text, BUFFER_SIZE,
                         "Solde insuffisant (%u DT). Vous ne pouvez pas miser %u DT.",
                         bal, offered);
                pthread_mutex_unlock(&room_mutex[room]);
                send_to(fd, &reply);
                continue;
            }

            if (valid) {
                a->current_price = offered;
                strncpy(a->best_bidder, clients[slot].name, MAX_NAME_LEN - 1);

                /* Record in history */
                if (a->history_count < MAX_HISTORY) {
                    BidRecord *rec = &a->history[a->history_count++];
                    strncpy(rec->bidder, clients[slot].name, MAX_NAME_LEN - 1);
                    rec->amount = offered;
                    time_t now = time(NULL);
                    struct tm *tm_now = localtime(&now);
                    strftime(rec->timestamp, sizeof(rec->timestamp), "%H:%M:%S", tm_now);
                }

                /* Timer extension */
                if (a->time_left <= TIMER_THRESHOLD) {
                    a->time_left = TIMER_EXTENSION;
                    log_msg("TIMER", "Salle %d: Extension! Timer remis a %d sec",
                            room + 1, TIMER_EXTENSION);

                    Message ext = {0};
                    ext.type   = MSG_CHAT;
                    ext.room   = (uint8_t)room;
                    snprintf(ext.text, BUFFER_SIZE,
                             "⏱ Mise tardive ! Timer etendu a %d secondes.", TIMER_EXTENSION);
                    pthread_mutex_lock(&clients_mutex);
                    broadcast_room(room, &ext);
                    pthread_mutex_unlock(&clients_mutex);
                }

                pthread_mutex_unlock(&room_mutex[room]);

                /* BID_OK to sender */
                reply.type    = MSG_BID_OK;
                reply.amount  = offered;
                reply.balance = bal;
                snprintf(reply.text, BUFFER_SIZE,
                         "Mise de %u DT acceptee ! Vous menez.", offered);
                send_to(fd, &reply);

                /* UPDATE broadcast */
                Message upd = {0};
                upd.type   = MSG_UPDATE;
                upd.room   = (uint8_t)room;
                upd.amount = offered;
                strncpy(upd.name, clients[slot].name, MAX_NAME_LEN - 1);
                snprintf(upd.text, BUFFER_SIZE,
                         "NOUVELLE MISE : %s → %u DT", clients[slot].name, offered);

                log_msg("BID", "Salle %d: %s mise %u DT", room + 1, clients[slot].name, offered);

                pthread_mutex_lock(&clients_mutex);
                broadcast_room(room, &upd);
                pthread_mutex_unlock(&clients_mutex);

            } else {
                reply.type    = MSG_BID_REJECT;
                reply.amount  = a->current_price;
                reply.balance = bal;
                if (!a->running) {
                    snprintf(reply.text, BUFFER_SIZE,
                             "Enchere terminee. Aucune mise acceptee.");
                } else {
                    snprintf(reply.text, BUFFER_SIZE,
                             "Mise refusee. Minimum requis : %u DT (actuelle %u + %d).",
                             min_required,
                             a->current_price > 0 ? a->current_price : a->start_price,
                             MIN_BID_INCREMENT);
                }
                pthread_mutex_unlock(&room_mutex[room]);
                send_to(fd, &reply);
            }
        }
    }

disconnect:
    {
        char name_copy[MAX_NAME_LEN] = {0};
        int r;
        pthread_mutex_lock(&clients_mutex);
        strncpy(name_copy, clients[slot].name, MAX_NAME_LEN - 1);
        r = clients[slot].room;
        clients[slot].active = 0;
        close(clients[slot].fd);
        pthread_mutex_unlock(&clients_mutex);

        if (strlen(name_copy)) {
            Message bye_msg = {0};
            bye_msg.type = MSG_CHAT;
            bye_msg.room = (uint8_t)r;
            snprintf(bye_msg.text, BUFFER_SIZE, "<<< %s a quitte la salle.", name_copy);
            pthread_mutex_lock(&clients_mutex);
            broadcast_room(r, &bye_msg);
            broadcast_connected_count(r);
            pthread_mutex_unlock(&clients_mutex);
        }
    }
    return NULL;
}

/* ══════════════════════════════════════════════════
   SIGINT handler
   ══════════════════════════════════════════════════ */
static void handle_sigint(int sig) {
    (void)sig;
    printf("\n");
    log_msg("SERVER", "Arret du serveur...");
    if (listen_fd >= 0) close(listen_fd);
    exit(0);
}

/* ══════════════════════════════════════════════════
   Setup: enter items from terminal
   ══════════════════════════════════════════════════ */
static void setup_items(void) {
    char buf[MAX_ITEM_LEN];
    uint32_t price;

    printf("\n");
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   SERVEUR D'ENCHERES EN TEMPS REEL v2.0      ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  Nouveautes:                                 ║\n");
    printf("║  • Encheres multiples enchainées             ║\n");
    printf("║  • Solde par joueur (%4d DT)                ║\n", STARTING_BALANCE);
    printf("║  • Mise minimum +%d DT                       ║\n", MIN_BID_INCREMENT);
    printf("║  • Extension timer si mise tardive           ║\n");
    printf("║  • %d salles paralleles                      ║\n", NUM_ROOMS);
    printf("║  • Historique + resultats.txt                ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    printf("  Combien d'articles a vendre ? (1-%d) : ", MAX_ITEMS);
    fflush(stdout);
    int count = 0;
    if (scanf("%d", &count) != 1 || count < 1) count = 1;
    if (count > MAX_ITEMS) count = MAX_ITEMS;
    while (getchar() != '\n');

    for (int i = 0; i < count; i++) {
        printf("\n  --- Article %d/%d ---\n", i + 1, count);
        printf("  Nom : ");
        fflush(stdout);
        if (fgets(buf, sizeof(buf), stdin)) {
            buf[strcspn(buf, "\n")] = '\0';
            strncpy(item_list[i].name, buf, MAX_ITEM_LEN - 1);
        }
        printf("  Prix de depart (DT) : ");
        fflush(stdout);
        if (scanf("%u", &price) == 1)
            item_list[i].start_price = price;
        else
            item_list[i].start_price = 10;
        while (getchar() != '\n');
    }
    item_count = count;

    printf("\n  %d article(s) configure(s).\n", item_count);
    printf("  Les encheres commencent dans toutes les salles.\n\n");
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

    signal(SIGINT, handle_sigint);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    memset(clients, 0, sizeof(clients));
    memset(rooms, 0, sizeof(rooms));

    for (int i = 0; i < NUM_ROOMS; i++)
        pthread_mutex_init(&room_mutex[i], NULL);

    /* Setup items */
    setup_items();

    /* Initialize all rooms with the first item */
    for (int i = 0; i < NUM_ROOMS; i++) {
        current_item_index[i] = 0;
        start_next_auction(i);
    }

    /* TCP socket */
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

    log_msg("SERVER", "En ecoute sur le port %d — %d salle(s) — %d article(s)",
            server_port, NUM_ROOMS, item_count);
    log_msg("SERVER", "Clients: ./bin/client <IP> %d", server_port);

    /* Start timer threads for all rooms */
    for (int i = 0; i < NUM_ROOMS; i++) {
        pthread_t tid;
        TimerArg *ta = malloc(sizeof(TimerArg));
        ta->room = i;
        pthread_create(&tid, NULL, timer_thread, ta);
        pthread_detach(tid);
    }

    /* Accept loop */
    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) break;

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
        clients[slot].balance = STARTING_BALANCE;
        clients[slot].room   = 0;
        memset(clients[slot].name, 0, MAX_NAME_LEN);
        inet_ntop(AF_INET, &caddr.sin_addr, clients[slot].ip, INET_ADDRSTRLEN);
        pthread_mutex_unlock(&clients_mutex);

        log_msg("SERVER", "Connexion depuis %s (slot %d) [total: %d]",
                clients[slot].ip, slot, count_all_connected());

        ClientArg *ca = malloc(sizeof(ClientArg));
        ca->slot = slot;
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, ca);
        pthread_detach(tid);
    }

    close(listen_fd);
    return 0;
}
