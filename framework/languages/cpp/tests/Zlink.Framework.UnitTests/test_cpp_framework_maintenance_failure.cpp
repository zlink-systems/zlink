/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/stateful_object_sync.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include "runtime/spots/actor_transfer_coordinator.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/allocation_counter.hpp"
#include <gtest/gtest.h>
#include <future>
#include <atomic>
#include <string>

using zlink::framework::test::begin_reserved_object;

namespace
{
constexpr std::size_t inventory_count = 257;
}

namespace zlink::framework
{
TEST (MaintenanceFailure, CompletionSnapshotFailureReleasesSourceSeal)
{
    detail::actor_transfer_coordinator_t transfers;
    for (std::size_t index = 0; index < inventory_count; ++index)
        ASSERT_TRUE (transfers.try_reserve_source (std::to_string (index)));
    auto &failure_size = zlink::framework::tests::process_allocation_failure_size;
    auto &failure_count = zlink::framework::tests::process_allocation_failure_count;
    failure_count.store (0, std::memory_order_relaxed);
    failure_size.store (inventory_count * sizeof (std::shared_ptr<task_completion_source_t<void>>),
                        std::memory_order_relaxed);
    const auto result = transfers.try_seal_maintenance_inventory ().result ();
    failure_size.store (0, std::memory_order_relaxed);
    ASSERT_EQ (failure_count.load (std::memory_order_relaxed), 1);
    ASSERT_FALSE (result);
    // state_lane maps std::exception to the existing InternalFailure contract.
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::internal_failure);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_STREQ (result.error ()->what (), std::bad_alloc ().what ());
    EXPECT_TRUE (transfers.try_reserve_source ("after-failure"));
    for (std::size_t index = 0; index < inventory_count; ++index)
        transfers.cancel_move (std::to_string (index));
    transfers.cancel_move ("after-failure");
}

TEST (MaintenanceFailure, NativeInventoryFailureReleasesSourceSeal)
{
    namespace st = runtime::stateful;
    st::stateful_object_runtime_t objects;
    for (std::size_t index = 0; index < inventory_count; ++index) {
        const st::object_ref_t object{
          st::object_kind_t::actor, std::to_string (index), 1, 1, "mesh", "source"};
        const auto created = begin_reserved_object (objects, object, "actor", {});
        ASSERT_EQ (created.error, st::stateful_error_t::none);
        ASSERT_EQ (objects.commit_create (created.attempt), st::stateful_error_t::none);
    }
    auto &failure_size = zlink::framework::tests::process_allocation_failure_size;
    auto &failure_count = zlink::framework::tests::process_allocation_failure_count;
    failure_count.store (0, std::memory_order_relaxed);
    failure_size.store (inventory_count * sizeof (st::object_inventory_t),
                        std::memory_order_relaxed);
    const auto result = objects.try_begin_maintenance_inventory ().result ();
    failure_size.store (0, std::memory_order_relaxed);
    ASSERT_EQ (failure_count.load (std::memory_order_relaxed), 1);
    ASSERT_FALSE (result);
    // state_lane maps std::exception to the existing InternalFailure contract.
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::internal_failure);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_STREQ (result.error ()->what (), std::bad_alloc ().what ());
    EXPECT_TRUE (objects.actor_transfers ()->try_reserve_source ("after-failure"));
    objects.actor_transfers ()->cancel_move ("after-failure");
    const auto next = objects.try_begin_maintenance_inventory ().result ();
    ASSERT_TRUE (next);
    ASSERT_TRUE (next.value ());
    EXPECT_EQ (next.value ()->size (), inventory_count);
    objects.end_maintenance_inventory ().result ().value ();
}

TEST (MaintenanceFailure, ApplicationClaimAllocationFailureReturnsWorkAccounting)
{
    namespace mesh = runtime::mesh;
    namespace host = runtime::host;
    auto owner = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = {"claim-allocation",
                       {'a'},
                       1,
                       1,
                       "tcp://127.0.0.1:0",
                       {},
                       mesh::service_node_state_t::preparing},
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto &mailbox = owner->transport ().mailbox ();
    const auto enqueue = [&] {
        mesh::service_mailbox_record_t original;
        original.owner = "node";
        original.domain = mesh::service_mailbox_domain_t::application;
        original.application.emplace ();
        original.application->owner.owner_kind = host::owner_kind_t::node;
        original.application->record.kind = host::record_kind_t::node_send;
        original.application->parts.push_back (zlink::message_t::from ("x"));
        ASSERT_TRUE (mailbox.try_enqueue (std::move (original)));
    };
    constexpr auto count = runtime::dispatch_limits::receive_batch_messages;
    for (std::size_t index = 0; index < count; ++index)
        enqueue ();
    std::size_t started = 0, rejected = 0, dispatched = 0;
    auto &failure_size = zlink::framework::tests::process_allocation_failure_size;
    auto &failure_count = zlink::framework::tests::process_allocation_failure_count;
    failure_count.store (0, std::memory_order_relaxed);
    // Dispatcher storage has fixed batch capacity, but a Node claim owns one
    // record. Fail that allocation once for each of the queued originals.
    for (std::size_t index = 0; index != count; ++index) {
        std::promise<void> begin_dispatch;
        auto begin = begin_dispatch.get_future ().share ();
        auto cold = std::async (std::launch::async, [&] {
            begin.wait ();
            failure_size.store (count * sizeof (mesh::service_mailbox_record_t),
                                std::memory_order_relaxed);
            try {
                auto result = owner->dispatch_application_owner (
                  "node", [&] (const auto &, const auto &, auto, auto) { ++dispatched; },
                  [&] { ++started; }, [&] { ++rejected; });
                failure_size.store (0, std::memory_order_relaxed);
                return result;
            }
            catch (...) {
                failure_size.store (0, std::memory_order_relaxed);
                throw;
            }
        });
        begin_dispatch.set_value ();
        auto pending = cold.get ();
        const auto &result = zlink::framework::test::application_owner_result (pending);
        ASSERT_EQ (failure_count.load (std::memory_order_relaxed), index + 1);
        ASSERT_FALSE (result);
        EXPECT_THROW (result.value (), std::bad_alloc);
        EXPECT_EQ (started, index + 1);
        EXPECT_EQ (rejected, index + 1);
        EXPECT_EQ (mailbox.pending_messages (mesh::service_mailbox_domain_t::application),
                   count - index - 1);
    }
    EXPECT_EQ (started, count);
    EXPECT_EQ (rejected, count);
    EXPECT_EQ (dispatched, 0u);
    EXPECT_EQ (mailbox.pending_messages (mesh::service_mailbox_domain_t::application), 0u);
    enqueue ();
    auto next = owner->dispatch_application_owner (
      "node", [&] (const auto &, const auto &, auto, auto) { ++dispatched; }, [&] { ++started; },
      [&] { ++rejected; });
    EXPECT_TRUE (zlink::framework::test::application_owner_result (next).value ());
    EXPECT_EQ (dispatched, 1u);
    EXPECT_EQ (started, count + 1);
    EXPECT_EQ (rejected, count);
    owner->close ();
}
}
