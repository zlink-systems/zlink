/* SPDX-License-Identifier: MPL-2.0 */

#include "sample_common.h"

int main (void)
{
    void *ctx = zlink_ctx_new ();
    assert (ctx != NULL);
    void *server = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    assert (server != NULL);
    assert (client != NULL);

    void *server_monitor =
      open_socket_monitor (server, ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY);
    void *client_monitor =
      open_socket_monitor (client, ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY);

    /* try_recv before any connection: should return no event */
    zlink_socket_monitor_event_t try_event;
    int rc = zlink_socket_monitor_recv (server_monitor, &try_event, ZLINK_DONTWAIT);
    assert (rc != 0); /* no event available */
    rc = zlink_socket_monitor_recv (client_monitor, &try_event, ZLINK_DONTWAIT);
    assert (rc != 0); /* no event available */

    rc = zlink_bind (server, "tcp://127.0.0.1:0");
    assert (rc == 0);
    char endpoint[256];
    get_last_endpoint (server, endpoint, sizeof (endpoint));
    rc = zlink_connect (client, endpoint);
    assert (rc == 0);

    /* blocking recv: expect CONNECTION_READY with value=1 */
    zlink_socket_monitor_event_t server_event;
    rc = zlink_socket_monitor_recv (server_monitor, &server_event, 0);
    assert (rc == 0);
    assert (server_event.event == ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY);

    zlink_socket_monitor_event_t client_event;
    rc = zlink_socket_monitor_recv (client_monitor, &client_event, 0);
    assert (rc == 0);
    assert (client_event.event == ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY);

    /* try_recv after consuming the event: should return no event */
    rc = zlink_socket_monitor_recv (server_monitor, &try_event, ZLINK_DONTWAIT);
    assert (rc != 0);
    rc = zlink_socket_monitor_recv (client_monitor, &try_event, ZLINK_DONTWAIT);
    assert (rc != 0);

    printf ("[monitor/recv] recv: \"connection-ready\" -> tryRecv: empty\n");

    zlink_monitor_close (&client_monitor);
    zlink_monitor_close (&server_monitor);
    zlink_close (client);
    zlink_close (server);
    zlink_ctx_term (ctx);
    return 0;
}
