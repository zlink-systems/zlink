/* SPDX-License-Identifier: MPL-2.0 */

// Core Polling §5: after zlink_ctx_shutdown, a poller wait that has an
// unclosed socket source (monitor handles included) of that context ends
// without events with ZLINK_CONFIG_INTERNAL_ERROR (ETERM) until the source is
// removed or closed. fd and timer sources are not affected.

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "completion_test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#if !defined(ZLINK_HAVE_WINDOWS)
#include <unistd.h>
#endif

void setUp ()
{
}

void tearDown ()
{
}

namespace
{
const long kPromptMs = 1000;

struct wait_observation_t
{
    wait_observation_t () :
        rc (-2), error (ZLINK_CONFIG_OK), err (0), events (0), elapsed_ms (0)
    {
    }
    int rc;
    zlink_config_result_t error;
    int err;
    short events;
    long elapsed_ms;
};

wait_observation_t wait_once (void *poller_, long timeout_ms_)
{
    wait_observation_t observed;
    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now ();
    errno = 0;
    observed.rc = zlink_poller_wait (poller_, &event, 1, timeout_ms_,
                                     &observed.error);
    observed.err = zlink_errno ();
    observed.elapsed_ms = static_cast<long> (
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now () - start)
        .count ());
    observed.events = observed.rc > 0 ? event.events : 0;
    return observed;
}

void assert_terminated_wait (const wait_observation_t &observed_)
{
    TEST_ASSERT_EQUAL_INT (-1, observed_.rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INTERNAL_ERROR, observed_.error);
    TEST_ASSERT_EQUAL_INT (ETERM, observed_.err);
    TEST_ASSERT_LESS_THAN_INT64 (kPromptMs, observed_.elapsed_ms);
}

void set_zero_linger (void *socket_)
{
    const int zero_linger = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero_linger,
                        sizeof (zero_linger)));
}

// (a) An indefinite wait in progress ends promptly with ETERM.
void test_indefinite_wait_ends_with_eterm_on_ctx_shutdown ()
{
    void *context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *socket = zlink_socket (context, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add (poller, socket, NULL, ZLINK_POLLIN));

    std::atomic<bool> entered (false);
    wait_observation_t observed;
    std::thread waiter ([&] {
        entered.store (true, std::memory_order_release);
        observed = wait_once (poller, -1);
    });
    while (!entered.load (std::memory_order_acquire))
        std::this_thread::yield ();
    msleep (50);
    const std::chrono::steady_clock::time_point shutdown_at =
      std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (context));
    waiter.join ();
    const long after_shutdown_ms = static_cast<long> (
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now () - shutdown_at)
        .count ());
    TEST_ASSERT_EQUAL_INT (-1, observed.rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INTERNAL_ERROR, observed.error);
    TEST_ASSERT_EQUAL_INT (ETERM, observed.err);
    TEST_ASSERT_LESS_THAN_INT64 (kPromptMs, after_shutdown_ms);

    // Later waits stay terminal until the source is removed.
    assert_terminated_wait (wait_once (poller, 5000));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, socket));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}

// (b) A REQUEST reply that arrived before shutdown does not turn the wait into
// a POLLCOMPLETION event, and the unread completion is discarded.
void run_ready_completion_case (bool register_after_shutdown_,
                                bool pull_before_wait_)
{
    void *context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *router = zlink_socket (context, ZLINK_SOCKET_ROUTER);
    void *dealer = zlink_socket (context, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_zero_linger (router);
    set_zero_linger (dealer);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_set_routing_id (dealer, "shutdown-req", 12));
    const std::string endpoint =
      register_after_shutdown_ ? "inproc://poller-shutdown-after"
                               : "inproc://poller-shutdown-before";
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint.c_str ()));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (dealer, endpoint.c_str ()));
    msleep (SETTLE_TIME);

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    if (!register_after_shutdown_)
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_add (poller, dealer, NULL, ZLINK_POLLCOMPLETION));

    zlink_msg_t request;
    init_part (&request, "shutdown-request");
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request (dealer, NULL, &request, 1, ZLINK_SEND_FLAGS_DONTWAIT,
                     120000, NULL, &completion_id));
    assert_part_consumed (&request);

    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t reply_token = 0;
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    size_t part_flag = 1;
    const int router_timeout = 5000;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_RCVTIMEO, &router_timeout,
                        sizeof (router_timeout)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (router, &source_rid, &reply_token, &part, 1,
                         &part_flag, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid);
    const zlink_routing_id_t rid = *source_rid;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));

    zlink_msg_t reply;
    init_part (&reply, "shutdown-reply");
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           zlink_reply (router, &rid, reply_token, &reply, 1));
    assert_part_consumed (&reply);

    // The reply is published as a ready completion before shutdown. A
    // separate poller observes it so the registration order under test is
    // the only difference between the two cases.
    void *probe = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (probe);
    if (register_after_shutdown_)
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_add (probe, dealer, NULL, ZLINK_POLLCOMPLETION));
    const wait_observation_t ready =
      wait_once (register_after_shutdown_ ? probe : poller, 5000);
    TEST_ASSERT_EQUAL_INT (1, ready.rc);
    TEST_ASSERT_TRUE ((ready.events & ZLINK_POLLCOMPLETION) != 0);
    if (register_after_shutdown_)
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_poller_remove (probe, dealer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&probe));

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (context));
    if (pull_before_wait_) {
        // Termination discards the unread completion even before any wait
        // or command processing observed the stop.
        zlink_completion_t early;
        init_empty_completion (&early);
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_TERMINATED,
          zlink_completion_recv (dealer, &early, ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (ETERM, zlink_errno ());
        assert_empty_completion (early);
    }
    if (register_after_shutdown_)
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_add (poller, dealer, NULL, ZLINK_POLLCOMPLETION));

    assert_terminated_wait (wait_once (poller, 5000));

    zlink_completion_t completion;
    init_empty_completion (&completion);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_TERMINATED,
      zlink_completion_recv (dealer, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ETERM, zlink_errno ());
    assert_empty_completion (completion);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, dealer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (router));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}

void test_ready_completion_registered_before_shutdown_is_terminated ()
{
    run_ready_completion_case (false, false);
}

void test_ready_completion_registered_after_shutdown_is_terminated ()
{
    run_ready_completion_case (true, false);
}

void test_ready_completion_pulled_right_after_shutdown_is_terminated ()
{
    run_ready_completion_case (false, true);
    run_ready_completion_case (true, true);
}

// (b) with the socket's commands owned by the async executor (a monitor
// retains that owner): the stop may not be applied yet when the pull or the
// wait runs. Odd rounds publish the reply as a ready completion and pull it
// right after shutdown; even rounds leave it unpulled in the pipe. Neither
// may deliver the reply.
void test_unpulled_reply_on_async_owned_socket_is_terminated ()
{
    for (int round = 0; round != 30; ++round) {
        void *context = zlink_ctx_new ();
        TEST_ASSERT_NOT_NULL (context);
        void *router = zlink_socket (context, ZLINK_SOCKET_ROUTER);
        void *dealer = zlink_socket (context, ZLINK_SOCKET_DEALER);
        TEST_ASSERT_NOT_NULL (router);
        TEST_ASSERT_NOT_NULL (dealer);
        set_zero_linger (router);
        set_zero_linger (dealer);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK, zlink_set_routing_id (dealer, "shutdown-mon", 12));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_BIND_OK, zlink_bind (router, "inproc://poller-shutdown-mon"));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONNECT_OK,
          zlink_connect (dealer, "inproc://poller-shutdown-mon"));
        zlink_socket_monitor_open_options_t options;
        memset (&options, 0, sizeof (options));
        options.events = ZLINK_EVENT_ALL;
        void *monitor = zlink_socket_monitor_open (dealer, &options);
        TEST_ASSERT_NOT_NULL (monitor);
        void *poller = zlink_poller_new ();
        TEST_ASSERT_NOT_NULL (poller);
        const bool pull_first = (round % 2) != 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_add (poller, dealer, NULL, ZLINK_POLLCOMPLETION));

        zlink_msg_t request;
        init_part (&request, "shutdown-request");
        zlink_completion_id_t completion_id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_request (dealer, NULL, &request, 1, ZLINK_SEND_FLAGS_DONTWAIT,
                         120000, NULL, &completion_id));
        assert_part_consumed (&request);
        const zlink_routing_id_t *source_rid = NULL;
        zlink_reply_token_t reply_token = 0;
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        size_t part_flag = 1;
        const int router_timeout = 5000;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (router, ZLINK_OPT_RCVTIMEO, &router_timeout,
                            sizeof (router_timeout)));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_router_recv (router, &source_rid, &reply_token, &part, 1,
                             &part_flag, ZLINK_RECV_FLAGS_NONE));
        const zlink_routing_id_t rid = *source_rid;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        zlink_msg_t reply;
        init_part (&reply, "shutdown-reply");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK, zlink_reply (router, &rid, reply_token, &reply, 1));
        assert_part_consumed (&reply);
        if (pull_first) {
            // Publish the reply as a ready completion before shutdown.
            const wait_observation_t ready = wait_once (poller, 5000);
            TEST_ASSERT_EQUAL_INT (1, ready.rc);
            TEST_ASSERT_TRUE ((ready.events & ZLINK_POLLCOMPLETION) != 0);
        } else {
            msleep (5);
        }

        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (context));
        zlink_completion_t completion;
        init_empty_completion (&completion);
        if (pull_first) {
            errno = 0;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_TERMINATED,
              zlink_completion_recv (dealer, &completion,
                                     ZLINK_RECV_FLAGS_DONTWAIT));
            TEST_ASSERT_EQUAL_INT (ETERM, zlink_errno ());
            assert_empty_completion (completion);
        }
        assert_terminated_wait (wait_once (poller, 5000));
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_TERMINATED,
          zlink_completion_recv (dealer, &completion,
                                 ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (ETERM, zlink_errno ());
        assert_empty_completion (completion);

        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_poller_remove (poller, dealer));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (router));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
    }
}
#if defined(ZLINK_HAVE_WINDOWS)
void test_fd_and_timer_only_poller_waits_until_timeout ()
{
    TEST_IGNORE_MESSAGE ("POSIX pipe helper unavailable on Windows");
}

void test_ready_fd_on_same_poller_still_ends_with_eterm ()
{
    TEST_IGNORE_MESSAGE ("POSIX pipe helper unavailable on Windows");
}
#else
// (c) A poller without a socket source of the context is not affected.
void test_fd_and_timer_only_poller_waits_until_timeout ()
{
    void *context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *socket = zlink_socket (context, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);
    int fds[2];
    TEST_ASSERT_SUCCESS_ERRNO (pipe (fds));
    void *timer = zlink_timer_new ();
    TEST_ASSERT_NOT_NULL (timer);
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add_fd (poller, fds[0], NULL, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add_timer (poller, timer, NULL));

    std::atomic<bool> entered (false);
    wait_observation_t observed;
    std::thread waiter ([&] {
        entered.store (true, std::memory_order_release);
        observed = wait_once (poller, 300);
    });
    while (!entered.load (std::memory_order_acquire))
        std::this_thread::yield ();
    msleep (50);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (context));
    waiter.join ();
    TEST_ASSERT_EQUAL_INT (0, observed.rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, observed.error);
    TEST_ASSERT_GREATER_OR_EQUAL_INT64 (250, observed.elapsed_ms);

    const wait_observation_t later = wait_once (poller, 100);
    TEST_ASSERT_EQUAL_INT (0, later.rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, later.error);
    TEST_ASSERT_GREATER_OR_EQUAL_INT64 (80, later.elapsed_ms);

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_timer_destroy (&timer));
    TEST_ASSERT_SUCCESS_ERRNO (close (fds[0]));
    TEST_ASSERT_SUCCESS_ERRNO (close (fds[1]));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}

// (f) A ready fd on the same poller does not turn the wait into an event.
void test_ready_fd_on_same_poller_still_ends_with_eterm ()
{
    void *context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *socket = zlink_socket (context, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);
    int fds[2];
    TEST_ASSERT_SUCCESS_ERRNO (pipe (fds));
    const char byte = 'x';
    TEST_ASSERT_EQUAL_INT (1, static_cast<int> (write (fds[1], &byte, 1)));
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add_fd (poller, fds[0], NULL, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add (poller, socket, NULL, ZLINK_POLLIN));

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (context));
    assert_terminated_wait (wait_once (poller, 5000));
    assert_terminated_wait (wait_once (poller, 0));

    // Removing the socket source restores the fd event.
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, socket));
    const wait_observation_t fd_ready = wait_once (poller, 1000);
    TEST_ASSERT_EQUAL_INT (1, fd_ready.rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLIN, fd_ready.events);

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_SUCCESS_ERRNO (close (fds[0]));
    TEST_ASSERT_SUCCESS_ERRNO (close (fds[1]));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}
#endif

// (d) Closing the socket after shutdown reports POLLERR once, then the wait
// times out.
void test_close_after_shutdown_reports_pollerr_once ()
{
    void *context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *socket = zlink_socket (context, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add (poller, socket, NULL, ZLINK_POLLIN));

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (context));
    assert_terminated_wait (wait_once (poller, 5000));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));

    const wait_observation_t closed = wait_once (poller, 1000);
    TEST_ASSERT_EQUAL_INT (1, closed.rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLERR, closed.events);

    //  Only a closed socket that already reported POLLERR is left, so the
    //  wait does not wait for its timeout (Polling 9).
    const wait_observation_t after = wait_once (poller, 1000);
    TEST_ASSERT_EQUAL_INT (0, after.rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, after.error);
    TEST_ASSERT_LESS_THAN_INT64 (500, after.elapsed_ms);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, socket));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}

// (e) An unclosed monitor handle source of the context is a socket source.
void test_monitor_handle_source_ends_with_eterm ()
{
    void *context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *socket = zlink_socket (context, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (socket);
    zlink_socket_monitor_open_options_t options;
    memset (&options, 0, sizeof (options));
    options.events = ZLINK_EVENT_ALL;
    void *monitor = zlink_socket_monitor_open (socket, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add (poller, monitor, NULL, ZLINK_POLLIN));

    std::atomic<bool> entered (false);
    wait_observation_t observed;
    std::thread waiter ([&] {
        entered.store (true, std::memory_order_release);
        observed = wait_once (poller, -1);
    });
    while (!entered.load (std::memory_order_acquire))
        std::this_thread::yield ();
    msleep (50);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (context));
    waiter.join ();
    TEST_ASSERT_EQUAL_INT (-1, observed.rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INTERNAL_ERROR, observed.error);
    TEST_ASSERT_EQUAL_INT (ETERM, observed.err);
    assert_terminated_wait (wait_once (poller, 5000));

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, monitor));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}

// Blocking send on a socket whose commands are owned by the async executor
// (a monitor retains that owner) is released promptly by ctx shutdown.
void test_async_owned_blocking_send_released_by_ctx_shutdown ()
{
    void *context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *socket = zlink_socket (context, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);
    const int infinite_timeout = -1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket, ZLINK_OPT_SNDTIMEO, &infinite_timeout,
                        sizeof (infinite_timeout)));
    zlink_socket_monitor_open_options_t options;
    memset (&options, 0, sizeof (options));
    options.events = ZLINK_EVENT_ALL;
    void *monitor = zlink_socket_monitor_open (socket, &options);
    TEST_ASSERT_NOT_NULL (monitor);

    std::atomic<bool> entered (false);
    std::atomic<int> submit_result (ZLINK_SUBMIT_INTERNAL_ERROR);
    std::atomic<int> submit_errno (0);
    std::thread sender ([&] {
        zlink_msg_t part;
        init_part (&part, "async-owned-blocking-send");
        zlink_completion_id_t completion_id = UINT64_MAX;
        entered.store (true, std::memory_order_release);
        errno = 0;
        submit_result.store (zlink_send (socket, &part, 1,
                                         ZLINK_SEND_FLAGS_NONE, NULL,
                                         &completion_id));
        submit_errno.store (zlink_errno ());
        zlink_msg_close (&part);
    });
    while (!entered.load (std::memory_order_acquire))
        std::this_thread::yield ();
    msleep (50);
    const std::chrono::steady_clock::time_point shutdown_at =
      std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (context));
    sender.join ();
    const long after_shutdown_ms = static_cast<long> (
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now () - shutdown_at)
        .count ());
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_TERMINATED, submit_result.load ());
    TEST_ASSERT_EQUAL_INT (ETERM, submit_errno.load ());
    TEST_ASSERT_LESS_THAN_INT64 (kPromptMs, after_shutdown_ms);

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_indefinite_wait_ends_with_eterm_on_ctx_shutdown);
    RUN_TEST (test_ready_completion_registered_before_shutdown_is_terminated);
    RUN_TEST (test_ready_completion_registered_after_shutdown_is_terminated);
    RUN_TEST (test_ready_completion_pulled_right_after_shutdown_is_terminated);
    RUN_TEST (test_unpulled_reply_on_async_owned_socket_is_terminated);
    RUN_TEST (test_fd_and_timer_only_poller_waits_until_timeout);
    RUN_TEST (test_close_after_shutdown_reports_pollerr_once);
    RUN_TEST (test_monitor_handle_source_ends_with_eterm);
    RUN_TEST (test_ready_fd_on_same_poller_still_ends_with_eterm);
    RUN_TEST (test_async_owned_blocking_send_released_by_ctx_shutdown);
    return UNITY_END ();
}
