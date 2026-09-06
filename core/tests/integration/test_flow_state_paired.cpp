/* SPDX-License-Identifier: MPL-2.0 */

// DEALER/ROUTER flow control through the public C API. Internal epoch,
// pipe topology and byte-credit probes live in unittest_flow_state_socket.

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <cstring>

namespace
{
std::chrono::steady_clock::time_point deadline_in_ms (int ms_)
{
    return std::chrono::steady_clock::now () + std::chrono::milliseconds (ms_);
}

bool deadline_expired (const std::chrono::steady_clock::time_point &deadline_)
{
    return std::chrono::steady_clock::now () >= deadline_;
}

void process_socket_commands (void *socket_)
{
    int events = 0;
    size_t size = sizeof (events);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_get_option (socket_, ZLINK_OPT_EVENTS,
                                             &events, &size));
}

zlink_routing_id_t recv_payload (void *socket_, const char *payload_,
                                zlink_part_flag_t expected_more_ = ZLINK_PART_FINAL,
                                bool router_ = true)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    const zlink_routing_id_t *rid = NULL;
    zlink_part_flag_t more = ZLINK_PART_FINAL;
    uint64_t request_seq = 0;
    const zlink_recv_result_t result_code = router_
      ? zlink_router_recv_part (socket_, &rid, &request_seq, &part, &more,
                                 ZLINK_RECV_FLAGS_NONE)
      : zlink_recv_part (socket_, &rid, &part, &more, ZLINK_RECV_FLAGS_NONE);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result_code);
    TEST_ASSERT_EQUAL_UINT (strlen (payload_), zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_MEMORY (payload_, zlink_msg_data (&part), strlen (payload_));
    TEST_ASSERT_EQUAL_INT (expected_more_, more);
    zlink_routing_id_t result;
    memset (&result, 0, sizeof (result));
    if (rid)
        result = *rid;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    return result;
}

zlink_submit_result_t send_payload (void *socket_, const char *payload_,
                                    const zlink_routing_id_t *rid_ = NULL,
                                    zlink_part_flag_t more_ = ZLINK_PART_FINAL)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (&part, strlen (payload_)));
    memcpy (zlink_msg_data (&part), payload_, strlen (payload_));
    return rid_ ? zlink_send_part_rid (socket_, rid_, &part,
                                       ZLINK_SEND_FLAGS_DONTWAIT, more_, NULL, NULL)
                : zlink_send_part (socket_, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                                    more_, NULL, NULL);
}

struct paired_fixture_t
{
    paired_fixture_t () :
        dealer (NULL), router (NULL), monitor (NULL), router_monitor (NULL)
    {
    }

    void create (int router_weight_ = 100, int dealer_weight_ = 100)
    {
        const int zero = 0;
        router = test_context_socket (ZLINK_SOCKET_ROUTER);
        dealer = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_router_option (
          router, ZLINK_ROUTER_OPT_WEIGHT, &router_weight_, sizeof (router_weight_)));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_dealer_option (
          dealer, ZLINK_DEALER_OPT_WEIGHT, &dealer_weight_, sizeof (dealer_weight_)));
        monitor = open_test_monitor_probe (
          dealer, ZLINK_EVENT_SEND_FLOW_PAUSED | ZLINK_EVENT_SEND_FLOW_RESUMED,
          &probe);
    }

    void setup (const char *inproc_ = NULL)
    {
        create ();
        if (inproc_) {
            snprintf (endpoint, sizeof (endpoint), "%s", inproc_);
            TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint));
        } else
            bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
        send_string_expect_success (dealer, "hello", 0);
        peer_rid = recv_payload (router, "hello");
        TEST_ASSERT_GREATER_THAN_INT (0, peer_rid.size);
    }

    bool wait_for_pause_count (uint64_t paused_, uint64_t transitions_, int ms_)
    {
        const std::chrono::steady_clock::time_point deadline = deadline_in_ms (ms_);
        while (!deadline_expired (deadline)) {
            process_socket_commands (router);
            process_socket_commands (dealer);
            zlink_monitor_status_t status;
            memset (&status, 0, sizeof (status));
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                                   zlink_monitor_status (monitor, &status));
            if (status.flow_paused_connections == paused_
                && status.flow_pause_applied_total == transitions_)
                return true;
            msleep (1);
        }
        return false;
    }

    void set_router_state (zlink_receive_flow_state_t state_)
    {
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_socket_set_receive_flow_state (router, state_));
    }

    void teardown ()
    {
        close_test_monitor_probe (&router_monitor, &router_probe);
        close_test_monitor_probe (&monitor, &probe);
        if (dealer)
            test_context_socket_close_zero_linger (dealer);
        if (router)
            test_context_socket_close_zero_linger (router);
    }

    void *dealer;
    void *router;
    void *monitor;
    void *router_monitor;
    test_monitor_probe_t probe;
    test_monitor_probe_t router_probe;
    char endpoint[MAX_SOCKET_STRING];
    zlink_routing_id_t peer_rid;
};

// Unity failures longjmp past local destructors. Keep the probes and their
// receiver threads alive until tearDown joins them and closes both monitors.
paired_fixture_t *test_fixture = NULL;

bool stays_blocked (void *socket_, int window_ms_)
{
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (window_ms_);
    while (!deadline_expired (deadline)) {
        const zlink_submit_result_t result = send_payload (socket_, "payload");
        if (result != ZLINK_SUBMIT_BACKPRESSURED)
            return false;
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }
    return true;
}

bool wait_for_send_success (void *socket_, int budget_ms_)
{
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (budget_ms_);
    while (!deadline_expired (deadline)) {
        const zlink_submit_result_t result = send_payload (socket_, "payload");
        if (result == ZLINK_SUBMIT_OK)
            return true;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, result);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }
    return false;
}

void test_remote_pause_blocks_sender_and_resume_releases_it ()
{
    paired_fixture_t &fixture = *test_fixture;
    fixture.setup ();
    fixture.set_router_state (ZLINK_RECEIVE_FLOW_PAUSED);
    TEST_ASSERT_TRUE (fixture.wait_for_pause_count (1, 1, 2000));
    TEST_ASSERT_TRUE (stays_blocked (fixture.dealer, 100));
    fixture.set_router_state (ZLINK_RECEIVE_FLOW_RUNNING);
    TEST_ASSERT_TRUE (fixture.wait_for_pause_count (0, 1, 2000));
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 5000));
    recv_payload (fixture.router, "payload");
}


void test_new_and_reconnected_pairs_receive_the_latest_state ()
{
    paired_fixture_t &fixture = *test_fixture;
    fixture.create ();
    bind_loopback_ipv4 (fixture.router, fixture.endpoint, sizeof (fixture.endpoint));
    fixture.set_router_state (ZLINK_RECEIVE_FLOW_PAUSED);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (fixture.dealer, fixture.endpoint));
    TEST_ASSERT_TRUE (fixture.wait_for_pause_count (1, 1, 4000));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&fixture.probe, 1, 4000));
    const zlink_monitor_event_t first = test_monitor_probe_record_at (&fixture.probe, 0);
    TEST_ASSERT_TRUE (first.connection_id != 0);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (fixture.dealer, fixture.endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (fixture.dealer, fixture.endpoint));
    TEST_ASSERT_TRUE (fixture.wait_for_pause_count (1, 2, 4000));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&fixture.probe, 2, 4000));
    const zlink_monitor_event_t second = test_monitor_probe_record_at (&fixture.probe, 1);
    TEST_ASSERT_TRUE (second.connection_id != 0);
    TEST_ASSERT_NOT_EQUAL (first.connection_id, second.connection_id);
    TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_SEND_FLOW_PAUSED, second.event);
    fixture.set_router_state (ZLINK_RECEIVE_FLOW_RUNNING);
    TEST_ASSERT_TRUE (fixture.wait_for_pause_count (0, 2, 4000));
}

void test_no_application_recv_returns_a_flow_frame ()
{
    paired_fixture_t &fixture = *test_fixture;
    fixture.setup ();
    for (int i = 0; i < 4; ++i) {
        fixture.set_router_state (ZLINK_RECEIVE_FLOW_PAUSED);
        fixture.set_router_state (ZLINK_RECEIVE_FLOW_RUNNING);
    }
    char buffer[256];
    const std::chrono::steady_clock::time_point quiet = deadline_in_ms (200);
    while (!deadline_expired (quiet)) {
        const int result = zlink_recv (fixture.dealer, buffer, sizeof (buffer),
                                       ZLINK_DONTWAIT);
        TEST_ASSERT_EQUAL_INT (-1, result);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (5);
    }
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 5000));
    const zlink_routing_id_t rid = recv_payload (fixture.router, "payload");
    TEST_ASSERT_GREATER_THAN_INT (0, rid.size);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           send_payload (fixture.router, "reply", &rid));
    recv_payload (fixture.dealer, "reply", ZLINK_PART_FINAL, false);
}

void test_peer_weight_change_does_not_leak_to_public_receive ()
{
    paired_fixture_t &fixture = *test_fixture;
    fixture.setup ("inproc://peer_weight_public_receive");
    const int weight = 37;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_router_option (
      fixture.router, ZLINK_ROUTER_OPT_WEIGHT, &weight, sizeof (weight)));

    zlink_msg_t request;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&request, 4));
    memcpy (zlink_msg_data (&request), "ping", 4);
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_request_part (
      fixture.dealer, NULL, &request, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
      1500, NULL, &completion_id));
    TEST_ASSERT_TRUE (completion_id != 0);
    const zlink_routing_id_t *peer_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_router_recv (
      fixture.router, &peer_rid, &request_seq, &parts, &part_count, 0));
    TEST_ASSERT_NOT_NULL (peer_rid);
    TEST_ASSERT_TRUE (request_seq != 0);
    const zlink_routing_id_t reply_rid = *peer_rid;
    zlink_multipart_close (parts, part_count);
    zlink_msg_t reply;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&reply, 4));
    memcpy (zlink_msg_data (&reply), "pong", 4);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_reply_part (
      fixture.router, &reply_rid, request_seq, &reply, ZLINK_PART_FINAL));
    bool completed = false;
    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (4000);
    while (!deadline_expired (deadline)) {
        const zlink_recv_result_t result = zlink_completion_recv (
          fixture.dealer, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            completed = true;
            break;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }
    TEST_ASSERT_TRUE (completed);
    TEST_ASSERT_EQUAL_UINT64 (completion_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    zlink_completion_close (&completion);
    char raw[32];
    TEST_ASSERT_EQUAL_INT (-1, zlink_recv (fixture.dealer, raw, sizeof (raw),
                                          ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
}

zlink_monitor_event_t wait_for_peer_weight (paired_fixture_t *fixture_,
                                           test_monitor_probe_t *probe_,
                                           uint64_t value_, int after_)
{
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (3000);
    while (!deadline_expired (deadline)) {
        process_socket_commands (fixture_->dealer);
        process_socket_commands (fixture_->router);
        const int count = test_monitor_probe_count (probe_);
        for (int i = after_; i < count; ++i) {
            const zlink_monitor_event_t event = test_monitor_probe_record_at (probe_, i);
            if (event.event == ZLINK_EVENT_PEER_WEIGHT_CHANGED && event.value == value_) {
                TEST_ASSERT_TRUE (event.connection_id != 0);
                TEST_ASSERT_TRUE (event.routing_id.size > 0);
                TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                                        event.transport_lane);
                return event;
            }
        }
        msleep (1);
    }
    fprintf (stderr, "Missing peer weight %llu on %s monitor after record %d\n",
             static_cast<unsigned long long> (value_),
             probe_ == &fixture_->probe ? "DEALER" : "ROUTER", after_);
    test_monitor_probe_t *probes[] = {&fixture_->probe, &fixture_->router_probe};
    for (size_t p = 0; p < 2; ++p) {
        const int count = test_monitor_probe_count (probes[p]);
        for (int i = 0; i < count; ++i) {
            const zlink_monitor_event_t event = test_monitor_probe_record_at (probes[p], i);
            fprintf (stderr, "%s monitor[%d]: event=%llu value=%llu connection=%llu lane=%u\n",
                     p == 0 ? "DEALER" : "ROUTER", i,
                     static_cast<unsigned long long> (event.event),
                     static_cast<unsigned long long> (event.value),
                     static_cast<unsigned long long> (event.connection_id),
                     static_cast<unsigned int> (event.transport_lane));
        }
    }
    TEST_FAIL_MESSAGE ("Peer weight did not reach the public monitor");
    zlink_monitor_event_t missing;
    memset (&missing, 0, sizeof (missing));
    return missing;
}

void peer_weight_replays_after_reconnection (const char *inproc_)
{
    paired_fixture_t &fixture = *test_fixture;
    fixture.create (23, 0);
    close_test_monitor_probe (&fixture.monitor, &fixture.probe);
    fixture.monitor = open_test_monitor_probe (
      fixture.dealer, ZLINK_EVENT_ALL, &fixture.probe);
    test_monitor_probe_t &router_probe = fixture.router_probe;
    fixture.router_monitor = open_test_monitor_probe (
      fixture.router, ZLINK_EVENT_ALL, &router_probe);
    if (inproc_) {
        snprintf (fixture.endpoint, sizeof (fixture.endpoint), "%s", inproc_);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (fixture.router, fixture.endpoint));
    } else
        bind_loopback_ipv4 (fixture.router, fixture.endpoint, sizeof (fixture.endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (fixture.dealer, fixture.endpoint));
    wait_for_peer_weight (&fixture, &fixture.probe, 23, 0);
    wait_for_peer_weight (&fixture, &router_probe, 0, 0);

    const int router_weight = 71;
    const int dealer_weight = 19;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_router_option (
      fixture.router, ZLINK_ROUTER_OPT_WEIGHT, &router_weight, sizeof (router_weight)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_dealer_option (
      fixture.dealer, ZLINK_DEALER_OPT_WEIGHT, &dealer_weight, sizeof (dealer_weight)));
    const zlink_monitor_event_t previous = wait_for_peer_weight (
      &fixture, &fixture.probe, router_weight, 0);
    const zlink_monitor_event_t router_previous = wait_for_peer_weight (
      &fixture, &router_probe, dealer_weight, 0);

    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      fixture.router, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover, sizeof (handover)));
    const int dealer_baseline = test_monitor_probe_count (&fixture.probe);
    const int router_baseline = test_monitor_probe_count (&router_probe);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (fixture.dealer, fixture.endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (fixture.dealer, fixture.endpoint));
    const zlink_monitor_event_t replacement = wait_for_peer_weight (
      &fixture, &fixture.probe, router_weight, dealer_baseline);
    const zlink_monitor_event_t router_replacement = wait_for_peer_weight (
      &fixture, &router_probe, dealer_weight, router_baseline);
    TEST_ASSERT_NOT_EQUAL (previous.connection_id, replacement.connection_id);
    TEST_ASSERT_NOT_EQUAL (router_previous.connection_id,
                           router_replacement.connection_id);
}

void test_network_peer_weight_replays_after_reconnection ()
{
    peer_weight_replays_after_reconnection (NULL);
}

void test_inproc_peer_weight_replays_after_reconnection ()
{
    peer_weight_replays_after_reconnection ("inproc://peer_weight_reconnection");
}

}

void setUp ()
{
    setup_test_context ();
    test_fixture = new paired_fixture_t;
}

void tearDown ()
{
    test_fixture->teardown ();
    delete test_fixture;
    test_fixture = NULL;
    teardown_test_context ();
}

int main ()
{
    setup_test_environment ();
    const char *selected_case = getenv ("ZLINK_TEST_CASE");
#define RUN_FLOW_CASE(TEST_FN)                                             \
    do {                                                                  \
        if (!selected_case || strcmp (selected_case, #TEST_FN) == 0)        \
            RUN_TEST (TEST_FN);                                            \
    } while (false)
    UNITY_BEGIN ();
    RUN_FLOW_CASE (test_remote_pause_blocks_sender_and_resume_releases_it);
    RUN_FLOW_CASE (test_new_and_reconnected_pairs_receive_the_latest_state);
    RUN_FLOW_CASE (test_no_application_recv_returns_a_flow_frame);
    RUN_FLOW_CASE (test_peer_weight_change_does_not_leak_to_public_receive);
    RUN_FLOW_CASE (test_network_peer_weight_replays_after_reconnection);
    RUN_FLOW_CASE (test_inproc_peer_weight_replays_after_reconnection);
    return UNITY_END ();
#undef RUN_FLOW_CASE
}
