/* SPDX-License-Identifier: MPL-2.0 */

//  Pins the write-turn admission invariant that
//  asio_stream_fastpath_policy::use_speculative_write_for owns.
//
//  A speculative synchronous write turn drains the session pipe straight into
//  the kernel and stops only when the pipe runs dry or the kernel send buffer
//  returns EAGAIN. Only the STREAM fast path is specified to do that, and only
//  because spec_write_budget_bytes bounds the turn
//  (core/doc/spec/core/socket/08-stream.ko.md:395-401); every other socket is
//  specified to use the Proactor write path
//  (core/doc/spec/core/systems/03-io-thread.ko.md section 4).
//
//  Admitting a general socket into that turn relocates the application queue
//  into kernel socket memory, where byte-HWM accounting cannot see or bound
//  it. Measured on PAIR/tcp/64B before the fix: 9,485 KiB resident in kernel
//  buffers against a 1 MiB pipe HWM, 0 backpressure parks in 12.25M sends, and
//  62.2 ms mean latency against 0.445 ms on the Proactor path.
//
//  These are structural assertions on a pure predicate, not timing assertions.

#include "../testutil_unity.hpp"

#include "engine/asio/asio_stream_fastpath_policy.hpp"

#include <unity.h>
#include <cstdlib>

namespace
{
namespace policy = zlink::asio_stream_fastpath_policy;

const char *const legacy_sync_write_env = "ZLINK_ASIO_LEGACY_SYNC_WRITE";
const char *const stream_async_write_env = "ZLINK_ASIO_STREAM_ASYNC_WRITE";

void set_diagnostic_option (const char *name_, bool enabled_)
{
#ifdef ZLINK_HAVE_WINDOWS
    TEST_ASSERT_EQUAL_INT (0, _putenv_s (name_, enabled_ ? "1" : ""));
#else
    if (enabled_)
        TEST_ASSERT_EQUAL_INT (0, setenv (name_, "1", 1));
    else
        TEST_ASSERT_EQUAL_INT (0, unsetenv (name_));
#endif
}

policy::connection_fastpath_policy_t make_policy (int socket_type_,
                                                  const char *transport_name_,
                                                  bool transport_supports_speculative_,
                                                  bool legacy_sync_write_ = false,
                                                  bool stream_async_write_ = false)
{
    const policy::speculative_write_diagnostics_t diagnostics = {
      legacy_sync_write_, stream_async_write_};
    return policy::connection_fastpath_policy_t (
      socket_type_, transport_name_, transport_supports_speculative_, diagnostics);
}

//  Every socket type the asio engine can carry except STREAM.
const int general_socket_types[] = {
  ZLINK_CORE_SOCKET_PAIR,   ZLINK_CORE_SOCKET_PUB,    ZLINK_CORE_SOCKET_SUB,
  ZLINK_CORE_SOCKET_DEALER, ZLINK_CORE_SOCKET_ROUTER, ZLINK_CORE_SOCKET_XPUB,
  ZLINK_CORE_SOCKET_XSUB};

const size_t general_socket_type_count =
  sizeof (general_socket_types) / sizeof (general_socket_types[0]);
}

void setUp ()
{
    set_diagnostic_option (legacy_sync_write_env, false);
    set_diagnostic_option (stream_async_write_env, false);
}

void tearDown ()
{
    set_diagnostic_option (legacy_sync_write_env, false);
    set_diagnostic_option (stream_async_write_env, false);
}

//  The one admitted case: STREAM over tcp, where the byte budget applies.
void test_stream_tcp_is_admitted ()
{
    const policy::connection_fastpath_policy_t connection =
      make_policy (ZLINK_CORE_SOCKET_STREAM, "tcp", true);
    TEST_ASSERT_TRUE (connection.tcp_transport ());
    TEST_ASSERT_TRUE (connection.speculative_write_enabled ());
}

//  STREAM off tcp has no budgeted turn, so it is not admitted either.
void test_stream_off_tcp_is_not_admitted ()
{
    const policy::connection_fastpath_policy_t connection =
      make_policy (ZLINK_CORE_SOCKET_STREAM, "wss", true);
    TEST_ASSERT_FALSE (connection.tcp_transport ());
    TEST_ASSERT_FALSE (connection.speculative_write_enabled ());
}

//  The regression itself. No general socket may drain synchronously, on tcp or
//  off it, whatever its transport reports. This assertion fails if the
//  unbounded turn is ever re-admitted for DEALER/ROUTER/PAIR/PUB/SUB.
void test_no_general_socket_is_admitted ()
{
    for (size_t i = 0; i < general_socket_type_count; ++i) {
        const int socket_type = general_socket_types[i];
        const policy::connection_fastpath_policy_t tcp_connection =
          make_policy (socket_type, "tcp", true);
        const policy::connection_fastpath_policy_t wss_connection =
          make_policy (socket_type, "wss", true);
        TEST_ASSERT_FALSE_MESSAGE (
          tcp_connection.speculative_write_enabled (),
          "general socket admitted into the synchronous write turn on tcp");
        TEST_ASSERT_FALSE_MESSAGE (
          wss_connection.speculative_write_enabled (),
          "general socket admitted into the synchronous write turn off tcp");
    }
}

//  A transport that does not support speculative write is never admitted.
void test_transport_without_speculative_support_is_not_admitted ()
{
    for (size_t i = 0; i < general_socket_type_count; ++i) {
        const policy::connection_fastpath_policy_t connection =
          make_policy (general_socket_types[i], "tcp", false, true);
        TEST_ASSERT_FALSE (connection.speculative_write_enabled ());
    }
}

//  The admitted turn must stay bounded: a zero budget would restore the
//  unbounded drain for STREAM too.
void test_admitted_turn_carries_a_positive_byte_budget ()
{
    TEST_ASSERT_TRUE (policy::spec_write_budget_bytes () > 0);
}

//  Both escape hatches are diagnostics and must be off unless asked for, so
//  the shipped default is the Proactor path for general sockets and the
//  spec'd speculative write for STREAM.
void test_diagnostic_opt_ins_default_off ()
{
    const policy::speculative_write_diagnostics_t diagnostics =
      policy::load_speculative_write_diagnostics ();
    TEST_ASSERT_FALSE (diagnostics.legacy_sync_write);
    TEST_ASSERT_FALSE (diagnostics.stream_async_write);
}

//  Environment options are creation-time diagnostics. Changing one affects
//  the next connection policy but cannot rewrite an existing connection.
void test_legacy_sync_write_is_snapshotted_per_connection ()
{
    const policy::connection_fastpath_policy_t before =
      policy::connection_fastpath_policy_t::from_environment (
        ZLINK_CORE_SOCKET_PAIR, "tcp", true);

    set_diagnostic_option (legacy_sync_write_env, true);
    const policy::connection_fastpath_policy_t enabled =
      policy::connection_fastpath_policy_t::from_environment (
        ZLINK_CORE_SOCKET_PAIR, "tcp", true);

    set_diagnostic_option (legacy_sync_write_env, false);
    const policy::connection_fastpath_policy_t after =
      policy::connection_fastpath_policy_t::from_environment (
        ZLINK_CORE_SOCKET_PAIR, "tcp", true);

    TEST_ASSERT_FALSE (before.speculative_write_enabled ());
    TEST_ASSERT_TRUE (enabled.speculative_write_enabled ());
    TEST_ASSERT_FALSE (after.speculative_write_enabled ());
}

void test_stream_async_write_is_snapshotted_per_connection ()
{
    const policy::connection_fastpath_policy_t before =
      policy::connection_fastpath_policy_t::from_environment (
        ZLINK_CORE_SOCKET_STREAM, "tcp", true);

    set_diagnostic_option (stream_async_write_env, true);
    const policy::connection_fastpath_policy_t async =
      policy::connection_fastpath_policy_t::from_environment (
        ZLINK_CORE_SOCKET_STREAM, "tcp", true);

    set_diagnostic_option (stream_async_write_env, false);
    const policy::connection_fastpath_policy_t after =
      policy::connection_fastpath_policy_t::from_environment (
        ZLINK_CORE_SOCKET_STREAM, "tcp", true);

    TEST_ASSERT_TRUE (before.speculative_write_enabled ());
    TEST_ASSERT_FALSE (async.speculative_write_enabled ());
    TEST_ASSERT_TRUE (after.speculative_write_enabled ());
}

int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (test_stream_tcp_is_admitted);
    RUN_TEST (test_stream_off_tcp_is_not_admitted);
    RUN_TEST (test_no_general_socket_is_admitted);
    RUN_TEST (test_transport_without_speculative_support_is_not_admitted);
    RUN_TEST (test_admitted_turn_carries_a_positive_byte_budget);
    RUN_TEST (test_diagnostic_opt_ins_default_off);
    RUN_TEST (test_legacy_sync_write_is_snapshotted_per_connection);
    RUN_TEST (test_stream_async_write_is_snapshotted_per_connection);
    return UNITY_END ();
}
