/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#if !defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
#error                                                                                             \
  "Allocation tests require ZLINK_FRAMEWORK_CPP_TEST_PROBES=ON and ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=OFF"
#endif

#include <zlink/framework/detail/test_creation_probe.hpp>
#include "runtime/mesh/service_mailbox.hpp"
#include <type_traits>
#include <gtest/gtest.h>

#if defined(ZLINK_TEST_COUNT_NATIVE_COPIES)
#include "native_call_counter.hpp"
#endif

namespace zlink::framework::tests
{
// A record's permit makes copying ill-formed on every platform. Keep aggregate
// and move semantics intact rather than adding special members for the test.
static_assert (!std::is_copy_constructible_v<runtime::mesh::service_mailbox_record_t>);
static_assert (!std::is_copy_assignable_v<runtime::mesh::service_mailbox_record_t>);
static_assert (std::is_move_constructible_v<runtime::mesh::service_mailbox_record_t>);
static_assert (std::is_aggregate_v<runtime::mesh::service_mailbox_record_t>);

struct creation_counts_t
{
    std::size_t shared_states;
    std::size_t coroutine_states;
    std::size_t payload_copies;
};

inline creation_counts_t creation_counts ()
{
    namespace probe = detail::test_creation_probe;
    return {probe::shared_states.load (std::memory_order_relaxed),
            probe::coroutine_states.load (std::memory_order_relaxed),
            probe::payload_copies.load (std::memory_order_relaxed)};
}

inline creation_counts_t operator- (creation_counts_t after, creation_counts_t before)
{
    return {after.shared_states - before.shared_states,
            after.coroutine_states - before.coroutine_states,
            after.payload_copies - before.payload_copies};
}
inline task_t<void> creation_counter_coroutine ()
{
    co_return;
}

inline void verify_creation_counter_controls ()
{
    const auto before = creation_counts ();
    task_completion_source_t<void> source;
    auto coroutine = creation_counter_coroutine ();
    EXPECT_TRUE (coroutine.result ());
    const auto states = creation_counts () - before;
    EXPECT_EQ (states.shared_states, 2u);
    EXPECT_EQ (states.coroutine_states, 1u);
    auto payload = encoded_payload_t::from_string ("counter control");
    const auto payload_before = creation_counts ();
    auto copied_payload = payload;
    EXPECT_EQ ((creation_counts () - payload_before).payload_copies, 1u);
#if defined(ZLINK_TEST_COUNT_NATIVE_COPIES)
    auto message = zlink::message_t::from ("native copy control");
    native_call_scope_t native;
    auto copied_message = message.copy ();
    EXPECT_TRUE (copied_message.valid ());
    EXPECT_EQ (native[3], 1u);
    EXPECT_EQ (native[6], 1u);
#endif
}
} // namespace zlink::framework::tests
