/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"
#include "../testutil_unity.hpp"

#include <zlink/core/api.h>
#include <zlink/eventing/api.h>
#include <zlink/message/api.h>
#include <zlink/socket/api.h>

#include <unity.h>

#include <cstddef>
#include <cstring>
#include <type_traits>

static_assert (sizeof (zlink_completion_id_t) == sizeof (uint64_t),
               "completion ID ABI must stay 64-bit");
static_assert (sizeof (zlink_reply_token_t) == sizeof (uint64_t),
               "reply token ABI must stay 64-bit");
static_assert (offsetof (zlink_completion_t, struct_size) == 0,
               "completion struct_size must be first");
static_assert (offsetof (zlink_completion_t, kind) == 4,
               "completion kind ABI offset changed");
static_assert (offsetof (zlink_completion_t, completion_id) == 8,
               "completion ID ABI offset changed");
static_assert (std::is_standard_layout<zlink_completion_t>::value,
               "completion must remain a standard-layout C aggregate");

#if UINTPTR_MAX == UINT64_MAX
static_assert (alignof (zlink_completion_t) == 8,
               "64-bit completion ABI alignment changed");
static_assert (sizeof (zlink_completion_t) == 312,
               "64-bit completion ABI size changed");
static_assert (offsetof (zlink_completion_t, user_context) == 16,
               "completion context ABI offset changed");
static_assert (offsetof (zlink_completion_t, peer_rid) == 24,
               "completion RID ABI offset changed");
static_assert (offsetof (zlink_completion_t, send_result) == 280,
               "completion send result ABI offset changed");
static_assert (offsetof (zlink_completion_t, send_terminal_errno) == 284,
               "completion send errno ABI offset changed");
static_assert (offsetof (zlink_completion_t, request_result) == 288,
               "completion request result ABI offset changed");
static_assert (offsetof (zlink_completion_t, reply_parts) == 296,
               "completion reply array ABI offset changed");
static_assert (offsetof (zlink_completion_t, reply_part_count) == 304,
               "completion reply count ABI offset changed");
#endif

static_assert (ZLINK_COMPLETION_SEND == 1, "SEND completion numeric changed");
static_assert (ZLINK_COMPLETION_REQUEST == 2,
               "REQUEST completion numeric changed");
static_assert (ZLINK_SEND_ADMITTED == 0, "ADMITTED numeric changed");
static_assert (ZLINK_SEND_TERMINAL == 202, "TERMINAL numeric changed");
static_assert (ZLINK_STREAM_RECV_MODE_UNSPECIFIED == 0,
               "STREAM unspecified numeric changed");
static_assert (ZLINK_STREAM_RECV_MODE_RAW == 1,
               "STREAM RAW numeric changed");
static_assert (ZLINK_STREAM_RECV_MODE_PACKET == 2,
               "STREAM PACKET numeric changed");
static_assert (ZLINK_STREAM_OPT_RECV_MODE == 0x3502,
               "STREAM mode option numeric changed");
static_assert (ZLINK_OPT_PENDING_MAX_MSGS == 0x303A,
               "pending message option numeric changed");
static_assert (ZLINK_OPT_PENDING_MAX_BYTES == 0x303B,
               "pending byte option numeric changed");

static_assert (offsetof (zlink_poller_event_t, source_kind) == 0,
               "poller source kind ABI offset changed");
#if UINTPTR_MAX == UINT64_MAX
static_assert (alignof (zlink_poller_event_t) == 8,
               "64-bit poller event ABI alignment changed");
static_assert (sizeof (zlink_poller_event_t) == 48,
               "64-bit poller event ABI size changed");
static_assert (offsetof (zlink_poller_event_t, socket) == 8,
               "poller socket ABI offset changed");
static_assert (offsetof (zlink_poller_event_t, fd) == 16,
               "poller fd ABI offset changed");
static_assert (offsetof (zlink_poller_event_t, timer) == 24,
               "poller timer ABI offset changed");
static_assert (offsetof (zlink_poller_event_t, user_data) == 32,
               "poller userdata ABI offset changed");
static_assert (offsetof (zlink_poller_event_t, events) == 40,
               "poller events ABI offset changed");
#elif UINTPTR_MAX == UINT32_MAX
static_assert (alignof (zlink_poller_event_t) == 4,
               "32-bit poller event ABI alignment changed");
static_assert (sizeof (zlink_poller_event_t) == 24,
               "32-bit poller event ABI size changed");
#endif

void setUp ()
{
}

void tearDown ()
{
}

void test_grouped_contract_headers_compile ()
{
    TEST_ASSERT_TRUE (ZLINK_VERSION_MAJOR >= 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, 0);
    TEST_ASSERT_NOT_NULL (reinterpret_cast<void *> (&zlink_ctx_new));
    TEST_ASSERT_NOT_NULL (reinterpret_cast<void *> (&zlink_msg_init));
    TEST_ASSERT_NOT_NULL (reinterpret_cast<void *> (&zlink_socket));
    TEST_ASSERT_NOT_NULL (reinterpret_cast<void *> (&zlink_socket_monitor_open));
}

void test_phase2_wrapper_boundary_contracts ()
{
    void *context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *pair = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *pub = zlink_socket (context, ZLINK_SOCKET_PUB);
    void *stream = zlink_socket (context, ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (pair);
    TEST_ASSERT_NOT_NULL (pub);
    TEST_ASSERT_NOT_NULL (stream);

    zlink_completion_id_t id = 66;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_HANDLE,
      zlink_send_part (pair, NULL, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
                       NULL, &id));
    TEST_ASSERT_EQUAL_INT (EFAULT, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, id);

    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    id = 77;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_send_part (pair, &part, static_cast<zlink_send_flags_t> (0x40),
                       ZLINK_PART_FINAL, NULL, &id));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, id);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    id = 88;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_HANDLE,
      zlink_send_part (NULL, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
                       NULL, &id));
    TEST_ASSERT_EQUAL_INT (EFAULT, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, id);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    id = 99;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_request_part (pair, NULL, &part,
                          static_cast<zlink_send_flags_t> (0x80),
                          ZLINK_PART_FINAL, 0, NULL, &id));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, id);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));

    zlink_completion_t completion;
    std::memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_HANDLE,
      zlink_completion_recv (NULL, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EFAULT, zlink_errno ());

    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_STATE,
      zlink_completion_recv (pair, &completion,
                             static_cast<zlink_recv_flags_t> (0x80)));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    completion.kind = ZLINK_COMPLETION_SEND;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_STATE,
      zlink_completion_recv (pair, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_SEND, completion.kind);
    zlink_completion_close (&completion);
    TEST_ASSERT_EQUAL_UINT32 (sizeof (completion), completion.struct_size);
    TEST_ASSERT_EQUAL_INT (0, static_cast<int> (completion.kind));

    zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_UNSPECIFIED;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_set_stream_option (stream, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    mode = ZLINK_STREAM_RECV_MODE_RAW;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_set_stream_option (stream, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode) - 1));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_stream_option (
                             stream, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                             sizeof (mode)));
    zlink_stream_recv_mode_t observed = ZLINK_STREAM_RECV_MODE_UNSPECIFIED;
    size_t observed_size = sizeof (observed);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_get_stream_option (stream, ZLINK_STREAM_OPT_RECV_MODE, &observed,
                               &observed_size));
    TEST_ASSERT_EQUAL_INT (ZLINK_STREAM_RECV_MODE_RAW, observed);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (observed), observed_size);

    const uint64_t unlimited = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_SUPPORTED,
      zlink_set_option (pub, ZLINK_OPT_PENDING_MAX_MSGS, &unlimited,
                        sizeof (unlimited)));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (pair, ZLINK_OPT_PENDING_MAX_MSGS, &unlimited,
                        sizeof (unlimited)));

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (stream));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (pub));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (pair));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}

void test_pending_limit_option_family_matrix_and_defaults ()
{
    void *context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);

    const zlink_socket_type_t supported_types[] = {
      ZLINK_SOCKET_PAIR, ZLINK_SOCKET_DEALER, ZLINK_SOCKET_ROUTER,
      ZLINK_SOCKET_STREAM};
    const zlink_socket_type_t unsupported_types[] = {
      ZLINK_SOCKET_PUB, ZLINK_SOCKET_SUB, ZLINK_SOCKET_XPUB,
      ZLINK_SOCKET_XSUB};
    const zlink_option_t options[] = {ZLINK_OPT_PENDING_MAX_MSGS,
                                     ZLINK_OPT_PENDING_MAX_BYTES};

    for (size_t type_index = 0;
         type_index != sizeof (supported_types) / sizeof (supported_types[0]);
         ++type_index) {
        void *socket = zlink_socket (context, supported_types[type_index]);
        TEST_ASSERT_NOT_NULL (socket);
        for (size_t option_index = 0;
             option_index != sizeof (options) / sizeof (options[0]);
             ++option_index) {
            uint64_t observed = UINT64_MAX;
            size_t observed_size = sizeof (observed);
            TEST_ASSERT_EQUAL_INT (
              ZLINK_CONFIG_OK,
              zlink_get_option (socket, options[option_index], &observed,
                                &observed_size));
            TEST_ASSERT_EQUAL_UINT64 (0, observed);
            TEST_ASSERT_EQUAL_UINT64 (sizeof (observed), observed_size);

            const uint64_t configured = 7;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_CONFIG_OK,
              zlink_set_option (socket, options[option_index], &configured,
                                sizeof (configured)));
            observed = 0;
            observed_size = sizeof (observed);
            TEST_ASSERT_EQUAL_INT (
              ZLINK_CONFIG_OK,
              zlink_get_option (socket, options[option_index], &observed,
                                &observed_size));
            TEST_ASSERT_EQUAL_UINT64 (configured, observed);
            TEST_ASSERT_EQUAL_UINT64 (sizeof (observed), observed_size);
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
    }

    for (size_t type_index = 0;
         type_index
         != sizeof (unsupported_types) / sizeof (unsupported_types[0]);
         ++type_index) {
        void *socket = zlink_socket (context, unsupported_types[type_index]);
        TEST_ASSERT_NOT_NULL (socket);
        for (size_t option_index = 0;
             option_index != sizeof (options) / sizeof (options[0]);
             ++option_index) {
            const uint64_t configured = 7;
            errno = 0;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_CONFIG_NOT_SUPPORTED,
              zlink_set_option (socket, options[option_index], &configured,
                                sizeof (configured)));
            TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());

            uint64_t observed = UINT64_MAX;
            size_t observed_size = sizeof (observed);
            errno = 0;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_CONFIG_NOT_SUPPORTED,
              zlink_get_option (socket, options[option_index], &observed,
                                &observed_size));
            TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
    }

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_grouped_contract_headers_compile);
    RUN_TEST (test_phase2_wrapper_boundary_contracts);
    RUN_TEST (test_pending_limit_option_family_matrix_and_defaults);
    return UNITY_END ();
}
