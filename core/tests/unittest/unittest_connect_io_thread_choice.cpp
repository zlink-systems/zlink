/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"
#include "../testutil.hpp"

#include "sockets/common/routed_submit_target.hpp"
#include "core/ctx.hpp"
#include "core/internal_defs.hpp"
#include "core/session_base.hpp"
#include "sockets/common/socket_base.hpp"

#include <algorithm>
#include <numeric>
#include <vector>

namespace zlink
{
class session_termination_test_access_t
{
  public:
    static std::vector<unsigned int>
    connect_session_counts (const std::vector<socket_base_t *> &sockets_,
                            io_thread_t *const *threads_,
                            size_t thread_count_)
    {
        std::vector<unsigned int> counts (thread_count_, 0);
        for (size_t j = 0; j != sockets_.size (); ++j) {
            socket_base_t *socket = sockets_[j];
            std::lock_guard<std::recursive_mutex> lock (
              socket->endpoint_runtime ().attempts_sync);
            for (socket_endpoints_t::const_iterator it =
                   socket->endpoint_runtime ().endpoints.begin ();
                 it != socket->endpoint_runtime ().endpoints.end (); ++it) {
                const session_base_t *session =
                  dynamic_cast<const session_base_t *> (it->second.endpoint);
                if (!session)
                    continue;
                for (size_t i = 0; i != thread_count_; ++i)
                    if (session->_io_thread == threads_[i])
                        ++counts[i];
            }
        }
        return counts;
    }
};
}

void setUp ()
{
}

void tearDown ()
{
}

namespace
{
void test_connect_session_round_robin (const char *protocol_)
{
    zlink::ctx_t *ctx = new zlink::ctx_t;
    TEST_ASSERT_NOT_NULL (ctx);

    const int io_threads = 4;
    TEST_ASSERT_SUCCESS_ERRNO (ctx->set (ZLINK_IO_THREADS, &io_threads, sizeof (io_threads)));
    const int zero_linger = 0;
    const int reconnect_ivl = 60000;

    zlink::socket_base_t *publisher = ctx->create_socket (ZLINK_CORE_SOCKET_PUB);
    TEST_ASSERT_NOT_NULL (publisher);
    TEST_ASSERT_SUCCESS_ERRNO (
      publisher->setsockopt (ZLINK_INTERNAL_OPT_LINGER, &zero_linger, sizeof zero_linger));
    tls_test_files_t tls_files;
#if defined ZLINK_HAVE_TLS && defined ZLINK_HAVE_ASIO_SSL
    if (strcmp (protocol_, "tls") == 0) {
        tls_files = make_tls_test_files ();
        TEST_ASSERT_SUCCESS_ERRNO (publisher->setsockopt (
          ZLINK_INTERNAL_OPT_TLS_CERT, tls_files.server_cert.c_str (), tls_files.server_cert.size ()));
        TEST_ASSERT_SUCCESS_ERRNO (publisher->setsockopt (
          ZLINK_INTERNAL_OPT_TLS_KEY, tls_files.server_key.c_str (), tls_files.server_key.size ()));
    }
#endif
    char endpoint[256];
    snprintf (endpoint, sizeof endpoint, "%s://127.0.0.1:*", protocol_);
    TEST_ASSERT_SUCCESS_ERRNO (publisher->bind (endpoint));
    size_t endpoint_size = sizeof endpoint;
    TEST_ASSERT_SUCCESS_ERRNO (
      publisher->getsockopt (ZLINK_INTERNAL_OPT_LAST_ENDPOINT, endpoint, &endpoint_size));

    zlink::io_thread_t *threads[4];
    for (size_t i = 0; i != 4; ++i) {
        threads[i] = ctx->choose_io_thread (uint64_t (1) << i);
        TEST_ASSERT_NOT_NULL (threads[i]);
    }
    printf ("%s io threads: %p/%p/%p/%p\n", protocol_, static_cast<void *> (threads[0]),
            static_cast<void *> (threads[1]), static_cast<void *> (threads[2]),
            static_cast<void *> (threads[3]));

    std::vector<zlink::socket_base_t *> subscribers;
    subscribers.reserve (100);
    for (int i = 0; i != 100; ++i) {
        zlink::socket_base_t *subscriber = ctx->create_socket (ZLINK_CORE_SOCKET_SUB);
        TEST_ASSERT_NOT_NULL (subscriber);
        TEST_ASSERT_SUCCESS_ERRNO (
          subscriber->setsockopt (ZLINK_INTERNAL_OPT_LINGER, &zero_linger, sizeof zero_linger));
        TEST_ASSERT_SUCCESS_ERRNO (subscriber->setsockopt (ZLINK_INTERNAL_OPT_RECONNECT_IVL,
                                                           &reconnect_ivl, sizeof reconnect_ivl));
#if defined ZLINK_HAVE_TLS && defined ZLINK_HAVE_ASIO_SSL
        if (strcmp (protocol_, "tls") == 0) {
            const int no_system_trust = 0;
            TEST_ASSERT_SUCCESS_ERRNO (subscriber->setsockopt (
              ZLINK_INTERNAL_OPT_TLS_TRUST_SYSTEM, &no_system_trust, sizeof no_system_trust));
            TEST_ASSERT_SUCCESS_ERRNO (subscriber->setsockopt (
              ZLINK_INTERNAL_OPT_TLS_CA, tls_files.ca_cert.c_str (), tls_files.ca_cert.size ()));
            TEST_ASSERT_SUCCESS_ERRNO (
              subscriber->setsockopt (ZLINK_INTERNAL_OPT_TLS_HOSTNAME, "localhost",
                                      strlen ("localhost")));
        }
#endif
        TEST_ASSERT_SUCCESS_ERRNO (subscriber->connect (endpoint));
        subscribers.push_back (subscriber);
    }

    const std::vector<unsigned int> counts =
      zlink::session_termination_test_access_t::connect_session_counts (subscribers, threads, 4);
    const unsigned int min_count = *std::min_element (counts.begin (), counts.end ());
    const unsigned int max_count = *std::max_element (counts.begin (), counts.end ());
    printf ("%s connect session counts: %u/%u/%u/%u\n", protocol_, counts[0], counts[1], counts[2],
            counts[3]);
    TEST_ASSERT_SUCCESS_ERRNO (ctx->close_socket_and_wait (publisher, 1000));
    for (size_t i = 0; i != subscribers.size (); ++i)
        TEST_ASSERT_SUCCESS_ERRNO (ctx->close_socket_and_wait (subscribers[i], 1000));
    ctx->terminate ();
    if (!tls_files.dir.empty ())
        cleanup_tls_test_files (tls_files);
    TEST_ASSERT_EQUAL_UINT (100, std::accumulate (counts.begin (), counts.end (), 0u));
    TEST_ASSERT_TRUE_MESSAGE (max_count - min_count <= 1, "connect sessions are not round-robin");
}

void test_tcp_connect_sessions_round_robin_across_io_threads ()
{
    test_connect_session_round_robin ("tcp");
}

#if defined ZLINK_HAVE_TLS && defined ZLINK_HAVE_ASIO_SSL
void test_tls_connect_sessions_round_robin_across_io_threads ()
{
    test_connect_session_round_robin ("tls");
}
#endif
}

extern "C" int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (test_tcp_connect_sessions_round_robin_across_io_threads);
#if defined ZLINK_HAVE_TLS && defined ZLINK_HAVE_ASIO_SSL
    RUN_TEST (test_tls_connect_sessions_round_robin_across_io_threads);
#endif
    return UNITY_END ();
}
