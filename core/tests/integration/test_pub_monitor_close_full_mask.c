/* SPDX-License-Identifier: MPL-2.0 */

#include <stdio.h>
#include <stdlib.h>

#include "zlink.h"

int main (void)
{
    void *ctx = zlink_ctx_new ();
    if (!ctx) {
        perror ("zlink_ctx_new");
        return 1;
    }

    void *pub = zlink_socket (ctx, ZLINK_SOCKET_PUB);
    if (!pub) {
        perror ("zlink_socket");
        zlink_ctx_term (ctx);
        return 1;
    }

    zlink_socket_monitor_open_options_t options;
    options.events = 0x7FFFFu;
    options.monitor_hwm_bytes = 0;
    void *monitor = zlink_socket_monitor_open (pub, &options);
    if (!monitor) {
        perror ("zlink_socket_monitor_open");
        zlink_close (pub);
        zlink_ctx_term (ctx);
        return 1;
    }

    fprintf (stderr, "opened pub=%p monitor=%p\n", pub, monitor);
    fflush (stderr);
    zlink_sleep (1);

    fprintf (stderr, "closing pub\n");
    fflush (stderr);
    if (zlink_close (pub) != ZLINK_CLOSE_OK) {
        perror ("zlink_close(pub)");
        zlink_ctx_term (ctx);
        return 1;
    }

    fprintf (stderr, "closed pub, waiting for reaper\n");
    fflush (stderr);
    zlink_sleep (1);
    (void) monitor;
    (void) ctx;
    return 0;
}
