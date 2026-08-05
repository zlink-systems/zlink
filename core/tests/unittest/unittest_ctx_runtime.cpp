/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"

#include "core/ctx.hpp"
#include "core/control_runtime.hpp"
#include "sockets/common/socket_base.hpp"

namespace
{
void set_zero_linger (zlink::socket_base_t *socket_)
{
    TEST_ASSERT_NOT_NULL (socket_);
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      socket_->setsockopt (ZLINK_INTERNAL_OPT_LINGER, &zero, sizeof (zero)));
}

void test_ctx_close_socket_and_wait_updates_socket_registry ()
{
    zlink::ctx_t *ctx = new zlink::ctx_t;
    TEST_ASSERT_NOT_NULL (ctx);
    zlink::socket_base_t *socket = ctx->create_socket (ZLINK_CORE_SOCKET_PAIR);

    TEST_ASSERT_NOT_NULL (socket);
    set_zero_linger (socket);
    TEST_ASSERT_EQUAL_UINT64 (1u, static_cast<uint64_t> (ctx->socket_count ()));
    TEST_ASSERT_FAILURE_ERRNO (ETIMEDOUT, ctx->wait_for_socket_count_at_most (0, 0));

    TEST_ASSERT_SUCCESS_ERRNO (ctx->close_socket_and_wait (socket, 1000));
    TEST_ASSERT_NULL (socket);
    TEST_ASSERT_EQUAL_UINT64 (0u, static_cast<uint64_t> (ctx->socket_count ()));
    TEST_ASSERT_SUCCESS_ERRNO (ctx->wait_for_socket_count_at_most (0, 0));
    TEST_ASSERT_SUCCESS_ERRNO (ctx->terminate ());
}

void test_ctx_reuses_released_socket_slot ()
{
    zlink::ctx_t *ctx = new zlink::ctx_t;
    TEST_ASSERT_NOT_NULL (ctx);
    zlink::socket_base_t *first = ctx->create_socket (ZLINK_CORE_SOCKET_PAIR);

    TEST_ASSERT_NOT_NULL (first);
    set_zero_linger (first);
    const uint32_t first_tid = first->get_tid ();
    TEST_ASSERT_SUCCESS_ERRNO (ctx->close_socket_and_wait (first, 1000));

    zlink::socket_base_t *second = ctx->create_socket (ZLINK_CORE_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (second);
    set_zero_linger (second);
    TEST_ASSERT_EQUAL_UINT32 (first_tid, second->get_tid ());
    TEST_ASSERT_SUCCESS_ERRNO (ctx->close_socket_and_wait (second, 1000));
    TEST_ASSERT_SUCCESS_ERRNO (ctx->terminate ());
}

void test_ctx_inproc_endpoint_registry_tracks_owner ()
{
    zlink::ctx_t *ctx = new zlink::ctx_t;
    TEST_ASSERT_NOT_NULL (ctx);

    zlink::socket_base_t *socket = ctx->create_socket (ZLINK_CORE_SOCKET_PAIR);
    zlink::socket_base_t *other_socket = ctx->create_socket (ZLINK_CORE_SOCKET_PAIR);

    TEST_ASSERT_NOT_NULL (socket);
    TEST_ASSERT_NOT_NULL (other_socket);
    set_zero_linger (socket);
    set_zero_linger (other_socket);

    const zlink::endpoint_t endpoint (socket, zlink::options_t ());

    TEST_ASSERT_SUCCESS_ERRNO (ctx->register_endpoint ("inproc://ctx-runtime-registry", endpoint));
    TEST_ASSERT_FAILURE_ERRNO (EADDRINUSE,
                               ctx->register_endpoint ("inproc://ctx-runtime-registry", endpoint));
    TEST_ASSERT_FAILURE_ERRNO (
      ENOENT, ctx->unregister_endpoint ("inproc://ctx-runtime-registry", other_socket));

    ctx->unregister_endpoints (socket);
    TEST_ASSERT_SUCCESS_ERRNO (ctx->register_endpoint ("inproc://ctx-runtime-registry", endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (ctx->unregister_endpoint ("inproc://ctx-runtime-registry", socket));

    TEST_ASSERT_SUCCESS_ERRNO (ctx->close_socket_and_wait (socket, 1000));
    TEST_ASSERT_SUCCESS_ERRNO (ctx->close_socket_and_wait (other_socket, 1000));
    TEST_ASSERT_SUCCESS_ERRNO (ctx->terminate ());
}

void test_ctx_io_thread_selection_respects_affinity ()
{
    zlink::ctx_t *ctx = new zlink::ctx_t;
    TEST_ASSERT_NOT_NULL (ctx);

    const int io_threads = 2;
    TEST_ASSERT_SUCCESS_ERRNO (ctx->set (ZLINK_IO_THREADS, &io_threads, sizeof (io_threads)));

    zlink::socket_base_t *socket = ctx->create_socket (ZLINK_CORE_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);
    set_zero_linger (socket);

    zlink::io_thread_t *first = ctx->choose_io_thread (uint64_t (1) << 0);
    zlink::io_thread_t *second = ctx->choose_io_thread (uint64_t (1) << 1);
    zlink::io_thread_t *stream_first = ctx->choose_io_thread_stream (uint64_t (1) << 0);
    zlink::io_thread_t *stream_second = ctx->choose_io_thread_stream (uint64_t (1) << 1);

    TEST_ASSERT_NOT_NULL (first);
    TEST_ASSERT_NOT_NULL (second);
    TEST_ASSERT_NOT_NULL (stream_first);
    TEST_ASSERT_NOT_NULL (stream_second);
    TEST_ASSERT_NOT_EQUAL (first, second);
    TEST_ASSERT_NOT_EQUAL (stream_first, stream_second);

    TEST_ASSERT_SUCCESS_ERRNO (ctx->close_socket_and_wait (socket, 1000));
    TEST_ASSERT_SUCCESS_ERRNO (ctx->terminate ());
}

void test_ctx_control_runtime_bootstraps_runtime_resources_once ()
{
    zlink::ctx_t *ctx = new zlink::ctx_t;
    TEST_ASSERT_NOT_NULL (ctx);

    zlink::control_runtime_t *first = ctx->control_runtime ();
    TEST_ASSERT_NOT_NULL (first);
    TEST_ASSERT_NOT_NULL (ctx->get_reaper ());

    zlink::control_runtime_t *second = ctx->control_runtime ();
    TEST_ASSERT_EQUAL_PTR (first, second);

    TEST_ASSERT_SUCCESS_ERRNO (ctx->terminate ());
}

}

extern "C" int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (test_ctx_close_socket_and_wait_updates_socket_registry);
    RUN_TEST (test_ctx_reuses_released_socket_slot);
    RUN_TEST (test_ctx_inproc_endpoint_registry_tracks_owner);
    RUN_TEST (test_ctx_io_thread_selection_respects_affinity);
    RUN_TEST (test_ctx_control_runtime_bootstraps_runtime_resources_once);
    return UNITY_END ();
}
