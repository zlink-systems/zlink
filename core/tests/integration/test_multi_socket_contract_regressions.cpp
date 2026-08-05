/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "../../src/runtime/sockets/common/socket_base.hpp"

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
struct delivery_ready_monitor_state_t
{
    delivery_ready_monitor_state_t () : ready (false), ready_count (0), error_code (0) {}

    std::mutex sync;
    std::condition_variable cv;
    bool ready;
    uint32_t ready_count;
    int error_code;
};

struct delivery_ready_monitor_t
{
    delivery_ready_monitor_t () : monitor (NULL), state (NULL) {}

    void *monitor;
    delivery_ready_monitor_state_t *state;
};

struct publish_start_gate_t
{
    publish_start_gate_t () : ready (0), go (false) {}

    std::mutex sync;
    std::condition_variable cv;
    std::atomic<int> ready;
    bool go;
};

struct publisher_probe_t
{
    publisher_probe_t () :
        gate (NULL), socket (NULL), phase ('?'), count (0), failed (false), publish_errno (0)
    {
    }

    publish_start_gate_t *gate;
    void *socket;
    char phase;
    int count;
    std::atomic<bool> failed;
    std::atomic<int> publish_errno;
};

struct pubsub_callback_probe_t
{
    pubsub_callback_probe_t () : warmup_count (0), drain_count (0), active_count (0), fatal (false)
    {
    }

    std::mutex sync;
    std::condition_variable cv;
    size_t warmup_count;
    size_t drain_count;
    size_t active_count;
    bool fatal;
};

static const char k_pubsub_topic[] = "bench";

void set_timeout_opts (void *socket_)
{
    const int timeout_ms = 2000;
    const int linger_ms = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &linger_ms, sizeof (linger_ms)));
}

void recv_parts_expect_payload (void *socket_,
                                const char *expected_source_rid_,
                                const char *expected_payload_)
{
    zlink_routing_id_t source_rid;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    const zlink_routing_id_t *peer_rid = NULL;
    uint64_t request_seq = 0;
    memset (&source_rid, 0, sizeof (source_rid));
    if (expected_source_rid_) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv (socket_, &peer_rid,
                                                      &request_seq, &parts, &part_count, 0));
        TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
        if (peer_rid)
            source_rid = *peer_rid;
    } else {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (socket_, NULL, &parts, &part_count, 0));
    }
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);

    const size_t expected_source_rid_size =
      expected_source_rid_ ? std::strlen (expected_source_rid_) : 0;
    TEST_ASSERT_EQUAL_UINT64 (expected_source_rid_size, source_rid.size);
    if (expected_source_rid_size > 0) {
        TEST_ASSERT_EQUAL_MEMORY (expected_source_rid_, source_rid.data, expected_source_rid_size);
    }

    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (std::strlen (expected_payload_), zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (expected_payload_, zlink_msg_data (&parts[0]),
                              std::strlen (expected_payload_));
    zlink_multipart_close (parts, part_count);
}

void send_single_payload (void *socket_, const char *payload_)
{
    zlink_msg_t part;
    const size_t payload_size = std::strlen (payload_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, payload_size));
    memcpy (zlink_msg_data (&part), payload_, payload_size);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (socket_, &part, 1, 0));
}

void send_router_envelope_payload (void *router_, const char *target_rid_, const char *payload_)
{
    zlink_msg_t parts[2];
    const size_t target_size = std::strlen (target_rid_);
    const size_t payload_size = std::strlen (payload_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[0], target_size));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[1], payload_size));
    memcpy (zlink_msg_data (&parts[0]), target_rid_, target_size);
    memcpy (zlink_msg_data (&parts[1]), payload_, payload_size);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (router_, parts, 2, 0));
}

void delivery_ready_monitor_handler (const zlink_monitor_event_t *event_, void *userdata_)
{
    delivery_ready_monitor_state_t *state =
      static_cast<delivery_ready_monitor_state_t *> (userdata_);
    if (!state || !event_)
        return;

    {
        std::lock_guard<std::mutex> lock (state->sync);
        switch (event_->event) {
            case ZLINK_EVENT_CONNECTION_READY:
                state->ready = true;
                ++state->ready_count;
                break;
            case ZLINK_EVENT_BIND_FAILED:
            case ZLINK_EVENT_ACCEPT_FAILED:
            case ZLINK_EVENT_CLOSE_FAILED:
            case ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL:
            case ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL:
            case ZLINK_EVENT_HANDSHAKE_FAILED_AUTH:
                if (state->error_code == 0)
                    state->error_code = event_->value > 0 ? static_cast<int> (event_->value) : EIO;
                break;
            default:
                break;
        }
    }

    state->cv.notify_all ();
}

bool open_delivery_ready_monitor (void *socket_, uint64_t events_, delivery_ready_monitor_t *out_)
{
    if (!socket_ || !out_)
        return false;

    delivery_ready_monitor_state_t *state = new (std::nothrow) delivery_ready_monitor_state_t;
    if (!state)
        return false;

    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = events_ | ZLINK_EVENT_BIND_FAILED | ZLINK_EVENT_ACCEPT_FAILED
                  | ZLINK_EVENT_CLOSE_FAILED | ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL
                  | ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL | ZLINK_EVENT_HANDSHAKE_FAILED_AUTH;
    void *monitor = zlink_socket_monitor_open (socket_, &opts);
    if (!monitor) {
        delete state;
        return false;
    }

    if (zlink_socket_monitor_handler (monitor, &delivery_ready_monitor_handler, state) != 0) {
        (void) zlink_monitor_close (&monitor);
        delete state;
        return false;
    }

    const int zero = 0;
    if (zlink_set_option (monitor, ZLINK_OPT_LINGER, &zero, sizeof (zero)) != ZLINK_CONFIG_OK) {
        (void) zlink_monitor_close (&monitor);
        delete state;
        return false;
    }

    out_->monitor = monitor;
    out_->state = state;
    return true;
}

bool wait_delivery_ready (delivery_ready_monitor_t *monitor_, int timeout_ms_)
{
    if (!monitor_ || !monitor_->state)
        return false;

    std::unique_lock<std::mutex> lock (monitor_->state->sync);
    return monitor_->state->cv.wait_for (
             lock, std::chrono::milliseconds (timeout_ms_ > 0 ? timeout_ms_ : 1),
             [monitor_] () { return monitor_->state->error_code != 0 || monitor_->state->ready; })
           && monitor_->state->error_code == 0 && monitor_->state->ready;
}

bool wait_delivery_ready_count (delivery_ready_monitor_t *monitor_,
                                uint32_t expected_count_,
                                int timeout_ms_)
{
    if (!monitor_ || !monitor_->state)
        return false;

    std::unique_lock<std::mutex> lock (monitor_->state->sync);
    return monitor_->state->cv.wait_for (
             lock, std::chrono::milliseconds (timeout_ms_ > 0 ? timeout_ms_ : 1),
             [monitor_, expected_count_] () {
                 return monitor_->state->error_code != 0
                        || monitor_->state->ready_count >= expected_count_;
             })
           && monitor_->state->error_code == 0 && monitor_->state->ready_count >= expected_count_;
}

void close_delivery_ready_monitor (delivery_ready_monitor_t *monitor_)
{
    if (!monitor_)
        return;

    void *monitor = monitor_->monitor;
    delivery_ready_monitor_state_t *state = monitor_->state;
    monitor_->monitor = NULL;
    monitor_->state = NULL;

    if (monitor)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    delete state;
}

void pubsub_handler (const zlink_routing_id_t *,
                     const char *topic_,
                     size_t topic_len_,
                     zlink_msg_t *parts_,
                     size_t part_count_,
                     void *userdata_)
{
    pubsub_callback_probe_t *probe = static_cast<pubsub_callback_probe_t *> (userdata_);
    if (!probe) {
        if (parts_)
            zlink_multipart_close (parts_, part_count_);
        return;
    }

    bool fatal = false;
    char phase = '\0';
    if (!topic_ || topic_len_ != std::strlen (k_pubsub_topic)
        || std::memcmp (topic_, k_pubsub_topic, topic_len_) != 0 || !parts_ || part_count_ != 1
        || zlink_msg_size (&parts_[0]) < 1) {
        fatal = true;
    } else {
        phase = *static_cast<const char *> (zlink_msg_data (&parts_[0]));
    }

    if (parts_)
        zlink_multipart_close (parts_, part_count_);

    {
        std::lock_guard<std::mutex> lock (probe->sync);
        if (fatal) {
            probe->fatal = true;
        } else if (phase == 'W') {
            ++probe->warmup_count;
        } else if (phase == 'D') {
            ++probe->drain_count;
        } else if (phase == 'A') {
            ++probe->active_count;
        } else {
            probe->fatal = true;
        }
    }
    probe->cv.notify_all ();
}

bool wait_probe_phase_count (pubsub_callback_probe_t *probe_,
                             char phase_,
                             size_t expected_count_,
                             int timeout_ms_)
{
    if (!probe_)
        return false;

    std::unique_lock<std::mutex> lock (probe_->sync);
    return probe_->cv.wait_for (lock, std::chrono::milliseconds (timeout_ms_ > 0 ? timeout_ms_ : 1),
                                [probe_, phase_, expected_count_] () {
                                    if (probe_->fatal)
                                        return true;
                                    if (phase_ == 'W')
                                        return probe_->warmup_count >= expected_count_;
                                    if (phase_ == 'D')
                                        return probe_->drain_count >= expected_count_;
                                    return probe_->active_count >= expected_count_;
                                });
}

void publish_phase_message (void *pub_, char phase_, size_t seq_)
{
    char payload[32];
    const int len = std::snprintf (payload, sizeof (payload), "%c%06zu", phase_, seq_);
    TEST_ASSERT_TRUE (len > 0);

    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, static_cast<size_t> (len)));
    memcpy (zlink_msg_data (&part), payload, static_cast<size_t> (len));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_publish (pub_, k_pubsub_topic, &part, 1, 0));
}

std::string make_fixed_size_payload (char phase_, size_t seq_, size_t size_)
{
    std::ostringstream stream;
    stream << phase_ << ":" << seq_ << ":payload";
    std::string payload = stream.str ();
    if (payload.size () > size_)
        payload.resize (size_);
    if (payload.size () < size_)
        payload.append (size_ - payload.size (), '#');
    return payload;
}

void publish_payload (void *pub_, const std::string &payload_)
{
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, payload_.size ()));
    memcpy (zlink_msg_data (&part), payload_.data (), payload_.size ());
    TEST_ASSERT_SUCCESS_ERRNO (zlink_publish (pub_, k_pubsub_topic, &part, 1, 0));
}

void wait_and_publish_payloads (publisher_probe_t *probe_)
{
    {
        std::unique_lock<std::mutex> lock (probe_->gate->sync);
        probe_->gate->ready.fetch_add (1, std::memory_order_acq_rel);
        probe_->gate->cv.notify_all ();
        probe_->gate->cv.wait (lock, [&] () { return probe_->gate->go; });
    }

    for (int i = 0; i < probe_->count; ++i) {
        const std::string payload =
          make_fixed_size_payload (probe_->phase, static_cast<size_t> (i), 64);

        zlink_msg_t part;
        if (zlink_msg_init_size (&part, payload.size ()) != 0) {
            probe_->failed.store (true, std::memory_order_release);
            probe_->publish_errno.store (errno, std::memory_order_release);
            return;
        }

        memcpy (zlink_msg_data (&part), payload.data (), payload.size ());
        if (zlink_publish (probe_->socket, k_pubsub_topic, &part, 1, 0) != 0) {
            const int err = errno;
            (void) zlink_msg_close (&part);
            probe_->failed.store (true, std::memory_order_release);
            probe_->publish_errno.store (err, std::memory_order_release);
            return;
        }
    }
}

bool parse_concurrent_publish_payload (const std::string &payload_, char *phase_out_, int *seq_out_)
{
    if (!phase_out_ || !seq_out_)
        return false;

    const std::string::size_type first_colon = payload_.find (':');
    if (first_colon != 1)
        return false;

    const std::string::size_type second_colon = payload_.find (':', first_colon + 1);
    if (second_colon == std::string::npos || second_colon == first_colon + 1)
        return false;

    int seq = -1;
    std::istringstream seq_stream (
      payload_.substr (first_colon + 1, second_colon - first_colon - 1));
    seq_stream >> seq;
    if (!seq_stream || !seq_stream.eof ())
        return false;

    *phase_out_ = payload_[0];
    *seq_out_ = seq;
    return true;
}

void publish_two_part_payload (void *pub_, const std::string &part_a_, const std::string &part_b_)
{
    zlink_msg_t parts[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[0], part_a_.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[1], part_b_.size ()));
    memcpy (zlink_msg_data (&parts[0]), part_a_.data (), part_a_.size ());
    memcpy (zlink_msg_data (&parts[1]), part_b_.data (), part_b_.size ());
    TEST_ASSERT_SUCCESS_ERRNO (zlink_publish (pub_, k_pubsub_topic, parts, 2, 0));
}

void recv_subscribe_expect_topic_and_payload (void *sub_, const std::string &payload_)
{
    char topic[32];
    memset (topic, 0, sizeof (topic));
    size_t topic_len = sizeof (topic);
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_subscribe (sub_, NULL, &parts, &part_count, topic, &topic_len, 0));
    TEST_ASSERT_EQUAL_UINT64 (std::strlen (k_pubsub_topic), topic_len);
    TEST_ASSERT_EQUAL_MEMORY (k_pubsub_topic, topic, topic_len);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_NOT_NULL (parts);
    TEST_ASSERT_EQUAL_UINT64 (payload_.size (), zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (payload_.data (), zlink_msg_data (&parts[0]), payload_.size ());

    zlink_multipart_close (parts, part_count);
}

void recv_subscribe_expect_payload_without_topic_copy (void *sub_, const std::string &payload_)
{
    size_t topic_len = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_subscribe (sub_, NULL, &parts, &part_count, NULL, &topic_len, 0));
    TEST_ASSERT_EQUAL_UINT64 (std::strlen (k_pubsub_topic), topic_len);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_NOT_NULL (parts);
    TEST_ASSERT_EQUAL_UINT64 (payload_.size (), zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (payload_.data (), zlink_msg_data (&parts[0]), payload_.size ());

    zlink_multipart_close (parts, part_count);
}

void recv_subscribe_expect_payload_parts_without_topic_copy (void *sub_,
                                                             const std::string &part_a_,
                                                             const std::string &part_b_)
{
    size_t topic_len = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_subscribe (sub_, NULL, &parts, &part_count, NULL, &topic_len, 0));
    TEST_ASSERT_EQUAL_UINT64 (std::strlen (k_pubsub_topic), topic_len);
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    TEST_ASSERT_NOT_NULL (parts);
    TEST_ASSERT_EQUAL_UINT64 (part_a_.size (), zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (part_a_.data (), zlink_msg_data (&parts[0]), part_a_.size ());
    TEST_ASSERT_EQUAL_UINT64 (part_b_.size (), zlink_msg_size (&parts[1]));
    TEST_ASSERT_EQUAL_MEMORY (part_b_.data (), zlink_msg_data (&parts[1]), part_b_.size ());

    zlink_multipart_close (parts, part_count);
}

bool try_recv_subscribe_expect_topic_and_payload_dontwait (void *sub_, const std::string &payload_)
{
    char topic[32];
    memset (topic, 0, sizeof (topic));
    size_t topic_len = sizeof (topic);
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;

    const int rc =
      zlink_subscribe (sub_, NULL, &parts, &part_count, topic, &topic_len, ZLINK_DONTWAIT);
    if (rc != 0) {
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        return false;
    }

    TEST_ASSERT_EQUAL_UINT64 (std::strlen (k_pubsub_topic), topic_len);
    TEST_ASSERT_EQUAL_MEMORY (k_pubsub_topic, topic, topic_len);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_NOT_NULL (parts);
    TEST_ASSERT_EQUAL_UINT64 (payload_.size (), zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (payload_.data (), zlink_msg_data (&parts[0]), payload_.size ());

    zlink_multipart_close (parts, part_count);
    return true;
}

void recv_subscribe_expect_topic_and_payload_eventually (void *sub_,
                                                         const std::string &payload_,
                                                         int timeout_ms_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_ > 0 ? timeout_ms_ : 1);

    while (std::chrono::steady_clock::now () < deadline) {
        if (try_recv_subscribe_expect_topic_and_payload_dontwait (sub_, payload_))
            return;
        std::this_thread::yield ();
    }

    TEST_FAIL_MESSAGE ("timed out waiting for subscribed payload");
}

} // namespace

void test_router_recv_with_source_rid_strips_routing_envelope_from_dealer ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    delivery_ready_monitor_t router_monitor;
    delivery_ready_monitor_t dealer_monitor;

    set_timeout_opts (router);
    set_timeout_opts (dealer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "D1", 2));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (router, ZLINK_EVENT_CONNECTION_READY, &router_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (dealer, ZLINK_EVENT_CONNECTION_READY, &dealer_monitor));

    char endpoint[MAX_SOCKET_STRING];
    test_bind (router, "tcp://127.0.0.1:*", endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    TEST_ASSERT_TRUE (wait_delivery_ready (&router_monitor, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready (&dealer_monitor, 5000));

    send_single_payload (dealer, "hello");
    recv_parts_expect_payload (router, "D1", "hello");

    close_delivery_ready_monitor (&dealer_monitor);
    close_delivery_ready_monitor (&router_monitor);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_dealer_recv_with_source_rid_hides_peer_routing_id ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    delivery_ready_monitor_t router_monitor;
    delivery_ready_monitor_t dealer_monitor;

    set_timeout_opts (router);
    set_timeout_opts (dealer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "D1", 2));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (router, ZLINK_EVENT_CONNECTION_READY, &router_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (dealer, ZLINK_EVENT_CONNECTION_READY, &dealer_monitor));

    char endpoint[MAX_SOCKET_STRING];
    test_bind (router, "tcp://127.0.0.1:*", endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    TEST_ASSERT_TRUE (wait_delivery_ready (&router_monitor, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready (&dealer_monitor, 5000));

    send_single_payload (dealer, "ping");
    recv_parts_expect_payload (router, "D1", "ping");

    send_router_envelope_payload (router, "D1", "pong");
    recv_parts_expect_payload (dealer, NULL, "pong");

    close_delivery_ready_monitor (&dealer_monitor);
    close_delivery_ready_monitor (&router_monitor);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_recv_dontwait_no_data_does_not_break_poller_rearm ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    delivery_ready_monitor_t router_monitor;
    delivery_ready_monitor_t dealer_monitor;

    set_timeout_opts (router);
    set_timeout_opts (dealer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "D1", 2));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (router, ZLINK_EVENT_CONNECTION_READY, &router_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (dealer, ZLINK_EVENT_CONNECTION_READY, &dealer_monitor));

    char endpoint[MAX_SOCKET_STRING];
    test_bind (router, "tcp://127.0.0.1:*", endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    TEST_ASSERT_TRUE (wait_delivery_ready (&router_monitor, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready (&dealer_monitor, 5000));

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, router, router, ZLINK_POLLIN));

    send_single_payload (dealer, "hello");

    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (poller, &event, 1, 5000, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLER_SOURCE_SOCKET, event.source_kind);
    TEST_ASSERT_EQUAL_PTR (router, event.socket);

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
                           zlink_router_recv (router, &source_rid, &request_seq,
                                              &parts, &part_count, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    zlink_multipart_close (parts, part_count);

    parts = NULL;
    part_count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA,
                           zlink_router_recv (router, &source_rid, &request_seq,
                                              &parts, &part_count, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    send_single_payload (dealer, "world");

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (5000);
    int wait_rc = 0;
    do {
        memset (&event, 0, sizeof (event));
        wait_rc = zlink_poller_wait (poller, &event, 1, 50, NULL);
        if (wait_rc == 1)
            break;
    } while (std::chrono::steady_clock::now () < deadline);

    TEST_ASSERT_EQUAL_INT (1, wait_rc);
    TEST_ASSERT_EQUAL_PTR (router, event.socket);

    parts = NULL;
    part_count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
                           zlink_router_recv (router, &source_rid, &request_seq,
                                              &parts, &part_count, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_STRING_LEN ("world",
                                  reinterpret_cast<const char *> (zlink_msg_data (&parts[0])),
                                  zlink_msg_size (&parts[0]));
    zlink_multipart_close (parts, part_count);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_destroy (&poller));
    close_delivery_ready_monitor (&dealer_monitor);
    close_delivery_ready_monitor (&router_monitor);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_pubsub_callback_is_supported_on_raw_sub_sockets ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub_a = test_context_socket (ZLINK_SOCKET_SUB);
    void *sub_b = test_context_socket (ZLINK_SOCKET_SUB);

    set_timeout_opts (pub);
    set_timeout_opts (sub_a);
    set_timeout_opts (sub_b);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub_a, ""));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub_b, ""));

    delivery_ready_monitor_t pub_monitor;
    delivery_ready_monitor_t sub_a_monitor;
    delivery_ready_monitor_t sub_b_monitor;
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (pub, ZLINK_EVENT_CONNECTION_READY, &pub_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (sub_a, ZLINK_EVENT_CONNECTION_READY, &sub_a_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (sub_b, ZLINK_EVENT_CONNECTION_READY, &sub_b_monitor));

    char endpoint[MAX_SOCKET_STRING];
    test_bind (pub, "tcp://127.0.0.1:*", endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub_a, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub_b, endpoint));

    TEST_ASSERT_TRUE (wait_delivery_ready (&pub_monitor, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready (&sub_a_monitor, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready (&sub_b_monitor, 5000));

    publish_phase_message (pub, 'W', 1);
    publish_phase_message (pub, 'A', 1);

    for (int i = 0; i < 2; ++i) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        char topic[32] = {0};
        size_t topic_len = sizeof (topic);
        char expected_payload[16];
        std::snprintf (expected_payload, sizeof (expected_payload), "%c%06d", i == 0 ? 'W' : 'A',
                       1);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_subscribe (sub_a, NULL, &parts, &part_count, topic, &topic_len, 0));
        TEST_ASSERT_EQUAL_UINT64 (1, part_count);
        TEST_ASSERT_EQUAL_STRING (k_pubsub_topic, topic);
        TEST_ASSERT_EQUAL_UINT64 (std::strlen (expected_payload), zlink_msg_size (&parts[0]));
        TEST_ASSERT_EQUAL_MEMORY (expected_payload, zlink_msg_data (&parts[0]),
                                  std::strlen (expected_payload));
        zlink_multipart_close (parts, part_count);
    }
    for (int i = 0; i < 2; ++i) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        char topic[32] = {0};
        size_t topic_len = sizeof (topic);
        char expected_payload[16];
        std::snprintf (expected_payload, sizeof (expected_payload), "%c%06d", i == 0 ? 'W' : 'A',
                       1);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_subscribe (sub_b, NULL, &parts, &part_count, topic, &topic_len, 0));
        TEST_ASSERT_EQUAL_UINT64 (1, part_count);
        TEST_ASSERT_EQUAL_STRING (k_pubsub_topic, topic);
        TEST_ASSERT_EQUAL_UINT64 (std::strlen (expected_payload), zlink_msg_size (&parts[0]));
        TEST_ASSERT_EQUAL_MEMORY (expected_payload, zlink_msg_data (&parts[0]),
                                  std::strlen (expected_payload));
        zlink_multipart_close (parts, part_count);
    }

    close_delivery_ready_monitor (&sub_b_monitor);
    close_delivery_ready_monitor (&sub_a_monitor);
    close_delivery_ready_monitor (&pub_monitor);
    test_context_socket_close_zero_linger (pub);
    test_context_socket_close_zero_linger (sub_b);
    test_context_socket_close_zero_linger (sub_a);
}

void test_pubsub_subscribe_preserves_topic_and_payload_shape_across_warmup ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub_a = test_context_socket (ZLINK_SOCKET_SUB);
    void *sub_b = test_context_socket (ZLINK_SOCKET_SUB);

    set_timeout_opts (pub);
    set_timeout_opts (sub_a);
    set_timeout_opts (sub_b);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub_a, ""));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub_b, ""));

    delivery_ready_monitor_t pub_monitor;
    delivery_ready_monitor_t sub_a_monitor;
    delivery_ready_monitor_t sub_b_monitor;
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (pub, ZLINK_EVENT_CONNECTION_READY, &pub_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (sub_a, ZLINK_EVENT_CONNECTION_READY, &sub_a_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (sub_b, ZLINK_EVENT_CONNECTION_READY, &sub_b_monitor));

    char endpoint[MAX_SOCKET_STRING];
    test_bind (pub, "tcp://127.0.0.1:*", endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub_a, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub_b, endpoint));

    TEST_ASSERT_TRUE (wait_delivery_ready (&pub_monitor, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready (&sub_a_monitor, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready (&sub_b_monitor, 5000));

    for (size_t i = 0; i < 8; ++i) {
        const std::string payload = make_fixed_size_payload ('W', i, 64);
        publish_payload (pub, payload);
        recv_subscribe_expect_topic_and_payload (sub_a, payload);
        recv_subscribe_expect_topic_and_payload (sub_b, payload);
    }

    for (size_t i = 0; i < 4; ++i) {
        const std::string payload = make_fixed_size_payload ('D', i, 64);
        publish_payload (pub, payload);
        recv_subscribe_expect_topic_and_payload (sub_a, payload);
        recv_subscribe_expect_topic_and_payload (sub_b, payload);
    }

    for (size_t i = 0; i < 8; ++i) {
        const std::string payload = make_fixed_size_payload ('A', i, 64);
        publish_payload (pub, payload);
        recv_subscribe_expect_topic_and_payload (sub_a, payload);
        recv_subscribe_expect_topic_and_payload (sub_b, payload);
    }

    close_delivery_ready_monitor (&sub_b_monitor);
    close_delivery_ready_monitor (&sub_a_monitor);
    close_delivery_ready_monitor (&pub_monitor);
    test_context_socket_close_zero_linger (sub_b);
    test_context_socket_close_zero_linger (sub_a);
    test_context_socket_close_zero_linger (pub);
}

void test_pubsub_subscribe_dontwait_preserves_perf_contract_during_burst ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub_a = test_context_socket (ZLINK_SOCKET_SUB);
    void *sub_b = test_context_socket (ZLINK_SOCKET_SUB);

    set_timeout_opts (pub);
    set_timeout_opts (sub_a);
    set_timeout_opts (sub_b);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub_a, ""));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub_b, ""));

    delivery_ready_monitor_t pub_monitor;
    delivery_ready_monitor_t sub_a_monitor;
    delivery_ready_monitor_t sub_b_monitor;
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (pub, ZLINK_EVENT_CONNECTION_READY, &pub_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (sub_a, ZLINK_EVENT_CONNECTION_READY, &sub_a_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (sub_b, ZLINK_EVENT_CONNECTION_READY, &sub_b_monitor));

    char endpoint[MAX_SOCKET_STRING];
    test_bind (pub, "tcp://127.0.0.1:*", endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub_a, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub_b, endpoint));

    TEST_ASSERT_TRUE (wait_delivery_ready (&pub_monitor, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready (&sub_a_monitor, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready (&sub_b_monitor, 5000));

    std::vector<std::string> payloads;
    payloads.reserve (64);
    for (size_t i = 0; i < 64; ++i) {
        payloads.push_back (make_fixed_size_payload ('W', i, 64));
        publish_payload (pub, payloads.back ());
    }

    for (size_t i = 0; i < payloads.size (); ++i) {
        recv_subscribe_expect_topic_and_payload_eventually (sub_a, payloads[i], 5000);
        recv_subscribe_expect_topic_and_payload_eventually (sub_b, payloads[i], 5000);
    }

    close_delivery_ready_monitor (&sub_b_monitor);
    close_delivery_ready_monitor (&sub_a_monitor);
    close_delivery_ready_monitor (&pub_monitor);
    test_context_socket_close_zero_linger (sub_b);
    test_context_socket_close_zero_linger (sub_a);
    test_context_socket_close_zero_linger (pub);
}

void test_pubsub_repeated_topic_stops_delivery_after_unsubscribe ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub_a = test_context_socket (ZLINK_SOCKET_SUB);
    void *sub_b = test_context_socket (ZLINK_SOCKET_SUB);

    set_timeout_opts (pub);
    set_timeout_opts (sub_a);
    set_timeout_opts (sub_b);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub_a, k_pubsub_topic));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub_b, k_pubsub_topic));

    delivery_ready_monitor_t pub_monitor;
    delivery_ready_monitor_t sub_a_monitor;
    delivery_ready_monitor_t sub_b_monitor;
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (pub, ZLINK_EVENT_CONNECTION_READY, &pub_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (sub_a, ZLINK_EVENT_CONNECTION_READY, &sub_a_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (sub_b, ZLINK_EVENT_CONNECTION_READY, &sub_b_monitor));

    char endpoint[MAX_SOCKET_STRING];
    test_bind (pub, "tcp://127.0.0.1:*", endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub_a, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub_b, endpoint));

    TEST_ASSERT_TRUE (wait_delivery_ready_count (&pub_monitor, 2, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready_count (&sub_a_monitor, 1, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready_count (&sub_b_monitor, 1, 5000));

    for (size_t i = 0; i < 4; ++i) {
        const std::string payload = make_fixed_size_payload ('W', i, 64);
        publish_payload (pub, payload);
        recv_subscribe_expect_topic_and_payload (sub_a, payload);
        recv_subscribe_expect_topic_and_payload (sub_b, payload);
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (sub_b, k_pubsub_topic));
    for (size_t i = 0; i < 8; ++i) {
        const std::string payload = make_fixed_size_payload ('A', i, 64);
        publish_payload (pub, payload);
        recv_subscribe_expect_topic_and_payload (sub_a, payload);

        char topic[32];
        memset (topic, 0, sizeof (topic));
        size_t topic_len = sizeof (topic);
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t rc =
          zlink_subscribe (sub_b, NULL, &parts, &part_count, topic, &topic_len, ZLINK_DONTWAIT);
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    }

    close_delivery_ready_monitor (&sub_b_monitor);
    close_delivery_ready_monitor (&sub_a_monitor);
    close_delivery_ready_monitor (&pub_monitor);
    test_context_socket_close_zero_linger (sub_b);
    test_context_socket_close_zero_linger (sub_a);
    test_context_socket_close_zero_linger (pub);
}

void test_pubsub_repeated_topic_keeps_delivering_after_peer_disconnect ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub_a = test_context_socket (ZLINK_SOCKET_SUB);
    void *sub_b = test_context_socket (ZLINK_SOCKET_SUB);

    set_timeout_opts (pub);
    set_timeout_opts (sub_a);
    set_timeout_opts (sub_b);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub_a, k_pubsub_topic));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub_b, k_pubsub_topic));

    delivery_ready_monitor_t pub_monitor;
    delivery_ready_monitor_t sub_a_monitor;
    delivery_ready_monitor_t sub_b_monitor;
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (pub, ZLINK_EVENT_CONNECTION_READY, &pub_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (sub_a, ZLINK_EVENT_CONNECTION_READY, &sub_a_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (sub_b, ZLINK_EVENT_CONNECTION_READY, &sub_b_monitor));

    char endpoint[MAX_SOCKET_STRING];
    test_bind (pub, "tcp://127.0.0.1:*", endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub_a, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub_b, endpoint));

    TEST_ASSERT_TRUE (wait_delivery_ready_count (&pub_monitor, 2, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready_count (&sub_a_monitor, 1, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready_count (&sub_b_monitor, 1, 5000));

    for (size_t i = 0; i < 4; ++i) {
        const std::string payload = make_fixed_size_payload ('W', i, 64);
        publish_payload (pub, payload);
        recv_subscribe_expect_topic_and_payload (sub_a, payload);
        recv_subscribe_expect_topic_and_payload (sub_b, payload);
    }

    close_delivery_ready_monitor (&sub_b_monitor);
    test_context_socket_close_zero_linger (sub_b);
    for (size_t i = 0; i < 8; ++i) {
        const std::string payload = make_fixed_size_payload ('A', i, 64);
        publish_payload (pub, payload);
        recv_subscribe_expect_topic_and_payload (sub_a, payload);
    }

    close_delivery_ready_monitor (&sub_a_monitor);
    close_delivery_ready_monitor (&pub_monitor);
    test_context_socket_close_zero_linger (sub_a);
    test_context_socket_close_zero_linger (pub);
}

void test_pubsub_subscribe_can_skip_topic_copy_and_keep_multipart_payload ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);

    set_timeout_opts (pub);
    set_timeout_opts (sub);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, k_pubsub_topic));

    delivery_ready_monitor_t pub_monitor;
    delivery_ready_monitor_t sub_monitor;
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (pub, ZLINK_EVENT_CONNECTION_READY, &pub_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (sub, ZLINK_EVENT_CONNECTION_READY, &sub_monitor));

    char endpoint[MAX_SOCKET_STRING];
    test_bind (pub, "tcp://127.0.0.1:*", endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));

    TEST_ASSERT_TRUE (wait_delivery_ready_count (&pub_monitor, 1, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready_count (&sub_monitor, 1, 5000));

    const std::string single = make_fixed_size_payload ('S', 1, 64);
    publish_payload (pub, single);
    recv_subscribe_expect_payload_without_topic_copy (sub, single);

    const std::string part_a = make_fixed_size_payload ('A', 2, 32);
    const std::string part_b = make_fixed_size_payload ('B', 3, 48);
    publish_two_part_payload (pub, part_a, part_b);
    recv_subscribe_expect_payload_parts_without_topic_copy (sub, part_a, part_b);

    close_delivery_ready_monitor (&sub_monitor);
    close_delivery_ready_monitor (&pub_monitor);
    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

void test_pubsub_publish_is_safe_from_multiple_threads ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);

    set_timeout_opts (pub);
    set_timeout_opts (sub);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, k_pubsub_topic));

    delivery_ready_monitor_t pub_monitor;
    delivery_ready_monitor_t sub_monitor;
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (pub, ZLINK_EVENT_CONNECTION_READY, &pub_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (sub, ZLINK_EVENT_CONNECTION_READY, &sub_monitor));

    char endpoint[MAX_SOCKET_STRING];
    test_bind (pub, "tcp://127.0.0.1:*", endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));

    TEST_ASSERT_TRUE (wait_delivery_ready_count (&pub_monitor, 1, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready_count (&sub_monitor, 1, 5000));

    publish_start_gate_t gate;
    const int per_publisher = 64;
    publisher_probe_t publisher_a;
    publisher_a.gate = &gate;
    publisher_a.socket = pub;
    publisher_a.phase = 'A';
    publisher_a.count = per_publisher;

    publisher_probe_t publisher_b;
    publisher_b.gate = &gate;
    publisher_b.socket = pub;
    publisher_b.phase = 'B';
    publisher_b.count = per_publisher;

    std::thread sender_a (wait_and_publish_payloads, &publisher_a);
    std::thread sender_b (wait_and_publish_payloads, &publisher_b);

    {
        std::unique_lock<std::mutex> lock (gate.sync);
        const bool ready = gate.cv.wait_for (lock, std::chrono::milliseconds (5000), [&] () {
            return gate.ready.load (std::memory_order_acquire) == 2;
        });
        TEST_ASSERT_TRUE (ready);
        gate.go = true;
    }
    gate.cv.notify_all ();

    std::vector<bool> seen_a (per_publisher, false);
    std::vector<bool> seen_b (per_publisher, false);

    for (int i = 0; i < per_publisher * 2; ++i) {
        char topic[32];
        memset (topic, 0, sizeof (topic));
        size_t topic_len = sizeof (topic);
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_subscribe (sub, NULL, &parts, &part_count, topic, &topic_len, 0));
        TEST_ASSERT_EQUAL_UINT64 (std::strlen (k_pubsub_topic), topic_len);
        TEST_ASSERT_EQUAL_MEMORY (k_pubsub_topic, topic, topic_len);
        TEST_ASSERT_EQUAL_UINT64 (1, part_count);

        const char *data = static_cast<const char *> (zlink_msg_data (&parts[0]));
        const std::string payload (data, zlink_msg_size (&parts[0]));
        char phase = '?';
        int seq = -1;
        const bool parsed = parse_concurrent_publish_payload (payload, &phase, &seq);
        zlink_multipart_close (parts, part_count);
        TEST_ASSERT_TRUE (parsed);
        TEST_ASSERT_TRUE (seq >= 0);
        TEST_ASSERT_TRUE (seq < per_publisher);

        std::vector<bool> *seen = NULL;
        if (phase == 'A')
            seen = &seen_a;
        else if (phase == 'B')
            seen = &seen_b;
        TEST_ASSERT_NOT_NULL (seen);
        TEST_ASSERT_FALSE ((*seen)[static_cast<size_t> (seq)]);
        (*seen)[static_cast<size_t> (seq)] = true;
    }

    sender_a.join ();
    sender_b.join ();

    TEST_ASSERT_FALSE (publisher_a.failed.load (std::memory_order_acquire));
    TEST_ASSERT_FALSE (publisher_b.failed.load (std::memory_order_acquire));
    for (int i = 0; i < per_publisher; ++i) {
        TEST_ASSERT_TRUE (seen_a[static_cast<size_t> (i)]);
        TEST_ASSERT_TRUE (seen_b[static_cast<size_t> (i)]);
    }

    close_delivery_ready_monitor (&sub_monitor);
    close_delivery_ready_monitor (&pub_monitor);
    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

void test_pubsub_publish_rollback_preserves_next_topic_boundary ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);

    set_timeout_opts (pub);
    set_timeout_opts (sub);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, k_pubsub_topic));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (pub, "inproc://pubsub_publish_eagain_preserves_boundary"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sub, "inproc://pubsub_publish_eagain_preserves_boundary"));
    const uint8_t subscription_frame[] = {1, 'b', 'e', 'n', 'c', 'h'};
    recv_array_expect_success (pub, subscription_frame, sizeof (subscription_frame));

    zlink_msg_t topic_part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&topic_part, std::strlen (k_pubsub_topic)));
    memcpy (zlink_msg_data (&topic_part), k_pubsub_topic, std::strlen (k_pubsub_topic));
    TEST_ASSERT_EQUAL_INT (static_cast<int> (std::strlen (k_pubsub_topic)),
                           test_send_single_msg (&topic_part, pub, ZLINK_SNDMORE));

    zlink::socket_base_t *pub_socket = static_cast<zlink::socket_base_t *> (pub);
    TEST_ASSERT_SUCCESS_ERRNO (pub_socket->rollback ());

    char topic[32];
    memset (topic, 0, sizeof (topic));
    size_t topic_len = sizeof (topic);
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_subscribe (sub, NULL, &parts, &part_count,
                                                                topic, &topic_len, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    const std::string recovered_payload = make_fixed_size_payload ('W', 1, 64);
    publish_payload (pub, recovered_payload);
    recv_subscribe_expect_topic_and_payload (sub, recovered_payload);
    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_router_recv_with_source_rid_strips_routing_envelope_from_dealer);
    RUN_TEST (test_dealer_recv_with_source_rid_hides_peer_routing_id);
    RUN_TEST (test_router_recv_dontwait_no_data_does_not_break_poller_rearm);
    RUN_TEST (test_pubsub_callback_is_supported_on_raw_sub_sockets);
    RUN_TEST (test_pubsub_subscribe_preserves_topic_and_payload_shape_across_warmup);
    RUN_TEST (test_pubsub_subscribe_dontwait_preserves_perf_contract_during_burst);
    RUN_TEST (test_pubsub_repeated_topic_stops_delivery_after_unsubscribe);
    RUN_TEST (test_pubsub_repeated_topic_keeps_delivering_after_peer_disconnect);
    RUN_TEST (test_pubsub_subscribe_can_skip_topic_copy_and_keep_multipart_payload);
    RUN_TEST (test_pubsub_publish_is_safe_from_multiple_threads);
    RUN_TEST (test_pubsub_publish_rollback_preserves_next_topic_boundary);
    return UNITY_END ();
}
