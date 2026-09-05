/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <algorithm>
#include <chrono>
#include <stdio.h>
#include <string.h>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int sample_count = 20;
const int reconnect_ivl_ms = 50;
const int disconnect_limit_ms = 200;
const int reconnect_limit_ms = reconnect_ivl_ms + 200;
const int event_timeout_ms = 3000;

enum transport_t
{
    transport_tcp,
    transport_inproc
};

enum server_break_t
{
    server_close,
    server_disconnect
};

typedef std::chrono::steady_clock monotonic_clock_t;

struct event_observation_t
{
    zlink_monitor_event_t event;
    int elapsed_ms;
};

struct timing_summary_t
{
    int minimum;
    int p50;
    int p95;
    int maximum;
};

void set_int_option (void *socket_, zlink_option_t option_, int value_)
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, option_, &value_, sizeof (value_)));
}

void set_zero_linger (void *socket_)
{
    set_int_option (socket_, ZLINK_OPT_LINGER, 0);
}

void close_socket (void *socket_)
{
    if (!socket_)
        return;
    test_context_socket_close_zero_linger (socket_);
}

void *new_socket (int type_)
{
    void *socket = test_context_socket (type_);
    TEST_ASSERT_NOT_NULL (socket);
    set_zero_linger (socket);
    return socket;
}

void make_inproc_endpoint (char *endpoint_, size_t size_, const char *case_name_)
{
    static unsigned int sequence = 0;
    ++sequence;
    snprintf (endpoint_, size_, "inproc://disconnect-progress-%s-%u",
              case_name_, sequence);
}

void bind_endpoint (void *server_, transport_t transport_, char *endpoint_,
                    size_t endpoint_size_, const char *case_name_)
{
    if (transport_ == transport_tcp) {
        bind_loopback_ipv4 (server_, endpoint_, endpoint_size_);
        return;
    }

    make_inproc_endpoint (endpoint_, endpoint_size_, case_name_);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (server_, endpoint_));
}

void *open_client_monitor (void *client_)
{
    zlink_socket_monitor_open_options_t options;
    memset (&options, 0, sizeof (options));
    options.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
    void *monitor = zlink_socket_monitor_open (client_, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    set_zero_linger (monitor);
    return monitor;
}

bool event_matches (const zlink_monitor_event_t &event_, uint64_t event_type_,
                    uint64_t connection_id_, bool require_ready_edge_)
{
    if (event_.event != event_type_)
        return false;
    if (connection_id_ != 0 && event_.connection_id != connection_id_)
        return false;
    if (require_ready_edge_
        && (event_.flags & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE) == 0)
        return false;
    return true;
}

bool wait_monitor_event (void *monitor_, uint64_t event_type_,
                         uint64_t connection_id_, bool require_ready_edge_,
                         int timeout_ms_, event_observation_t *observation_)
{
    const monotonic_clock_t::time_point started = monotonic_clock_t::now ();
    const monotonic_clock_t::time_point deadline =
      started + std::chrono::milliseconds (timeout_ms_);

    while (monotonic_clock_t::now () < deadline) {
        const long remaining = static_cast<long> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            deadline - monotonic_clock_t::now ())
            .count ());
        zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        const int poll_result = zlink_poll (&item, 1, remaining > 0 ? remaining : 0,
                                            &error);
        if (poll_result < 0 || error != ZLINK_CONFIG_OK)
            return false;
        if (poll_result == 0)
            continue;

        for (;;) {
            zlink_monitor_event_t event;
            memset (&event, 0, sizeof (event));
            const zlink_recv_result_t recv_result = zlink_socket_monitor_recv (
              monitor_, &event, ZLINK_RECV_FLAGS_DONTWAIT);
            if (recv_result == ZLINK_RECV_NO_DATA)
                break;
            if (recv_result != ZLINK_RECV_OK)
                return false;

            if (!event_matches (event, event_type_, connection_id_,
                                require_ready_edge_))
                continue;

            if (observation_) {
                observation_->event = event;
                observation_->elapsed_ms = static_cast<int> (
                  std::chrono::duration_cast<std::chrono::milliseconds> (
                    monotonic_clock_t::now () - started)
                    .count ());
            }
            return true;
        }
    }
    return false;
}

bool wait_replacement_events (void *monitor_, uint64_t old_connection_id_,
                              int timeout_ms_,
                              event_observation_t *disconnected_out_,
                              event_observation_t *ready_out_)
{
    bool disconnected_seen = false;
    bool ready_seen = false;
    const monotonic_clock_t::time_point started = monotonic_clock_t::now ();
    const monotonic_clock_t::time_point deadline =
      started + std::chrono::milliseconds (timeout_ms_);

    while (monotonic_clock_t::now () < deadline
           && (!disconnected_seen || !ready_seen)) {
        const long remaining = static_cast<long> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            deadline - monotonic_clock_t::now ())
            .count ());
        zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        const int poll_result = zlink_poll (&item, 1, remaining > 0 ? remaining : 0,
                                            &error);
        if (poll_result < 0 || error != ZLINK_CONFIG_OK)
            return false;
        if (poll_result == 0)
            continue;

        for (;;) {
            zlink_monitor_event_t event;
            memset (&event, 0, sizeof (event));
            const zlink_recv_result_t recv_result = zlink_socket_monitor_recv (
              monitor_, &event, ZLINK_RECV_FLAGS_DONTWAIT);
            if (recv_result == ZLINK_RECV_NO_DATA)
                break;
            if (recv_result != ZLINK_RECV_OK)
                return false;

            const int elapsed_ms = static_cast<int> (
              std::chrono::duration_cast<std::chrono::milliseconds> (
                monotonic_clock_t::now () - started)
                .count ());
            if (!disconnected_seen
                && event_matches (event, ZLINK_EVENT_DISCONNECTED,
                                  old_connection_id_, false)) {
                disconnected_out_->event = event;
                disconnected_out_->elapsed_ms = elapsed_ms;
                disconnected_seen = true;
            } else if (!ready_seen
                       && event.event == ZLINK_EVENT_CONNECTION_READY
                       && event.connection_id != old_connection_id_
                       && (event.flags
                           & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE)
                            != 0) {
                ready_out_->event = event;
                ready_out_->elapsed_ms = elapsed_ms;
                ready_seen = true;
            }
        }
    }
    return disconnected_seen && ready_seen;
}

timing_summary_t summarize (std::vector<int> values_)
{
    TEST_ASSERT_EQUAL_INT (sample_count, values_.size ());
    std::sort (values_.begin (), values_.end ());
    timing_summary_t summary;
    summary.minimum = values_.front ();
    summary.p50 = values_[(values_.size () - 1) * 50 / 100];
    summary.p95 = values_[(values_.size () - 1) * 95 / 100];
    summary.maximum = values_.back ();
    return summary;
}

void print_summary (const char *transport_name_, const char *poller_name_,
                    const char *edge_name_, const timing_summary_t &summary_)
{
    printf ("TIMING transport=%s poller=%s edge=%s samples=%d "
            "min=%d p50=%d p95=%d max=%d ms\n",
            transport_name_, poller_name_, edge_name_, sample_count,
            summary_.minimum, summary_.p50, summary_.p95, summary_.maximum);
}

void run_disconnect_progress_case (transport_t transport_, bool register_client_,
                                   server_break_t server_break_)
{
    const char *const transport_name =
      transport_ == transport_tcp ? "tcp" : "inproc";
    const char *const poller_name = register_client_ ? "registered" : "absent";
    const char *const break_name =
      server_break_ == server_close ? "close" : "disconnect";

    void *server = new_socket (ZLINK_SOCKET_ROUTER);
    char endpoint[MAX_SOCKET_STRING];
    bind_endpoint (server, transport_, endpoint, sizeof (endpoint), poller_name);

    void *client = new_socket (ZLINK_SOCKET_DEALER);
    set_int_option (client, ZLINK_OPT_RECONNECT_IVL, reconnect_ivl_ms);
    void *monitor = open_client_monitor (client);
    void *idle_poller = NULL;
    if (register_client_) {
        idle_poller = zlink_poller_new ();
        TEST_ASSERT_NOT_NULL (idle_poller);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_add (idle_poller, client, client, ZLINK_POLLIN));
    }

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));
    event_observation_t ready;
    TEST_ASSERT_TRUE (wait_monitor_event (
      monitor, ZLINK_EVENT_CONNECTION_READY, 0, true, event_timeout_ms,
      &ready));
    TEST_ASSERT_NOT_EQUAL (0, ready.event.connection_id);

    std::vector<int> disconnected_samples;
    std::vector<int> ready_samples;
    disconnected_samples.reserve (sample_count);
    ready_samples.reserve (sample_count);

    for (int sample = 0; sample < sample_count; ++sample) {
        const uint64_t old_connection_id = ready.event.connection_id;
        const monotonic_clock_t::time_point disconnect_started =
          monotonic_clock_t::now ();
        if (server_break_ == server_close) {
            close_socket (server);
            server = NULL;
        } else {
            TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                                   zlink_disconnect (server, endpoint));
        }

        event_observation_t disconnected;
        TEST_ASSERT_TRUE (wait_monitor_event (
          monitor, ZLINK_EVENT_DISCONNECTED, old_connection_id, false,
          event_timeout_ms, &disconnected));
        disconnected.elapsed_ms = static_cast<int> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            monotonic_clock_t::now () - disconnect_started)
            .count ());
        disconnected_samples.push_back (disconnected.elapsed_ms);

        if (!server)
            server = new_socket (ZLINK_SOCKET_ROUTER);
        const monotonic_clock_t::time_point bind_started =
          monotonic_clock_t::now ();
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (server, endpoint));

        TEST_ASSERT_TRUE (wait_monitor_event (
          monitor, ZLINK_EVENT_CONNECTION_READY, 0, true, event_timeout_ms,
          &ready));
        ready.elapsed_ms = static_cast<int> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            monotonic_clock_t::now () - bind_started)
            .count ());
        TEST_ASSERT_NOT_EQUAL (old_connection_id, ready.event.connection_id);
        ready_samples.push_back (ready.elapsed_ms);
    }

    const timing_summary_t disconnected_summary =
      summarize (disconnected_samples);
    const timing_summary_t ready_summary = summarize (ready_samples);
    char case_name[64];
    snprintf (case_name, sizeof (case_name), "%s-%s", poller_name,
              break_name);
    print_summary (transport_name, case_name, "disconnected",
                   disconnected_summary);
    print_summary (transport_name, case_name, "reconnect-ready",
                   ready_summary);
    TEST_ASSERT_LESS_OR_EQUAL_INT (disconnect_limit_ms,
                                   disconnected_summary.p95);
    TEST_ASSERT_LESS_OR_EQUAL_INT (reconnect_limit_ms, ready_summary.p95);

    if (idle_poller) {
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_poller_remove (idle_poller, client));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                               zlink_poller_destroy (&idle_poller));
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor));
    close_socket (client);
    close_socket (server);
}

void init_message (zlink_msg_t *message_, const char *text_)
{
    const size_t size = strlen (text_);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (message_, size));
    if (size != 0)
        memcpy (zlink_msg_data (message_), text_, size);
}

void init_completion (zlink_completion_t *completion_)
{
    memset (completion_, 0, sizeof (*completion_));
    completion_->struct_size = sizeof (*completion_);
}

bool wait_completion (void *client_, zlink_completion_t *completion_,
                      int timeout_ms_)
{
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, client_, client_, ZLINK_POLLCOMPLETION));
    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    const int poll_result =
      zlink_poller_wait (poller, &event, 1, timeout_ms_, &error);
    bool received = false;
    if (poll_result == 1 && error == ZLINK_CONFIG_OK) {
        init_completion (completion_);
        received = zlink_completion_recv (
                     client_, completion_, ZLINK_RECV_FLAGS_DONTWAIT)
                   == ZLINK_RECV_OK;
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, client_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    return received;
}

bool receive_request_and_reply (void *server_, const char *expected_,
                                int timeout_ms_)
{
    zlink_pollitem_t item = {server_, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    if (zlink_poll (&item, 1, timeout_ms_, &error) != 1
        || error != ZLINK_CONFIG_OK)
        return false;

    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    zlink_msg_t request;
    zlink_part_flag_t more = ZLINK_PART_MORE;
    if (zlink_msg_init (&request) != ZLINK_CONFIG_OK)
        return false;
    const zlink_recv_result_t recv_result = zlink_router_recv_part (
      server_, &source, &token, &request, &more, ZLINK_RECV_FLAGS_DONTWAIT);
    const bool payload_ok =
      recv_result == ZLINK_RECV_OK && source && token != 0
      && more == ZLINK_PART_FINAL && zlink_msg_size (&request) == strlen (expected_)
      && memcmp (zlink_msg_data (&request), expected_, strlen (expected_)) == 0;
    zlink_msg_close (&request);
    if (!payload_ok)
        return false;

    zlink_msg_t reply;
    init_message (&reply, "reply-new-connection");
    return zlink_reply_part (server_, source, token, &reply, ZLINK_PART_FINAL)
           == ZLINK_SUBMIT_OK;
}

void run_immediate_reconnect_request_case (transport_t transport_,
                                           bool handover_)
{
    const char *const transport_name =
      transport_ == transport_tcp ? "tcp" : "inproc";
    void *server = new_socket (ZLINK_SOCKET_ROUTER);
    if (handover_)
        set_int_option (server, ZLINK_OPT_RID_DUPLICATE_POLICY,
                        ZLINK_RID_DUPLICATE_HANDOVER);
    char endpoint[MAX_SOCKET_STRING];
    bind_endpoint (server, transport_, endpoint, sizeof (endpoint),
                   "immediate");
    void *client = new_socket (ZLINK_SOCKET_DEALER);
    set_int_option (client, ZLINK_OPT_RECONNECT_IVL, reconnect_ivl_ms);
    void *monitor = open_client_monitor (client);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));
    event_observation_t initial_ready;
    TEST_ASSERT_TRUE (wait_monitor_event (
      monitor, ZLINK_EVENT_CONNECTION_READY, 0, true, event_timeout_ms,
      &initial_ready));

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_disconnect (client, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));

    zlink_msg_t first_attempt;
    init_message (&first_attempt, "request-new-connection");
    zlink_completion_id_t completion_id = 0;
    const zlink_submit_result_t first_result = zlink_request_part (
      client, NULL, &first_attempt, ZLINK_SEND_FLAGS_DONTWAIT,
      ZLINK_PART_FINAL, 500, reinterpret_cast<void *> (0x4),
      &completion_id);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&first_attempt));
    zlink_msg_close (&first_attempt);

    event_observation_t disconnected;
    event_observation_t replacement_ready;
    TEST_ASSERT_TRUE (wait_replacement_events (
      monitor, initial_ready.event.connection_id, event_timeout_ms,
      &disconnected, &replacement_ready));
    TEST_ASSERT_NOT_EQUAL (initial_ready.event.connection_id,
                           replacement_ready.event.connection_id);

    zlink_completion_id_t request_id = 0;
    if (first_result == ZLINK_SUBMIT_BACKPRESSURED) {
        TEST_ASSERT_NOT_EQUAL (0, completion_id);
        zlink_completion_t writable;
        TEST_ASSERT_TRUE (wait_completion (client, &writable,
                                           event_timeout_ms));
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, writable.kind);
        TEST_ASSERT_EQUAL_UINT64 (completion_id, writable.completion_id);
        TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, writable.send_result);
        zlink_completion_close (&writable);

        zlink_msg_t retry;
        init_message (&retry, "request-new-connection");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_request_part (client, NULL, &retry,
                              ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
                              500, reinterpret_cast<void *> (0x4),
                              &request_id));
        zlink_msg_close (&retry);
    } else {
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, first_result);
        request_id = completion_id;
    }
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    const bool request_delivered = receive_request_and_reply (
      server, "request-new-connection", handover_ ? event_timeout_ms : 250);
    if (handover_)
        TEST_ASSERT_TRUE (request_delivered);

    zlink_completion_t reply;
    TEST_ASSERT_TRUE (wait_completion (client, &reply, event_timeout_ms));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, reply.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, reply.completion_id);
    if (request_delivered) {
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply.request_result);
        TEST_ASSERT_EQUAL_UINT64 (1, reply.reply_part_count);
        TEST_ASSERT_EQUAL_UINT64 (strlen ("reply-new-connection"),
                                  zlink_msg_size (&reply.reply_parts[0]));
    } else {
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT,
                               reply.request_result);
        TEST_ASSERT_EQUAL_UINT64 (0, reply.reply_part_count);
    }
    zlink_completion_close (&reply);

    // The public reply token is intentionally opaque. Once the old terminal
    // edge was observed, a delivered request can only belong to the sole live
    // replacement; 0 records that a timed-out request cannot be attributed.
    printf ("IMMEDIATE transport=%s duplicate_policy=%s first_result=%d "
            "old_connection=%llu new_connection=%llu reply=%s "
            "reply_connection=%llu\n",
            transport_name, handover_ ? "handover" : "default-reject",
            static_cast<int> (first_result),
            static_cast<unsigned long long> (
              initial_ready.event.connection_id),
            static_cast<unsigned long long> (
              replacement_ready.event.connection_id),
            request_delivered ? "ok" : "timed-out",
            static_cast<unsigned long long> (
              request_delivered ? replacement_ready.event.connection_id : 0));

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor));
    close_socket (client);
    close_socket (server);
}
}

void test_tcp_registered_without_waiter_progresses ()
{
    run_disconnect_progress_case (transport_tcp, true, server_close);
}

void test_tcp_without_registration_progresses ()
{
    run_disconnect_progress_case (transport_tcp, false, server_close);
}

void test_inproc_registered_without_waiter_progresses ()
{
    run_disconnect_progress_case (transport_inproc, true, server_close);
}

void test_inproc_without_registration_progresses ()
{
    run_disconnect_progress_case (transport_inproc, false, server_close);
}

void test_tcp_registered_server_disconnect_progresses ()
{
    run_disconnect_progress_case (transport_tcp, true, server_disconnect);
}

void test_tcp_unregistered_server_disconnect_progresses ()
{
    run_disconnect_progress_case (transport_tcp, false, server_disconnect);
}

void test_inproc_registered_server_disconnect_progresses ()
{
    run_disconnect_progress_case (transport_inproc, true, server_disconnect);
}

void test_inproc_unregistered_server_disconnect_progresses ()
{
    run_disconnect_progress_case (transport_inproc, false, server_disconnect);
}

void test_tcp_immediate_disconnect_connect_default_policy_observation ()
{
    run_immediate_reconnect_request_case (transport_tcp, false);
}

void test_inproc_immediate_disconnect_connect_default_policy_observation ()
{
    run_immediate_reconnect_request_case (transport_inproc, false);
}

void test_tcp_immediate_disconnect_connect_handover_uses_replacement ()
{
    run_immediate_reconnect_request_case (transport_tcp, true);
}

void test_inproc_immediate_disconnect_connect_handover_uses_replacement ()
{
    run_immediate_reconnect_request_case (transport_inproc, true);
}

int main ()
{
    setup_test_environment (60);
    UNITY_BEGIN ();
    RUN_TEST (test_tcp_registered_without_waiter_progresses);
    RUN_TEST (test_tcp_without_registration_progresses);
    RUN_TEST (test_inproc_registered_without_waiter_progresses);
    RUN_TEST (test_inproc_without_registration_progresses);
    RUN_TEST (test_tcp_registered_server_disconnect_progresses);
    RUN_TEST (test_tcp_unregistered_server_disconnect_progresses);
    RUN_TEST (test_inproc_registered_server_disconnect_progresses);
    RUN_TEST (test_inproc_unregistered_server_disconnect_progresses);
    RUN_TEST (test_tcp_immediate_disconnect_connect_default_policy_observation);
    RUN_TEST (test_inproc_immediate_disconnect_connect_default_policy_observation);
    RUN_TEST (test_tcp_immediate_disconnect_connect_handover_uses_replacement);
    RUN_TEST (test_inproc_immediate_disconnect_connect_handover_uses_replacement);
    return UNITY_END ();
}
