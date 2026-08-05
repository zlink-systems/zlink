/* SPDX-License-Identifier: MPL-2.0 */

#include "sample_common.h"

typedef struct
{
    void *router;
    void *dealer;
    char reply[64];
    size_t reply_len;
} dealer_router_sample_t;

static void dealer_router_router_thread (void *arg_)
{
    dealer_router_sample_t *sample = (dealer_router_sample_t *) arg_;
    const zlink_routing_id_t *source_node_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    zlink_msg_t reply;

    assert (zlink_msg_init (&part) == 0);
    assert (zlink_router_recv_part (sample->router, &source_node_rid, &request_seq, &part,
                                    &has_more, 0)
            == ZLINK_RECV_OK);
    assert (source_node_rid != NULL);
    assert (source_node_rid->size > 0);
    assert (request_seq == 0);
    assert (has_more == ZLINK_PART_FINAL);
    assert (zlink_msg_size (&part) == strlen (k_dealer_router_request));
    assert (
      memcmp (zlink_msg_data (&part), k_dealer_router_request, strlen (k_dealer_router_request))
      == 0);
    zlink_msg_close (&part);

    make_message (&reply, k_dealer_router_reply);
    assert (zlink_send_part_rid (sample->router, source_node_rid, &reply, 0, ZLINK_PART_FINAL)
            == ZLINK_SUBMIT_OK);
}

static void dealer_router_dealer_thread (void *arg_)
{
    dealer_router_sample_t *sample = (dealer_router_sample_t *) arg_;
    zlink_msg_t outbound;
    const zlink_routing_id_t *rid = NULL;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;

    make_message (&outbound, k_dealer_router_request);
    assert (zlink_send_part (sample->dealer, &outbound, 0, ZLINK_PART_FINAL) == ZLINK_SUBMIT_OK);

    assert (zlink_msg_init (&part) == 0);
    assert (zlink_recv_part (sample->dealer, &rid, &part, &has_more, 0) == ZLINK_RECV_OK);
    assert (has_more == ZLINK_PART_FINAL);

    sample->reply_len = zlink_msg_size (&part);
    assert (sample->reply_len == strlen (k_dealer_router_reply));
    memcpy (sample->reply, zlink_msg_data (&part), sample->reply_len);
    sample->reply[sample->reply_len] = '\0';
    zlink_msg_close (&part);
}

int main (void)
{
    dealer_router_sample_t sample;
    memset (&sample, 0, sizeof (sample));

    void *ctx = zlink_ctx_new ();
    assert (ctx != NULL);
    sample.router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    sample.dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    assert (sample.router != NULL);
    assert (sample.dealer != NULL);

    void *router_monitor =
      open_socket_monitor (sample.router, ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY);
    void *dealer_monitor =
      open_socket_monitor (sample.dealer, ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY);

    assert (zlink_bind (sample.router, "tcp://127.0.0.1:0") == ZLINK_BIND_OK);
    char endpoint[256];
    get_last_endpoint (sample.router, endpoint, sizeof (endpoint));
    assert (zlink_connect (sample.dealer, endpoint) == ZLINK_CONNECT_OK);
    assert (wait_for_socket_monitor_event_with_activity (
      router_monitor, sample.router, ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY, -1, 2000));
    assert (wait_for_socket_monitor_event (dealer_monitor,
                                           ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY, -1, 2000));

    void *router = zlink_thread_start (&dealer_router_router_thread, &sample);
    void *dealer = zlink_thread_start (&dealer_router_dealer_thread, &sample);
    assert (router != NULL);
    assert (dealer != NULL);

    zlink_thread_join (dealer);
    zlink_thread_join (router);

    assert (strcmp (sample.reply, k_dealer_router_reply) == 0);
    printf ("[dealer-router/recv] send: \"%s\" -> recv: \"%.*s\"\n", k_dealer_router_request,
            (int) sample.reply_len, sample.reply);

    zlink_monitor_close (&dealer_monitor);
    zlink_monitor_close (&router_monitor);
    zlink_close (sample.dealer);
    zlink_close (sample.router);
    zlink_ctx_term (ctx);
    return 0;
}
