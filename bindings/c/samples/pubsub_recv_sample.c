/* SPDX-License-Identifier: MPL-2.0 */

#include "sample_common.h"

typedef struct
{
    void *publisher;
    void *subscriber;
    char topic[64];
    char payload[64];
    size_t payload_len;
} pubsub_sample_t;

static void pubsub_subscriber_thread (void *arg_)
{
    pubsub_sample_t *sample = (pubsub_sample_t *) arg_;
    const zlink_routing_id_t *rid = NULL;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    size_t topic_len = sizeof (sample->topic);

    assert (zlink_msg_init (&part) == 0);
    assert (zlink_subscribe_part (sample->subscriber, &rid, sample->topic, topic_len, &topic_len,
                                  &part, &has_more, 0)
            == ZLINK_RECV_OK);
    assert (has_more == ZLINK_PART_FINAL);
    sample->payload_len = zlink_msg_size (&part);
    assert (sample->payload_len == strlen (k_pubsub_payload));
    memcpy (sample->payload, zlink_msg_data (&part), sample->payload_len);
    sample->payload[sample->payload_len] = '\0';
    zlink_msg_close (&part);
}

static void pubsub_publisher_thread (void *arg_)
{
    pubsub_sample_t *sample = (pubsub_sample_t *) arg_;
    zlink_msg_t outbound;

    make_message (&outbound, k_pubsub_payload);
    assert (zlink_publish_part (sample->publisher, k_pubsub_topic, &outbound, 0, ZLINK_PART_FINAL)
            == ZLINK_SUBMIT_OK);
}

int main (void)
{
    pubsub_sample_t sample;
    memset (&sample, 0, sizeof (sample));

    void *ctx = zlink_ctx_new ();
    assert (ctx != NULL);
    sample.publisher = zlink_socket (ctx, ZLINK_SOCKET_XPUB);
    sample.subscriber = zlink_socket (ctx, ZLINK_SOCKET_SUB);
    assert (sample.publisher != NULL);
    assert (sample.subscriber != NULL);

    void *pub_monitor =
      open_socket_monitor (sample.publisher, ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY);
    void *sub_monitor =
      open_socket_monitor (sample.subscriber, ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY);

    assert (zlink_bind (sample.publisher, "tcp://127.0.0.1:0") == ZLINK_BIND_OK);
    char endpoint[256];
    get_last_endpoint (sample.publisher, endpoint, sizeof (endpoint));
    assert (zlink_connect (sample.subscriber, endpoint) == ZLINK_CONNECT_OK);

    assert (zlink_set_subscription (sample.subscriber, k_pubsub_topic) == 0);

    void *subscriber = zlink_thread_start (&pubsub_subscriber_thread, &sample);
    assert (subscriber != NULL);

    assert (wait_connected (pub_monitor, sub_monitor, 2000));

    zlink_routing_id_t event_rid;
    int subscribed = 0;
    char event_topic[256];
    size_t event_topic_len = sizeof (event_topic);
    memset (&event_rid, 0, sizeof (event_rid));
    assert (wait_for_subscription_event (sample.publisher, &subscribed, event_topic,
                                         &event_topic_len, 2000));
    assert (subscribed == 1);
    assert (strcmp (event_topic, k_pubsub_topic) == 0);

    void *publisher = zlink_thread_start (&pubsub_publisher_thread, &sample);
    assert (publisher != NULL);

    zlink_thread_join (publisher);
    zlink_thread_join (subscriber);

    assert (strcmp (sample.topic, k_pubsub_topic) == 0);
    assert (strcmp (sample.payload, k_pubsub_payload) == 0);
    printf ("[pubsub/recv] publish: \"%s/%s\" -> subscribe: \"%s/%.*s\"\n", k_pubsub_topic,
            k_pubsub_payload, sample.topic, (int) sample.payload_len, sample.payload);

    zlink_monitor_close (&sub_monitor);
    zlink_monitor_close (&pub_monitor);
    zlink_close (sample.subscriber);
    zlink_close (sample.publisher);
    zlink_ctx_term (ctx);
    return 0;
}
