/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int wake_timeout_ms = 2000;
const size_t stress_iterations = 200;

enum pattern_t
{
    pair_pair,
    dealer_dealer,
    dealer_router,
    router_router,
    pub_sub
};

const char *pattern_name (pattern_t pattern_)
{
    switch (pattern_) {
        case pair_pair:
            return "PAIR";
        case dealer_dealer:
            return "DEALER";
        case dealer_router:
            return "ROUTER";
        case router_router:
            return "ROUTER_ROUTER";
        case pub_sub:
            return "SUB";
    }
    return "unknown";
}

void configure_socket (void *socket_)
{
    const int linger = 0;
    const int timeout = wake_timeout_ms;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &timeout,
                        sizeof (timeout)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &timeout,
                        sizeof (timeout)));
}

zlink_routing_id_t make_routing_id (const char *value_)
{
    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    const size_t size = strlen (value_);
    TEST_ASSERT_TRUE (size <= sizeof (rid.data));
    rid.size = static_cast<uint8_t> (size);
    memcpy (rid.data, value_, size);
    return rid;
}

struct fixture_t
{
    fixture_t (pattern_t pattern_, const char *transport_, size_t serial_) :
        pattern (pattern_),
        transport (transport_),
        receiver (NULL),
        sender (NULL),
        monitor (NULL),
        receiver_rid (make_routing_id ("wake-receiver"))
    {
        int receiver_type = ZLINK_SOCKET_PAIR;
        int sender_type = ZLINK_SOCKET_PAIR;
        switch (pattern) {
            case pair_pair:
                break;
            case dealer_dealer:
                receiver_type = ZLINK_SOCKET_DEALER;
                sender_type = ZLINK_SOCKET_DEALER;
                break;
            case dealer_router:
                receiver_type = ZLINK_SOCKET_ROUTER;
                sender_type = ZLINK_SOCKET_DEALER;
                break;
            case router_router:
                receiver_type = ZLINK_SOCKET_ROUTER;
                sender_type = ZLINK_SOCKET_ROUTER;
                break;
            case pub_sub:
                receiver_type = ZLINK_SOCKET_SUB;
                sender_type = ZLINK_SOCKET_PUB;
                break;
        }

        receiver = test_context_socket (
          static_cast<zlink_socket_type_t> (receiver_type));
        sender = test_context_socket (
          static_cast<zlink_socket_type_t> (sender_type));
        TEST_ASSERT_NOT_NULL (receiver);
        TEST_ASSERT_NOT_NULL (sender);
        configure_socket (receiver);
        configure_socket (sender);

        if (pattern == dealer_router || pattern == router_router) {
            TEST_ASSERT_EQUAL_INT (
              ZLINK_CONFIG_OK,
              zlink_set_routing_id (receiver, receiver_rid.data,
                                    receiver_rid.size));
            TEST_ASSERT_EQUAL_INT (
              ZLINK_CONFIG_OK,
              zlink_set_routing_id (sender, "wake-sender",
                                    strlen ("wake-sender")));
        }
        if (pattern == router_router) {
            TEST_ASSERT_EQUAL_INT (
              ZLINK_CONFIG_OK,
              zlink_set_router_option (
                sender, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                receiver_rid.data, receiver_rid.size));
        }
        if (pattern == pub_sub) {
            TEST_ASSERT_EQUAL_INT (
              ZLINK_CONFIG_OK, zlink_set_subscription (receiver, "wake"));
        }

        zlink_socket_monitor_open_options_t options;
        memset (&options, 0, sizeof (options));
        options.events = ZLINK_EVENT_CONNECTION_READY
                         | ZLINK_EVENT_BIND_FAILED
                         | ZLINK_EVENT_ACCEPT_FAILED
                         | ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL
                         | ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL
                         | ZLINK_EVENT_HANDSHAKE_FAILED_AUTH;
        monitor = zlink_socket_monitor_open (receiver, &options);
        TEST_ASSERT_NOT_NULL (monitor);

        void *const bound = pattern == pub_sub ? sender : receiver;
        void *const connected = pattern == pub_sub ? receiver : sender;
        char resolved[MAX_SOCKET_STRING];
        memset (resolved, 0, sizeof (resolved));
        if (strcmp (transport, "inproc") == 0) {
            snprintf (resolved, sizeof (resolved),
                      "inproc://wake-invariant-%u-%u",
                      static_cast<unsigned> (pattern),
                      static_cast<unsigned> (serial_));
            TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                                   zlink_bind (bound, resolved));
        } else if (strcmp (transport, "tcp") == 0) {
            test_bind (bound, "tcp://127.0.0.1:*", resolved,
                       sizeof (resolved));
        } else {
            TEST_ASSERT_EQUAL_STRING ("ipc", transport);
            test_bind (bound, "ipc://*", resolved, sizeof (resolved));
        }
        endpoint = resolved;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                               zlink_connect (connected, endpoint.c_str ()));
        TEST_ASSERT_TRUE_MESSAGE (wait_connection_ready (),
                                  "CONNECTION_READY was not observed");
    }

    bool wait_connection_ready ()
    {
        const std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::now ()
          + std::chrono::milliseconds (wake_timeout_ms);
        while (std::chrono::steady_clock::now () < deadline) {
            const int64_t remaining =
              std::chrono::duration_cast<std::chrono::milliseconds> (
                deadline - std::chrono::steady_clock::now ())
                .count ();
            zlink_pollitem_t item = {monitor, 0, ZLINK_POLLIN, 0};
            if (zlink_poll (&item, 1,
                            static_cast<long> (remaining > 0 ? remaining : 1),
                            NULL)
                != 1)
                return false;
            if ((item.revents & ZLINK_POLLIN) == 0)
                return false;

            for (;;) {
                zlink_socket_monitor_event_t event;
                memset (&event, 0, sizeof (event));
                const zlink_recv_result_t result = zlink_socket_monitor_recv (
                  monitor, &event, ZLINK_RECV_FLAGS_DONTWAIT);
                if (result == ZLINK_RECV_NO_DATA)
                    break;
                if (result != ZLINK_RECV_OK)
                    return false;
                if (event.event == ZLINK_EVENT_CONNECTION_READY)
                    return true;
                if (event.event == ZLINK_EVENT_BIND_FAILED
                    || event.event == ZLINK_EVENT_ACCEPT_FAILED
                    || event.event
                         == ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL
                    || event.event
                         == ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL
                    || event.event == ZLINK_EVENT_HANDSHAKE_FAILED_AUTH)
                    return false;
            }
        }
        return false;
    }

    void close_monitor ()
    {
        if (monitor) {
            TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                                   zlink_monitor_close (&monitor));
            TEST_ASSERT_NULL (monitor);
        }
    }

    bool send_one (const char *payload_, bool dontwait_)
    {
        zlink_msg_t part;
        if (zlink_msg_init_size (&part, strlen (payload_)) != ZLINK_CONFIG_OK)
            return false;
        memcpy (zlink_msg_data (&part), payload_, strlen (payload_));

        const zlink_send_flags_t flags =
          dontwait_ ? ZLINK_SEND_FLAGS_DONTWAIT : ZLINK_SEND_FLAGS_NONE;
        zlink_submit_result_t result;
        if (pattern == router_router) {
            result = zlink_send_part_rid (
              sender, &receiver_rid, &part, flags, ZLINK_PART_FINAL, NULL,
              NULL);
        } else if (pattern == pub_sub) {
            result = zlink_publish_part (sender, "wake", &part, flags,
                                         ZLINK_PART_FINAL);
        } else {
            result = zlink_send_part (sender, &part, flags,
                                      ZLINK_PART_FINAL, NULL, NULL);
        }
        const int saved_errno = zlink_errno ();
        zlink_msg_close (&part);
        errno = saved_errno;
        return result == ZLINK_SUBMIT_OK;
    }

    bool receive_one ()
    {
        zlink_msg_t part;
        if (zlink_msg_init (&part) != ZLINK_CONFIG_OK)
            return false;
        zlink_part_flag_t more = ZLINK_PART_MORE;
        zlink_recv_result_t result = ZLINK_RECV_INTERNAL_ERROR;
        if (pattern == dealer_router || pattern == router_router) {
            const zlink_routing_id_t *source = NULL;
            zlink_reply_token_t token = UINT64_MAX;
            result = zlink_router_recv_part (
              receiver, &source, &token, &part, &more,
              ZLINK_RECV_FLAGS_DONTWAIT);
        } else if (pattern == pub_sub) {
            const zlink_routing_id_t *source = NULL;
            char topic[32];
            size_t topic_size = 0;
            result = zlink_subscribe_part (
              receiver, &source, topic, sizeof (topic), &topic_size, &part,
              &more, ZLINK_RECV_FLAGS_DONTWAIT);
        } else {
            const zlink_routing_id_t *source = NULL;
            result = zlink_recv_part (receiver, &source, &part, &more,
                                      ZLINK_RECV_FLAGS_DONTWAIT);
        }
        const bool ok = result == ZLINK_RECV_OK
                        && more == ZLINK_PART_FINAL;
        zlink_msg_close (&part);
        return ok;
    }

    void prime_subscription ()
    {
        if (pattern != pub_sub)
            return;

        const std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::now ()
          + std::chrono::milliseconds (wake_timeout_ms);
        while (std::chrono::steady_clock::now () < deadline) {
            TEST_ASSERT_TRUE (send_one ("subscription-prime", false));
            zlink_pollitem_t item = {receiver, 0, ZLINK_POLLIN, 0};
            const int result = zlink_poll (&item, 1, 20, NULL);
            if (result == 1 && (item.revents & ZLINK_POLLIN) != 0
                && receive_one ())
                return;
        }
        TEST_FAIL_MESSAGE ("SUB subscription was not propagated");
    }

    void close ()
    {
        close_monitor ();
        sender = test_context_socket_close_zero_linger (sender);
        receiver = test_context_socket_close_zero_linger (receiver);
    }

    pattern_t pattern;
    const char *transport;
    void *receiver;
    void *sender;
    void *monitor;
    zlink_routing_id_t receiver_rid;
    std::string endpoint;
};

struct wait_result_t
{
    wait_result_t () : armed (false), count (-1), error (ZLINK_CONFIG_OK),
                      events (0), elapsed_ms (-1)
    {
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool armed;
    int count;
    zlink_config_result_t error;
    short events;
    int64_t elapsed_ms;
};

void wait_for_registered_poller_event (void *poller_, wait_result_t *out_)
{
    {
        std::lock_guard<std::mutex> lock (out_->mutex);
        out_->armed = true;
        out_->cv.notify_all ();
    }

    const std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now ();
    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    out_->count = zlink_poller_wait (poller_, &event, 1,
                                     wake_timeout_ms, &out_->error);
    out_->events = event.events;
    out_->elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now () - started)
        .count ();
}

void assert_wait_armed (wait_result_t *result_)
{
    std::unique_lock<std::mutex> lock (result_->mutex);
    TEST_ASSERT_TRUE_MESSAGE (
      result_->cv.wait_for (lock, std::chrono::milliseconds (wake_timeout_ms),
                            [result_] { return result_->armed; }),
      "waiter did not arm");
}

bool wait_until_poller_wait_is_active (void *poller_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (wake_timeout_ms);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
        const int size = zlink_poller_size (poller_, &error);
        if (size == -1 && error == ZLINK_CONFIG_BUSY)
            return true;
        if (size < 0)
            return false;
        std::this_thread::yield ();
    }
    return false;
}

void assert_wake_result (const wait_result_t &result_, short event_,
                         const char *label_)
{
    std::ostringstream message;
    message << label_ << ": count=" << result_.count
            << " events=" << result_.events
            << " error=" << static_cast<int> (result_.error)
            << " elapsed_ms=" << result_.elapsed_ms;
    TEST_ASSERT_TRUE_MESSAGE (
      result_.count == 1 && result_.error == ZLINK_CONFIG_OK
        && (result_.events & event_) != 0
        && result_.elapsed_ms >= 0
        && result_.elapsed_ms < wake_timeout_ms,
      message.str ().c_str ());
}

void run_monitor_close_first_message (pattern_t pattern_,
                                      const char *transport_,
                                      bool use_poller_, size_t serial_)
{
    fixture_t fixture (pattern_, transport_, serial_);
    void *poller = NULL;
    if (use_poller_) {
        poller = zlink_poller_new ();
        TEST_ASSERT_NOT_NULL (poller);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_add (poller, fixture.receiver, fixture.receiver,
                            ZLINK_POLLIN));
    }

    // The monitor command owner has observed CONNECTION_READY. Closing it
    // immediately before the first DATA record reproduces the detach/re-arm
    // boundary fixed by 1344022a3e.
    fixture.close_monitor ();
    TEST_ASSERT_TRUE (fixture.send_one ("first-after-monitor-close", false));

    const std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now ();
    int count = -1;
    short events = 0;
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    if (poller) {
        zlink_poller_event_t event;
        memset (&event, 0, sizeof (event));
        count = zlink_poller_wait (poller, &event, 1, wake_timeout_ms,
                                   &error);
        events = event.events;
    } else {
        zlink_pollitem_t item = {fixture.receiver, 0, ZLINK_POLLIN, 0};
        count = zlink_poll (&item, 1, wake_timeout_ms, &error);
        events = item.revents;
    }
    const int64_t elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now () - started)
        .count ();
    std::ostringstream message;
    message << pattern_name (pattern_) << '/' << transport_
            << " monitor-close first message: count=" << count
            << " events=" << events << " error=" << error
            << " elapsed_ms=" << elapsed;
    TEST_ASSERT_TRUE_MESSAGE (
      count == 1 && error == ZLINK_CONFIG_OK
        && (events & ZLINK_POLLIN) != 0 && elapsed >= 0
        && elapsed < wake_timeout_ms,
      message.str ().c_str ());
    TEST_ASSERT_TRUE (fixture.receive_one ());

    if (poller) {
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_poller_remove (poller,
                                                    fixture.receiver));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                               zlink_poller_destroy (&poller));
    }
    fixture.close ();
}

void test_monitor_close_first_message_wakes_public_waiters ()
{
    run_monitor_close_first_message (dealer_router, "inproc", false, 1);
    run_monitor_close_first_message (dealer_router, "tcp", true, 2);
    run_monitor_close_first_message (router_router, "inproc", true, 3);
}

void configure_hwm (void *socket_)
{
    const uint64_t hwm = 512;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
}

void run_hwm_pollout_recovery (bool keep_monitor_owner_, size_t serial_)
{
    fixture_t fixture (dealer_router, "inproc", serial_);

    // Runtime HWM changes are applied to the established pipe before fill.
    configure_hwm (fixture.sender);
    configure_hwm (fixture.receiver);
    if (!keep_monitor_owner_)
        fixture.close_monitor ();

    const std::string payload (64, 'h');
    size_t accepted = 0;
    int send_result = 0;
    for (; accepted != 4096; ++accepted) {
        errno = 0;
        send_result = zlink_send (fixture.sender, payload.data (),
                                  payload.size (), ZLINK_DONTWAIT);
        if (send_result == -1)
            break;
        TEST_ASSERT_EQUAL_INT (static_cast<int> (payload.size ()),
                               send_result);
    }
    TEST_ASSERT_TRUE_MESSAGE (accepted != 0 && accepted != 4096,
                              "DONTWAIT send did not reach HWM");
    TEST_ASSERT_EQUAL_INT (-1, send_result);
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    zlink_pollitem_t not_writable = {fixture.sender, 0, ZLINK_POLLOUT, 0};
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&not_writable, 1, 0, NULL));
    TEST_ASSERT_EQUAL_INT (0, not_writable.revents);

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, fixture.sender, fixture.sender,
                        ZLINK_POLLOUT));
    wait_result_t wake;
    std::thread waiter (wait_for_registered_poller_event, poller, &wake);
    assert_wait_armed (&wake);
    TEST_ASSERT_TRUE_MESSAGE (wait_until_poller_wait_is_active (poller),
                              "POLLOUT poller did not enter wait");

    for (size_t i = 0; i != accepted; ++i) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_routing_id_t *source = NULL;
        zlink_reply_token_t token = UINT64_MAX;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_router_recv (fixture.receiver, &source, &token, &parts,
                             &part_count, 0));
        TEST_ASSERT_EQUAL_UINT64 (1, part_count);
        zlink_multipart_close (parts, part_count);
    }
    waiter.join ();

    const char *const label = keep_monitor_owner_
                                ? "HWM recovery with monitor owner"
                                : "HWM recovery without monitor owner";
    assert_wake_result (wake, ZLINK_POLLOUT, label);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, fixture.sender));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    fixture.close ();
}

void test_hwm_eagain_drain_wakes_pollout_with_and_without_async_owner ()
{
    run_hwm_pollout_recovery (false, 4);
    run_hwm_pollout_recovery (true, 5);
}

struct stress_state_t
{
    stress_state_t () : armed_iteration (no_iteration),
                       completed_iteration (no_iteration), failed (false),
                       failure_iteration (no_iteration), wait_count (0),
                       wait_error (ZLINK_CONFIG_OK), wait_events (0),
                       elapsed_ms (-1), max_elapsed_ms (0)
    {
    }

    static const size_t no_iteration = std::numeric_limits<size_t>::max ();
    std::mutex mutex;
    std::condition_variable cv;
    size_t armed_iteration;
    size_t completed_iteration;
    bool failed;
    size_t failure_iteration;
    int wait_count;
    zlink_config_result_t wait_error;
    short wait_events;
    int64_t elapsed_ms;
    int64_t max_elapsed_ms;
};

void run_stress_waiter (fixture_t *fixture_, void *poller_,
                        stress_state_t *state_)
{
    for (size_t iteration = 0; iteration != stress_iterations; ++iteration) {
        {
            std::lock_guard<std::mutex> lock (state_->mutex);
            state_->armed_iteration = iteration;
            state_->cv.notify_all ();
        }

        zlink_poller_event_t event;
        memset (&event, 0, sizeof (event));
        zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
        const std::chrono::steady_clock::time_point started =
          std::chrono::steady_clock::now ();
        const int count = zlink_poller_wait (
          poller_, &event, 1, wake_timeout_ms, &error);
        const int64_t elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::steady_clock::now () - started)
            .count ();
        const bool received = count == 1 && error == ZLINK_CONFIG_OK
                              && (event.events & ZLINK_POLLIN) != 0
                              && fixture_->receive_one ();

        std::lock_guard<std::mutex> lock (state_->mutex);
        if (!received || elapsed >= wake_timeout_ms) {
            state_->failed = true;
            state_->failure_iteration = iteration;
            state_->wait_count = count;
            state_->wait_error = error;
            state_->wait_events = event.events;
            state_->elapsed_ms = elapsed;
            state_->cv.notify_all ();
            return;
        }
        if (elapsed > state_->max_elapsed_ms)
            state_->max_elapsed_ms = elapsed;
        state_->completed_iteration = iteration;
        state_->cv.notify_all ();
    }
}

void run_level_wake_stress (pattern_t pattern_, const char *transport_,
                            size_t serial_)
{
    fixture_t fixture (pattern_, transport_, serial_);
    fixture.prime_subscription ();
    fixture.close_monitor ();

    zlink_pollitem_t empty = {fixture.receiver, 0, ZLINK_POLLIN, 0};
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&empty, 1, 0, NULL));
    TEST_ASSERT_EQUAL_INT (0, empty.revents);

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, fixture.receiver, fixture.receiver,
                        ZLINK_POLLIN));

    stress_state_t state;
    std::thread waiter (run_stress_waiter, &fixture, poller, &state);
    bool main_failed = false;
    size_t main_failure_iteration = stress_state_t::no_iteration;
    int main_failure_errno = 0;
    for (size_t iteration = 0; iteration != stress_iterations; ++iteration) {
        {
            std::unique_lock<std::mutex> lock (state.mutex);
            if (!state.cv.wait_for (
                  lock, std::chrono::milliseconds (wake_timeout_ms),
                  [&] {
                      return state.failed
                             || state.armed_iteration == iteration;
                  })
                || state.failed) {
                main_failed = true;
                main_failure_iteration = iteration;
                break;
            }
        }
        if (!wait_until_poller_wait_is_active (poller)) {
            main_failed = true;
            main_failure_iteration = iteration;
            break;
        }

        if (!fixture.send_one ("level-transition", false)) {
            main_failed = true;
            main_failure_iteration = iteration;
            main_failure_errno = zlink_errno ();
            break;
        }

        std::unique_lock<std::mutex> lock (state.mutex);
        if (!state.cv.wait_for (
              lock, std::chrono::milliseconds (wake_timeout_ms + 250),
              [&] {
                  return state.failed
                         || state.completed_iteration == iteration;
              })
            || state.failed) {
            main_failed = true;
            main_failure_iteration = iteration;
            break;
        }
    }
    waiter.join ();

    std::ostringstream message;
    message << pattern_name (pattern_) << '/' << transport_
            << " false->true wake failed at iteration="
            << (state.failed ? state.failure_iteration
                             : main_failure_iteration)
            << " elapsed_ms=" << state.elapsed_ms
            << " count=" << state.wait_count
            << " events=" << state.wait_events
            << " wait_error=" << state.wait_error
            << " send_errno=" << main_failure_errno;
    TEST_ASSERT_FALSE_MESSAGE (main_failed || state.failed,
                               message.str ().c_str ());

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, fixture.receiver));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    fixture.close ();
}

void test_level_triggered_wake_stress_across_socket_transport_matrix ()
{
    const pattern_t patterns[] = {pair_pair, dealer_dealer, dealer_router,
                                  pub_sub};
    const char *const transports[] = {"inproc", "tcp", "ipc"};
    size_t serial = 100;
    for (size_t pattern_index = 0;
         pattern_index != sizeof (patterns) / sizeof (patterns[0]);
         ++pattern_index) {
        for (size_t transport_index = 0;
             transport_index != sizeof (transports) / sizeof (transports[0]);
             ++transport_index) {
#if !defined(ZLINK_HAVE_IPC)
            if (strcmp (transports[transport_index], "ipc") == 0)
                continue;
#endif
            run_level_wake_stress (patterns[pattern_index],
                                   transports[transport_index], serial++);
        }
    }
}
}

int main ()
{
    setup_test_environment (180);
    UNITY_BEGIN ();
    RUN_TEST (test_monitor_close_first_message_wakes_public_waiters);
    RUN_TEST (
      test_hwm_eagain_drain_wakes_pollout_with_and_without_async_owner);
    RUN_TEST (
      test_level_triggered_wake_stress_across_socket_transport_matrix);
    return UNITY_END ();
}
