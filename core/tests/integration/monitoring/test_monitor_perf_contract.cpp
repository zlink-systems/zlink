/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#undef MAX_SOCKET_STRING

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string.h>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
int parse_positive_env (const char *name_, int default_value_)
{
    if (!name_)
        return default_value_;

    const char *env = std::getenv (name_);
    if (!env || !*env)
        return default_value_;

    errno = 0;
    char *end = NULL;
    const long parsed = std::strtol (env, &end, 10);
    if (errno != 0 || end == env || parsed <= 0)
        return default_value_;

    if (parsed > INT_MAX)
        return INT_MAX;
    return static_cast<int> (parsed);
}

bool bench_debug_enabled ()
{
    static const bool enabled = std::getenv ("PERF_DEBUG") != NULL;
    return enabled;
}

bool set_sockopt_int (void *socket_, zlink_option_t option_, int value_, const char *name_)
{
    const int rc = zlink_set_option (socket_, option_, &value_, sizeof (value_));
    if (rc != 0 && bench_debug_enabled ()) {
        std::cerr << "setsockopt(" << name_ << ") failed: " << zlink_strerror (zlink_errno ())
                  << std::endl;
    }
    return rc == 0;
}

bool set_hwm_bytes (void *socket_, zlink_option_t option_, uint64_t value_, const char *name_)
{
    const int rc = zlink_set_option (socket_, option_, &value_, sizeof (value_));
    if (rc != 0 && bench_debug_enabled ()) {
        std::cerr << "setsockopt(" << name_ << ") failed: " << zlink_strerror (zlink_errno ())
                  << std::endl;
    }
    return rc == 0;
}

namespace perf_multi_metric
{
inline size_t header_size ()
{
    return sizeof (uint32_t) * 4 + sizeof (uint64_t) * 2;
}
} // namespace perf_multi_metric

static const size_t kPerfMonitorEndpointSize = 256;

std::atomic<unsigned int> g_inproc_endpoint_counter (0);
std::atomic<int> g_send_ready_self_close_rc (0);
std::atomic<int> g_send_ready_self_close_errno (0);
std::atomic<int> g_async_send_ready_self_close_rc (0);
std::atomic<int> g_async_send_ready_self_close_errno (0);
std::atomic<long long> g_async_send_ready_self_close_elapsed_ms (0);
std::atomic<bool> g_async_send_ready_callback_changed_thread (false);
std::atomic<bool> g_async_request_completed (false);
std::thread::id g_async_send_ready_test_thread;
static const char kPerfPubsubTopic[] = "bench";

struct connect_monitor_state_t
{
    connect_monitor_state_t () :
        connection_ready_count (0), accepted_count (0), connected_count (0), error_code (0)
    {
    }

    std::mutex sync;
    std::condition_variable cv;
    size_t connection_ready_count;
    size_t accepted_count;
    size_t connected_count;
    int error_code;
};

struct connect_monitor_t
{
    connect_monitor_t () : monitor (NULL), state (NULL) {}

    void *monitor;
    connect_monitor_state_t *state;
};

struct delivery_ready_monitor_state_t
{
    delivery_ready_monitor_state_t () : ready (false), error_code (0) {}

    std::mutex sync;
    std::condition_variable cv;
    bool ready;
    int error_code;
};

struct delivery_ready_monitor_t
{
    delivery_ready_monitor_t () : monitor (NULL), state (NULL) {}

    void *monitor;
    delivery_ready_monitor_state_t *state;
};

struct queue_probe_t
{
    queue_probe_t (void *send_socket_,
                   void *recv_socket_,
                   bool enable_send_sampling_,
                   bool enable_recv_sampling_) :
        send_socket (send_socket_),
        recv_socket (recv_socket_),
        enable_send_sampling (enable_send_sampling_),
        enable_recv_sampling (enable_recv_sampling_),
        send_samples (0),
        recv_samples (0),
        sample_failed (false)
    {
    }

    void force_sample_send ()
    {
        if (!enable_send_sampling)
            return;
        sample (send_socket, true);
    }

    void force_sample_recv ()
    {
        if (!enable_recv_sampling)
            return;
        sample (recv_socket, false);
    }

    void sample_send_if_due ()
    {
        if (!enable_send_sampling) {
            ++send_samples;
            return;
        }
        if ((send_samples + 1) % 8 == 0)
            sample (send_socket, true);
        ++send_samples;
    }

    void sample_recv_if_due ()
    {
        if (!enable_recv_sampling) {
            ++recv_samples;
            return;
        }
        if ((recv_samples + 1) % 8 == 0)
            sample (recv_socket, false);
        ++recv_samples;
    }

    void assert_ok () const { TEST_ASSERT_FALSE (sample_failed); }

    void *send_socket;
    void *recv_socket;
    bool enable_send_sampling;
    bool enable_recv_sampling;
    size_t send_samples;
    size_t recv_samples;
    bool sample_failed;

  private:
    void sample (void *socket_, bool send_path_)
    {
        if (!socket_) {
            sample_failed = true;
            return;
        }

        int events = 0;
        size_t events_size = sizeof (events);
        if (zlink_get_option (socket_, ZLINK_OPT_EVENTS, &events, &events_size) != 0) {
            sample_failed = true;
            return;
        }

        (void) send_path_;
        (void) events;
    }
};

void close_socket_zero_linger (void *socket_)
{
    if (!socket_)
        return;
    const int zero = 0;
    (void) zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero, sizeof (zero));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_));
}

void perf_pubsub_delivery_ready_monitor_handler (const zlink_monitor_event_t *event_,
                                                 void *userdata_)
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

bool open_perf_like_delivery_ready_monitor (void *socket_,
                                            uint64_t events_,
                                            delivery_ready_monitor_t *out_)
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
    if (!monitor
        || zlink_socket_monitor_handler (monitor, &perf_pubsub_delivery_ready_monitor_handler,
                                         state)
             != 0) {
        if (monitor)
            (void) zlink_monitor_close (&monitor);
        delete state;
        return false;
    }

    const int monitor_hwm = parse_positive_env ("PERF_MONITOR_HWM", 1000);
    set_sockopt_int (monitor, ZLINK_OPT_LINGER, 0, "ZLINK_OPT_LINGER");
    if (monitor_hwm > 0) {
        const uint64_t monitor_hwm_bytes =
          static_cast<uint64_t> (monitor_hwm) * sizeof (zlink_msg_t);
        set_hwm_bytes (monitor, ZLINK_OPT_SNDHWM, monitor_hwm_bytes, "ZLINK_OPT_SNDHWM");
        set_hwm_bytes (monitor, ZLINK_OPT_RCVHWM, monitor_hwm_bytes, "ZLINK_OPT_RCVHWM");
    }

    out_->monitor = monitor;
    out_->state = state;
    return true;
}

bool wait_perf_like_delivery_ready (delivery_ready_monitor_t *monitor_, int timeout_ms_)
{
    if (!monitor_ || !monitor_->state)
        return false;

    delivery_ready_monitor_state_t *state = monitor_->state;
    std::unique_lock<std::mutex> lock (state->sync);
    if (state->error_code != 0)
        return false;
    if (state->ready)
        return true;

    const bool signaled =
      state->cv.wait_for (lock, std::chrono::milliseconds (timeout_ms_ > 0 ? timeout_ms_ : 1),
                          [state] () { return state->error_code != 0 || state->ready; });
    return signaled && state->error_code == 0 && state->ready;
}

void close_perf_like_delivery_ready_monitor (delivery_ready_monitor_t *monitor_)
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

void send_pubsub_perf_payload_expect_success (void *server_, const char *payload_)
{
    TEST_ASSERT_NOT_NULL (server_);
    TEST_ASSERT_NOT_NULL (payload_);
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, strlen (payload_)));
    memcpy (zlink_msg_data (&part), payload_, strlen (payload_));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_publish (server_, kPerfPubsubTopic, &part, 1, 0));
}

void recv_pubsub_perf_payload_expect_success (void *client_, const char *payload_)
{
    char topic[32] = {0};
    size_t topic_len = sizeof (topic);
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_subscribe (client_, NULL, &parts, &part_count, topic, &topic_len, 0));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (strlen (kPerfPubsubTopic), topic_len);
    TEST_ASSERT_EQUAL_MEMORY (kPerfPubsubTopic, topic, topic_len);
    TEST_ASSERT_EQUAL_UINT64 (strlen (payload_), zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (payload_, zlink_msg_data (&parts[0]), strlen (payload_));
    zlink_multipart_close (parts, part_count);
}

void discard_socket_message (const zlink_routing_id_t *,
                             zlink_msg_t *parts_,
                             size_t part_count_,
                             void *)
{
    for (size_t i = 0; i < part_count_; ++i)
        zlink_msg_close (&parts_[i]);
}

void configure_bounded_pair_socket (void *socket_, int timeout_ms_)
{
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &timeout_ms_, sizeof (timeout_ms_)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &timeout_ms_, sizeof (timeout_ms_)));
}

void configure_send_ready_backpressure_socket (void *socket_)
{
    const uint64_t hwm = 65536u + sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
}

void arm_send_ready_notification_via_backpressure (void *send_socket_)
{
    char payload[65536];
    memset (payload, 'p', sizeof (payload));
    bool armed = false;
    for (int i = 0; i < 8192; ++i) {
        errno = 0;
        const int rc = zlink_send (send_socket_, payload, sizeof (payload), ZLINK_DONTWAIT);
        if (rc >= 0)
            continue;
        TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
        armed = true;
        break;
    }
    TEST_ASSERT_TRUE (armed);
}

void perf_like_connect_monitor_handler (const zlink_monitor_event_t *event_, void *userdata_)
{
    connect_monitor_state_t *state = static_cast<connect_monitor_state_t *> (userdata_);
    if (!state || !event_)
        return;

    {
        std::lock_guard<std::mutex> lock (state->sync);
        switch (event_->event) {
            case ZLINK_EVENT_CONNECTION_READY:
                ++state->connection_ready_count;
                break;

            case ZLINK_EVENT_ACCEPTED:
                ++state->accepted_count;
                break;

            case ZLINK_EVENT_CONNECTED:
                ++state->connected_count;
                break;

            case ZLINK_EVENT_BIND_FAILED:
            case ZLINK_EVENT_ACCEPT_FAILED:
            case ZLINK_EVENT_CLOSE_FAILED:
            case ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL:
            case ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL:
            case ZLINK_EVENT_HANDSHAKE_FAILED_AUTH:
                if (state->error_code == 0) {
                    state->error_code = event_->value > 0 ? static_cast<int> (event_->value) : EIO;
                }
                break;

            default:
                break;
        }
    }

    state->cv.notify_all ();
}

size_t connect_ready_count (const connect_monitor_state_t *state_)
{
    if (!state_)
        return 0;
    return std::max (std::max (state_->connection_ready_count, state_->accepted_count),
                     state_->connected_count);
}

bool open_perf_like_connect_monitor (void *socket_, connect_monitor_t *out_)
{
    if (!socket_ || !out_)
        return false;

    connect_monitor_state_t *state = new (std::nothrow) connect_monitor_state_t;
    if (!state)
        return false;

    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_CONNECTED | ZLINK_EVENT_ACCEPTED
                  | ZLINK_EVENT_BIND_FAILED | ZLINK_EVENT_ACCEPT_FAILED | ZLINK_EVENT_CLOSE_FAILED
                  | ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL | ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL
                  | ZLINK_EVENT_HANDSHAKE_FAILED_AUTH;
    void *monitor = zlink_socket_monitor_open (socket_, &opts);
    if (!monitor) {
        delete state;
        return false;
    }
    if (zlink_socket_monitor_handler (monitor, &perf_like_connect_monitor_handler, state) != 0) {
        zlink_monitor_close (&monitor);
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

void close_perf_like_connect_monitor (connect_monitor_t *monitor_);

bool open_perf_like_connect_monitor_with_perf_sockopts (void *socket_, connect_monitor_t *out_)
{
    if (!open_perf_like_connect_monitor (socket_, out_))
        return false;

    const uint64_t monitor_hwm = 1000u * sizeof (zlink_msg_t);
    if (!set_sockopt_int (out_->monitor, ZLINK_OPT_LINGER, 0, "ZLINK_OPT_LINGER")
        || !set_hwm_bytes (out_->monitor, ZLINK_OPT_SNDHWM, monitor_hwm, "ZLINK_OPT_SNDHWM")
        || !set_hwm_bytes (out_->monitor, ZLINK_OPT_RCVHWM, monitor_hwm, "ZLINK_OPT_RCVHWM")) {
        close_perf_like_connect_monitor (out_);
        return false;
    }

    return true;
}

bool wait_perf_like_connect_ready (connect_monitor_t *monitor_, int timeout_ms_)
{
    if (!monitor_ || !monitor_->state)
        return false;

    std::unique_lock<std::mutex> lock (monitor_->state->sync);
    return monitor_->state->cv.wait_for (lock, std::chrono::milliseconds (timeout_ms_),
                                         [monitor_] () {
                                             return monitor_->state->error_code != 0
                                                    || connect_ready_count (monitor_->state) >= 1;
                                         })
           && monitor_->state->error_code == 0 && connect_ready_count (monitor_->state) >= 1;
}

void close_perf_like_connect_monitor (connect_monitor_t *monitor_)
{
    if (!monitor_ || !monitor_->monitor)
        return;

    void *monitor = monitor_->monitor;
    connect_monitor_state_t *state = monitor_->state;
    monitor_->monitor = NULL;
    monitor_->state = NULL;

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    delete state;
}

bool recv_exact (void *socket_, const std::vector<char> &expected_)
{
    std::vector<char> actual (expected_.size (), 0);
    const int rc = zlink_recv (socket_, &actual[0], actual.size (), 0);
    return rc == static_cast<int> (expected_.size ())
           && memcmp (&actual[0], &expected_[0], expected_.size ()) == 0;
}

void make_unique_inproc_endpoint (char *endpoint_, size_t size_)
{
    TEST_ASSERT_NOT_NULL (endpoint_);
    const unsigned int endpoint_id =
      g_inproc_endpoint_counter.fetch_add (1, std::memory_order_acq_rel) + 1;
    snprintf (endpoint_, size_, "inproc://monitor-perf-pair-%u", endpoint_id);
}

void send_ready_self_close_handler (void *subject_, void *)
{
    void *socket = subject_;
    g_send_ready_self_close_rc.store (zlink_close (socket), std::memory_order_release);
    g_send_ready_self_close_errno.store (errno, std::memory_order_release);
}

void async_send_ready_self_close_handler (void *subject_, void *)
{
    g_async_send_ready_callback_changed_thread.store (
      std::this_thread::get_id () != g_async_send_ready_test_thread,
      std::memory_order_release);
    const std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now ();
    g_async_send_ready_self_close_rc.store (zlink_close (subject_),
                                            std::memory_order_release);
    g_async_send_ready_self_close_errno.store (errno, std::memory_order_release);
    const long long elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now () - started)
        .count ();
    g_async_send_ready_self_close_elapsed_ms.store (elapsed_ms,
                                                    std::memory_order_release);
}

void discard_request_completion (zlink_request_result_t,
                                 zlink_msg_t *parts_,
                                 size_t part_count_,
                                 void *)
{
    if (parts_)
        zlink_multipart_close (parts_, part_count_);
    g_async_request_completed.store (true, std::memory_order_release);
}
}

void run_pair_perf_like_monitor_sampling_case (bool sample_send_, bool sample_recv_)
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *server = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    configure_bounded_pair_socket (server, 200);
    configure_bounded_pair_socket (client, 200);
    configure_send_ready_backpressure_socket (server);
    configure_send_ready_backpressure_socket (client);

    connect_monitor_t server_monitor;
    connect_monitor_t client_monitor;
    TEST_ASSERT_TRUE (open_perf_like_connect_monitor (server, &server_monitor));
    TEST_ASSERT_TRUE (open_perf_like_connect_monitor (client, &client_monitor));

    char endpoint[kPerfMonitorEndpointSize];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    TEST_ASSERT_TRUE (wait_perf_like_connect_ready (&server_monitor, 3000));
    TEST_ASSERT_TRUE (wait_perf_like_connect_ready (&client_monitor, 3000));

    close_perf_like_connect_monitor (&client_monitor);
    close_perf_like_connect_monitor (&server_monitor);

    std::vector<char> payload (1024, 'p');
    queue_probe_t probe (client, server, sample_send_, sample_recv_);
    probe.force_sample_send ();
    probe.force_sample_recv ();
    probe.assert_ok ();

    const int message_count = 16;
    std::atomic<bool> recv_failed (false);

    std::thread receiver ([&] () {
        for (int i = 0; i < message_count; ++i) {
            if (!recv_exact (server, payload)) {
                recv_failed.store (true, std::memory_order_release);
                return;
            }
            probe.sample_recv_if_due ();
            if (probe.sample_failed) {
                recv_failed.store (true, std::memory_order_release);
                return;
            }
        }
    });

    bool send_failed = false;
    for (int i = 0; i < message_count; ++i) {
        const int rc = zlink_send (client, &payload[0], payload.size (), 0);
        if (rc != static_cast<int> (payload.size ())) {
            send_failed = true;
            break;
        }
        probe.sample_send_if_due ();
        if (probe.sample_failed) {
            send_failed = true;
            break;
        }
    }

    receiver.join ();
    probe.force_sample_send ();
    probe.force_sample_recv ();
    probe.assert_ok ();
    TEST_ASSERT_FALSE (send_failed);
    TEST_ASSERT_FALSE (recv_failed.load (std::memory_order_acquire));

    close_socket_zero_linger (client);
    close_socket_zero_linger (server);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_pair_perf_like_send_sampling_preserves_oneway_delivery ()
{
    run_pair_perf_like_monitor_sampling_case (true, false);
}

void test_pair_perf_like_recv_sampling_preserves_oneway_delivery ()
{
    run_pair_perf_like_monitor_sampling_case (false, true);
}

void test_pair_perf_like_bidirectional_sampling_preserves_oneway_delivery ()
{
    run_pair_perf_like_monitor_sampling_case (true, true);
}

void test_pair_inproc_perf_like_monitor_ready_implies_bidirectional_delivery ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *server = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    configure_bounded_pair_socket (server, 200);
    configure_bounded_pair_socket (client, 200);

    char endpoint[128];
    make_unique_inproc_endpoint (endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, endpoint));

    connect_monitor_t server_monitor;
    connect_monitor_t client_monitor;
    TEST_ASSERT_TRUE (open_perf_like_connect_monitor (server, &server_monitor));
    TEST_ASSERT_TRUE (open_perf_like_connect_monitor (client, &client_monitor));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    TEST_ASSERT_TRUE (wait_perf_like_connect_ready (&server_monitor, 3000));
    TEST_ASSERT_TRUE (wait_perf_like_connect_ready (&client_monitor, 3000));

    close_perf_like_connect_monitor (&client_monitor);
    close_perf_like_connect_monitor (&server_monitor);

    static const char hello[] = "pair-inproc-hello";
    static const char ack[] = "pair-inproc-ack";

    send_string_expect_success (client, hello, 0);
    recv_string_expect_success (server, hello, 0);

    send_string_expect_success (server, ack, 0);
    recv_string_expect_success (client, ack, 0);

    close_socket_zero_linger (client);
    close_socket_zero_linger (server);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_pubsub_perf_like_monitor_sockopts_preserve_connect_ready ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *server = zlink_socket (ctx, ZLINK_SOCKET_PUB);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_SUB);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    configure_bounded_pair_socket (server, 500);
    configure_bounded_pair_socket (client, 500);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (client, ""));

    connect_monitor_t server_monitor;
    connect_monitor_t client_monitor;
    TEST_ASSERT_TRUE (open_perf_like_connect_monitor_with_perf_sockopts (server, &server_monitor));
    TEST_ASSERT_TRUE (open_perf_like_connect_monitor_with_perf_sockopts (client, &client_monitor));

    char endpoint[kPerfMonitorEndpointSize];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    TEST_ASSERT_TRUE (wait_perf_like_connect_ready (&server_monitor, 3000));
    TEST_ASSERT_TRUE (wait_perf_like_connect_ready (&client_monitor, 3000));

    close_perf_like_connect_monitor (&client_monitor);
    close_perf_like_connect_monitor (&server_monitor);

    send_string_expect_success (server, "pubsub-monitor-payload", 0);
    recv_string_expect_success (client, "pubsub-monitor-payload", 0);

    close_socket_zero_linger (client);
    close_socket_zero_linger (server);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_dealer_dealer_perf_like_monitor_sockopts_preserve_connect_ready ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *server = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    configure_bounded_pair_socket (server, 500);
    configure_bounded_pair_socket (client, 500);

    connect_monitor_t server_monitor;
    connect_monitor_t client_monitor;
    TEST_ASSERT_TRUE (open_perf_like_connect_monitor_with_perf_sockopts (server, &server_monitor));
    TEST_ASSERT_TRUE (open_perf_like_connect_monitor_with_perf_sockopts (client, &client_monitor));

    char endpoint[kPerfMonitorEndpointSize];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    TEST_ASSERT_TRUE (wait_perf_like_connect_ready (&server_monitor, 3000));
    TEST_ASSERT_TRUE (wait_perf_like_connect_ready (&client_monitor, 3000));

    close_perf_like_connect_monitor (&client_monitor);
    close_perf_like_connect_monitor (&server_monitor);

    send_string_expect_success (client, "dealer-monitor-payload", 0);
    recv_string_expect_success (server, "dealer-monitor-payload", 0);

    close_socket_zero_linger (client);
    close_socket_zero_linger (server);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_dealer_router_perf_like_client_monitor_preserves_bidirectional_delivery ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (ctx, ZLINK_CTX_OPT_BLOCKY, 0));

    void *server = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    configure_bounded_pair_socket (server, 500);
    configure_bounded_pair_socket (client, 500);

    static const char dealer_id[] = "PERF-DEALER";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, dealer_id, sizeof (dealer_id) - 1));

    connect_monitor_t client_monitor;
    TEST_ASSERT_TRUE (open_perf_like_connect_monitor_with_perf_sockopts (client, &client_monitor));

    char endpoint[kPerfMonitorEndpointSize];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
    TEST_ASSERT_TRUE (wait_perf_like_connect_ready (&client_monitor, 3000));
    close_perf_like_connect_monitor (&client_monitor);

    send_string_expect_success (client, "dealer-router-monitor-ping", 0);

    unsigned char rid_buf[255];
    const int rid_size =
      TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (server, rid_buf, sizeof (rid_buf), 0));
    TEST_ASSERT_EQUAL_INT (sizeof (dealer_id) - 1, rid_size);
    TEST_ASSERT_EQUAL_MEMORY (dealer_id, rid_buf, rid_size);
    recv_string_expect_success (server, "dealer-router-monitor-ping", 0);

    TEST_ASSERT_EQUAL_INT (
      rid_size, TEST_ASSERT_SUCCESS_ERRNO (
                  zlink_send (server, rid_buf, static_cast<size_t> (rid_size), ZLINK_SNDMORE)));
    send_string_expect_success (server, "dealer-router-monitor-pong", 0);
    recv_string_expect_success (client, "dealer-router-monitor-pong", 0);

    close_socket_zero_linger (client);
    close_socket_zero_linger (server);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_router_router_perf_like_client_monitor_preserves_bidirectional_delivery ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (ctx, ZLINK_CTX_OPT_BLOCKY, 0));

    void *server = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    configure_bounded_pair_socket (server, 500);
    configure_bounded_pair_socket (client, 500);

    static const char server_id[] = "SERVER";
    static const char client_id[] = "CLIENT";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server, server_id, sizeof (server_id) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, client_id, sizeof (client_id) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                                        server_id, sizeof (server_id) - 1));

    connect_monitor_t client_monitor;
    TEST_ASSERT_TRUE (open_perf_like_connect_monitor_with_perf_sockopts (client, &client_monitor));

    char endpoint[kPerfMonitorEndpointSize];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
    TEST_ASSERT_TRUE (wait_perf_like_connect_ready (&client_monitor, 3000));
    close_perf_like_connect_monitor (&client_monitor);

    TEST_ASSERT_EQUAL_INT (static_cast<int> (sizeof (server_id) - 1),
                           TEST_ASSERT_SUCCESS_ERRNO (zlink_send (
                             client, server_id, sizeof (server_id) - 1, ZLINK_SNDMORE)));
    send_string_expect_success (client, "router-router-monitor-ping", 0);

    unsigned char rid_buf[255];
    const int rid_size =
      TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (server, rid_buf, sizeof (rid_buf), 0));
    TEST_ASSERT_EQUAL_INT (sizeof (client_id) - 1, rid_size);
    TEST_ASSERT_EQUAL_MEMORY (client_id, rid_buf, rid_size);
    recv_string_expect_success (server, "router-router-monitor-ping", 0);

    TEST_ASSERT_EQUAL_INT (
      rid_size, TEST_ASSERT_SUCCESS_ERRNO (
                  zlink_send (server, rid_buf, static_cast<size_t> (rid_size), ZLINK_SNDMORE)));
    send_string_expect_success (server, "router-router-monitor-pong", 0);

    unsigned char reply_rid[255];
    const int reply_rid_size =
      TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (client, reply_rid, sizeof (reply_rid), 0));
    TEST_ASSERT_EQUAL_INT (sizeof (server_id) - 1, reply_rid_size);
    TEST_ASSERT_EQUAL_MEMORY (server_id, reply_rid, reply_rid_size);
    recv_string_expect_success (client, "router-router-monitor-pong", 0);

    close_socket_zero_linger (client);
    close_socket_zero_linger (server);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_pubsub_perf_like_delivery_ready_preserves_oneway_delivery_recv ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *server = zlink_socket (ctx, ZLINK_SOCKET_PUB);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_SUB);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    configure_bounded_pair_socket (server, 500);
    configure_bounded_pair_socket (client, 500);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (client, ""));

    delivery_ready_monitor_t server_monitor;
    delivery_ready_monitor_t client_monitor;
    TEST_ASSERT_TRUE (open_perf_like_delivery_ready_monitor (server, ZLINK_EVENT_CONNECTION_READY,
                                                             &server_monitor));
    TEST_ASSERT_TRUE (open_perf_like_delivery_ready_monitor (client, ZLINK_EVENT_CONNECTION_READY,
                                                             &client_monitor));

    char endpoint[kPerfMonitorEndpointSize];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    TEST_ASSERT_TRUE (wait_perf_like_delivery_ready (&server_monitor, 3000));
    TEST_ASSERT_TRUE (wait_perf_like_delivery_ready (&client_monitor, 3000));

    static const char payload[] = "pubsub-perf-delivery";
    send_pubsub_perf_payload_expect_success (server, payload);
    recv_pubsub_perf_payload_expect_success (client, payload);

    close_perf_like_delivery_ready_monitor (&client_monitor);
    close_perf_like_delivery_ready_monitor (&server_monitor);
    close_socket_zero_linger (client);
    close_socket_zero_linger (server);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_pubsub_raw_socket_callback_model_is_accepted ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *client = zlink_socket (ctx, ZLINK_SOCKET_SUB);
    TEST_ASSERT_NOT_NULL (client);

    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    char topic[32];
    size_t topic_len = sizeof (topic);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_subscribe (client, NULL, &parts, &part_count,
                                                                topic, &topic_len, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    close_socket_zero_linger (client);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_pubsub_raw_socket_rejects_multipart_send_api ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *pub = zlink_socket (ctx, ZLINK_SOCKET_PUB);
    TEST_ASSERT_NOT_NULL (pub);

    zlink_msg_t parts[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[0], strlen (kPerfPubsubTopic)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[1], perf_multi_metric::header_size ()));

    memcpy (zlink_msg_data (&parts[0]), kPerfPubsubTopic, strlen (kPerfPubsubTopic));
    memset (zlink_msg_data (&parts[1]), 0, perf_multi_metric::header_size ());

    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_NOT_SUPPORTED, zlink_send (pub, parts, 2, 0));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);

    zlink_msg_close (&parts[0]);
    zlink_msg_close (&parts[1]);
    close_socket_zero_linger (pub);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_send_ready_self_close_blocks_followup_operational_api ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *server = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    configure_bounded_pair_socket (server, 200);
    configure_bounded_pair_socket (client, 200);

    char endpoint[128];
    make_unique_inproc_endpoint (endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
    g_send_ready_self_close_rc.store (INT_MIN, std::memory_order_release);
    g_send_ready_self_close_errno.store (0, std::memory_order_release);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send_ready_handler (client, &send_ready_self_close_handler, NULL));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_poller_add (poller, client, NULL, ZLINK_POLLOUT));
    arm_send_ready_notification_via_backpressure (client);
    std::thread drain_thread ([server] {
        char payload[65536];
        while (zlink_recv (server, payload, sizeof (payload), 0) >= 0) {
        }
    });

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (g_send_ready_self_close_rc.load (std::memory_order_acquire) == INT_MIN
           && std::chrono::steady_clock::now () < deadline) {
        zlink_poller_event_t event;
        (void) zlink_poller_wait (poller, &event, 1, 10, NULL);
    }
    drain_thread.join ();
    TEST_ASSERT_NOT_EQUAL (INT_MIN,
                           g_send_ready_self_close_rc.load (std::memory_order_acquire));

    TEST_ASSERT_EQUAL_INT (0, g_send_ready_self_close_rc.load (std::memory_order_acquire));

    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, zlink_send (client, "post-close-send", 16, 0));
    TEST_ASSERT_TRUE (errno == ESHUTDOWN || errno == EFAULT);

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    close_socket_zero_linger (server);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_async_mailbox_send_ready_self_close_quiesces_before_reap ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *server = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    configure_bounded_pair_socket (server, 200);
    configure_bounded_pair_socket (client, 200);

    char endpoint[128];
    make_unique_inproc_endpoint (endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
    msleep (SETTLE_TIME);

    g_async_send_ready_test_thread = std::this_thread::get_id ();
    g_async_send_ready_self_close_rc.store (INT_MIN, std::memory_order_release);
    g_async_send_ready_self_close_errno.store (0, std::memory_order_release);
    g_async_send_ready_self_close_elapsed_ms.store (0, std::memory_order_release);
    g_async_send_ready_callback_changed_thread.store (false, std::memory_order_release);
    g_async_request_completed.store (false, std::memory_order_release);

    // A pending request starts the completion async mailbox executor. Send-ready
    // recovery below must therefore close from that executor, not this test thread.
    zlink_msg_t request;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&request, 1));
    *static_cast<unsigned char *> (zlink_msg_data (&request)) = 'r';
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (client, &request, 1, &discard_request_completion,
                            NULL, 0, 30000));

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *request_parts = NULL;
    size_t request_part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (server, &source_rid, &request_seq, &request_parts,
                         &request_part_count, ZLINK_RECV_FLAGS_NONE));
    zlink_routing_id_t reply_rid = *source_rid;
    zlink_multipart_close (request_parts, request_part_count);

    zlink_msg_t reply;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&reply, 1));
    *static_cast<unsigned char *> (zlink_msg_data (&reply)) = 'a';
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_router_reply (server, &reply_rid, request_seq, &reply, 1));
    TEST_ASSERT_TRUE (zlink_test_wait_until_step (3000, 10, [] {
        return g_async_request_completed.load (std::memory_order_acquire);
    }));

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send_ready_handler (client, &async_send_ready_self_close_handler, NULL));

    arm_send_ready_notification_via_backpressure (client);
    std::thread drain_thread ([server] {
        char payload[65536];
        while (zlink_recv (server, payload, sizeof (payload), 0) >= 0) {
        }
    });

    const bool callback_observed = zlink_test_wait_until_step (3000, 10, [] {
        return g_async_send_ready_self_close_rc.load (std::memory_order_acquire)
               != INT_MIN;
    });
    drain_thread.join ();

    TEST_ASSERT_TRUE (callback_observed);
    TEST_ASSERT_TRUE (
      g_async_send_ready_callback_changed_thread.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (
      0, g_async_send_ready_self_close_errno.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CLOSE_OK,
      g_async_send_ready_self_close_rc.load (std::memory_order_acquire));
    TEST_ASSERT_LESS_THAN_INT64 (
      1000, g_async_send_ready_self_close_elapsed_ms.load (std::memory_order_acquire));

    close_socket_zero_linger (server);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_pair_perf_like_send_sampling_preserves_oneway_delivery);
    RUN_TEST (test_pair_perf_like_recv_sampling_preserves_oneway_delivery);
    RUN_TEST (test_pair_perf_like_bidirectional_sampling_preserves_oneway_delivery);
    RUN_TEST (test_pair_inproc_perf_like_monitor_ready_implies_bidirectional_delivery);
    RUN_TEST (test_pubsub_perf_like_monitor_sockopts_preserve_connect_ready);
    RUN_TEST (test_pubsub_perf_like_delivery_ready_preserves_oneway_delivery_recv);
    RUN_TEST (test_pubsub_raw_socket_callback_model_is_accepted);
    RUN_TEST (test_pubsub_raw_socket_rejects_multipart_send_api);
    RUN_TEST (test_dealer_dealer_perf_like_monitor_sockopts_preserve_connect_ready);
    RUN_TEST (test_dealer_router_perf_like_client_monitor_preserves_bidirectional_delivery);
    RUN_TEST (test_router_router_perf_like_client_monitor_preserves_bidirectional_delivery);
    RUN_TEST (test_send_ready_self_close_blocks_followup_operational_api);
    RUN_TEST (test_async_mailbox_send_ready_self_close_quiesces_before_reap);
    return UNITY_END ();
}
