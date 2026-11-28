// SPDX-License-Identifier: MPL-2.0

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlink.h"

static int from_binding(void *caller)
{
    Dl_info info;
    return dladdr(caller, &info) && strstr(info.dli_fname, "/libzlink.so") == NULL;
}

static void record(const char *operation, const char *event)
{
    const char *path = getenv("ZLINK_AUDIT_MARKER");
    const int saved_errno = errno;
    FILE *trace = path ? fopen(path, "a") : NULL;
    if (!trace)
        abort();
    if (operation)
        fprintf(trace, "%s:", operation);
    fprintf(trace, "%s\n", event);
    fclose(trace);
    errno = saved_errno;
}

zlink_config_result_t zlink_msg_init(zlink_msg_t *message)
{
    zlink_config_result_t (*next)(zlink_msg_t *) = dlsym(RTLD_NEXT, "zlink_msg_init");
    if (getenv("ZLINK_AUDIT_FAIL_MESSAGE_INIT")
        && from_binding(__builtin_return_address(0))) {
        record(NULL, "message-init-failed-ENOMEM");
        errno = ENOMEM;
        return ZLINK_CONFIG_INTERNAL_ERROR;
    }
    return next(message);
}

zlink_config_result_t zlink_msg_adopt(zlink_msg_t *dest, zlink_msg_t *src)
{
    zlink_config_result_t (*next)(zlink_msg_t *, zlink_msg_t *) =
        dlsym(RTLD_NEXT, "zlink_msg_adopt");
    if (getenv("ZLINK_AUDIT_FAIL_INITIALIZED_ADOPT")
        && from_binding(__builtin_return_address(0))) {
        const unsigned char empty[sizeof(zlink_msg_t)] = {0};
        if (memcmp(dest, empty, sizeof(empty)) != 0) {
            record(NULL, "adopt-rejected-initialized-destination");
            errno = EINVAL;
            return ZLINK_CONFIG_INVALID_ARGUMENT;
        }
        record(NULL, "adopt-accepted-uninitialized-destination");
    }
    return next(dest, src);
}

zlink_config_result_t zlink_msg_copy(zlink_msg_t *dest, zlink_msg_t *src)
{
    zlink_config_result_t (*next)(zlink_msg_t *, zlink_msg_t *) =
        dlsym(RTLD_NEXT, "zlink_msg_copy");
    const char *operation = getenv("ZLINK_AUDIT_OPERATION");
    if (operation && from_binding(__builtin_return_address(0)))
        record(operation, "copy");
    return next(dest, src);
}

void zlink_multipart_close(zlink_msg_t *parts, size_t count)
{
    void (*next)(zlink_msg_t *, size_t) = dlsym(RTLD_NEXT, "zlink_multipart_close");
    const char *operation = getenv("ZLINK_AUDIT_OPERATION");
    if (operation && from_binding(__builtin_return_address(0))) {
        char event[64];
        snprintf(event, sizeof(event), "multipart-close-%zu", count);
        record(operation, event);
    }
    next(parts, count);
}

zlink_submit_result_t zlink_send(
    void *socket, zlink_msg_t *parts, size_t count, zlink_send_flags_t flags,
    void *context, zlink_completion_id_t *completion)
{
    zlink_submit_result_t (*next)(
        void *, zlink_msg_t *, size_t, zlink_send_flags_t, void *, zlink_completion_id_t *) =
        dlsym(RTLD_NEXT, "zlink_send");
    const char *operation = getenv("ZLINK_AUDIT_OPERATION");
    if (operation && from_binding(__builtin_return_address(0)))
        record(operation, "submit-send");
    return next(socket, parts, count, flags, context, completion);
}

zlink_submit_result_t zlink_reply(
    void *socket, const zlink_routing_id_t *source, zlink_reply_token_t token,
    zlink_msg_t *parts, size_t count)
{
    zlink_submit_result_t (*next)(
        void *, const zlink_routing_id_t *, zlink_reply_token_t, zlink_msg_t *, size_t) =
        dlsym(RTLD_NEXT, "zlink_reply");
    const char *operation = getenv("ZLINK_AUDIT_OPERATION");
    if (operation && from_binding(__builtin_return_address(0)))
        record(operation, "submit-reply");
    return next(socket, source, token, parts, count);
}
