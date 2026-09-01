/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"

#include "../../../src/runtime/engine/asio/asio_zmp_engine.hpp"
#include "../../../src/runtime/sockets/common/socket_base.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
struct raw_delivery_probe_t
{
    raw_delivery_probe_t () : calls (0), part_count (0), close_failures (0), rid_size (0)
    {
        memset (rid, 0, sizeof (rid));
        memset (parts, 0, sizeof (parts));
    }

    std::mutex mutex;
    std::condition_variable cv;
    int calls;
    size_t part_count;
    int close_failures;
    size_t rid_size;
    unsigned char rid[255];
    char parts[4][64];
};

struct passive_ready_write_drain_gate_t
{
    passive_ready_write_drain_gate_t () :
        released (false),
        arrivals (0),
        pair_id (0),
        pair_generation (0),
        identity_consistent (true)
    {
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool released;
    unsigned int arrivals;
    uint64_t pair_id;
    uint64_t pair_generation;
    bool identity_consistent;
};

raw_delivery_probe_t *g_raw_delivery_probe_a = NULL;
raw_delivery_probe_t *g_raw_delivery_probe_b = NULL;
passive_ready_write_drain_gate_t g_passive_ready_write_drain_gate;

zlink::socket_base_t *as_internal_socket (void *socket_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    return handle.socket;
}

void pause_passive_ready_after_write_drain (uint64_t pair_id_,
                                            uint64_t pair_generation_,
                                            void *userdata_)
{
    passive_ready_write_drain_gate_t *gate =
      static_cast<passive_ready_write_drain_gate_t *> (userdata_);
    if (!gate)
        return;

    std::unique_lock<std::mutex> lock (gate->mutex);
    if (gate->arrivals == 0) {
        gate->pair_id = pair_id_;
        gate->pair_generation = pair_generation_;
    } else if (gate->pair_id != pair_id_
               || gate->pair_generation != pair_generation_) {
        gate->identity_consistent = false;
    }
    ++gate->arrivals;
    gate->cv.notify_all ();

    //  A bounded wait keeps a failed assertion or interrupted test from
    //  stranding the engine thread until the process-wide CTest timeout.
    gate->cv.wait_for (lock, std::chrono::seconds (8),
                       [gate] () { return gate->released; });
}

void reset_passive_ready_write_drain_gate ()
{
    std::lock_guard<std::mutex> lock (g_passive_ready_write_drain_gate.mutex);
    g_passive_ready_write_drain_gate.released = false;
    g_passive_ready_write_drain_gate.arrivals = 0;
    g_passive_ready_write_drain_gate.pair_id = 0;
    g_passive_ready_write_drain_gate.pair_generation = 0;
    g_passive_ready_write_drain_gate.identity_consistent = true;
}

bool wait_for_passive_ready_write_drain (int timeout_ms_)
{
    std::unique_lock<std::mutex> lock (g_passive_ready_write_drain_gate.mutex);
    return g_passive_ready_write_drain_gate.cv.wait_for (
      lock, std::chrono::milliseconds (timeout_ms_), [] () {
          return g_passive_ready_write_drain_gate.arrivals > 0;
      });
}

void release_passive_ready_write_drain_gate ()
{
    {
        std::lock_guard<std::mutex> lock (
          g_passive_ready_write_drain_gate.mutex);
        g_passive_ready_write_drain_gate.released = true;
    }
    g_passive_ready_write_drain_gate.cv.notify_all ();
}

void close_raw_delivery_parts (raw_delivery_probe_t *probe_,
                               zlink_msg_t *parts_,
                               size_t part_count_)
{
    for (size_t i = 0; i < part_count_; ++i) {
        const int rc = zlink_msg_close (&parts_[i]);
        if (rc != 0 && probe_) {
            std::unique_lock<std::mutex> lock (probe_->mutex);
            ++probe_->close_failures;
        }
    }
}

void capture_raw_delivery_into (raw_delivery_probe_t *probe_,
                                const zlink_routing_id_t *source_rid_,
                                zlink_msg_t *parts_,
                                size_t part_count_,
                                void *)
{
    if (!probe_) {
        close_raw_delivery_parts (NULL, parts_, part_count_);
        return;
    }

    {
        std::unique_lock<std::mutex> lock (probe_->mutex);
        probe_->part_count = part_count_;
        probe_->rid_size = source_rid_ ? source_rid_->size : 0;
        if (source_rid_ && source_rid_->size > 0) {
            memcpy (probe_->rid, source_rid_->data, source_rid_->size);
        }

        const size_t copy_count = std::min (part_count_, size_t (4));
        for (size_t i = 0; i < copy_count; ++i) {
            const size_t size = zlink_msg_size (&parts_[i]);
            const size_t copy_size = std::min (size, sizeof (probe_->parts[i]) - 1);
            if (copy_size > 0) {
                memcpy (probe_->parts[i], zlink_msg_data (&parts_[i]), copy_size);
            }
            probe_->parts[i][copy_size] = '\0';
        }

        ++probe_->calls;
    }

    close_raw_delivery_parts (probe_, parts_, part_count_);
    probe_->cv.notify_all ();
}

void capture_raw_delivery_a (const zlink_routing_id_t *source_rid_,
                             zlink_msg_t *parts_,
                             size_t part_count_,
                             void *)
{
    capture_raw_delivery_into (g_raw_delivery_probe_a, source_rid_, parts_, part_count_, NULL);
}

void capture_raw_delivery_b (const zlink_routing_id_t *source_rid_,
                             zlink_msg_t *parts_,
                             size_t part_count_,
                             void *)
{
    capture_raw_delivery_into (g_raw_delivery_probe_b, source_rid_, parts_, part_count_, NULL);
}

bool wait_for_probe_calls (raw_delivery_probe_t *probe_, int expected_calls_, int timeout_ms_)
{
    if (!probe_)
        return false;

    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->cv.wait_for (
      lock, std::chrono::milliseconds (timeout_ms_),
      [probe_, expected_calls_] () { return probe_->calls >= expected_calls_; });
}
}

static void assert_auto_routing_id (void *socket_)
{
    zlink_routing_id_t routing_id;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_routing_id (socket_, &routing_id));
    TEST_ASSERT_EQUAL_UINT (16, routing_id.size);
}

static bool wait_for_event (void *monitor_, uint64_t expected_event_, zlink_monitor_event_t *out_)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
        const int poll_rc = zlink_poll (&item, 1, 200, NULL);
        if (poll_rc <= 0 || (item.revents & ZLINK_POLLIN) == 0)
            continue;

        zlink_monitor_event_t ev;
        while (recv_monitor_event_from_socket (monitor_, &ev, ZLINK_DONTWAIT) == 0) {
            if (ev.event == expected_event_) {
                if (out_)
                    *out_ = ev;
                return true;
            }
        }
    }
    return false;
}

static void set_bounded_socket_timeouts (void *socket_, int timeout_ms_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &timeout_ms_, sizeof (timeout_ms_)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &timeout_ms_, sizeof (timeout_ms_)));
}

static void set_zero_linger (void *socket_)
{
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
}

static void *open_raw_monitor (void *socket_, uint64_t events_)
{
    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = events_;
    void *monitor = zlink_socket_monitor_open (socket_, &opts);
    TEST_ASSERT_NOT_NULL (monitor);
    set_zero_linger (monitor);
    return monitor;
}

static void subscribe_all_if_needed (void *socket_, int type_)
{
    if (type_ != ZLINK_SOCKET_SUB)
        return;
    const char *all_topics = "";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (socket_, all_topics));
}

struct monitor_sequence_probe_t
{
    monitor_sequence_probe_t () :
        accepted_seen (false),
        ready_seen (false),
        disconnected_seen (false),
        ready_before_accepted (false),
        disconnected_before_ready (false)
    {
        memset (&accepted, 0, sizeof (accepted));
        memset (&ready, 0, sizeof (ready));
        memset (&disconnected, 0, sizeof (disconnected));
    }

    bool accepted_seen;
    bool ready_seen;
    bool disconnected_seen;
    bool ready_before_accepted;
    bool disconnected_before_ready;
    zlink_monitor_event_t accepted;
    zlink_monitor_event_t ready;
    zlink_monitor_event_t disconnected;
};

static bool routing_id_equal (const zlink_routing_id_t *lhs_, const zlink_routing_id_t *rhs_)
{
    if (!lhs_ || !rhs_)
        return false;
    if (lhs_->size != rhs_->size)
        return false;
    if (lhs_->size == 0)
        return true;
    return memcmp (lhs_->data, rhs_->data, lhs_->size) == 0;
}

static void close_local_socket_zero_linger (void *socket_)
{
    if (!socket_)
        return;
    set_zero_linger (socket_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_));
}

static void
collect_sequence_events (void *monitor_, monitor_sequence_probe_t *probe_, int poll_timeout_ms_)
{
    if (!monitor_ || !probe_)
        return;

    zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
    const int poll_rc = zlink_poll (&item, 1, poll_timeout_ms_, NULL);
    if (poll_rc <= 0 || (item.revents & ZLINK_POLLIN) == 0)
        return;

    for (;;) {
        zlink_monitor_event_t ev;
        if (recv_monitor_event_from_socket (monitor_, &ev, ZLINK_DONTWAIT) != 0)
            break;

        if (ev.event == ZLINK_EVENT_ACCEPTED) {
            if (!probe_->accepted_seen) {
                probe_->accepted = ev;
                probe_->accepted_seen = true;
            }
            continue;
        }

        if (ev.event == ZLINK_EVENT_CONNECTION_READY) {
            if (!probe_->accepted_seen)
                probe_->ready_before_accepted = true;
            if (!probe_->ready_seen) {
                probe_->ready = ev;
                probe_->ready_seen = true;
            }
            continue;
        }

        if (ev.event == ZLINK_EVENT_DISCONNECTED) {
            if (!probe_->ready_seen)
                probe_->disconnected_before_ready = true;
            if (!probe_->disconnected_seen) {
                probe_->disconnected = ev;
                probe_->disconnected_seen = true;
            }
        }
    }
}

static bool wait_for_sequence (void *monitor_,
                               monitor_sequence_probe_t *probe_,
                               bool require_disconnected_,
                               int timeout_ms_)
{
    const int slice_ms = 20;
    const int loops = timeout_ms_ > 0 ? timeout_ms_ / slice_ms + 1 : 1;

    for (int i = 0; i < loops; ++i) {
        collect_sequence_events (monitor_, probe_, slice_ms);
        if (probe_->accepted_seen && probe_->ready_seen
            && (!require_disconnected_ || probe_->disconnected_seen))
            return true;
    }

    collect_sequence_events (monitor_, probe_, 0);
    return probe_->accepted_seen && probe_->ready_seen
           && (!require_disconnected_ || probe_->disconnected_seen);
}

static void run_client_monitor_ready_disconnected_test (int client_type_, int server_type_)
{
    void *server = test_context_socket (server_type_);
    void *client = test_context_socket (client_type_);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    subscribe_all_if_needed (server, server_type_);
    subscribe_all_if_needed (client, client_type_);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);

    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
    void *mon = zlink_socket_monitor_open (client, &opts);
    TEST_ASSERT_NOT_NULL (mon);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (mon, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    zlink_monitor_event_t ready;
    TEST_ASSERT_TRUE (wait_for_event (mon, ZLINK_EVENT_CONNECTION_READY, &ready));
    TEST_ASSERT_TRUE (ready.remote_addr[0] != '\0' || ready.local_addr[0] != '\0');

    test_context_socket_close_zero_linger (server);

    zlink_monitor_event_t disconnected;
    TEST_ASSERT_TRUE (wait_for_event (mon, ZLINK_EVENT_DISCONNECTED, &disconnected));
    TEST_ASSERT_TRUE (disconnected.remote_addr[0] != '\0' || disconnected.local_addr[0] != '\0');
    if (ready.routing_id.size > 0 && disconnected.routing_id.size > 0) {
        TEST_ASSERT_TRUE (routing_id_equal (&ready.routing_id, &disconnected.routing_id));
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&mon));
    test_context_socket_close_zero_linger (client);
}

void test_auto_routing_id_generation ()
{
    const int types[] = {ZLINK_SOCKET_PAIR,   ZLINK_SOCKET_PUB,    ZLINK_SOCKET_SUB,
                         ZLINK_SOCKET_DEALER, ZLINK_SOCKET_ROUTER, ZLINK_SOCKET_XPUB,
                         ZLINK_SOCKET_XSUB,   ZLINK_SOCKET_STREAM};

    for (size_t i = 0; i < sizeof (types) / sizeof (types[0]); ++i) {
        void *sock = test_context_socket (types[i]);
        TEST_ASSERT_NOT_NULL (sock);
        assert_auto_routing_id (sock);
        test_context_socket_close (sock);
    }
}

void test_monitor_open_and_connection_ready ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);

    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
    void *mon = zlink_socket_monitor_open (server, &opts);
    TEST_ASSERT_NOT_NULL (mon);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    zlink_monitor_event_t ev;
    TEST_ASSERT_TRUE (wait_for_event (mon, ZLINK_EVENT_CONNECTION_READY, &ev));
    TEST_ASSERT_TRUE (ev.routing_id.size > 0);
    TEST_ASSERT_TRUE (ev.remote_addr[0] != '\0' || ev.local_addr[0] != '\0');

    test_context_socket_close_zero_linger (client);

    TEST_ASSERT_TRUE (wait_for_event (mon, ZLINK_EVENT_DISCONNECTED, NULL));

    int linger = 0;
    zlink_set_option (mon, ZLINK_OPT_LINGER, &linger, sizeof (linger));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&mon));
    test_context_socket_close_zero_linger (server);
}

void test_passive_paired_ready_waits_for_ready_reply_write_drain ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    set_zero_linger (server);
    set_zero_linger (client);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);
    void *monitor = open_raw_monitor (
      server, ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED);

    zlink::socket_base_t *const internal_server = as_internal_socket (server);
    TEST_ASSERT_NOT_NULL (internal_server);

    reset_passive_ready_write_drain_gate ();
    zlink::test_set_zmp_passive_ready_write_drained_hook (
      &pause_passive_ready_after_write_drain,
      &g_passive_ready_write_drain_gate);

    //  Do not assert while the hook can own an I/O thread. Every exit below
    //  first releases and removes the gate, then reports the captured result.
    const int connect_rc = zlink_connect (client, endpoint);
    const bool drain_reached =
      connect_rc == 0 && wait_for_passive_ready_write_drain (5000);

    uint64_t pair_id = 0;
    uint64_t pair_generation = 0;
    bool identity_consistent = false;
    {
        std::lock_guard<std::mutex> lock (
          g_passive_ready_write_drain_gate.mutex);
        pair_id = g_passive_ready_write_drain_gate.pair_id;
        pair_generation =
          g_passive_ready_write_drain_gate.pair_generation;
    }

    const uint32_t ready_count_before =
      internal_server->test_monitor_ready_count ();
    const bool pair_ready_before =
      pair_id != 0 && pair_generation != 0
      && internal_server->test_pair_is_ready (pair_id, pair_generation);

    release_passive_ready_write_drain_gate ();

    zlink_monitor_event_t ready_event;
    memset (&ready_event, 0, sizeof (ready_event));
    const bool ready_seen =
      connect_rc == 0
      && wait_for_event (monitor, ZLINK_EVENT_CONNECTION_READY,
                         &ready_event);
    const uint32_t ready_count_after =
      internal_server->test_monitor_ready_count ();
    const bool pair_ready_after =
      pair_id != 0 && pair_generation != 0
      && internal_server->test_pair_is_ready (pair_id, pair_generation);

    unsigned int drain_arrivals = 0;
    {
        std::lock_guard<std::mutex> lock (
          g_passive_ready_write_drain_gate.mutex);
        drain_arrivals = g_passive_ready_write_drain_gate.arrivals;
        identity_consistent =
          g_passive_ready_write_drain_gate.identity_consistent;
    }
    zlink::test_set_zmp_passive_ready_write_drained_hook (NULL, NULL);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);

    TEST_ASSERT_EQUAL_INT (0, connect_rc);
    TEST_ASSERT_TRUE (drain_reached);
    TEST_ASSERT_TRUE (pair_id != 0);
    TEST_ASSERT_TRUE (pair_generation != 0);
    TEST_ASSERT_EQUAL_UINT (2, drain_arrivals);
    TEST_ASSERT_TRUE (identity_consistent);
    TEST_ASSERT_EQUAL_UINT32 (0, ready_count_before);
    TEST_ASSERT_FALSE (pair_ready_before);

    TEST_ASSERT_TRUE (ready_seen);
    TEST_ASSERT_EQUAL_UINT32 (1, ready_count_after);
    TEST_ASSERT_TRUE (pair_ready_after);
    TEST_ASSERT_EQUAL_UINT64 (1, ready_event.value);
    TEST_ASSERT_TRUE (ready_event.connection_id != 0);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            ready_event.transport_lane);
    TEST_ASSERT_TRUE (
      (ready_event.flags & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE)
      != 0);
}

void test_pair_monitor_ready_implies_first_bidirectional_delivery ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *server = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    set_zero_linger (server);
    set_zero_linger (client);
    set_bounded_socket_timeouts (server, 200);
    set_bounded_socket_timeouts (client, 200);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);

    void *server_monitor = open_raw_monitor (
      server, ZLINK_EVENT_ACCEPTED | ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED);
    void *client_monitor =
      open_raw_monitor (client, ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    monitor_sequence_probe_t server_monitor_probe;
    TEST_ASSERT_TRUE (wait_for_sequence (server_monitor, &server_monitor_probe, false, 5000));
    TEST_ASSERT_FALSE (server_monitor_probe.ready_before_accepted);
    TEST_ASSERT_TRUE (server_monitor_probe.accepted_seen);
    TEST_ASSERT_TRUE (server_monitor_probe.ready_seen);

    zlink_monitor_event_t client_ready;
    memset (&client_ready, 0, sizeof (client_ready));
    TEST_ASSERT_TRUE (wait_for_event (client_monitor, ZLINK_EVENT_CONNECTION_READY, &client_ready));

    send_string_expect_success (client, "pair-hello", 0);
    recv_string_expect_success (server, "pair-hello", 0);

    send_string_expect_success (server, "pair-ack", 0);
    recv_string_expect_success (client, "pair-ack", 0);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&client_monitor));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&server_monitor));
    close_local_socket_zero_linger (client);
    close_local_socket_zero_linger (server);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_dealer_router_monitor_ready_implies_first_bidirectional_delivery ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *server = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    const char dealer_id[] = "MONREG1";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, dealer_id, sizeof (dealer_id) - 1));

    set_zero_linger (server);
    set_zero_linger (client);
    set_bounded_socket_timeouts (server, 200);
    set_bounded_socket_timeouts (client, 200);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);

    void *server_monitor = open_raw_monitor (
      server, ZLINK_EVENT_ACCEPTED | ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED);
    void *client_monitor =
      open_raw_monitor (client, ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    monitor_sequence_probe_t server_monitor_probe;
    TEST_ASSERT_TRUE (wait_for_sequence (server_monitor, &server_monitor_probe, false, 5000));
    TEST_ASSERT_FALSE (server_monitor_probe.ready_before_accepted);
    TEST_ASSERT_TRUE (server_monitor_probe.accepted_seen);
    TEST_ASSERT_TRUE (server_monitor_probe.ready_seen);
    TEST_ASSERT_TRUE (server_monitor_probe.ready.routing_id.size > 0);

    zlink_monitor_event_t client_ready;
    memset (&client_ready, 0, sizeof (client_ready));
    TEST_ASSERT_TRUE (wait_for_event (client_monitor, ZLINK_EVENT_CONNECTION_READY, &client_ready));

    send_string_expect_success (client, "dealer-msg", 0);
    unsigned char rid_buf[255];
    const int rid_size =
      TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (server, rid_buf, sizeof (rid_buf), 0));
    TEST_ASSERT_EQUAL_INT (server_monitor_probe.ready.routing_id.size, rid_size);
    TEST_ASSERT_EQUAL_MEMORY (server_monitor_probe.ready.routing_id.data, rid_buf, rid_size);
    recv_string_expect_success (server, "dealer-msg", 0);

    TEST_ASSERT_EQUAL_INT (server_monitor_probe.ready.routing_id.size,
                           TEST_ASSERT_SUCCESS_ERRNO (zlink_send (
                             server, server_monitor_probe.ready.routing_id.data,
                             server_monitor_probe.ready.routing_id.size, ZLINK_SNDMORE)));
    send_string_expect_success (server, "router-reply", 0);
    recv_string_expect_success (client, "router-reply", 0);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&client_monitor));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&server_monitor));
    close_local_socket_zero_linger (client);
    close_local_socket_zero_linger (server);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_peer_enumeration ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *server = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);

    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *mon = zlink_socket_monitor_open (server, &opts);
    TEST_ASSERT_NOT_NULL (mon);

    zlink_monitor_event_t ready;
    memset (&ready, 0, sizeof (ready));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
    TEST_ASSERT_TRUE (wait_for_event (mon, ZLINK_EVENT_CONNECTION_READY, &ready));

    zlink_monitor_status_t status;
    memset (&status, 0, sizeof (status));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_status (mon, &status));
    TEST_ASSERT_TRUE (ready.routing_id.size > 0);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_SOURCE_SOCKET, status.source_kind);
    TEST_ASSERT_TRUE ((status.state_flags & ZLINK_MONITOR_STATE_READY) != 0);

    int linger = 0;
    zlink_set_option (mon, ZLINK_OPT_LINGER, &linger, sizeof (linger));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&mon));
    close_local_socket_zero_linger (client);
    close_local_socket_zero_linger (server);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_router_monitor_event_sequence_timing ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);

    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = ZLINK_EVENT_ACCEPTED | ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
    void *mon = zlink_socket_monitor_open (server, &opts);
    TEST_ASSERT_NOT_NULL (mon);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (mon, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    monitor_sequence_probe_t probe;
    TEST_ASSERT_TRUE (wait_for_sequence (mon, &probe, false, 5000));
    TEST_ASSERT_FALSE (probe.ready_before_accepted);
    TEST_ASSERT_TRUE (probe.accepted_seen);
    TEST_ASSERT_EQUAL_UINT (0, probe.accepted.routing_id.size);
    TEST_ASSERT_TRUE (probe.ready_seen);
    TEST_ASSERT_TRUE (probe.ready.routing_id.size > 0);

    test_context_socket_close_zero_linger (client);

    TEST_ASSERT_TRUE (wait_for_sequence (mon, &probe, true, 5000));
    TEST_ASSERT_FALSE (probe.disconnected_before_ready);
    if (probe.disconnected.routing_id.size > 0) {
        TEST_ASSERT_TRUE (
          routing_id_equal (&probe.ready.routing_id, &probe.disconnected.routing_id));
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&mon));
    test_context_socket_close_zero_linger (server);
}

void test_dealer_monitor_ready_and_disconnected ()
{
    run_client_monitor_ready_disconnected_test (ZLINK_SOCKET_DEALER, ZLINK_SOCKET_ROUTER);
}

void test_pub_monitor_ready_and_disconnected ()
{
    run_client_monitor_ready_disconnected_test (ZLINK_SOCKET_PUB, ZLINK_SOCKET_SUB);
}

void test_sub_monitor_ready_and_disconnected ()
{
    run_client_monitor_ready_disconnected_test (ZLINK_SOCKET_SUB, ZLINK_SOCKET_PUB);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_auto_routing_id_generation);
    RUN_TEST (test_monitor_open_and_connection_ready);
    RUN_TEST (test_passive_paired_ready_waits_for_ready_reply_write_drain);
    RUN_TEST (test_pair_monitor_ready_implies_first_bidirectional_delivery);
    RUN_TEST (test_dealer_router_monitor_ready_implies_first_bidirectional_delivery);
    RUN_TEST (test_peer_enumeration);
    RUN_TEST (test_router_monitor_event_sequence_timing);
    RUN_TEST (test_dealer_monitor_ready_and_disconnected);
    RUN_TEST (test_pub_monitor_ready_and_disconnected);
    RUN_TEST (test_sub_monitor_ready_and_disconnected);
    return UNITY_END ();
}
