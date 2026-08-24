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

void setUp ()
{
}

void tearDown ()
{
}

namespace
{
namespace policy = zlink::asio_stream_fastpath_policy;

//  Every socket type the asio engine can carry except STREAM.
const int general_socket_types[] = {
  ZLINK_CORE_SOCKET_PAIR,   ZLINK_CORE_SOCKET_PUB,    ZLINK_CORE_SOCKET_SUB,
  ZLINK_CORE_SOCKET_DEALER, ZLINK_CORE_SOCKET_ROUTER, ZLINK_CORE_SOCKET_XPUB,
  ZLINK_CORE_SOCKET_XSUB};

const size_t general_socket_type_count =
  sizeof (general_socket_types) / sizeof (general_socket_types[0]);
}

//  The one admitted case: STREAM over tcp, where the byte budget applies.
void test_stream_tcp_is_admitted ()
{
    TEST_ASSERT_TRUE (
      policy::use_speculative_write_for (ZLINK_CORE_SOCKET_STREAM, true, true));
}

//  STREAM off tcp has no budgeted turn, so it is not admitted either.
void test_stream_off_tcp_is_not_admitted ()
{
    TEST_ASSERT_FALSE (
      policy::use_speculative_write_for (ZLINK_CORE_SOCKET_STREAM, false, true));
}

//  The regression itself. No general socket may drain synchronously, on tcp or
//  off it, whatever its transport reports. This assertion fails if the
//  unbounded turn is ever re-admitted for DEALER/ROUTER/PAIR/PUB/SUB.
void test_no_general_socket_is_admitted ()
{
    for (size_t i = 0; i < general_socket_type_count; ++i) {
        const int socket_type = general_socket_types[i];
        TEST_ASSERT_FALSE_MESSAGE (
          policy::use_speculative_write_for (socket_type, true, true),
          "general socket admitted into the synchronous write turn on tcp");
        TEST_ASSERT_FALSE_MESSAGE (
          policy::use_speculative_write_for (socket_type, false, true),
          "general socket admitted into the synchronous write turn off tcp");
    }
}

//  A transport that does not support speculative write is never admitted.
void test_transport_without_speculative_support_is_not_admitted ()
{
    for (size_t i = 0; i < general_socket_type_count; ++i) {
        TEST_ASSERT_FALSE (
          policy::use_speculative_write_for (general_socket_types[i], true, false));
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
    TEST_ASSERT_FALSE (policy::legacy_sync_write_opt_in ());
    TEST_ASSERT_FALSE (policy::stream_async_write_opt_in ());
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
    return UNITY_END ();
}
