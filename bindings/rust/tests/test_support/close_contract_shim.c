#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static void record(char call) {
    const char *path = getenv("ZLINK_RUST_CLOSE_TRACE");
    if (!path) return;
    FILE *file = fopen(path, "a");
    if (!file) abort();
    fputc(call, file);
    fclose(file);
}

int zlink_ctx_shutdown(void *context) {
    int (*next)(void *) = dlsym(RTLD_NEXT, "zlink_ctx_shutdown");
    record('S');
    return next(context);
}

int zlink_ctx_term(void *context) {
    int (*next)(void *) = dlsym(RTLD_NEXT, "zlink_ctx_term");
    record('T');
    return next(context);
}

int zlink_poller_destroy(void **poller) {
    static int first = 1;
    if (getenv("ZLINK_RUST_POLLER_BUSY_ONCE") && first) {
        first = 0;
        errno = EBUSY;
        return 401;
    }
    int (*next)(void **) = dlsym(RTLD_NEXT, "zlink_poller_destroy");
    return next(poller);
}
