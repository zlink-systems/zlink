/* SPDX-License-Identifier: MPL-2.0 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlink.h"

zlink_config_result_t zlink_subscription_at (
    void *socket, size_t index, char *filter, size_t *size, int *pattern)
{
    static unsigned calls;
    const char *scenario = getenv ("ZLINK_NODE_SUBSCRIPTION_CASE");
    const size_t capacity = filter ? *size : 0;
    zlink_config_result_t result;

    if (strcmp (scenario, "invalid-argument") == 0) {
        *size = 4096;
        errno = EINVAL;
        result = ZLINK_CONFIG_INVALID_ARGUMENT;
    } else if (calls == 1 && strcmp (scenario, "buffer-still-too-small") == 0) {
        *size = 8192;
        errno = ENOBUFS;
        result = ZLINK_CONFIG_BUFFER_TOO_SMALL;
    } else {
        /* Node opens the addon and its Core dependency with local visibility. */
        void *core = dlopen ("libzlink.so.0", RTLD_LAZY | RTLD_NOLOAD);
        if (!core) {
            fprintf (stderr, "%s\n", dlerror ());
            abort ();
        }
        if (calls == 1 && strcmp (scenario, "removed-before-retry") == 0) {
            char long_filter[4097];
            memset (long_filter, 'x', sizeof (long_filter) - 1);
            long_filter[sizeof (long_filter) - 1] = '\0';
            zlink_config_result_t (*unset) (void *, const char *) =
              dlsym (core, "zlink_unset_subscription");
            if (!unset || unset (socket, long_filter) != ZLINK_CONFIG_OK)
                abort ();
        }
        zlink_config_result_t (*query) (void *, size_t, char *, size_t *, int *) =
          dlsym (core, "zlink_subscription_at");
        if (!query)
            abort ();
        result = query (socket, index, filter, size, pattern);
        const int query_errno = errno;
        dlclose (core);
        errno = query_errno;
    }

    ++calls;
    const int saved_errno = errno;
    FILE *trace = fopen (getenv ("ZLINK_NODE_SUBSCRIPTION_TRACE"), "a");
    if (!trace)
        abort ();
    fprintf (trace, "%zu %zu %d\n", index, capacity, (int) result);
    fclose (trace);
    errno = saved_errno;
    return result;
}
