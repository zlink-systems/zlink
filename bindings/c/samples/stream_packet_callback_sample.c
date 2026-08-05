/* SPDX-License-Identifier: MPL-2.0 */

#include "sample_common.h"

typedef struct
{
    callback_signal_t signal;
    char payload[256];
    size_t payload_len;
} stream_callback_ctx_t;

typedef struct
{
    void *server;
    void *server_monitor;
    callback_signal_t send_signal;
    char endpoint[256];
    stream_callback_ctx_t callback_ctx;
} stream_packet_sample_t;

static void store_u16_be (unsigned char *dst, uint16_t value)
{
    dst[0] = (unsigned char) ((value >> 8) & 0xffu);
    dst[1] = (unsigned char) (value & 0xffu);
}

static void store_u32_be (unsigned char *dst, uint32_t value)
{
    dst[0] = (unsigned char) ((value >> 24) & 0xffu);
    dst[1] = (unsigned char) ((value >> 16) & 0xffu);
    dst[2] = (unsigned char) ((value >> 8) & 0xffu);
    dst[3] = (unsigned char) (value & 0xffu);
}

static int send_all (int fd, const unsigned char *data, size_t size)
{
    size_t sent = 0;

    while (sent < size) {
        ssize_t rc = send (fd, data + sent, size - sent, 0);
        if (rc <= 0)
            return -1;
        sent += (size_t) rc;
    }

    return 0;
}

static int send_stream_packet_frame (int fd,
                                     const unsigned char *header,
                                     size_t header_size,
                                     const unsigned char *body,
                                     size_t body_size)
{
    unsigned char frame[512];
    const size_t frame_size = 6u + header_size + body_size;

    assert (frame_size <= sizeof (frame));
    store_u16_be (&frame[0], (uint16_t) header_size);
    store_u32_be (&frame[2], (uint32_t) body_size);
    if (header_size > 0)
        memcpy (&frame[6], header, header_size);
    if (body_size > 0)
        memcpy (&frame[6 + header_size], body, body_size);
    return send_all (fd, frame, frame_size);
}

static void stream_packet_callback (void *stream_,
                                    const zlink_routing_id_t *source_rid,
                                    zlink_msg_t *header,
                                    zlink_msg_t *body,
                                    void *userdata)
{
    stream_callback_ctx_t *cb = (stream_callback_ctx_t *) userdata;

    (void) stream_;
    assert (cb != NULL);
    assert (source_rid != NULL);
    assert (header != NULL);
    assert (body != NULL);
    assert (zlink_msg_size (header) == 0);

    cb->payload_len = zlink_msg_size (body);
    assert (cb->payload_len < sizeof (cb->payload));
    memcpy (cb->payload, zlink_msg_data (body), cb->payload_len);
    cb->payload[cb->payload_len] = '\0';

    zlink_msg_close (header);
    zlink_msg_close (body);
    callback_signal_set (&cb->signal);
}

static void stream_packet_server_thread (void *arg_)
{
    stream_packet_sample_t *sample = (stream_packet_sample_t *) arg_;

    assert (
      zlink_stream_packet_handler (sample->server, &stream_packet_callback, &sample->callback_ctx)
      == ZLINK_HANDLER_OK);
    assert (callback_signal_wait (&sample->callback_ctx.signal, 2000));
}

static void stream_packet_client_thread (void *arg_)
{
    stream_packet_sample_t *sample = (stream_packet_sample_t *) arg_;
    int client_fd = raw_tcp_connect (sample->endpoint);

    assert (callback_signal_wait (&sample->send_signal, 2000));
    assert (send_stream_packet_frame (client_fd, NULL, 0, (const unsigned char *) k_stream_payload,
                                      strlen (k_stream_payload))
            == 0);
    close (client_fd);
}

int main (void)
{
    stream_packet_sample_t sample;
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

    callback_signal_init (&sample.callback_ctx.signal);
    callback_signal_init (&sample.send_signal);

    void *server = zlink_thread_start (&stream_packet_server_thread, &sample);
    void *client = zlink_thread_start (&stream_packet_client_thread, &sample);
    assert (server != NULL);
    assert (client != NULL);

    assert (wait_stream_connected (sample.server_monitor, 2000));
    callback_signal_set (&sample.send_signal);

    zlink_thread_join (client);
    zlink_thread_join (server);

    assert (strcmp (sample.callback_ctx.payload, k_stream_payload) == 0);
    printf ("[stream/packet-callback] send: \"%s\" -> recv: \"%.*s\"\n", k_stream_payload,
            (int) sample.callback_ctx.payload_len, sample.callback_ctx.payload);

    callback_signal_destroy (&sample.send_signal);
    callback_signal_destroy (&sample.callback_ctx.signal);
    zlink_monitor_close (&sample.server_monitor);
    zlink_close (sample.server);
    zlink_ctx_term (ctx);
    return 0;
}
