#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>

typedef int ssize_t;
typedef HANDLE pthread_t;
typedef CRITICAL_SECTION pthread_mutex_t;

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef PTHREAD_MUTEX_INITIALIZER
#define PTHREAD_MUTEX_INITIALIZER {0}
#endif

#define close closesocket
#define sleep(seconds) Sleep((seconds) * 1000)

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr) {
    (void)attr;
    InitializeCriticalSection(mutex);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex) {
    EnterCriticalSection(mutex);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    LeaveCriticalSection(mutex);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    DeleteCriticalSection(mutex);
    return 0;
}

typedef struct {
    void *(*start_routine)(void *);
    void  *arg;
} pthread_start_info;

static unsigned __stdcall pthread_thread_trampoline(void *data) {
    pthread_start_info *info = (pthread_start_info *)data;
    void *result = info->start_routine(info->arg);
    free(info);
    return (unsigned)(uintptr_t)result;
}

static inline int pthread_create(pthread_t *thread, const void *attr,
                                 void *(*start_routine)(void *), void *arg) {
    (void)attr;
    pthread_start_info *info = (pthread_start_info *)malloc(sizeof(*info));
    if (!info)
        return -1;
    info->start_routine = start_routine;
    info->arg = arg;

    uintptr_t handle = _beginthreadex(NULL, 0, pthread_thread_trampoline, info, 0, NULL);
    if (!handle) {
        free(info);
        return -1;
    }

    *thread = (HANDLE)handle;
    return 0;
}

static inline int pthread_detach(pthread_t thread) {
    return CloseHandle(thread) ? 0 : -1;
}

static inline int platform_network_init(void) {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data);
}

static inline void platform_network_cleanup(void) {
    WSACleanup();
}

static inline void platform_mutex_init(pthread_mutex_t *mutex) {
    InitializeCriticalSection(mutex);
}
#else
#include <unistd.h>

static inline int platform_network_init(void) {
    return 0;
}

static inline void platform_network_cleanup(void) {
}

static inline void platform_mutex_init(pthread_mutex_t *mutex) {
    (void)mutex;
}
#endif

/* ─── Réseau ──────────────────────────────────────── */
#define DEFAULT_PORT     8080
#define MAX_CLIENTS      32
#define MAX_NAME_LEN     32
#define MAX_ITEM_LEN     64
#define BUFFER_SIZE      256

/* ─── Durée de l'enchère (secondes) ──────────────── */
#define AUCTION_DURATION 60

/* ══════════════════════════════════════════════════
   PROTOCOLE DE MESSAGES
   Chaque message échangé sur le socket a un type
   suivi d'une payload fixe (struct Message).
   ══════════════════════════════════════════════════ */
typedef enum {
    MSG_JOIN        = 1,  /* client → serveur : "je m'appelle X"         */
    MSG_BID         = 2,  /* client → serveur : "je mise N DT"           */
    MSG_AUCTION_INFO= 3,  /* serveur → client : article + prix départ    */
    MSG_BID_OK      = 4,  /* serveur → client : mise acceptée            */
    MSG_BID_REJECT  = 5,  /* serveur → client : mise refusée             */
    MSG_UPDATE      = 6,  /* serveur → tous   : nouveau meilleur prix    */
    MSG_TIMER       = 7,  /* serveur → tous   : temps restant            */
    MSG_WINNER      = 8,  /* serveur → tous   : gagnant de l'enchère     */
    MSG_CHAT        = 9,  /* serveur → tous   : message texte libre      */
    MSG_BYE         = 10  /* client → serveur : déconnexion propre       */
} MsgType;

typedef struct {
    uint8_t  type;                  /* MsgType                        */
    char     name[MAX_NAME_LEN];    /* nom du client ou gagnant       */
    char     item[MAX_ITEM_LEN];    /* article mis aux enchères       */
    uint32_t amount;                /* mise / prix départ / temps     */
    char     text[BUFFER_SIZE];     /* message texte libre            */
} Message;

/* ─── Utilitaire log horodaté ────────────────────── */
static inline void log_msg(const char *tag, const char *fmt, ...) {
    va_list ap;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[10];
    strftime(ts, sizeof(ts), "%H:%M:%S", t);
    printf("\033[90m[%s]\033[0m \033[1m%-8s\033[0m ", ts, tag);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

#endif /* COMMON_H */