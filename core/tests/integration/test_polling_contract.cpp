/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#if !defined(ZLINK_HAVE_WINDOWS)
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const short undefined_event_bit = 128;

void test_poll_timeout_and_socket_mask_validation ()
{
    void *pair = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (pair);

    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    zlink_pollitem_t item = {pair, 0, ZLINK_POLLIN, 123};
    errno = 0;
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&item, 1, 5, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_EQUAL_INT (0, item.revents);

    item.events = ZLINK_POLLCOMPLETION;
    TEST_ASSERT_EQUAL_INT (-1, zlink_poll (&item, 1, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_ARGUMENT, error);
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    item.events = undefined_event_bit;
    TEST_ASSERT_EQUAL_INT (-1, zlink_poll (&item, 1, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_ARGUMENT, error);
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    item.events = ZLINK_POLLITEMS_DFLT;
    TEST_ASSERT_EQUAL_INT (-1, zlink_poll (&item, 1, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_ARGUMENT, error);
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    item.events = ZLINK_POLLPRI;
    TEST_ASSERT_EQUAL_INT (-1, zlink_poll (&item, 1, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_NOT_SUPPORTED, error);
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);

    TEST_ASSERT_EQUAL_INT (0, zlink_poll (NULL, 0, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    test_context_socket_close_zero_linger (pair);
}

void test_poller_socket_registration_error_contracts ()
{
    void *poller = zlink_poller_new ();
    void *pair = test_context_socket (ZLINK_SOCKET_PAIR);
    void *missing = test_context_socket (ZLINK_SOCKET_PAIR);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_NOT_NULL (pair);
    TEST_ASSERT_NOT_NULL (missing);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (stream);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_poller_add (poller, pair, pair, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_CONFLICT,
      zlink_poller_add (poller, pair, pair, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (EEXIST, errno);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_FOUND,
      zlink_poller_modify (poller, missing, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (ENOENT, errno);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_FOUND, zlink_poller_remove (poller, missing));
    TEST_ASSERT_EQUAL_INT (ENOENT, errno);

    // Completion ownership can be added by modify, including as the only bit.
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_modify (poller, pair, ZLINK_POLLCOMPLETION));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_SUPPORTED,
      zlink_poller_modify (poller, pair, ZLINK_POLLPRI));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, pair));

    // Every completion-bearing socket accepts the bit through both add and
    // modify. Removing and restoring only the completion bit must not require
    // a connected endpoint or an already queued record.
    void *const completion_sockets[] = {pair, dealer, router, stream};
    for (size_t i = 0;
         i != sizeof (completion_sockets) / sizeof (completion_sockets[0]);
         ++i) {
        void *const socket = completion_sockets[i];
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_add (poller, socket, socket, ZLINK_POLLCOMPLETION));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_modify (
            poller, socket,
            static_cast<short> (ZLINK_POLLIN | ZLINK_POLLCOMPLETION)));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_modify (poller, socket, ZLINK_POLLIN));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_modify (poller, socket, ZLINK_POLLCOMPLETION));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_poller_remove (poller, socket));
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (stream);
    test_context_socket_close_zero_linger (router);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (missing);
    test_context_socket_close_zero_linger (pair);
}

#if defined(ZLINK_HAVE_WINDOWS)
void test_poller_fd_mask_and_registration_error_contracts ()
{
    TEST_IGNORE_MESSAGE ("POSIX pipe helper unavailable on Windows");
}
#else
void test_poller_fd_mask_and_registration_error_contracts ()
{
    int fds[2];
    TEST_ASSERT_SUCCESS_ERRNO (pipe (fds));
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_poller_add_fd (poller, fds[0], NULL, ZLINK_POLLCOMPLETION));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_poller_add_fd (poller, fds[0], NULL, undefined_event_bit));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add_fd (poller, fds[0], NULL,
                           ZLINK_POLLIN | ZLINK_POLLPRI));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_CONFLICT,
      zlink_poller_add_fd (poller, fds[0], NULL, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (EEXIST, errno);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_FOUND,
      zlink_poller_modify_fd (poller, fds[1], ZLINK_POLLOUT));
    TEST_ASSERT_EQUAL_INT (ENOENT, errno);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_FOUND, zlink_poller_remove_fd (poller, fds[1]));
    TEST_ASSERT_EQUAL_INT (ENOENT, errno);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove_fd (poller, fds[0]));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_SUCCESS_ERRNO (close (fds[0]));
    TEST_ASSERT_SUCCESS_ERRNO (close (fds[1]));
}
#endif

void test_timer_event_hides_internal_fd_and_reports_registration_errors ()
{
    void *poller = zlink_poller_new ();
    void *timer = zlink_timer_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_NOT_NULL (timer);

    int user_tag = 9;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_poller_add_timer (poller, timer, &user_tag));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_CONFLICT,
      zlink_poller_add_timer (poller, timer, &user_tag));
    TEST_ASSERT_EQUAL_INT (EEXIST, errno);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_timer_start (timer, 1000000ULL, 1));
    zlink_poller_event_t event;
    memset (&event, 0xff, sizeof (event));
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poller_wait (poller, &event, 1, 1000, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLER_SOURCE_TIMER, event.source_kind);
    TEST_ASSERT_NULL (event.socket);
    TEST_ASSERT_EQUAL_INT (0, event.fd);
    TEST_ASSERT_EQUAL_PTR (timer, event.timer);
    TEST_ASSERT_EQUAL_PTR (&user_tag, event.user_data);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLIN, event.events);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove_timer (poller, timer));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_FOUND,
      zlink_poller_remove_timer (poller, timer));
    TEST_ASSERT_EQUAL_INT (ENOENT, errno);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_timer_destroy (&timer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
}

void expect_empty_poller_wait_returns_now (void *poller_, long timeout_)
{
    zlink_poller_event_t event = {};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    errno = EINVAL;
    const std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (poller_, &event, 1, timeout_, &error));
    const long long elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now () - started)
        .count ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_EQUAL_INT (0, errno);
    TEST_ASSERT_TRUE (elapsed_ms < 1000);
}

void test_poller_without_sources_returns_without_waiting ()
{
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    expect_empty_poller_wait_returns_now (poller, -1);
    expect_empty_poller_wait_returns_now (poller, 5000);

    void *pair = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add (poller, pair, NULL, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, pair));
    expect_empty_poller_wait_returns_now (poller, -1);

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (pair);
}

long long elapsed_since (std::chrono::steady_clock::time_point started_)
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (
             std::chrono::steady_clock::now () - started_)
      .count ();
}

//  A timer is a source that can become ready: the wait lasts until it fires,
//  and an idle timer keeps a finite wait for its whole timeout.
void test_poller_with_only_timer_waits ()
{
    void *poller = zlink_poller_new ();
    void *timer = zlink_timer_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_NOT_NULL (timer);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add_timer (poller, timer, NULL));
    zlink_poller_event_t event = {};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;

    std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (poller, &event, 1, 300, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_TRUE (elapsed_since (started) >= 250);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_timer_start (timer, 200000000ULL, 1));
    started = std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (poller, &event, 1, -1, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLER_SOURCE_TIMER, event.source_kind);
    TEST_ASSERT_TRUE (elapsed_since (started) >= 150);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove_timer (poller, timer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_timer_destroy (&timer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
}

void expect_poller_wait_returns_now (void *poller_, long timeout_,
                                     int expected_, short expected_events_)
{
    zlink_poller_event_t event = {};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    const std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (expected_,
                           zlink_poller_wait (poller_, &event, 1, timeout_, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_TRUE (elapsed_since (started) < 1000);
    if (expected_ > 0)
        TEST_ASSERT_EQUAL_INT (expected_events_, event.events);
}

//  A registration without events can never become ready.
void test_poller_with_only_zero_event_registration_returns_now ()
{
    void *pair = test_context_socket (ZLINK_SOCKET_PAIR);
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_add (poller, pair, NULL, 0));
    expect_poller_wait_returns_now (poller, -1, 0, 0);
    expect_poller_wait_returns_now (poller, 5000, 0, 0);

    zlink_pollitem_t item = {pair, 0, 0, 123};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    const std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&item, 1, -1, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_EQUAL_INT (0, item.revents);
    TEST_ASSERT_TRUE (elapsed_since (started) < 1000);

    //  Closing the socket still reports its POLLERR once (Polling 5).
    test_context_socket_close_zero_linger (pair);
    expect_poller_wait_returns_now (poller, -1, 1, ZLINK_POLLERR);
    expect_poller_wait_returns_now (poller, -1, 0, 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, pair));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
}

//  A closed socket reports its POLLERR once; after that nothing registered
//  can become ready, whether it closed before or after the first wait.
void test_poller_with_only_closed_socket_returns_now ()
{
    for (int closed_after_first_wait = 0; closed_after_first_wait < 2;
         ++closed_after_first_wait) {
        void *pair = test_context_socket (ZLINK_SOCKET_PAIR);
        void *poller = zlink_poller_new ();
        TEST_ASSERT_NOT_NULL (poller);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_poller_add (poller, pair, NULL, ZLINK_POLLIN));
        if (closed_after_first_wait)
            expect_poller_wait_returns_now (poller, 0, 0, 0);
        test_context_socket_close_zero_linger (pair);
        expect_poller_wait_returns_now (poller, -1, 1, ZLINK_POLLERR);
        expect_poller_wait_returns_now (poller, 5000, 0, 0);
        expect_poller_wait_returns_now (poller, -1, 0, 0);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, pair));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    }
}

#if defined(ZLINK_HAVE_WINDOWS)
void test_poller_with_only_fd_waits ()
{
    TEST_IGNORE_MESSAGE ("POSIX pipe helper unavailable on Windows");
}
#else
//  An fd is a source that can become ready: the wait lasts until it is
//  readable, and an idle fd keeps a finite wait for its whole timeout.
void test_poller_with_only_fd_waits ()
{
    int fds[2];
    TEST_ASSERT_SUCCESS_ERRNO (pipe (fds));
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add_fd (poller, fds[0], NULL, ZLINK_POLLIN));
    zlink_poller_event_t event = {};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;

    std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (poller, &event, 1, 300, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_TRUE (elapsed_since (started) >= 250);

    std::thread writer ([&] {
        std::this_thread::sleep_for (std::chrono::milliseconds (200));
        const char byte = 'x';
        const ssize_t written = write (fds[1], &byte, 1);
        (void) written;
    });
    started = std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (poller, &event, 1, -1, &error));
    const long long elapsed_ms = elapsed_since (started);
    writer.join ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLER_SOURCE_FD, event.source_kind);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLIN, event.events);
    TEST_ASSERT_TRUE (elapsed_ms >= 150);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove_fd (poller, fds[0]));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_SUCCESS_ERRNO (close (fds[0]));
    TEST_ASSERT_SUCCESS_ERRNO (close (fds[1]));
}
#endif

#if !defined(ZLINK_HAVE_WINDOWS)
std::atomic<int> interrupt_count (0);

void count_interrupt (int)
{
    interrupt_count.fetch_add (1);
}

struct interrupted_wait_t
{
    void *poller; //  NULL: zlink_poll on socket
    void *socket;
    long timeout_ms;
    std::atomic<bool> done;
    int rc;
    zlink_config_result_t error;
    zlink_poller_event_t event;
    long long elapsed_ms;
};

//  Interrupts the waiting thread with SIGURG every 10 ms until the wait ends
//  or the budget runs out, so every system wait inside it sees EINTR.
void interrupt_wait (std::thread &waiter_, interrupted_wait_t &wait_,
                     int budget_ms_)
{
    const pthread_t target = waiter_.native_handle ();
    for (int elapsed = 0; elapsed < budget_ms_ && !wait_.done.load ();
         elapsed += 10) {
        TEST_ASSERT_EQUAL_INT (0, pthread_kill (target, SIGURG));
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
}

void run_wait (interrupted_wait_t *wait_)
{
    const std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now ();
    if (wait_->poller)
        wait_->rc = zlink_poller_wait (wait_->poller, &wait_->event, 1,
                                       wait_->timeout_ms, &wait_->error);
    else {
        zlink_pollitem_t item = {wait_->socket, 0, ZLINK_POLLIN, 0};
        wait_->rc = zlink_poll (&item, 1, wait_->timeout_ms, &wait_->error);
    }
    wait_->elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                          std::chrono::steady_clock::now () - started)
                          .count ();
    wait_->done.store (true);
}

void test_poller_wait_resumes_after_signal ()
{
    struct sigaction action = {};
    struct sigaction previous = {};
    action.sa_handler = count_interrupt;
    sigemptyset (&action.sa_mask);
    TEST_ASSERT_EQUAL_INT (0, sigaction (SIGURG, &action, &previous));

    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (receiver, "inproc://poller-signal-resume"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (sender, "inproc://poller-signal-resume"));
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add (poller, receiver, NULL, ZLINK_POLLIN));

    //  A finite wait keeps its original deadline and ends as a timeout.
    interrupted_wait_t finite;
    finite.poller = poller;
    finite.socket = NULL;
    finite.timeout_ms = 300;
    finite.done.store (false);
    finite.rc = -2;
    finite.error = ZLINK_CONFIG_INTERNAL_ERROR;
    interrupt_count.store (0);
    std::thread finite_waiter (run_wait, &finite);
    interrupt_wait (finite_waiter, finite, 2000);
    finite_waiter.join ();
    TEST_ASSERT_TRUE (interrupt_count.load () > 0);
    TEST_ASSERT_EQUAL_INT (0, finite.rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, finite.error);
    TEST_ASSERT_TRUE (finite.elapsed_ms >= 250);
    TEST_ASSERT_TRUE (finite.elapsed_ms < 2000);

    //  An infinite wait keeps waiting through signals until the event.
    interrupted_wait_t infinite;
    infinite.poller = poller;
    infinite.socket = NULL;
    infinite.timeout_ms = -1;
    infinite.done.store (false);
    infinite.rc = -2;
    infinite.error = ZLINK_CONFIG_INTERNAL_ERROR;
    interrupt_count.store (0);
    std::thread infinite_waiter (run_wait, &infinite);
    interrupt_wait (infinite_waiter, infinite, 200);
    TEST_ASSERT_TRUE (interrupt_count.load () > 0);
    send_string_expect_success (sender, "wake", 0);
    infinite_waiter.join ();
    TEST_ASSERT_EQUAL_INT (1, infinite.rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, infinite.error);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLIN, infinite.event.events);

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
    TEST_ASSERT_EQUAL_INT (0, sigaction (SIGURG, &previous, NULL));
}

void test_poll_resumes_after_signal ()
{
    struct sigaction action = {};
    struct sigaction previous = {};
    action.sa_handler = count_interrupt;
    sigemptyset (&action.sa_mask);
    TEST_ASSERT_EQUAL_INT (0, sigaction (SIGURG, &action, &previous));
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (receiver, "inproc://poll-signal-resume"));

    //  zlink_poll shares the same wait loop and keeps its deadline too.
    interrupted_wait_t poll_once;
    poll_once.poller = NULL;
    poll_once.socket = receiver;
    poll_once.timeout_ms = 300;
    poll_once.done.store (false);
    poll_once.rc = -2;
    poll_once.error = ZLINK_CONFIG_INTERNAL_ERROR;
    interrupt_count.store (0);
    std::thread poll_waiter (run_wait, &poll_once);
    interrupt_wait (poll_waiter, poll_once, 2000);
    poll_waiter.join ();
    TEST_ASSERT_TRUE (interrupt_count.load () > 0);
    TEST_ASSERT_EQUAL_INT (0, poll_once.rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_once.error);
    TEST_ASSERT_TRUE (poll_once.elapsed_ms >= 250);
    TEST_ASSERT_TRUE (poll_once.elapsed_ms < 2000);

    test_context_socket_close_zero_linger (receiver);
    TEST_ASSERT_EQUAL_INT (0, sigaction (SIGURG, &previous, NULL));
}
#endif
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_poll_timeout_and_socket_mask_validation);
    RUN_TEST (test_poller_socket_registration_error_contracts);
    RUN_TEST (test_poller_fd_mask_and_registration_error_contracts);
    RUN_TEST (test_timer_event_hides_internal_fd_and_reports_registration_errors);
    RUN_TEST (test_poller_without_sources_returns_without_waiting);
    RUN_TEST (test_poller_with_only_timer_waits);
    RUN_TEST (test_poller_with_only_fd_waits);
    RUN_TEST (test_poller_with_only_zero_event_registration_returns_now);
    RUN_TEST (test_poller_with_only_closed_socket_returns_now);
#if !defined(ZLINK_HAVE_WINDOWS)
    RUN_TEST (test_poller_wait_resumes_after_signal);
    RUN_TEST (test_poll_resumes_after_signal);
#endif
    return UNITY_END ();
}
