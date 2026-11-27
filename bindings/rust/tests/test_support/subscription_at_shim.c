// SPDX-License-Identifier: MPL-2.0

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlink.h"

zlink_config_result_t zlink_subscription_at(
    void *socket, size_t index, char *filter, size_t *size, int *pattern)
{
    static unsigned calls;
    zlink_config_result_t (*next)(void *, size_t, char *, size_t *, int *) =
        dlsym(RTLD_NEXT, "zlink_subscription_at");
    const char *test = getenv("ZLINK_RUST_SUBSCRIPTION_TEST");
    size_t capacity = filter ? *size : 0;
    zlink_config_result_t result;
    if (strcmp(test, "invalid_argument") == 0) {
        *size = 4096;
        errno = EINVAL;
        result = ZLINK_CONFIG_INVALID_ARGUMENT;
    } else if (calls == 1 && strcmp(test, "removed_before_retry") == 0) {
        errno = ENOENT;
        result = ZLINK_CONFIG_NOT_FOUND;
    } else {
        result = next(socket, index, filter, size, pattern);
    }
    ++calls;
    int saved_errno = errno;
    FILE *trace = fopen(getenv("ZLINK_RUST_SUBSCRIPTION_TRACE"), "a");
    if (!trace)
        abort();
    fprintf(trace, "%zu %d\n", capacity, (int)result);
    fclose(trace);
    errno = saved_errno;
    return result;
}
