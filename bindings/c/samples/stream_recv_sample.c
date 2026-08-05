/* SPDX-License-Identifier: MPL-2.0 */

#include "sample_common.h"

typedef struct
{
    void *server;
    void *server_monitor;
    callback_signal_t send_signal;
    char endpoint[256];
    char payload[64];
    size_t payload_len;
} stream_recv_sample_t;

static void stream_server_thread (void *arg_)
{
    stream_recv_sample_t *sample = (stream_recv_sample_t *) arg_;
    const zlink_routing_id_t *rid = NULL;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;

    assert (zlink_msg_init (&part) == 0);
    assert (zlink_recv_part (sample->server, &rid, &part, &has_more, 0) == ZLINK_RECV_OK);
    assert (rid != NULL);
    assert (rid->size > 0);
    assert (has_more == ZLINK_PART_FINAL);

    sample->payload_len = zlink_msg_size (&part);
    assert (sample->payload_len == strlen (k_stream_payload));
    memcpy (sample->payload, zlink_msg_data (&part), sample->payload_len);
    sample->payload[sample->payload_len] = '\0';
    zlink_msg_close (&part);
}

static void stream_client_thread (void *arg_)
{
    stream_recv_sample_t *sample = (stream_recv_sample_t *) arg_;
    const size_t request_size = strlen (k_stream_payload);
    int client_fd = raw_tcp_connect (sample->endpoint);
    ssize_t sent;

    assert (callback_signal_wait (&sample->send_signal, 2000));
    sent = send (client_fd, k_stream_payload, request_size, 0);
    assert (sent == (ssize_t) request_size);
    close (client_fd);
}

int main (void)
{
    stream_recv_sample_t sample;
    memset (&sample, 0, sizeof (sample));

    void *ctx = zlink_ctx_new ();
    assert (ctx != NULL);
    sample.server = zlink_socket (ctx, ZLINK_SOCKET_STREAM);
    assert (sample.server != NULL);

    int notify_off = 0;
    assert (zlink_set_stream_option (sample.server, ZLINK_STREAM_OPT_NOTIFY, &notify_off,
                                     sizeof (notify_off))
            == 0);

    sample.server_monitor =
      open_socket_monitor (sample.server, ZLINK_SOCKET_MONITOR_EVENT_ACCEPTED);

    assert (zlink_bind (sample.server, "tcp://127.0.0.1:0") == ZLINK_BIND_OK);
    get_last_endpoint (sample.server, sample.endpoint, sizeof (sample.endpoint));

    callback_signal_init (&sample.send_signal);

    void *receiver = zlink_thread_start (&stream_server_thread, &sample);
    void *client = zlink_thread_start (&stream_client_thread, &sample);
    assert (receiver != NULL);
    assert (client != NULL);

    assert (wait_stream_connected (sample.server_monitor, 2000));
    callback_signal_set (&sample.send_signal);

    zlink_thread_join (client);
    zlink_thread_join (receiver);

    assert (strcmp (sample.payload, k_stream_payload) == 0);
    printf ("[stream/recv] send: \"%s\" -> recv: \"%.*s\"\n", k_stream_payload,
            (int) sample.payload_len, sample.payload);

    callback_signal_destroy (&sample.send_signal);
    zlink_monitor_close (&sample.server_monitor);
    zlink_close (sample.server);
    zlink_ctx_term (ctx);
    return 0;
}
