// SPDX-License-Identifier: MPL-2.0

#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include "zlink.h"

zlink_config_result_t zlink_get_option(
    void *socket, zlink_option_t option, void *value, size_t *size)
{
    zlink_config_result_t (*next)(void *, zlink_option_t, void *, size_t *) =
        dlsym(RTLD_NEXT, "zlink_get_option");
    zlink_config_result_t result = next(socket, option, value, size);
    if (result == ZLINK_CONFIG_OK && option == ZLINK_OPT_RECONNECT_IVL
        && value && *size == sizeof(int)) {
        const int negative = -1;
        memcpy(value, &negative, sizeof(negative));
    }
    return result;
}
