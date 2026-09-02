/* SPDX-License-Identifier: MPL-2.0 */

#include "sample_common.h"

typedef struct
{
    void *server;
    void *client;
    char received[64];
    size_t received_len;
} pair_sample_t;

static void pair_receiver_thread (void *arg_)
{
    pair_sample_t *sample = (pair_sample_t *) arg_;
    const zlink_routing_id_t *rid = NULL;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;

    assert (zlink_msg_init (&part) == 0);
    assert (zlink_recv_part (sample->server, &rid, &part, &has_more, 0) == ZLINK_RECV_OK);
    assert (has_more == ZLINK_PART_FINAL);

    sample->received_len = zlink_msg_size (&part);
    assert (sample->received_len == strlen (k_pair_payload));
    memcpy (sample->received, zlink_msg_data (&part), sample->received_len);
    sample->received[sample->received_len] = '\0';
    zlink_msg_close (&part);
}

static void pair_sender_thread (void *arg_)
{
    pair_sample_t *sample = (pair_sample_t *) arg_;
    zlink_msg_t outbound;

    make_message (&outbound, k_pair_payload);
    assert (zlink_send_part (sample->client, &outbound, 0, ZLINK_PART_FINAL, NULL, NULL)
            == ZLINK_SUBMIT_OK);
}

int main (void)
{
    pair_sample_t sample;
    memset (&sample, 0, sizeof (sample));

    void *ctx = zlink_ctx_new ();
    assert (ctx != NULL);
    sample.server = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    sample.client = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    assert (sample.server != NULL);
    assert (sample.client != NULL);

    void *server_monitor =
      open_socket_monitor (sample.server, ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY);
    void *client_monitor =
      open_socket_monitor (sample.client, ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY);

    assert (zlink_bind (sample.server, "tcp://127.0.0.1:0") == ZLINK_BIND_OK);
    char endpoint[256];
    get_last_endpoint (sample.server, endpoint, sizeof (endpoint));
    assert (zlink_connect (sample.client, endpoint) == ZLINK_CONNECT_OK);
    assert (wait_connected (server_monitor, client_monitor, 2000));

    void *receiver = zlink_thread_start (&pair_receiver_thread, &sample);
    void *sender = zlink_thread_start (&pair_sender_thread, &sample);
    assert (receiver != NULL);
    assert (sender != NULL);

    zlink_thread_join (sender);
    zlink_thread_join (receiver);

    assert (strcmp (sample.received, k_pair_payload) == 0);
    printf ("[pair/recv] send: \"%s\" -> recv: \"%.*s\"\n", k_pair_payload,
            (int) sample.received_len, sample.received);

    zlink_monitor_close (&client_monitor);
    zlink_monitor_close (&server_monitor);
    zlink_close (sample.client);
    zlink_close (sample.server);
    zlink_ctx_term (ctx);
    return 0;
}
