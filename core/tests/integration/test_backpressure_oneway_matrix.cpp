/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int kTimeoutMs = 10000;
const int kSpotTimeoutMs = 30000;
const int kPayloadSize = 1024;
const uint64_t kSmallHwm = kPayloadSize + sizeof (zlink_msg_t);
const uint64_t kLargeHwm = 10000u * (kPayloadSize + sizeof (zlink_msg_t));
const int kSocketBufferBytes = 4096;
const char kTopic[] = "bp.matrix";
const int kHwmBuckets[] = {1, 10, 100, 1000, 10000};
const char *const kTransports[] = {"tcp", "tls", "ws", "wss"};

enum raw_pattern_t
{
    raw_pattern_dealer_dealer = 0,
    raw_pattern_dealer_router = 1,
    raw_pattern_router_router = 2
};

const char *raw_pattern_name (raw_pattern_t pattern_)
{
    switch (pattern_) {
        case raw_pattern_dealer_dealer:
            return "DEALER_DEALER";
        case raw_pattern_dealer_router:
            return "DEALER_ROUTER";
        case raw_pattern_router_router:
            return "ROUTER_ROUTER";
    }

    return "UNKNOWN";
}

struct ready_monitor_state_t
{
    ready_monitor_state_t () : ready_count (0), error_code (0) {}

    std::mutex sync;
    std::condition_variable cv;
    size_t ready_count;
    int error_code;
};

struct ready_monitor_t
{
    ready_monitor_t () : monitor (NULL), state (NULL) {}

    void *monitor;
    ready_monitor_state_t *state;
};

struct drain_gate_t
{
    drain_gate_t () : start (false), done (false), received (0), error_code (0) {}

    std::mutex sync;
    std::condition_variable cv;
    bool start;
    bool done;
    size_t received;
    int error_code;
};

struct raw_case_t
{
    raw_case_t () : sender (NULL), receiver (NULL), has_target_rid (false), tls_enabled (false)
    {
        memset (&target_rid, 0, sizeof (target_rid));
    }

    void *sender;
    void *receiver;
    ready_monitor_t sender_monitor;
    ready_monitor_t receiver_monitor;
    zlink_routing_id_t target_rid;
    bool has_target_rid;
    bool tls_enabled;
    tls_test_files_t tls_files;
};

struct pubsub_case_t
{
    pubsub_case_t () : pub (NULL), sub (NULL), tls_enabled (false) {}

    void *pub;
    void *sub;
    ready_monitor_t pub_monitor;
    ready_monitor_t sub_monitor;
    bool tls_enabled;
    tls_test_files_t tls_files;
};

static bool is_transport_available (const char *transport_)
{
    if (strcmp (transport_, "tcp") == 0)
        return true;

    return zlink_has (transport_) != 0;
}

static bool is_tls_transport (const char *transport_)
{
    return strcmp (transport_, "tls") == 0 || strcmp (transport_, "wss") == 0;
}


static void configure_tls (void *server_, void *client_, const tls_test_files_t &files_)
{
    const int trust_system = 0;
    const char hostname[] = "localhost";

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client_, ZLINK_OPT_TLS_TRUST_SYSTEM, &trust_system, sizeof (trust_system)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server_, ZLINK_OPT_TLS_CERT, files_.server_cert.c_str (), files_.server_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server_, ZLINK_OPT_TLS_KEY, files_.server_key.c_str (), files_.server_key.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client_, ZLINK_OPT_TLS_CA, files_.ca_cert.c_str (),
                                                 files_.ca_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client_, ZLINK_OPT_TLS_HOSTNAME, hostname, strlen (hostname)));
}

static void bind_endpoint (
  void *socket_, const char *transport_, const char *name_, char *endpoint_, size_t endpoint_len_)
{
    if (strcmp (transport_, "tcp") == 0) {
        test_bind (socket_, "tcp://127.0.0.1:*", endpoint_, endpoint_len_);
        return;
    }

    if (strcmp (transport_, "ws") == 0) {
        test_bind (socket_, "ws://127.0.0.1:*", endpoint_, endpoint_len_);
        return;
    }

    if (strcmp (transport_, "wss") == 0) {
        test_bind (socket_, "wss://127.0.0.1:*", endpoint_, endpoint_len_);
        return;
    }

    if (strcmp (transport_, "tls") == 0) {
        test_bind (socket_, "tls://127.0.0.1:*", endpoint_, endpoint_len_);
        return;
    }

    snprintf (endpoint_, endpoint_len_, "inproc://%s", name_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (socket_, endpoint_));
}

static void configure_sender_socket (void *socket_, uint64_t sndhwm_)
{
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDHWM, &sndhwm_, sizeof (sndhwm_)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_SNDBUF, &kSocketBufferBytes,
                                                 sizeof (kSocketBufferBytes)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_RCVBUF, &kSocketBufferBytes,
                                                 sizeof (kSocketBufferBytes)));
}

static void configure_receiver_socket (void *socket_, uint64_t rcvhwm_)
{
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &kTimeoutMs, sizeof (kTimeoutMs)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVHWM, &rcvhwm_, sizeof (rcvhwm_)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_SNDBUF, &kSocketBufferBytes,
                                                 sizeof (kSocketBufferBytes)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_RCVBUF, &kSocketBufferBytes,
                                                 sizeof (kSocketBufferBytes)));
}

static void ready_monitor_handler (const zlink_monitor_event_t *event_, void *userdata_)
{
    ready_monitor_state_t *state = static_cast<ready_monitor_state_t *> (userdata_);
    if (!state || !event_)
        return;

    std::lock_guard<std::mutex> lock (state->sync);
    switch (event_->event) {
        case ZLINK_EVENT_CONNECTION_READY:
            ++state->ready_count;
            break;

        case ZLINK_EVENT_BIND_FAILED:
        case ZLINK_EVENT_ACCEPT_FAILED:
        case ZLINK_EVENT_CLOSE_FAILED:
        case ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL:
        case ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL:
        case ZLINK_EVENT_HANDSHAKE_FAILED_AUTH:
            if (state->error_code == 0)
                state->error_code = event_->value != 0 ? static_cast<int> (event_->value) : EIO;
            break;

        default:
            break;
    }
    state->cv.notify_all ();
}

static bool open_ready_monitor (void *socket_, ready_monitor_t *out_)
{
    if (!socket_ || !out_)
        return false;

    ready_monitor_state_t *state = new (std::nothrow) ready_monitor_state_t;
    if (!state)
        return false;

    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_BIND_FAILED | ZLINK_EVENT_ACCEPT_FAILED
                  | ZLINK_EVENT_CLOSE_FAILED | ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL
                  | ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL | ZLINK_EVENT_HANDSHAKE_FAILED_AUTH;
    void *monitor = zlink_socket_monitor_open (socket_, &opts);
    if (!monitor || zlink_socket_monitor_handler (monitor, &ready_monitor_handler, state) != 0) {
        if (monitor)
            (void) zlink_monitor_close (&monitor);
        delete state;
        return false;
    }

    out_->monitor = monitor;
    out_->state = state;
    return true;
}

static bool wait_ready_count (ready_monitor_t *monitor_, size_t expected_count_, int timeout_ms_)
{
    if (!monitor_ || !monitor_->state)
        return false;

    ready_monitor_state_t *state = monitor_->state;
    std::unique_lock<std::mutex> lock (state->sync);
    return state->cv.wait_for (lock, std::chrono::milliseconds (timeout_ms_),
                               [state, expected_count_] () {
                                   return state->error_code != 0
                                          || state->ready_count >= expected_count_;
                               })
           && state->error_code == 0 && state->ready_count >= expected_count_;
}

static void close_ready_monitor (ready_monitor_t *monitor_)
{
    if (!monitor_)
        return;

    if (monitor_->monitor)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor_->monitor));
    delete monitor_->state;
    monitor_->state = NULL;
}

static zlink_msg_t make_payload_part ()
{
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, kPayloadSize));
    memset (zlink_msg_data (&part), 'p', kPayloadSize);
    return part;
}

static int classify_nonblocking_send_errno (zlink_submit_result_t *result_out_)
{
    if (!result_out_) {
        errno = EFAULT;
        return -1;
    }

    switch (errno) {
        case EAGAIN:
            *result_out_ = ZLINK_SUBMIT_BACKPRESSURED;
            return 0;
        case ENOTCONN:
        case EHOSTUNREACH:
            *result_out_ = ZLINK_SUBMIT_NOT_CONNECTED;
            return 0;
        default:
            return -1;
    }
}

static int try_send_raw_part (void *sender_,
                              const zlink_routing_id_t *target_rid_,
                              zlink_submit_result_t *result_out_)
{
    zlink_msg_t part = make_payload_part ();
    const int rc = target_rid_ ? zlink_send_rid (sender_, target_rid_, &part, 1, ZLINK_DONTWAIT)
                               : zlink_send (sender_, &part, 1, ZLINK_DONTWAIT);
    if (rc == 0) {
        *result_out_ = ZLINK_SUBMIT_OK;
        return 0;
    }
    const int saved_errno = errno;
    zlink_msg_close (&part);
    errno = saved_errno;
    return classify_nonblocking_send_errno (result_out_);
}

static int try_publish_part (void *subject_, zlink_submit_result_t *result_out_)
{
    zlink_msg_t part = make_payload_part ();
    const int rc = zlink_publish (subject_, kTopic, &part, 1, ZLINK_DONTWAIT);
    if (rc == 0) {
        *result_out_ = ZLINK_SUBMIT_OK;
        return 0;
    }
    const int saved_errno = errno;
    zlink_msg_close (&part);
    errno = saved_errno;
    return classify_nonblocking_send_errno (result_out_);
}

static int
recv_one_raw_message (void *socket_, bool wants_routing_id_, zlink_routing_id_t *source_rid_out_);

static void set_send_timeout (void *socket_, int timeout_ms_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &timeout_ms_, sizeof (timeout_ms_)));
}

static void send_raw_part_blocking (void *sender_, const zlink_routing_id_t *target_rid_)
{
    zlink_msg_t part = make_payload_part ();
    if (target_rid_) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_send_rid (sender_, target_rid_, &part, 1, 0));
        return;
    }
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (sender_, &part, 1, 0));
}

static void prime_raw_case (raw_case_t *raw_, raw_pattern_t pattern_)
{
    TEST_ASSERT_NOT_NULL (raw_);

    set_send_timeout (raw_->sender, kTimeoutMs);
    send_raw_part_blocking (raw_->sender, raw_->has_target_rid ? &raw_->target_rid : NULL);
    set_send_timeout (raw_->sender, 0);

    const bool wants_routing_id =
      pattern_ == raw_pattern_dealer_router || pattern_ == raw_pattern_router_router;
    TEST_ASSERT_SUCCESS_ERRNO (recv_one_raw_message (raw_->receiver, wants_routing_id, NULL));
    msleep (50);
}

static void publish_part_blocking (void *subject_)
{
    zlink_msg_t part = make_payload_part ();
    TEST_ASSERT_SUCCESS_ERRNO (zlink_publish (subject_, kTopic, &part, 1, 0));
}

static size_t measure_send_window_raw (void *sender_,
                                       const zlink_routing_id_t *target_rid_,
                                       size_t attempt_limit_,
                                       bool *backpressured_out_)
{
    size_t sent = 0;
    if (backpressured_out_)
        *backpressured_out_ = false;

    for (size_t i = 0; i < attempt_limit_; ++i) {
        zlink_submit_result_t result = ZLINK_SUBMIT_NOT_CONNECTED;
        TEST_ASSERT_SUCCESS_ERRNO (try_send_raw_part (sender_, target_rid_, &result));
        if (result == ZLINK_SUBMIT_OK) {
            ++sent;
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, result);
        if (backpressured_out_)
            *backpressured_out_ = true;
        break;
    }

    return sent;
}

static size_t
measure_send_window_pubsub (void *pub_, size_t attempt_limit_, bool *backpressured_out_)
{
    size_t sent = 0;
    if (backpressured_out_)
        *backpressured_out_ = false;

    for (size_t i = 0; i < attempt_limit_; ++i) {
        zlink_submit_result_t result = ZLINK_SUBMIT_NOT_CONNECTED;
        TEST_ASSERT_SUCCESS_ERRNO (try_publish_part (pub_, &result));
        if (result == ZLINK_SUBMIT_OK) {
            ++sent;
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, result);
        if (backpressured_out_)
            *backpressured_out_ = true;
        break;
    }

    return sent;
}

static void start_drain (drain_gate_t *gate_)
{
    std::lock_guard<std::mutex> lock (gate_->sync);
    gate_->start = true;
    gate_->cv.notify_all ();
}

static bool wait_drain_done (drain_gate_t *gate_, int timeout_ms_)
{
    std::unique_lock<std::mutex> lock (gate_->sync);
    return gate_->cv.wait_for (lock, std::chrono::milliseconds (timeout_ms_),
                               [gate_] () { return gate_->done; });
}

static void wait_drain_start (drain_gate_t *gate_)
{
    std::unique_lock<std::mutex> lock (gate_->sync);
    gate_->cv.wait (lock, [gate_] () { return gate_->start; });
}

static void finish_drain (drain_gate_t *gate_, size_t received_, int error_code_)
{
    std::lock_guard<std::mutex> lock (gate_->sync);
    gate_->received = received_;
    gate_->error_code = error_code_;
    gate_->done = true;
    gate_->cv.notify_all ();
}

static int
recv_one_raw_message (void *socket_, bool wants_routing_id_, zlink_routing_id_t *source_rid_out_)
{
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    zlink_routing_id_t source_rid;
    memset (&source_rid, 0, sizeof (source_rid));

    int rc = -1;
    if (wants_routing_id_) {
        const zlink_routing_id_t *peer_rid = NULL;
        uint64_t request_seq = 0;
        rc = zlink_router_recv (socket_, &peer_rid, &request_seq, &parts,
                                &part_count, 0);
        if (rc == 0) {
            if (request_seq != 0) {
                if (parts)
                    zlink_multipart_close (parts, part_count);
                errno = EPROTO;
                return -1;
            }
            if (peer_rid)
                source_rid = *peer_rid;
        }
    } else {
        rc = zlink_recv (socket_, NULL, &parts, &part_count, 0);
    }
    if (rc != 0)
        return -1;

    if (source_rid_out_ && wants_routing_id_)
        *source_rid_out_ = source_rid;
    zlink_multipart_close (parts, part_count);
    return 0;
}

static void drain_raw_receiver (void *receiver_,
                                raw_pattern_t pattern_,
                                size_t expected_messages_,
                                drain_gate_t *gate_)
{
    wait_drain_start (gate_);

    size_t received = 0;
    int error_code = 0;
    const bool wants_routing_id =
      pattern_ == raw_pattern_dealer_router || pattern_ == raw_pattern_router_router;

    while (received < expected_messages_) {
        if (recv_one_raw_message (receiver_, wants_routing_id, NULL) != 0) {
            error_code = errno != 0 ? errno : EIO;
            break;
        }
        ++received;
    }

    finish_drain (gate_, received, error_code);
}

static void drain_raw_receiver_with_ack (void *receiver_,
                                         raw_pattern_t pattern_,
                                         size_t expected_messages_,
                                         drain_gate_t *gate_)
{
    wait_drain_start (gate_);

    size_t received = 0;
    int error_code = 0;
    zlink_routing_id_t last_source_rid;
    memset (&last_source_rid, 0, sizeof (last_source_rid));
    const bool wants_routing_id =
      pattern_ == raw_pattern_dealer_router || pattern_ == raw_pattern_router_router;

    while (received < expected_messages_) {
        if (recv_one_raw_message (receiver_, wants_routing_id,
                                  wants_routing_id ? &last_source_rid : NULL)
            != 0) {
            error_code = errno != 0 ? errno : EIO;
            break;
        }
        ++received;
    }

    if (error_code == 0 && pattern_ != raw_pattern_router_router) {
        set_send_timeout (receiver_, kTimeoutMs);
        if (pattern_ == raw_pattern_dealer_dealer) {
            send_raw_part_blocking (receiver_, NULL);
        } else {
            send_raw_part_blocking (receiver_, &last_source_rid);
        }
        set_send_timeout (receiver_, 0);
    }

    finish_drain (gate_, received, error_code);
}

static void drain_subscription_receiver (void *sub_, size_t expected_messages_, drain_gate_t *gate_)
{
    wait_drain_start (gate_);

    size_t received = 0;
    int error_code = 0;
    while (received < expected_messages_) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        char topic[256];
        size_t topic_len = sizeof (topic);
        memset (topic, 0, sizeof (topic));
        if (zlink_subscribe (sub_, NULL, &parts, &part_count, topic, &topic_len, 0) != 0) {
            error_code = errno != 0 ? errno : EIO;
            break;
        }
        if (topic_len != strlen (kTopic) || memcmp (topic, kTopic, topic_len) != 0) {
            error_code = EPROTO;
            zlink_multipart_close (parts, part_count);
            break;
        }
        zlink_multipart_close (parts, part_count);
        ++received;
    }

    finish_drain (gate_, received, error_code);
}

static void drain_available_subscription_messages (void *sub_, size_t attempt_limit_)
{
    for (size_t i = 0; i < attempt_limit_; ++i) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        char topic[256];
        size_t topic_len = sizeof (topic);
        memset (topic, 0, sizeof (topic));
        if (zlink_subscribe (sub_, NULL, &parts, &part_count, topic, &topic_len, ZLINK_DONTWAIT)
            != 0) {
            if (errno == EAGAIN || errno == EINTR)
                return;
            return;
        }
        zlink_multipart_close (parts, part_count);
    }
}

static void close_raw_case (raw_case_t *raw_)
{
    close_ready_monitor (&raw_->sender_monitor);
    close_ready_monitor (&raw_->receiver_monitor);
    if (raw_->sender)
        test_context_socket_close_zero_linger (raw_->sender);
    if (raw_->receiver)
        test_context_socket_close_zero_linger (raw_->receiver);
    if (raw_->tls_enabled)
        cleanup_tls_test_files (raw_->tls_files);
    raw_->sender = NULL;
    raw_->receiver = NULL;
}

static void close_pubsub_case (pubsub_case_t *pubsub_)
{
    close_ready_monitor (&pubsub_->pub_monitor);
    close_ready_monitor (&pubsub_->sub_monitor);
    if (pubsub_->sub)
        test_context_socket_close_zero_linger (pubsub_->sub);
    if (pubsub_->pub)
        test_context_socket_close_zero_linger (pubsub_->pub);
    if (pubsub_->tls_enabled)
        cleanup_tls_test_files (pubsub_->tls_files);
    pubsub_->pub = NULL;
    pubsub_->sub = NULL;
}


static void setup_raw_case (
  raw_pattern_t pattern_, const char *transport_, uint64_t sndhwm_,
  uint64_t rcvhwm_, raw_case_t *out_)
{
    int sender_type = ZLINK_SOCKET_DEALER;
    int receiver_type = ZLINK_SOCKET_DEALER;
    const char *sender_rid = NULL;
    const char *receiver_rid = NULL;

    switch (pattern_) {
        case raw_pattern_dealer_dealer:
            sender_type = ZLINK_SOCKET_DEALER;
            receiver_type = ZLINK_SOCKET_DEALER;
            break;

        case raw_pattern_dealer_router:
            sender_type = ZLINK_SOCKET_DEALER;
            receiver_type = ZLINK_SOCKET_ROUTER;
            sender_rid = "RAW-DEALER";
            break;

        case raw_pattern_router_router:
            sender_type = ZLINK_SOCKET_ROUTER;
            receiver_type = ZLINK_SOCKET_ROUTER;
            sender_rid = "RAW-CLIENT";
            receiver_rid = "RAW-SERVER";
            break;
    }

    out_->sender = test_context_socket (sender_type);
    out_->receiver = test_context_socket (receiver_type);
    configure_sender_socket (out_->sender, sndhwm_);
    configure_receiver_socket (out_->receiver, rcvhwm_);
    if (sender_type == ZLINK_SOCKET_ROUTER) {
        const int mandatory = 1;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
          out_->sender, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof (mandatory)));
    }
    if (sender_rid) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_routing_id (out_->sender, sender_rid, strlen (sender_rid)));
    }
    if (receiver_rid) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_routing_id (out_->receiver, receiver_rid, strlen (receiver_rid)));
        if (pattern_ == raw_pattern_router_router) {
            static const char kConnectRid[] = "RAW-CONNECT";
            TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (out_->sender,
                                                                ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                                                kConnectRid, strlen (kConnectRid)));
            out_->target_rid.size = static_cast<uint8_t> (strlen (kConnectRid));
            memcpy (out_->target_rid.data, kConnectRid, strlen (kConnectRid));
            out_->has_target_rid = true;
        } else {
            out_->target_rid.size = static_cast<uint8_t> (strlen (receiver_rid));
            memcpy (out_->target_rid.data, receiver_rid, strlen (receiver_rid));
            out_->has_target_rid = true;
        }
    }

    TEST_ASSERT_TRUE (open_ready_monitor (out_->sender, &out_->sender_monitor));
    TEST_ASSERT_TRUE (open_ready_monitor (out_->receiver, &out_->receiver_monitor));

    if (is_tls_transport (transport_)) {
        out_->tls_enabled = true;
        out_->tls_files = make_tls_test_files ();
        configure_tls (out_->receiver, out_->sender, out_->tls_files);
    }

    char endpoint[MAX_SOCKET_STRING];
    char name[64];
    snprintf (name, sizeof (name), "bp-%s-%s", raw_pattern_name (pattern_), transport_);
    bind_endpoint (out_->receiver, transport_, name, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (out_->sender, endpoint));
    TEST_ASSERT_TRUE (wait_ready_count (&out_->sender_monitor, 1, kTimeoutMs));
    TEST_ASSERT_TRUE (wait_ready_count (&out_->receiver_monitor, 1, kTimeoutMs));
    if (pattern_ == raw_pattern_dealer_router || pattern_ == raw_pattern_router_router)
        prime_raw_case (out_, pattern_);
}

static void setup_pubsub_case (
  const char *transport_, uint64_t sndhwm_, uint64_t rcvhwm_, pubsub_case_t *out_)
{
    out_->pub = test_context_socket (ZLINK_SOCKET_XPUB);
    out_->sub = test_context_socket (ZLINK_SOCKET_SUB);

    configure_sender_socket (out_->pub, sndhwm_);
    configure_receiver_socket (out_->sub, rcvhwm_);
    const int nodrop = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_pub_option (out_->pub, ZLINK_PUB_OPT_NODROP, &nodrop, sizeof (nodrop)));

    TEST_ASSERT_TRUE (open_ready_monitor (out_->pub, &out_->pub_monitor));
    TEST_ASSERT_TRUE (open_ready_monitor (out_->sub, &out_->sub_monitor));

    if (is_tls_transport (transport_)) {
        out_->tls_enabled = true;
        out_->tls_files = make_tls_test_files ();
        configure_tls (out_->pub, out_->sub, out_->tls_files);
    }

    char endpoint[MAX_SOCKET_STRING];
    bind_endpoint (out_->pub, transport_, "bp-pubsub", endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (out_->sub, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (out_->sub, kTopic));
    TEST_ASSERT_TRUE (wait_ready_count (&out_->pub_monitor, 1, kTimeoutMs));
    TEST_ASSERT_TRUE (wait_ready_count (&out_->sub_monitor, 1, kTimeoutMs));
}





static void assert_progress_monotonic (const std::vector<size_t> &counts_, const char *label_)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE (sizeof (kHwmBuckets) / sizeof (kHwmBuckets[0]), counts_.size (),
                                    label_);
    for (size_t i = 1; i < counts_.size (); ++i)
        TEST_ASSERT_TRUE_MESSAGE (counts_[i] >= counts_[i - 1], label_);
    TEST_ASSERT_TRUE_MESSAGE (counts_.back () > counts_.front (), label_);
}

static void verify_raw_progress_matrix ()
{
    for (size_t transport_index = 0;
         transport_index < sizeof (kTransports) / sizeof (kTransports[0]); ++transport_index) {
        const char *transport = kTransports[transport_index];
        if (!is_transport_available (transport))
            continue;

        for (int pattern_value = raw_pattern_dealer_dealer;
             pattern_value <= raw_pattern_router_router; ++pattern_value) {
            const raw_pattern_t pattern = static_cast<raw_pattern_t> (pattern_value);
            std::vector<size_t> counts;
            for (size_t i = 0; i < sizeof (kHwmBuckets) / sizeof (kHwmBuckets[0]); ++i) {
                raw_case_t raw;
                const uint64_t hwm_bytes =
                  static_cast<uint64_t> (kHwmBuckets[i])
                  * (kPayloadSize + sizeof (zlink_msg_t));
                setup_raw_case (pattern, transport, hwm_bytes, kLargeHwm, &raw);
                counts.push_back (
                  measure_send_window_raw (raw.sender, raw.has_target_rid ? &raw.target_rid : NULL,
                                           static_cast<size_t> (kHwmBuckets[i]) + 1, NULL));
                close_raw_case (&raw);
            }

            char label[128];
            snprintf (label, sizeof (label), "%s %s sndhwm progress", raw_pattern_name (pattern),
                      transport);
            assert_progress_monotonic (counts, label);
        }
    }
}

static void verify_raw_pressure_entry_and_resume ()
{
    for (size_t transport_index = 0;
         transport_index < sizeof (kTransports) / sizeof (kTransports[0]); ++transport_index) {
        const char *transport = kTransports[transport_index];
        if (!is_transport_available (transport))
            continue;

        for (int pattern_value = raw_pattern_dealer_dealer;
             pattern_value <= raw_pattern_router_router; ++pattern_value) {
            const raw_pattern_t pattern = static_cast<raw_pattern_t> (pattern_value);
            raw_case_t raw;
            setup_raw_case (pattern, transport, kSmallHwm, kSmallHwm, &raw);

            bool backpressured = false;
            const size_t queued = measure_send_window_raw (
              raw.sender, raw.has_target_rid ? &raw.target_rid : NULL, 4096, &backpressured);
            TEST_ASSERT_TRUE_MESSAGE (backpressured, raw_pattern_name (pattern));
            TEST_ASSERT_TRUE (queued > 0);

            if (pattern == raw_pattern_router_router) {
                close_raw_case (&raw);
                continue;
            }

            drain_gate_t drain;
            std::thread drain_thread (drain_raw_receiver_with_ack, raw.receiver, pattern, queued,
                                      &drain);
            start_drain (&drain);

            TEST_ASSERT_TRUE (wait_drain_done (&drain, kTimeoutMs));
            drain_thread.join ();
            TEST_ASSERT_EQUAL_UINT (queued, drain.received);
            TEST_ASSERT_EQUAL_INT (0, drain.error_code);

            if (pattern == raw_pattern_dealer_dealer) {
                TEST_ASSERT_SUCCESS_ERRNO (recv_one_raw_message (raw.sender, false, NULL));
            } else if (pattern == raw_pattern_dealer_router) {
                TEST_ASSERT_SUCCESS_ERRNO (recv_one_raw_message (raw.sender, false, NULL));
            }

            set_send_timeout (raw.sender, kTimeoutMs);
            send_raw_part_blocking (raw.sender, raw.has_target_rid ? &raw.target_rid : NULL);
            set_send_timeout (raw.sender, 0);

            TEST_ASSERT_SUCCESS_ERRNO (recv_one_raw_message (
              raw.receiver,
              pattern == raw_pattern_dealer_router || pattern == raw_pattern_router_router, NULL));
            close_raw_case (&raw);
        }
    }
}

static void verify_raw_rcvhwm_effect ()
{
    for (size_t transport_index = 0;
         transport_index < sizeof (kTransports) / sizeof (kTransports[0]); ++transport_index) {
        const char *transport = kTransports[transport_index];
        if (!is_transport_available (transport))
            continue;

        for (int pattern_value = raw_pattern_dealer_dealer;
             pattern_value <= raw_pattern_router_router; ++pattern_value) {
            const raw_pattern_t pattern = static_cast<raw_pattern_t> (pattern_value);

            raw_case_t low_rcv;
            setup_raw_case (pattern, transport, kLargeHwm, kSmallHwm, &low_rcv);
            const size_t low_count = measure_send_window_raw (
              low_rcv.sender, low_rcv.has_target_rid ? &low_rcv.target_rid : NULL, 512, NULL);
            close_raw_case (&low_rcv);

            raw_case_t high_rcv;
            setup_raw_case (pattern, transport, kLargeHwm, kLargeHwm, &high_rcv);
            const size_t high_count = measure_send_window_raw (
              high_rcv.sender, high_rcv.has_target_rid ? &high_rcv.target_rid : NULL, 512, NULL);
            close_raw_case (&high_rcv);

            char label[128];
            snprintf (label, sizeof (label), "%s %s rcvhwm effect", raw_pattern_name (pattern),
                      transport);
            TEST_ASSERT_TRUE_MESSAGE (low_count <= high_count, label);
        }
    }
}

static void verify_pubsub_matrix ()
{
    for (size_t transport_index = 0;
         transport_index < sizeof (kTransports) / sizeof (kTransports[0]); ++transport_index) {
        const char *transport = kTransports[transport_index];
        if (!is_transport_available (transport))
            continue;

        std::vector<size_t> counts;
        for (size_t i = 0; i < sizeof (kHwmBuckets) / sizeof (kHwmBuckets[0]); ++i) {
            pubsub_case_t pubsub;
            const uint64_t hwm_bytes =
              static_cast<uint64_t> (kHwmBuckets[i])
              * (kPayloadSize + sizeof (zlink_msg_t));
            setup_pubsub_case (transport, hwm_bytes, kLargeHwm, &pubsub);
            counts.push_back (measure_send_window_pubsub (
              pubsub.pub, static_cast<size_t> (kHwmBuckets[i]) + 1, NULL));
            close_pubsub_case (&pubsub);
        }

        char label[128];
        snprintf (label, sizeof (label), "PUBSUB %s sndhwm progress", transport);
        assert_progress_monotonic (counts, label);
    }
}

static void verify_pubsub_pressure_entry_and_resume ()
{
    for (size_t transport_index = 0;
         transport_index < sizeof (kTransports) / sizeof (kTransports[0]); ++transport_index) {
        const char *transport = kTransports[transport_index];
        if (!is_transport_available (transport))
            continue;

        pubsub_case_t entry_case;
        setup_pubsub_case (transport, kSmallHwm, kSmallHwm, &entry_case);

        bool backpressured = false;
        const size_t queued = measure_send_window_pubsub (entry_case.pub, 4096, &backpressured);
        TEST_ASSERT_TRUE_MESSAGE (backpressured, transport);
        TEST_ASSERT_TRUE (queued > 0);

        drain_gate_t drain;
        std::thread drain_thread (drain_subscription_receiver, entry_case.sub, queued + 1, &drain);
        start_drain (&drain);
        set_send_timeout (entry_case.pub, kTimeoutMs);
        publish_part_blocking (entry_case.pub);
        set_send_timeout (entry_case.pub, 0);

        TEST_ASSERT_TRUE (wait_drain_done (&drain, kTimeoutMs));
        drain_thread.join ();
        TEST_ASSERT_EQUAL_UINT (queued + 1, drain.received);
        TEST_ASSERT_EQUAL_INT (0, drain.error_code);
        close_pubsub_case (&entry_case);

        pubsub_case_t low_rcv;
        setup_pubsub_case (transport, kLargeHwm, kSmallHwm, &low_rcv);
        const size_t low_count = measure_send_window_pubsub (low_rcv.pub, 512, NULL);
        close_pubsub_case (&low_rcv);

        pubsub_case_t high_rcv;
        setup_pubsub_case (transport, kLargeHwm, kLargeHwm, &high_rcv);
        const size_t high_count = measure_send_window_pubsub (high_rcv.pub, 512, NULL);
        close_pubsub_case (&high_rcv);

        char label[128];
        snprintf (label, sizeof (label), "PUBSUB %s rcvhwm effect", transport);
        TEST_ASSERT_TRUE_MESSAGE (low_count <= high_count, label);
    }
}

} // namespace

void test_single_socket_backpressure_oneway_matrix ()
{
    verify_raw_progress_matrix ();
    verify_raw_pressure_entry_and_resume ();
    verify_raw_rcvhwm_effect ();
}

void test_multi_pubsub_backpressure_oneway_matrix ()
{
    verify_pubsub_matrix ();
    verify_pubsub_pressure_entry_and_resume ();
}


static bool should_run_case (const char *name_)
{
    const char *selected = getenv ("ZLINK_TEST_CASE");
    return !selected || !*selected || strcmp (selected, name_) == 0;
}

int main (int, char **)
{
    setup_test_environment (600);

    UNITY_BEGIN ();
    if (should_run_case ("test_single_socket_backpressure_oneway_matrix"))
        RUN_TEST (test_single_socket_backpressure_oneway_matrix);
    if (should_run_case ("test_multi_pubsub_backpressure_oneway_matrix"))
        RUN_TEST (test_multi_pubsub_backpressure_oneway_matrix);
    const int status = UNITY_END ();
    fflush (NULL);
    return status;
}
