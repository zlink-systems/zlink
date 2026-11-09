/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "completion_test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
void *active_poller = NULL;
void *active_monitor = NULL;
void *active_sender = NULL;
}

void setUp ()
{
    setup_test_context ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_ctx_set (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_ENABLE, 0));
}

void tearDown ()
{
    // Unity can abort a case before close_pair. Release registrations before
    // test-context teardown closes their sockets.
    if (active_poller) {
        (void) zlink_poller_remove (active_poller, active_sender);
        (void) zlink_poller_destroy (&active_poller);
    }
    if (active_monitor)
        (void) zlink_monitor_close (&active_monitor);
    active_sender = NULL;
    teardown_test_context ();
}

namespace
{
typedef std::vector<std::string> record_t;
typedef std::chrono::steady_clock clock_type;

struct router_pair_t
{
    void *sender;
    void *receiver;
    void *monitor;
    void *poller;
    zlink_routing_id_t target;
};

void wait_flow (const router_pair_t &pair_, bool paused_);

void poll_socket (void *socket_, short events_)
{
    zlink_pollitem_t item = {socket_, 0, events_, 0};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_TRUE (zlink_poll (&item, 1, 10, &error) >= 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
}

router_pair_t make_pair (uint64_t hwm_, int64_t max_size_ = -1)
{
    router_pair_t pair = {};
    pair.sender = test_context_socket (ZLINK_SOCKET_ROUTER);
    pair.receiver = test_context_socket (ZLINK_SOCKET_ROUTER);
    const int zero = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (pair.sender, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_set_option (pair.receiver, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (pair.sender, ZLINK_OPT_SNDHWM, &hwm_, sizeof (hwm_)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_set_option (pair.receiver, ZLINK_OPT_RCVHWM, &hwm_, sizeof (hwm_)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_option (pair.receiver, ZLINK_OPT_MAXMSGSIZE,
                                                              &max_size_, sizeof (max_size_)));
    const char *name = "complete-record-target";
    pair.target.size = static_cast<uint8_t> (strlen (name));
    memcpy (pair.target.data, name, pair.target.size);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (pair.receiver, name, pair.target.size));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_socket_set_receive_flow_state (
                                              pair.receiver, ZLINK_RECEIVE_FLOW_PAUSED));

    zlink_socket_monitor_open_options_t options = {};
    options.events = ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY;
    pair.monitor = zlink_socket_monitor_open (pair.sender, &options);
    TEST_ASSERT_NOT_NULL (pair.monitor);
    active_monitor = pair.monitor;
    pair.poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (pair.poller);
    active_poller = pair.poller;
    active_sender = pair.sender;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (pair.poller, pair.sender, NULL, ZLINK_POLLOUT | ZLINK_POLLCOMPLETION));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (pair.receiver, "inproc://router-complete-record-hwm"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (pair.sender, "inproc://router-complete-record-hwm"));

    bool ready = false;
    const clock_type::time_point deadline = clock_type::now () + std::chrono::seconds (3);
    while (!ready && clock_type::now () < deadline) {
        poll_socket (pair.monitor, ZLINK_POLLIN);
        zlink_monitor_event_t event = {};
        if (zlink_socket_monitor_recv (pair.monitor, &event, ZLINK_RECV_FLAGS_DONTWAIT)
            == ZLINK_RECV_OK)
            ready = event.event == ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY;
    }
    TEST_ASSERT_TRUE_MESSAGE (ready, "ROUTER connection did not become ready");
    wait_flow (pair, true);
    return pair;
}

void wait_flow (const router_pair_t &pair_, bool paused_)
{
    const clock_type::time_point deadline = clock_type::now () + std::chrono::seconds (3);
    do {
        // Both inproc endpoint owners must process attach and flow controls.
        void *sockets[] = {pair_.receiver, pair_.sender};
        for (size_t i = 0; i < 2; ++i) {
            int events = 0;
            size_t size = sizeof (events);
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                                   zlink_get_option (sockets[i], ZLINK_OPT_EVENTS, &events, &size));
        }
        zlink_poller_event_t event = {};
        zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
        TEST_ASSERT_TRUE (zlink_poller_wait (pair_.poller, &event, 1, 10, &error) >= 0);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
        zlink_monitor_status_t status = {};
        status.abi_version = ZLINK_MONITOR_STATUS_ABI_VERSION;
        status.struct_size = sizeof (status);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_monitor_status (pair_.monitor, &status));
        if ((status.flow_paused_connections != 0) == paused_)
            return;
    } while (clock_type::now () < deadline);
    TEST_FAIL_MESSAGE ("remote receive-flow state was not applied");
}

zlink_submit_result_t
submit (const router_pair_t &pair_, const record_t &record_, zlink_completion_id_t *token_)
{
    std::vector<zlink_msg_t> parts (record_.size ());
    for (size_t i = 0; i < parts.size (); ++i)
        init_part (&parts[i], record_[i]);
    *token_ = 0;
    const zlink_submit_result_t result =
      zlink_send_rid (pair_.sender, &pair_.target, parts.data (), parts.size (),
                      ZLINK_SEND_FLAGS_DONTWAIT, NULL, token_);
    const int saved_errno = zlink_errno ();
    // The public API consumes every input slot on success and refusal.
    for (size_t i = 0; i < parts.size (); ++i)
        assert_part_consumed (&parts[i]);
    errno = saved_errno;
    return result;
}

void wait_writable (const router_pair_t &pair_, zlink_completion_id_t token_)
{
    const clock_type::time_point deadline = clock_type::now () + std::chrono::seconds (3);
    do {
        zlink_completion_t completion;
        init_empty_completion (&completion);
        const zlink_recv_result_t result =
          zlink_completion_recv (pair_.sender, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, completion.kind);
            TEST_ASSERT_EQUAL_UINT64 (token_, completion.completion_id);
            TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
            zlink_completion_close (&completion);
            return;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        zlink_poller_event_t event = {};
        zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
        TEST_ASSERT_TRUE (zlink_poller_wait (pair_.poller, &event, 1, 10, &error) >= 0);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    } while (clock_type::now () < deadline);
    TEST_FAIL_MESSAGE ("no WRITABLE completion for the refused record");
}

void assert_no_record (const router_pair_t &pair_)
{
    zlink_msg_t parts[4];
    size_t count = 0;
    const zlink_routing_id_t *rid = NULL;
    zlink_reply_token_t reply_token = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA,
                           zlink_router_recv (pair_.receiver, &rid, &reply_token, parts, 4, &count,
                                              ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, count);
}

void receive_record (const router_pair_t &pair_, const record_t &record_)
{
    zlink_pollitem_t item = {pair_.receiver, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&item, 1, 3000, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    zlink_msg_t parts[4];
    size_t count = 0;
    const zlink_routing_id_t *rid = NULL;
    zlink_reply_token_t reply_token = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
                           zlink_router_recv (pair_.receiver, &rid, &reply_token, parts, 4, &count,
                                              ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_NOT_NULL (rid);
    TEST_ASSERT_EQUAL_UINT64 (record_.size (), count);
    for (size_t i = 0; i < count; ++i) {
        TEST_ASSERT_EQUAL_UINT64 (record_[i].size (), zlink_msg_size (&parts[i]));
        TEST_ASSERT_EQUAL_MEMORY (record_[i].data (), zlink_msg_data (&parts[i]),
                                  record_[i].size ());
    }
    zlink_multipart_close (parts, count);
}

void close_pair (router_pair_t &pair_)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (pair_.poller, pair_.sender));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&pair_.poller));
    active_poller = NULL;
    active_sender = NULL;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_monitor_close (&pair_.monitor));
    active_monitor = NULL;
    test_context_socket_close_zero_linger (pair_.sender);
    test_context_socket_close_zero_linger (pair_.receiver);
}

void run_resume (const record_t &record_, uint64_t hwm_)
{
    router_pair_t pair = make_pair (hwm_);
    zlink_completion_id_t token = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, submit (pair, record_, &token));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_NOT_EQUAL (0, token);
    assert_no_record (pair);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_socket_set_receive_flow_state (
                                              pair.receiver, ZLINK_RECEIVE_FLOW_RUNNING));
    wait_writable (pair, token);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit (pair, record_, &token));
    TEST_ASSERT_EQUAL_UINT64 (0, token);
    receive_record (pair, record_);
    assert_no_record (pair);
    fprintf (stderr, "resume hwm=%llu frames=%zu admitted; complete record received\n",
             static_cast<unsigned long long> (hwm_), record_.size ());
    close_pair (pair);
}
}

void test_small_first_complete_record_resumes_at_hwm_16 ()
{
    run_resume ({std::string (6, 'h'), std::string (1024, 'b')}, 16);
}

void test_large_first_complete_record_resumes_at_hwm_16 ()
{
    run_resume ({std::string (1024, 'b'), std::string (4, 't')}, 16);
}

void test_three_part_complete_record_resumes_at_hwm_16 ()
{
    run_resume ({std::string (6, 'h'), std::string (512, 'm'), std::string (512, 't')}, 16);
}

void test_complete_record_resumes_at_hwm_2048 ()
{
    run_resume ({std::string (6, 'h'), std::string (1024, 'b')}, 2048);
    run_resume ({std::string (1024, 'b'), std::string (4, 't')}, 2048);
    run_resume ({std::string (6, 'h'), std::string (512, 'm'), std::string (512, 't')}, 2048);
}

void test_single_frame_resume_controls ()
{
    run_resume ({std::string (1024, 's')}, 16);
    run_resume ({std::string (1024, 's')}, 2048);
}

void test_complete_record_exception_keeps_maxmsgsize_and_rolls_back ()
{
    // Each frame fits MAXMSGSIZE, but their complete payload does not.
    router_pair_t pair = make_pair (16, 1024);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_socket_set_receive_flow_state (
                                              pair.receiver, ZLINK_RECEIVE_FLOW_RUNNING));
    wait_flow (pair, false);
    const record_t record = {std::string (6, 'h'), std::string (1024, 'b')};
    zlink_completion_id_t token = 0;
    const zlink_submit_result_t result = submit (pair, record, &token);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_ARGUMENT, result);
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, token);
    assert_no_record (pair);
    // A later valid record proves the refused prefix was discarded.
    const record_t valid = {std::string (6, 'v'), std::string (8, 't')};
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit (pair, valid, &token));
    receive_record (pair, valid);
    assert_no_record (pair);
    close_pair (pair);
}

void test_second_oversize_record_waits_for_first_dequeue ()
{
    router_pair_t pair = make_pair (16);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_socket_set_receive_flow_state (
                                              pair.receiver, ZLINK_RECEIVE_FLOW_RUNNING));
    wait_flow (pair, false);
    const record_t first = {std::string (6, 'a'), std::string (1024, 'b')};
    const record_t second = {std::string (1024, 'c'), std::string (4, 'd')};
    zlink_completion_id_t token = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit (pair, first, &token));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, submit (pair, second, &token));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_NOT_EQUAL (0, token);
    receive_record (pair, first);
    assert_no_record (pair);
    wait_writable (pair, token);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit (pair, second, &token));
    receive_record (pair, second);
    assert_no_record (pair);
    close_pair (pair);
}

void test_nonempty_queue_refuses_final_without_delivering_prefix ()
{
    router_pair_t pair = make_pair (2048);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_socket_set_receive_flow_state (
                                              pair.receiver, ZLINK_RECEIVE_FLOW_RUNNING));
    wait_flow (pair, false);
    const record_t first = {std::string (16, 'a')};
    const record_t refused = {std::string (6, 'h'), std::string (4096, 'b')};
    zlink_completion_id_t token = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit (pair, first, &token));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, submit (pair, refused, &token));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    receive_record (pair, first);
    assert_no_record (pair);
    wait_writable (pair, token);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit (pair, refused, &token));
    receive_record (pair, refused);
    assert_no_record (pair);
    close_pair (pair);
}

void test_concurrent_complete_records_share_one_empty_exception ()
{
    router_pair_t pair = make_pair (16);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_socket_set_receive_flow_state (
                                              pair.receiver, ZLINK_RECEIVE_FLOW_RUNNING));
    wait_flow (pair, false);
    const record_t records[] = {{std::string (6, 'a'), std::string (1024, 'b')},
                                {std::string (1024, 'c'), std::string (4, 'd')}};
    zlink_msg_t parts[2][2];
    for (size_t i = 0; i < 2; ++i)
        for (size_t j = 0; j < 2; ++j)
            init_part (&parts[i][j], records[i][j]);
    zlink_submit_result_t results[2];
    zlink_completion_id_t tokens[2] = {0, 0};
    int errors[2] = {0, 0};
    std::atomic<unsigned> ready (0);
    std::atomic<bool> go (false);
    const auto send = [&] (size_t index_) {
        ready.fetch_add (1, std::memory_order_release);
        while (!go.load (std::memory_order_acquire))
            std::this_thread::yield ();
        results[index_] = zlink_send_rid (pair.sender, &pair.target, parts[index_], 2,
                                          ZLINK_SEND_FLAGS_DONTWAIT, NULL, &tokens[index_]);
        errors[index_] = zlink_errno ();
    };
    std::thread first (send, 0);
    std::thread second (send, 1);
    while (ready.load (std::memory_order_acquire) != 2)
        std::this_thread::yield ();
    go.store (true, std::memory_order_release);
    first.join ();
    second.join ();
    const size_t winner = results[0] == ZLINK_SUBMIT_OK ? 0 : 1;
    const size_t loser = 1 - winner;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, results[winner]);
    TEST_ASSERT_EQUAL_UINT64 (0, tokens[winner]);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, results[loser]);
    TEST_ASSERT_EQUAL_INT (EAGAIN, errors[loser]);
    TEST_ASSERT_NOT_EQUAL (0, tokens[loser]);
    for (size_t i = 0; i < 2; ++i)
        for (size_t j = 0; j < 2; ++j)
            assert_part_consumed (&parts[i][j]);
    receive_record (pair, records[winner]);
    assert_no_record (pair);
    wait_writable (pair, tokens[loser]);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit (pair, records[loser], &tokens[loser]));
    receive_record (pair, records[loser]);
    assert_no_record (pair);
    close_pair (pair);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_small_first_complete_record_resumes_at_hwm_16);
    RUN_TEST (test_large_first_complete_record_resumes_at_hwm_16);
    RUN_TEST (test_three_part_complete_record_resumes_at_hwm_16);
    RUN_TEST (test_complete_record_resumes_at_hwm_2048);
    RUN_TEST (test_single_frame_resume_controls);
    RUN_TEST (test_complete_record_exception_keeps_maxmsgsize_and_rolls_back);
    RUN_TEST (test_second_oversize_record_waits_for_first_dequeue);
    RUN_TEST (test_nonempty_queue_refuses_final_without_delivering_prefix);
    RUN_TEST (test_concurrent_complete_records_share_one_empty_exception);
    return UNITY_END ();
}
