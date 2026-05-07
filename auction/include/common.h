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
#define DEFAULT_PORT       8080
#define MAX_CLIENTS        32
#define MAX_NAME_LEN       32
#define MAX_ITEM_LEN       64
#define BUFFER_SIZE        256

/* ─── Enchère ────────────────────────────────────── */
#define AUCTION_DURATION   60
#define MIN_BID_INCREMENT  10
#define TIMER_EXTENSION    30
#define TIMER_THRESHOLD    10
#define STARTING_BALANCE   1000
#define MAX_ITEMS          10
#define MAX_HISTORY        64
#define NUM_ROOMS          4

/* ─── Fichier résultats ──────────────────────────── */
#define RESULTS_FILE       "resultats.txt"

/* ══════════════════════════════════════════════════
   PROTOCOLE DE MESSAGES
   ══════════════════════════════════════════════════ */
typedef enum {
    MSG_JOIN         = 1,
    MSG_BID          = 2,
    MSG_AUCTION_INFO = 3,
    MSG_BID_OK       = 4,
    MSG_BID_REJECT   = 5,
    MSG_UPDATE       = 6,
    MSG_TIMER        = 7,
    MSG_WINNER       = 8,
    MSG_CHAT         = 9,
    MSG_BYE          = 10,
    MSG_CONNECTED    = 11,
    MSG_HISTORY      = 12,
    MSG_BALANCE      = 13,
    MSG_NEXT_AUCTION = 14,
    MSG_ROOM_LIST    = 15,
    MSG_ROOM_JOIN    = 16
} MsgType;

typedef struct {
    uint8_t  type;
    char     name[MAX_NAME_LEN];
    char     item[MAX_ITEM_LEN];
    uint32_t amount;
    uint32_t balance;
    uint8_t  room;
    char     text[BUFFER_SIZE];
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
