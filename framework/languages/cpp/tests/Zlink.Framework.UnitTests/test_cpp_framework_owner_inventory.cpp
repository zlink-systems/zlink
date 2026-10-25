/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/stateful_object_sync.hpp"
#include "allocation_counter.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include <gtest/gtest.h>
#include <cstdio>
#include <thread>

using zlink::framework::test::begin_reserved_object;

namespace
{
namespace fw = zlink::framework;
namespace stateful = fw::runtime::stateful;
namespace host = fw::runtime::host;

std::shared_ptr<host::public_host_runtime_t> make_host ()
{
    auto runtime = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "owner-inventory",
                              .node_routing_id = {'n'},
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:7182"},
               .runtime_failures = std::make_shared<fw::runtime::runtime_failure_collector_t> ()}});
    runtime->configure_stateful_dispatch (
      [] (const stateful::accepted_record_authority_query_t &)
        -> fw::task_t<std::optional<stateful::accepted_record_authority_t>> {
          return fw::task_t<std::optional<stateful::accepted_record_authority_t>> (
            fw::result_t<std::optional<stateful::accepted_record_authority_t>>::success ({}));
      });
    runtime->start ();
    return runtime;
}

stateful::object_ref_t create (stateful::stateful_object_runtime_t &objects,
                               stateful::object_kind_t kind,
                               std::string key,
                               std::string node = "n")
{
    stateful::object_ref_t owner{kind, std::move (key), 1, 1, "owner-inventory", std::move (node)};
    const auto created = begin_reserved_object (objects, owner, std::string (80, 't'), {});
    EXPECT_EQ (created.error, stateful::stateful_error_t::none);
    EXPECT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    return owner;
}

TEST (OwnerInventory, IdleOwnersDoNotAddDrainAllocations)
{
    auto runtime = make_host ();
    std::size_t notifications = 0;
    runtime->transport ().mailbox ().bind_application_dispatch ({}, {}, [&] (const auto &, auto) {
        ++notifications;
        return fw::task_t<bool> (fw::result_t<bool>::success (false));
    });
    const auto drain = [&] {
        auto pending = runtime->dispatch_ready ([] (const auto &, const auto &, auto) {});
        ASSERT_TRUE (pending.await_ready ());
        EXPECT_EQ (pending.result ().value (), 0u);
    };
    drain ();
    const auto baseline = fw::tests::measure_allocations (drain);
    for (unsigned i = 0; i != 8; ++i) {
        create (runtime->objects (), stateful::object_kind_t::user_spot,
                std::string (80, 's') + std::to_string (i));
        create (runtime->objects (), stateful::object_kind_t::actor,
                std::string (80, 'a') + std::to_string (i));
    }
    drain ();
    notifications = 0;
    const auto populated = fw::tests::measure_allocations (drain);
    std::printf ("idle owners: baseline=%zu/%zu populated=%zu/%zu notifications=%zu\n",
                 baseline.count, baseline.bytes, populated.count, populated.bytes, notifications);
    // Compare the same public drain on this ABI; an empty ready projection has
    // no per-object storage or claim, regardless of the inventory's size.
    EXPECT_EQ (populated.count, baseline.count);
    EXPECT_EQ (populated.bytes, baseline.bytes);
    EXPECT_EQ (notifications, 0u);
    runtime->close ();
}

TEST (OwnerInventory, ProjectsOnlyLocalQueuedWorkInInventoryOrder)
{
    stateful::stateful_object_runtime_t objects;
    const auto z = create (objects, stateful::object_kind_t::user_spot, "z");
    const auto a = create (objects, stateful::object_kind_t::user_spot, "a");
    const auto actor = create (objects, stateful::object_kind_t::actor, "actor");
    const auto idle = create (objects, stateful::object_kind_t::actor, "idle");
    const auto remote = create (objects, stateful::object_kind_t::actor, "remote", "remote");
    for (const auto &owner : {z, a, actor, remote})
        ASSERT_EQ (objects.enqueue (owner, stateful::turn_domain_t::application, {1, {1}}),
                   stateful::stateful_error_t::none);
    ASSERT_EQ (objects.enqueue (idle, stateful::turn_domain_t::infrastructure, {1, {1}}),
               stateful::stateful_error_t::none);
    std::vector<stateful::object_ref_t> expected;
    const auto inventory = objects.inventory_async ().result ().value ();
    for (const auto &item : inventory)
        if (item.owner.node_id == "n" && item.owner != idle)
            expected.push_back (item.owner);
    const auto ready = objects.application_owner_refs_async ("n").result ().value ();
    EXPECT_EQ (ready, expected);
    EXPECT_EQ (ready.size (), 3u);
}

TEST (OwnerInventory, ActiveAndYieldedOwnersKeepTheExistingClaimGate)
{
    stateful::stateful_object_runtime_t objects;
    const auto owner = create (objects, stateful::object_kind_t::user_spot, "active");
    const auto domain = stateful::turn_domain_t::application;
    ASSERT_EQ (objects.enqueue (owner, domain, {1, {1}}), stateful::stateful_error_t::none);
    ASSERT_TRUE (objects.try_claim (owner, domain).second.has_value ());
    // An active turn without queued work has nothing for a later drain to claim.
    EXPECT_TRUE (objects.application_owner_refs_async ("n").result ().value ().empty ());
    ASSERT_EQ (objects.enqueue (owner, domain, {2, {2}}), stateful::stateful_error_t::none);
    EXPECT_EQ (objects.application_owner_refs_async ("n").result ().value (),
               std::vector<stateful::object_ref_t>{owner});
    auto waiting = objects.try_claim_async (owner, domain);
    EXPECT_FALSE (waiting.await_ready ());
    ASSERT_EQ (objects.complete_claim (owner, domain), stateful::stateful_error_t::none);
    const auto claimed = waiting.result ().value ();
    ASSERT_TRUE (claimed.turn.has_value ());
    EXPECT_EQ (claimed.turn->sequence, 2u);
    ASSERT_EQ (objects.yield_claim (owner, {3, {3}}), stateful::stateful_error_t::none);
    EXPECT_EQ (objects.application_owner_refs_async ("n").result ().value (),
               std::vector<stateful::object_ref_t>{owner});
    ASSERT_EQ (objects.enqueue (owner, domain, {4, {4}}), stateful::stateful_error_t::none);
    EXPECT_EQ (objects.application_owner_refs_async ("n").result ().value (),
               std::vector<stateful::object_ref_t>{owner});
    const auto resumed = objects.try_claim_async (owner, domain).result ().value ();
    ASSERT_TRUE (resumed.turn.has_value ());
    EXPECT_EQ (resumed.turn->sequence, 3u);
    ASSERT_EQ (objects.complete_claim (owner, domain), stateful::stateful_error_t::none);
    const auto next = objects.try_claim (owner, domain);
    ASSERT_TRUE (next.second.has_value ());
    EXPECT_EQ (next.second->sequence, 4u);
    EXPECT_EQ (objects.complete_claim (owner, domain), stateful::stateful_error_t::none);
    EXPECT_TRUE (objects.application_owner_refs_async ("n").result ().value ().empty ());
}

void enqueue_native_spot (stateful::stateful_object_runtime_t &objects,
                          const stateful::object_ref_t &owner)
{
    namespace protocol = fw::runtime::protocol;
    protocol::spot_message_header_t header;
    header.operation = {1, 1};
    header.target = {owner.key, owner.object_generation,          {'n'},
                     1,         owner.authority_owner_generation, 1};
    stateful::turn_record_t turn;
    turn.application_record = stateful::accepted_spot_record (
      header, {"source", 1, {'n'}, 1},
      protocol::application_payload_t::from_parts ({zlink::message_t::from ("payload")}), false);
    turn.accepted_owner = std::make_shared<fw::runtime::serial_application_record_t> ();
    ASSERT_EQ (objects.enqueue (owner, stateful::turn_domain_t::application, std::move (turn)),
               stateful::stateful_error_t::none);
}

TEST (OwnerInventory, StandaloneDrainKeepsOwnerOrderAndFindsConcurrentEnqueueOnNextDrain)
{
    auto runtime = make_host ();
    auto &objects = runtime->objects ();
    const auto z = create (objects, stateful::object_kind_t::user_spot, "z");
    const auto a = create (objects, stateful::object_kind_t::user_spot, "a");
    const auto late = create (objects, stateful::object_kind_t::user_spot, "late");
    enqueue_native_spot (objects, z);
    enqueue_native_spot (objects, a);
    std::vector<std::string> seen;
    const auto dispatch = [&] (const auto &owner, const auto &record, auto) {
        seen.push_back (owner.spot_id);
        ASSERT_NE (record.application_record, nullptr);
        record.application_record->release_native_claim ();
        if (owner.spot_id == "a") {
            // The initial projection is already captured. A concurrent lane
            // enqueue remains queued until the next bounded receive turn.
            std::thread producer ([&] { enqueue_native_spot (objects, late); });
            producer.join ();
        }
    };
    EXPECT_EQ (runtime->dispatch_ready (dispatch, false).result ().value (), 0u);
    EXPECT_TRUE (seen.empty ());
    const auto first = runtime->dispatch_ready (dispatch, true, [] { return true; }).result ();
    EXPECT_EQ (first.value (), 2u);
    EXPECT_EQ (seen, (std::vector<std::string>{"a", "z"}));
    EXPECT_EQ (objects.pending (late, stateful::turn_domain_t::application), 1u);
    EXPECT_EQ (runtime->dispatch_ready (dispatch).result ().value (), 1u);
    EXPECT_EQ (seen, (std::vector<std::string>{"a", "z", "late"}));
    EXPECT_TRUE (objects.application_owner_refs_async ("n").result ().value ().empty ());
    runtime->close ();
}

TEST (OwnerInventory, DefaultDrainStopsAfterOneApplicationOwner)
{
    auto runtime = make_host ();
    const auto a = create (runtime->objects (), stateful::object_kind_t::user_spot, "a");
    const auto z = create (runtime->objects (), stateful::object_kind_t::user_spot, "z");
    enqueue_native_spot (runtime->objects (), z);
    enqueue_native_spot (runtime->objects (), a);
    std::vector<std::string> seen;
    const auto dispatch = [&] (const auto &owner, const auto &record, auto) {
        seen.push_back (owner.spot_id);
        ASSERT_NE (record.application_record, nullptr);
        record.application_record->release_native_claim ();
    };
    EXPECT_EQ (runtime->dispatch_ready (dispatch).result ().value (), 1u);
    EXPECT_EQ (seen, (std::vector<std::string>{"a"}));
    EXPECT_EQ (runtime->objects ().pending (z, stateful::turn_domain_t::application), 1u);
    EXPECT_EQ (runtime->dispatch_ready (dispatch).result ().value (), 1u);
    EXPECT_EQ (seen, (std::vector<std::string>{"a", "z"}));
    runtime->close ();
}
} // namespace
