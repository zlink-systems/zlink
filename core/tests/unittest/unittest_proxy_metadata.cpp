/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "contract_socket_pair_fixture.hpp"
#include <condition_variable>
#include "testutil_unity.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "sockets/proxy/proxy.hpp"

#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

#define CONTENT_SIZE 13
#define CONTENT_SIZE_MAX 32
#define ROUTING_ID_SIZE 10
#define ROUTING_ID_SIZE_MAX 32
#define QT_WORKERS 5
#define QT_CLIENTS 3
#define is_verbose 0

struct proxy_thread_data
{
    proxy_thread_data (void *frontend_, void *backend_, void *capture_) :
        frontend (frontend_), backend (backend_), capture (capture_),
        result (ZLINK_CONFIG_OK), error (0), done (false)
    {
    }

    void *frontend;
    void *backend;
    void *capture;
    zlink_config_result_t result;
    int error;
    std::atomic<bool> done;
    std::mutex mutex;
    std::condition_variable changed;
};

void setUp ()
{
    setup_test_context ();
    zlink::test_reset_proxy_state ();
}

void tearDown ()
{
    zlink::test_reset_proxy_state ();
    teardown_test_context ();
}

static void metadata_proxy_task (void *arg_)
{
    proxy_thread_data *const data = static_cast<proxy_thread_data *> (arg_);
    data->result = zlink_proxy (data->frontend, data->backend, data->capture);
    data->error = zlink_errno ();
    {
        std::lock_guard<std::mutex> lock (data->mutex);
        data->done.store (true, std::memory_order_release);
    }
    data->changed.notify_all ();
}

static void assert_raw_dealer_part (void *socket_,
                                    const char *expected_,
                                    zlink_part_flag_t expected_more_)
{
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));

    const int timeout = 1000;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      socket_, ZLINK_OPT_RCVTIMEO, &timeout, sizeof (timeout)));
    const zlink_recv_result_t result = zlink_recv_part (
      socket_, NULL, &part, &has_more, ZLINK_RECV_FLAGS_NONE);

    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
    TEST_ASSERT_EQUAL_INT (expected_more_, has_more);
    TEST_ASSERT_EQUAL_STRING_LEN (
      expected_, static_cast<const char *> (zlink_msg_data (&part)),
      strlen (expected_));

    unsigned char retained_kind = 0xff;
    uint64_t retained_sequence = UINT64_MAX;
    TEST_ASSERT_FALSE (
      reinterpret_cast<zlink::msg_t *> (&part)
        ->get_request_reply_metadata (&retained_kind, &retained_sequence));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
}

// Supply the proxy's private input directly: public send deliberately validates
// request/reply metadata before it reaches this component boundary.
static void inject_proxy_part (zlink::pipe_t *source_, zlink_msg_t *part_,
                                zlink_part_flag_t more_)
{
    zlink::msg_t *const internal = reinterpret_cast<zlink::msg_t *> (part_);
    if (more_ == ZLINK_PART_MORE)
        internal->set_flags (zlink::msg_t::more);
    TEST_ASSERT_TRUE (source_->write (internal));
    TEST_ASSERT_SUCCESS_ERRNO (internal->init ());
    if (more_ == ZLINK_PART_FINAL)
        source_->flush ();
}

static bool wait_for_proxy_exit (proxy_thread_data *data_)
{
    std::unique_lock<std::mutex> lock (data_->mutex);
    return data_->changed.wait_for (lock, std::chrono::milliseconds (1000),
      [data_] { return data_->done.load (std::memory_order_acquire); });
}

static void assert_raw_pair_part (void *socket_, const char *expected_)
{
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    const int timeout = 1000;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      socket_, ZLINK_OPT_RCVTIMEO, &timeout, sizeof (timeout)));
    const zlink_recv_result_t result = zlink_recv_part (
      socket_, NULL, &part, &has_more, ZLINK_RECV_FLAGS_NONE);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING_LEN (
      expected_, static_cast<const char *> (zlink_msg_data (&part)),
      strlen (expected_));
    TEST_ASSERT_EQUAL_UINT64 (strlen (expected_), zlink_msg_size (&part));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
}

void test_proxy_and_capture_clear_request_reply_metadata ()
{
    void *const context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);

    void *const frontend = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const backend = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const capture = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const source = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const sink = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const capture_sink = zlink_socket (context, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (frontend);
    TEST_ASSERT_NOT_NULL (backend);
    TEST_ASSERT_NOT_NULL (capture);
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_NOT_NULL (sink);
    TEST_ASSERT_NOT_NULL (capture_sink);

    const int zero = 0;
    void *const sockets[] = {frontend, backend, capture, source, sink,
                             capture_sink};
    for (size_t i = 0; i < sizeof (sockets) / sizeof (sockets[0]); ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (sockets[i], ZLINK_OPT_LINGER, &zero,
                            sizeof (zero)));
    }

    contract_socket_pair_t source_pair (source, frontend, 0);
    contract_socket_pair_t sink_pair (backend, sink, 0);
    contract_socket_pair_t capture_pair (capture, capture_sink, 0);

    proxy_thread_data proxy_data (frontend, backend, capture);
    void *const proxy_thread =
      zlink_thread_start (&metadata_proxy_task, &proxy_data);
    TEST_ASSERT_NOT_NULL (proxy_thread);

    zlink_msg_t head;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&head, 4));
    memcpy (zlink_msg_data (&head), "head", 4);
    TEST_ASSERT_SUCCESS_ERRNO (
      reinterpret_cast<zlink::msg_t *> (&head)
        ->set_request_reply_metadata (zlink::request_reply::request_type,
                                      0x1122334455667788ULL));
    inject_proxy_part (source_pair.application[0], &head, ZLINK_PART_MORE);

    zlink_msg_t tail;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&tail, 4));
    memcpy (zlink_msg_data (&tail), "tail", 4);
    inject_proxy_part (source_pair.application[0], &tail, ZLINK_PART_FINAL);

    assert_raw_dealer_part (sink, "head", ZLINK_PART_MORE);
    assert_raw_dealer_part (sink, "tail", ZLINK_PART_FINAL);
    assert_raw_dealer_part (capture_sink, "head", ZLINK_PART_MORE);
    assert_raw_dealer_part (capture_sink, "tail", ZLINK_PART_FINAL);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (context));
    zlink_thread_join (proxy_thread);
    TEST_ASSERT_TRUE (
      proxy_data.result == ZLINK_CONFIG_OK
      || (proxy_data.result == ZLINK_CONFIG_INTERNAL_ERROR
          && proxy_data.error == ETERM));

    for (size_t i = 0; i < sizeof (sockets) / sizeof (sockets[0]); ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_close (sockets[i]));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (context));
}

void test_proxy_rejects_request_reply_metadata_after_first_part ()
{
    void *const context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *const frontend = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const backend = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const capture = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const source = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const sink = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const capture_sink = zlink_socket (context, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (frontend);
    TEST_ASSERT_NOT_NULL (backend);
    TEST_ASSERT_NOT_NULL (capture);
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_NOT_NULL (sink);
    TEST_ASSERT_NOT_NULL (capture_sink);

    const int zero = 0;
    void *const sockets[] = {frontend, backend, capture, source, sink,
                             capture_sink};
    for (size_t i = 0; i < sizeof (sockets) / sizeof (sockets[0]); ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (sockets[i], ZLINK_OPT_LINGER, &zero,
                            sizeof (zero)));
    }
    contract_socket_pair_t source_pair (source, frontend, 0);
    contract_socket_pair_t sink_pair (backend, sink, 0);
    contract_socket_pair_t capture_pair (capture, capture_sink, 0);

    proxy_thread_data proxy_data (frontend, backend, capture);
    void *const proxy_thread =
      zlink_thread_start (&metadata_proxy_task, &proxy_data);
    TEST_ASSERT_NOT_NULL (proxy_thread);

    zlink_msg_t head;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&head, 4));
    memcpy (zlink_msg_data (&head), "head", 4);
    inject_proxy_part (source_pair.application[0], &head, ZLINK_PART_MORE);
    zlink_msg_t tail;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&tail, 4));
    memcpy (zlink_msg_data (&tail), "tail", 4);
    TEST_ASSERT_SUCCESS_ERRNO (
      reinterpret_cast<zlink::msg_t *> (&tail)
        ->set_request_reply_metadata (zlink::request_reply::reply_type, 45));
    inject_proxy_part (source_pair.application[0], &tail, ZLINK_PART_FINAL);

    const bool completed_before_shutdown = wait_for_proxy_exit (&proxy_data);
    if (!completed_before_shutdown)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (context));
    zlink_thread_join (proxy_thread);
    TEST_ASSERT_TRUE_MESSAGE (
      completed_before_shutdown,
      "proxy did not reject malformed inproc multipart promptly");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_STATE, proxy_data.result);
    TEST_ASSERT_EQUAL_INT (EPROTO, proxy_data.error);

    void *const receivers[] = {sink, capture_sink};
    for (size_t i = 0; i < sizeof (receivers) / sizeof (receivers[0]); ++i) {
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_NO_DATA,
          zlink_recv_part (receivers[i], NULL, &part, &has_more,
                           ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (context));
    for (size_t i = 0; i < sizeof (sockets) / sizeof (sockets[0]); ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_close (sockets[i]));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (context));
}

void test_proxy_rolls_back_capture_after_destination_send_failure ()
{
    void *const context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *const first_frontend = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const second_frontend = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const backend = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const capture = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const first_source = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const second_source = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const sink = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const capture_sink = zlink_socket (context, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (first_frontend);
    TEST_ASSERT_NOT_NULL (second_frontend);
    TEST_ASSERT_NOT_NULL (backend);
    TEST_ASSERT_NOT_NULL (capture);
    TEST_ASSERT_NOT_NULL (first_source);
    TEST_ASSERT_NOT_NULL (second_source);
    TEST_ASSERT_NOT_NULL (sink);
    TEST_ASSERT_NOT_NULL (capture_sink);

    const int zero = 0;
    void *const sockets[] = {first_frontend, second_frontend, backend, capture,
                             first_source, second_source, sink, capture_sink};
    for (size_t i = 0; i != sizeof (sockets) / sizeof (sockets[0]); ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (sockets[i], ZLINK_OPT_LINGER, &zero,
                            sizeof (zero)));
    }

    contract_socket_pair_t first_source_pair (first_source, first_frontend, 0);
    contract_socket_pair_t second_source_pair (second_source, second_frontend, 0);
    contract_socket_pair_t sink_pair (backend, sink, 0);
    contract_socket_pair_t capture_pair (capture, capture_sink, 0);

    proxy_thread_data first_proxy (first_frontend, backend, capture);
    zlink::test_fail_next_proxy_destination_send ();
    void *const first_proxy_thread =
      zlink_thread_start (&metadata_proxy_task, &first_proxy);
    TEST_ASSERT_NOT_NULL (first_proxy_thread);

    zlink_msg_t orphan_head;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&orphan_head, 6));
    memcpy (zlink_msg_data (&orphan_head), "orphan", 6);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (first_source, &orphan_head, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_MORE, NULL, NULL));
    zlink_msg_t orphan_tail;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&orphan_tail, 4));
    memcpy (zlink_msg_data (&orphan_tail), "tail", 4);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (first_source, &orphan_tail, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));

    TEST_ASSERT_TRUE_MESSAGE (
      wait_for_proxy_exit (&first_proxy),
      "proxy did not report the injected destination send failure");
    zlink_thread_join (first_proxy_thread);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INTERNAL_ERROR, first_proxy.result);
    TEST_ASSERT_EQUAL_INT (EAGAIN, first_proxy.error);

    proxy_thread_data second_proxy (second_frontend, backend, capture);
    void *const second_proxy_thread =
      zlink_thread_start (&metadata_proxy_task, &second_proxy);
    TEST_ASSERT_NOT_NULL (second_proxy_thread);

    zlink_msg_t fresh;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&fresh, 5));
    memcpy (zlink_msg_data (&fresh), "fresh", 5);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (second_source, &fresh, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));

    assert_raw_pair_part (sink, "fresh");
    assert_raw_pair_part (capture_sink, "fresh");

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (context));
    zlink_thread_join (second_proxy_thread);
    TEST_ASSERT_TRUE (
      second_proxy.result == ZLINK_CONFIG_OK
      || (second_proxy.result == ZLINK_CONFIG_INTERNAL_ERROR
          && second_proxy.error == ETERM));
    for (size_t i = 0; i != sizeof (sockets) / sizeof (sockets[0]); ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_close (sockets[i]));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (context));
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_proxy_and_capture_clear_request_reply_metadata);
    RUN_TEST (test_proxy_rejects_request_reply_metadata_after_first_part);
    RUN_TEST (test_proxy_rolls_back_capture_after_destination_send_failure);
    return UNITY_END ();
}
