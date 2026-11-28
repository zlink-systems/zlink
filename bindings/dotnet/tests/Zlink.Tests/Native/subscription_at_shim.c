/* SPDX-License-Identifier: MPL-2.0 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlink_errno.h"

zlink_config_result_t zlink_subscription_at(
    void *socket, size_t index, char *filter, size_t *size, int *pattern)
{
    static unsigned calls;
    zlink_config_result_t (*query)(void *, size_t, char *, size_t *, int *) =
        dlsym(RTLD_NEXT, "zlink_subscription_at");
    const char *scenario = getenv("ZLINK_DOTNET_SUBSCRIPTION_CASE");
    size_t capacity = filter ? *size : 0;
    zlink_config_result_t result;
    if (strcmp(scenario, "invalid-argument") == 0) {
        *size = 4096;
        errno = EINVAL;
        result = ZLINK_CONFIG_INVALID_ARGUMENT;
    } else {
        if (calls == 1 && strcmp(scenario, "removed-before-retry") == 0) {
            zlink_config_result_t (*unset)(void *, const char *) =
                dlsym(RTLD_NEXT, "zlink_unset_subscription");
            char long_filter[4097];
            memset(long_filter, 'x', sizeof(long_filter) - 1);
            long_filter[sizeof(long_filter) - 1] = '\0';
            if (unset(socket, long_filter) != ZLINK_CONFIG_OK)
                abort();
        }
        result = query(socket, index, filter, size, pattern);
    }
    ++calls;

    int saved_errno = errno;
    FILE *trace = fopen(getenv("ZLINK_DOTNET_SUBSCRIPTION_TRACE"), "a");
    if (!trace)
        abort();
    fprintf(trace, "%zu %d\n", capacity, (int)result);
    fclose(trace);
    errno = saved_errno;
    return result;
}
