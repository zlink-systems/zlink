/* SPDX-License-Identifier: MPL-2.0 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "zlink.h"

static atomic_uint close_calls;

int zlink_test_take_close_calls (void)
{
    return (int) atomic_exchange (&close_calls, 0);
}

zlink_close_result_t zlink_ctx_shutdown (void *context)
{
    // The first term performs shutdown while it still owns the live context.
    (void) context;
    return ZLINK_CLOSE_OK;
}

zlink_close_result_t zlink_ctx_term (void *context)
{
    if (atomic_fetch_add (&close_calls, 1) != 0)
        return ZLINK_CLOSE_OK;

    zlink_close_result_t (*shutdown) (void *) = dlsym (RTLD_NEXT, "zlink_ctx_shutdown");
    zlink_close_result_t (*term) (void *) = dlsym (RTLD_NEXT, "zlink_ctx_term");
    if (!shutdown || !term)
        abort ();
    const zlink_close_result_t result = shutdown (context);
    return result == ZLINK_CLOSE_OK ? term (context) : result;
}

static zlink_close_result_t destroy_once (const char *symbol, void **holder)
{
    // Record duplicate downcalls without letting Core destroy a handle twice.
    if (atomic_fetch_add (&close_calls, 1) != 0)
        return ZLINK_CLOSE_OK;

    zlink_close_result_t (*destroy) (void **) = dlsym (RTLD_NEXT, symbol);
    if (!destroy)
        abort ();
    return destroy (holder);
}

zlink_close_result_t zlink_monitor_close (void **monitor)
{
    return destroy_once ("zlink_monitor_close", monitor);
}

zlink_close_result_t zlink_poller_destroy (void **poller)
{
    return destroy_once ("zlink_poller_destroy", poller);
}

zlink_close_result_t zlink_timer_destroy (void **timer)
{
    return destroy_once ("zlink_timer_destroy", timer);
}
