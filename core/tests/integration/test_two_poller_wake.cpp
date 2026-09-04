/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(ZLINK_HAVE_WINDOWS)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const size_t stress_iterations = 100;
const uint64_t watchdog_interval_ns = 250 * 1000 * 1000ULL;
const long receive_wait_timeout_ms = 2000;
const long completion_wait_timeout_ms = 10000;
const int coordination_timeout_ms = 3000;

const size_t no_iteration = std::numeric_limits<size_t>::max ();

std::string payload_for (const char *label_, size_t iteration_)
{
    std::ostringstream payload;
    payload << label_ << '-' << iteration_;
    return payload.str ();
}

bool wait_until_poller_wait_is_active (void *poller_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (coordination_timeout_ms);
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

bool send_message (void *socket_, const std::string &payload_)
{
    zlink_msg_t part;
    if (zlink_msg_init_size (&part, payload_.size ()) != ZLINK_CONFIG_OK)
        return false;
    if (!payload_.empty ())
        memcpy (zlink_msg_data (&part), payload_.data (), payload_.size ());

    const zlink_submit_result_t result =
      zlink_send_part (socket_, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL, NULL);
    const int saved_errno = zlink_errno ();
    zlink_msg_close (&part);
    errno = saved_errno;
    return result == ZLINK_SUBMIT_OK;
}

zlink_recv_result_t
receive_message_result (void *socket_, bool router_, zlink_msg_t *part_, zlink_part_flag_t *more_)
{
    if (router_) {
        const zlink_routing_id_t *source = NULL;
        zlink_reply_token_t reply_token = UINT64_MAX;
        return zlink_router_recv_part (socket_, &source, &reply_token, part_, more_,
                                       ZLINK_RECV_FLAGS_DONTWAIT);
    }

    const zlink_routing_id_t *source = NULL;
    return zlink_recv_part (socket_, &source, part_, more_, ZLINK_RECV_FLAGS_DONTWAIT);
}

bool receive_message (void *socket_, bool router_, const std::string &expected_)
{
    zlink_msg_t part;
    if (zlink_msg_init (&part) != ZLINK_CONFIG_OK)
        return false;
    zlink_part_flag_t more = ZLINK_PART_MORE;
    const zlink_recv_result_t result = receive_message_result (socket_, router_, &part, &more);
    const bool ok =
      result == ZLINK_RECV_OK && more == ZLINK_PART_FINAL
      && zlink_msg_size (&part) == expected_.size ()
      && (expected_.empty ()
          || memcmp (zlink_msg_data (&part), expected_.data (), expected_.size ()) == 0);
    zlink_msg_close (&part);
    return ok;
}

bool message_receiver_is_empty (void *socket_, bool router_)
{
    zlink_msg_t part;
    if (zlink_msg_init (&part) != ZLINK_CONFIG_OK)
        return false;
    zlink_part_flag_t more = ZLINK_PART_MORE;
    const zlink_recv_result_t result = receive_message_result (socket_, router_, &part, &more);
    zlink_msg_close (&part);
    return result == ZLINK_RECV_NO_DATA;
}

void configure_socket (void *socket_)
{
    const int linger = 0;
    const int timeout = coordination_timeout_ms;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (socket_, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &timeout, sizeof (timeout)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &timeout, sizeof (timeout)));
}

void prime_message_connection (void *sender_, void *receiver_, bool router_, const char *label_)
{
    const std::string payload = payload_for (label_, no_iteration);
    TEST_ASSERT_TRUE (send_message (sender_, payload));

    zlink_pollitem_t item = {receiver_, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&item, 1, coordination_timeout_ms, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLIN, item.revents);
    TEST_ASSERT_TRUE (receive_message (receiver_, router_, payload));
    TEST_ASSERT_TRUE (message_receiver_is_empty (receiver_, router_));
}

struct wait_observation_t
{
    wait_observation_t () :
        count (-1),
        error (ZLINK_CONFIG_OK),
        saved_errno (0),
        socket_ready (false),
        watchdog_ready (false),
        unexpected_event (false),
        consumed (false),
        elapsed_ms (-1)
    {
    }

    int count;
    zlink_config_result_t error;
    int saved_errno;
    bool socket_ready;
    bool watchdog_ready;
    bool unexpected_event;
    bool consumed;
    int64_t elapsed_ms;
};

struct receive_state_t
{
    receive_state_t () :
        armed_iteration (no_iteration),
        completed_iteration (no_iteration),
        acknowledged_iteration (no_iteration),
        stop (false)
    {
    }

    std::mutex mutex;
    std::condition_variable cv;
    size_t armed_iteration;
    size_t completed_iteration;
    size_t acknowledged_iteration;
    bool stop;
    wait_observation_t observation;
};

struct completion_observation_t
{
    completion_observation_t () :
        count (-1),
        error (ZLINK_CONFIG_OK),
        saved_errno (0),
        stop_timer_ready (false),
        unexpected_event (false)
    {
    }

    int count;
    zlink_config_result_t error;
    int saved_errno;
    bool stop_timer_ready;
    bool unexpected_event;
};

struct harness_result_t
{
    bool ok;
    std::string diagnostic;
};

typedef std::function<bool (size_t)> transition_fn_t;

void run_receive_waiter (void *receiver_,
                         void *poller_,
                         void *watchdog_,
                         const transition_fn_t &consume_,
                         receive_state_t *state_)
{
    for (size_t iteration = 0; iteration != stress_iterations; ++iteration) {
        {
            std::lock_guard<std::mutex> lock (state_->mutex);
            if (state_->stop)
                return;
            state_->armed_iteration = iteration;
            state_->cv.notify_all ();
        }

        zlink_poller_event_t events[2];
        memset (events, 0, sizeof (events));
        zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
        const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now ();
        const int count = zlink_poller_wait (poller_, events, 2, receive_wait_timeout_ms, &error);
        const int saved_errno = zlink_errno ();

        wait_observation_t observation;
        observation.count = count;
        observation.error = error;
        observation.saved_errno = saved_errno;
        observation.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                                   std::chrono::steady_clock::now () - started)
                                   .count ();

        if (count > 0) {
            for (int i = 0; i != count; ++i) {
                if (events[i].source_kind == ZLINK_POLLER_SOURCE_SOCKET
                    && events[i].socket == receiver_ && (events[i].events & ZLINK_POLLIN) != 0) {
                    observation.socket_ready = true;
                } else if (events[i].source_kind == ZLINK_POLLER_SOURCE_TIMER
                           && events[i].timer == watchdog_
                           && (events[i].events & ZLINK_POLLIN) != 0) {
                    observation.watchdog_ready = true;
                } else {
                    observation.unexpected_event = true;
                }
            }
        }
        if (observation.socket_ready)
            observation.consumed = consume_ (iteration);

        std::unique_lock<std::mutex> lock (state_->mutex);
        state_->observation = observation;
        state_->completed_iteration = iteration;
        state_->cv.notify_all ();
        state_->cv.wait (
          lock, [&] { return state_->stop || state_->acknowledged_iteration == iteration; });
        if (state_->stop)
            return;
    }
}

void run_completion_waiter (void *poller_,
                            void *stop_timer_,
                            completion_observation_t *observation_)
{
    zlink_poller_event_t events[2];
    memset (events, 0, sizeof (events));
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    const int count = zlink_poller_wait (poller_, events, 2, completion_wait_timeout_ms, &error);

    observation_->count = count;
    observation_->error = error;
    observation_->saved_errno = zlink_errno ();
    if (count <= 0)
        return;

    for (int i = 0; i != count; ++i) {
        if (events[i].source_kind == ZLINK_POLLER_SOURCE_TIMER && events[i].timer == stop_timer_
            && (events[i].events & ZLINK_POLLIN) != 0) {
            observation_->stop_timer_ready = true;
        } else {
            observation_->unexpected_event = true;
        }
    }
}

harness_result_t run_two_poller_wake_case (void *receiver_,
                                           const char *label_,
                                           const transition_fn_t &produce_,
                                           const transition_fn_t &consume_,
                                           bool completion_primary_,
                                           bool remove_primary_before_wait_ = false)
{
    void *departing_primary_poller = remove_primary_before_wait_ ? zlink_poller_new () : NULL;
    void *receive_poller = zlink_poller_new ();
    void *completion_poller = zlink_poller_new ();
    void *watchdog = zlink_timer_new ();
    void *completion_stop_timer = zlink_timer_new ();

    if (remove_primary_before_wait_)
        TEST_ASSERT_NOT_NULL (departing_primary_poller);
    TEST_ASSERT_NOT_NULL (receive_poller);
    TEST_ASSERT_NOT_NULL (completion_poller);
    TEST_ASSERT_NOT_NULL (watchdog);
    TEST_ASSERT_NOT_NULL (completion_stop_timer);
    if (remove_primary_before_wait_) {
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_add (departing_primary_poller, receiver_, receiver_, ZLINK_POLLIN));
        zlink_poller_event_t primary_setup_event;
        memset (&primary_setup_event, 0, sizeof (primary_setup_event));
        TEST_ASSERT_EQUAL_INT (
          0, zlink_poller_wait (departing_primary_poller, &primary_setup_event, 1, 0, NULL));
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add (receive_poller, receiver_, receiver_, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add_timer (receive_poller, watchdog, watchdog));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_add (completion_poller, receiver_,
                                                              receiver_, ZLINK_POLLCOMPLETION));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add_timer (completion_poller, completion_stop_timer, completion_stop_timer));

    // The first poller rebuilt for a socket keeps the primary mailbox
    // descriptor. Later pollers get independent secondary wake channels.
    // Rebuild in the requested order so both orientations stay covered.
    zlink_poller_event_t setup_events[2];
    memset (setup_events, 0, sizeof (setup_events));
    if (completion_primary_) {
        TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (completion_poller, setup_events, 2, 0, NULL));
        TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (receive_poller, setup_events, 2, 0, NULL));
    } else {
        TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (receive_poller, setup_events, 2, 0, NULL));
        TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (completion_poller, setup_events, 2, 0, NULL));
    }

    // Keep both real waiters on secondary channels while removing the poller
    // that owned the primary descriptor. This exercises the all-secondary
    // drain path without letting a later registration reclaim the shared FD.
    if (remove_primary_before_wait_) {
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_poller_remove (departing_primary_poller, receiver_));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&departing_primary_poller));

        // Existing secondary refs keep this replacement on a secondary
        // channel too. After it leaves, both real waiters remain in the
        // deterministic all-secondary epoch exercised below.
        void *replacement_poller = zlink_poller_new ();
        TEST_ASSERT_NOT_NULL (replacement_poller);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_add (replacement_poller, receiver_,
                                                                  receiver_, ZLINK_POLLIN));
        zlink_poller_event_t replacement_setup_event;
        memset (&replacement_setup_event, 0, sizeof (replacement_setup_event));
        TEST_ASSERT_EQUAL_INT (
          0, zlink_poller_wait (replacement_poller, &replacement_setup_event, 1, 0, NULL));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_poller_remove (replacement_poller, receiver_));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&replacement_poller));
    }

    receive_state_t receive_state;
    completion_observation_t completion_observation;
    std::thread completion_thread (run_completion_waiter, completion_poller, completion_stop_timer,
                                   &completion_observation);
    std::thread receive_thread (run_receive_waiter, receiver_, receive_poller, watchdog,
                                std::cref (consume_), &receive_state);

    bool ok = true;
    std::string failure;
    size_t failure_iteration = no_iteration;
    wait_observation_t failure_observation;

    if (!wait_until_poller_wait_is_active (completion_poller)) {
        ok = false;
        failure = "POLLCOMPLETION waiter did not enter wait";
    }

    for (size_t iteration = 0; ok && iteration != stress_iterations; ++iteration) {
        {
            std::unique_lock<std::mutex> lock (receive_state.mutex);
            if (!receive_state.cv.wait_for (
                  lock, std::chrono::milliseconds (coordination_timeout_ms), [&] {
                      return receive_state.stop || receive_state.armed_iteration == iteration;
                  })) {
                ok = false;
                failure = "POLLIN waiter did not arm";
                failure_iteration = iteration;
            }
        }
        if (!ok)
            break;

        if (!wait_until_poller_wait_is_active (receive_poller)
            || !wait_until_poller_wait_is_active (completion_poller)) {
            ok = false;
            failure = "both poller waits were not concurrently active";
            failure_iteration = iteration;
            break;
        }

        if (zlink_timer_start (watchdog, watchdog_interval_ns, 1) != ZLINK_CONFIG_OK) {
            ok = false;
            failure = "failed to start POLLIN watchdog";
            failure_iteration = iteration;
        }
        if (ok && !produce_ (iteration)) {
            ok = false;
            failure = "producer failed";
            failure_iteration = iteration;
        }

        bool completed = false;
        {
            std::unique_lock<std::mutex> lock (receive_state.mutex);
            completed = receive_state.cv.wait_for (
              lock, std::chrono::milliseconds (receive_wait_timeout_ms + coordination_timeout_ms),
              [&] { return receive_state.completed_iteration == iteration; });
            if (completed)
                failure_observation = receive_state.observation;
        }
        (void) zlink_timer_stop (watchdog);

        if (!completed && ok) {
            ok = false;
            failure = "POLLIN waiter did not complete";
            failure_iteration = iteration;
        } else if (completed && ok) {
            const wait_observation_t &observed = failure_observation;
            if (observed.count <= 0 || observed.error != ZLINK_CONFIG_OK || !observed.socket_ready
                || observed.watchdog_ready || observed.unexpected_event || !observed.consumed) {
                ok = false;
                failure = observed.watchdog_ready ? "POLLIN required the watchdog wake"
                                                  : "POLLIN wait returned an invalid result";
                failure_iteration = iteration;
            }
        }

        {
            std::lock_guard<std::mutex> lock (receive_state.mutex);
            receive_state.acknowledged_iteration = iteration;
            receive_state.cv.notify_all ();
        }
    }

    {
        std::lock_guard<std::mutex> lock (receive_state.mutex);
        receive_state.stop = true;
        receive_state.cv.notify_all ();
    }
    const zlink_config_result_t stop_timer_start =
      zlink_timer_start (completion_stop_timer, 1000 * 1000ULL, 1);

    receive_thread.join ();
    completion_thread.join ();

    if (ok && stop_timer_start != ZLINK_CONFIG_OK) {
        ok = false;
        failure = "failed to start completion-wait stop timer";
    }
    if (ok
        && (completion_observation.count <= 0 || completion_observation.error != ZLINK_CONFIG_OK
            || !completion_observation.stop_timer_ready
            || completion_observation.unexpected_event)) {
        ok = false;
        failure = "POLLCOMPLETION waiter exited unexpectedly";
    }

    bool cleanup_ok = true;
    cleanup_ok = zlink_timer_stop (watchdog) == ZLINK_CONFIG_OK && cleanup_ok;
    cleanup_ok = zlink_timer_stop (completion_stop_timer) == ZLINK_CONFIG_OK && cleanup_ok;
    cleanup_ok =
      zlink_poller_remove_timer (receive_poller, watchdog) == ZLINK_CONFIG_OK && cleanup_ok;
    cleanup_ok = zlink_poller_remove (receive_poller, receiver_) == ZLINK_CONFIG_OK && cleanup_ok;
    cleanup_ok =
      zlink_poller_remove_timer (completion_poller, completion_stop_timer) == ZLINK_CONFIG_OK
      && cleanup_ok;
    cleanup_ok =
      zlink_poller_remove (completion_poller, receiver_) == ZLINK_CONFIG_OK && cleanup_ok;
    cleanup_ok = zlink_timer_destroy (&watchdog) == ZLINK_CLOSE_OK && cleanup_ok;
    cleanup_ok = zlink_timer_destroy (&completion_stop_timer) == ZLINK_CLOSE_OK && cleanup_ok;
    cleanup_ok = zlink_poller_destroy (&receive_poller) == ZLINK_CLOSE_OK && cleanup_ok;
    cleanup_ok = zlink_poller_destroy (&completion_poller) == ZLINK_CLOSE_OK && cleanup_ok;
    if (ok && !cleanup_ok) {
        ok = false;
        failure = "poller/timer cleanup failed";
    }

    std::ostringstream diagnostic;
    diagnostic << label_ << ": " << (failure.empty () ? "ok" : failure);
    if (failure_iteration != no_iteration) {
        diagnostic << " iteration=" << failure_iteration << " count=" << failure_observation.count
                   << " events_socket=" << failure_observation.socket_ready
                   << " events_watchdog=" << failure_observation.watchdog_ready
                   << " unexpected=" << failure_observation.unexpected_event
                   << " consumed=" << failure_observation.consumed
                   << " error=" << failure_observation.error
                   << " errno=" << failure_observation.saved_errno
                   << " elapsed_ms=" << failure_observation.elapsed_ms;
    }
    diagnostic << " completion_count=" << completion_observation.count
               << " completion_error=" << completion_observation.error
               << " completion_errno=" << completion_observation.saved_errno
               << " completion_stop=" << completion_observation.stop_timer_ready
               << " cleanup=" << cleanup_ok;
    return harness_result_t{ok, diagnostic.str ()};
}

void run_message_socket_case (int receiver_type_,
                              int sender_type_,
                              bool router_receive_,
                              const char *label_,
                              const char *endpoint_)
{
    void *receiver = test_context_socket (receiver_type_);
    void *sender = test_context_socket (sender_type_);
    TEST_ASSERT_NOT_NULL (receiver);
    TEST_ASSERT_NOT_NULL (sender);
    configure_socket (receiver);
    configure_socket (sender);

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (receiver, endpoint_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (sender, endpoint_));
    prime_message_connection (sender, receiver, router_receive_, label_);

    const transition_fn_t produce = [sender, label_] (size_t iteration) {
        return send_message (sender, payload_for (label_, iteration));
    };
    const transition_fn_t consume = [receiver, router_receive_, label_] (size_t iteration) {
        return receive_message (receiver, router_receive_, payload_for (label_, iteration))
               && message_receiver_is_empty (receiver, router_receive_);
    };
    const bool completion_primary = receiver_type_ == ZLINK_SOCKET_ROUTER;
    const harness_result_t result =
      run_two_poller_wake_case (receiver, label_, produce, consume, completion_primary);

    sender = test_context_socket_close_zero_linger (sender);
    receiver = test_context_socket_close_zero_linger (receiver);
    TEST_ASSERT_TRUE_MESSAGE (result.ok, result.diagnostic.c_str ());
}

void test_pair_two_poller_pollin_wake ()
{
    run_message_socket_case (ZLINK_SOCKET_PAIR, ZLINK_SOCKET_PAIR, false, "PAIR",
                             "inproc://two-poller-wake-pair");
}

void test_dealer_two_poller_pollin_wake ()
{
    run_message_socket_case (ZLINK_SOCKET_DEALER, ZLINK_SOCKET_DEALER, false, "DEALER",
                             "inproc://two-poller-wake-dealer");
}

void test_router_two_poller_pollin_wake ()
{
    run_message_socket_case (ZLINK_SOCKET_ROUTER, ZLINK_SOCKET_DEALER, true, "ROUTER",
                             "inproc://two-poller-wake-router");
}

#if !defined(ZLINK_HAVE_WINDOWS)
bool parse_tcp_endpoint (const char *endpoint_, char host_[64], int *port_)
{
    char protocol[8] = {0};
    int port = 0;
    if (!endpoint_ || !host_ || !port_
        || sscanf (endpoint_, "%7[^:]://%63[^:]:%d", protocol, host_, &port) != 3
        || strcmp (protocol, "tcp") != 0 || port <= 0 || port > 65535)
        return false;
    *port_ = port;
    return true;
}

int connect_raw_tcp (const char *endpoint_)
{
    char host[64];
    int port = 0;
    if (!parse_tcp_endpoint (endpoint_, host, &port)) {
        errno = EINVAL;
        return -1;
    }

    const int fd = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;

    sockaddr_in address;
    memset (&address, 0, sizeof (address));
    address.sin_family = AF_INET;
    address.sin_port = htons (static_cast<uint16_t> (port));
    if (inet_pton (AF_INET, host, &address.sin_addr) != 1
        || connect (fd, reinterpret_cast<const sockaddr *> (&address), sizeof (address)) != 0) {
        const int saved_errno = errno;
        close (fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

bool send_all (int fd_, const unsigned char *data_, size_t size_)
{
    size_t offset = 0;
    while (offset != size_) {
#if defined(MSG_NOSIGNAL)
        const int send_flags = MSG_NOSIGNAL;
#else
        const int send_flags = 0;
#endif
        const ssize_t result = send (fd_, data_ + offset, size_ - offset, send_flags);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return false;
        offset += static_cast<size_t> (result);
    }
    return true;
}

bool send_stream_packet (int fd_, const std::string &body_)
{
    std::vector<unsigned char> frame (6 + body_.size (), 0);
    const uint32_t body_size = static_cast<uint32_t> (body_.size ());
    frame[2] = static_cast<unsigned char> ((body_size >> 24) & 0xffu);
    frame[3] = static_cast<unsigned char> ((body_size >> 16) & 0xffu);
    frame[4] = static_cast<unsigned char> ((body_size >> 8) & 0xffu);
    frame[5] = static_cast<unsigned char> (body_size & 0xffu);
    if (!body_.empty ())
        memcpy (&frame[6], body_.data (), body_.size ());
    return send_all (fd_, frame.data (), frame.size ());
}

bool receive_stream_packet (void *stream_, const std::string &expected_)
{
    zlink_msg_t header;
    zlink_msg_t body;
    if (zlink_msg_init (&header) != ZLINK_CONFIG_OK)
        return false;
    if (zlink_msg_init (&body) != ZLINK_CONFIG_OK) {
        zlink_msg_close (&header);
        return false;
    }

    const zlink_routing_id_t *source = NULL;
    const zlink_recv_result_t result =
      zlink_stream_recv_packet (stream_, &source, &header, &body, ZLINK_RECV_FLAGS_DONTWAIT);
    const bool ok =
      result == ZLINK_RECV_OK && source && zlink_msg_size (&header) == 0
      && zlink_msg_size (&body) == expected_.size ()
      && (expected_.empty ()
          || memcmp (zlink_msg_data (&body), expected_.data (), expected_.size ()) == 0);
    zlink_msg_close (&body);
    zlink_msg_close (&header);
    return ok;
}

bool stream_receiver_is_empty (void *stream_)
{
    zlink_msg_t header;
    zlink_msg_t body;
    if (zlink_msg_init (&header) != ZLINK_CONFIG_OK)
        return false;
    if (zlink_msg_init (&body) != ZLINK_CONFIG_OK) {
        zlink_msg_close (&header);
        return false;
    }
    const zlink_routing_id_t *source = NULL;
    const zlink_recv_result_t result =
      zlink_stream_recv_packet (stream_, &source, &header, &body, ZLINK_RECV_FLAGS_DONTWAIT);
    zlink_msg_close (&body);
    zlink_msg_close (&header);
    return result == ZLINK_RECV_NO_DATA;
}

void test_stream_two_poller_pollin_wake ()
{
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (stream);
    configure_socket (stream);

    const int notify = 0;
    const zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (stream, ZLINK_STREAM_OPT_NOTIFY, &notify, sizeof (notify)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (stream, ZLINK_STREAM_OPT_RECV_MODE, &mode, sizeof (mode)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (stream, endpoint, sizeof (endpoint));
    const int client = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client >= 0);

    const std::string prime = payload_for ("STREAM", no_iteration);
    TEST_ASSERT_TRUE (send_stream_packet (client, prime));
    zlink_pollitem_t item = {stream, 0, ZLINK_POLLIN, 0};
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&item, 1, coordination_timeout_ms, NULL));
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLIN, item.revents);
    TEST_ASSERT_TRUE (receive_stream_packet (stream, prime));
    TEST_ASSERT_TRUE (stream_receiver_is_empty (stream));

    const transition_fn_t produce = [client] (size_t iteration) {
        return send_stream_packet (client, payload_for ("STREAM", iteration));
    };
    const transition_fn_t consume = [stream] (size_t iteration) {
        return receive_stream_packet (stream, payload_for ("STREAM", iteration))
               && stream_receiver_is_empty (stream);
    };
    const harness_result_t result =
      run_two_poller_wake_case (stream, "STREAM", produce, consume, false);
    const harness_result_t primary_departure_result =
      result.ok ? run_two_poller_wake_case (stream, "STREAM-primary-departure", produce, consume,
                                            false, true)
                : harness_result_t{true, std::string ()};

    close (client);
    stream = test_context_socket_close_zero_linger (stream);
    TEST_ASSERT_TRUE_MESSAGE (result.ok, result.diagnostic.c_str ());
    TEST_ASSERT_TRUE_MESSAGE (primary_departure_result.ok,
                              primary_departure_result.diagnostic.c_str ());
}
#else
void test_stream_two_poller_pollin_wake ()
{
    TEST_IGNORE_MESSAGE ("raw TCP STREAM helper is POSIX-only");
}
#endif
}

int main ()
{
    setup_test_environment (60);
    UNITY_BEGIN ();
    RUN_TEST (test_stream_two_poller_pollin_wake);
    RUN_TEST (test_pair_two_poller_pollin_wake);
    RUN_TEST (test_dealer_two_poller_pollin_wake);
    RUN_TEST (test_router_two_poller_pollin_wake);
    return UNITY_END ();
}
