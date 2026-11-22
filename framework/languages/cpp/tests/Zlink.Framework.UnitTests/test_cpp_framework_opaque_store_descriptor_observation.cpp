/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/opaque_store_descriptor_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "../support/location_reclaim_clock.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include <fstream>
#include <condition_variable>
#include <zlink/framework/contracts/configuration/app.hpp>

#include <gtest/gtest.h>
#include <zlink/framework/contracts/detail/handler_invocation.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "../support/opaque_store_record_fixture.hpp"
#include "../support/opaque_store_reclaim_fixture.hpp"

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace zlink::framework::test::opaque_store;

TEST (CppFrameworkOpaqueLocationStore, ReclaimWaitUsesExactRecordTokenAndRestartsObservation)
{
    in_memory_location_repository_t repository;
    repository.configure_reclaim_margin (5s);
    location_reclaim_test_access_t clock;
    clock.bind (repository);
    using access = location_reclaim_test_access_t;
    const location_owner_token_t owner{"owner", 1};
    const location_owner_token_t next{"owner", 2};
    EXPECT_FALSE (access::observe (repository, "actor:a", owner));
    clock.advance (15s - 1ms);
    EXPECT_FALSE (access::observe (repository, "actor:a", owner));
    EXPECT_FALSE (access::observe (repository, "actor:b", owner));
    clock.advance (1ms);
    EXPECT_TRUE (access::observe (repository, "actor:a", owner));
    EXPECT_FALSE (access::observe (repository, "actor:b", owner));
    EXPECT_FALSE (access::observe (repository, "actor:a", next));
    clock.advance (15s - 1ms);
    EXPECT_FALSE (access::observe (repository, "actor:a", next));
    clock.advance (1ms);
    EXPECT_TRUE (access::observe (repository, "actor:a", next));
    access::restart_observer (repository);
    EXPECT_FALSE (access::observe (repository, "actor:a", next));
    clock.advance (15s);
    EXPECT_TRUE (access::observe (repository, "actor:a", next));
    EXPECT_FALSE (access::observe (repository, "actor:a", next, true));
    EXPECT_EQ (access::observations (repository), 0u);
    EXPECT_FALSE (access::observe (repository, "actor:a", next));
    access::missing (repository, "actor:a");
    EXPECT_EQ (access::observations (repository), 0u);
    EXPECT_FALSE (access::observe (repository, "actor:a", next));
}

TEST (CppFrameworkOpaqueLocationStore, DescriptorDeletionClearsOnlyDeletedTokenObservation)
{
    for (const bool use_provider : {false, true}) {
        SCOPED_TRACE (use_provider ? "provider" : "in-memory");
        reclaim_scan_store_t opaque;
        provider_location_repository_t provider (opaque);
        provider.configure_reclaim_margin (std::chrono::milliseconds::zero ());
        in_memory_location_repository_t memory;
        memory.configure_reclaim_margin (std::chrono::milliseconds::zero ());
        location_repository_t &repository = use_provider
                                              ? static_cast<location_repository_t &> (provider)
                                              : static_cast<location_repository_t &> (memory);
        location_reclaim_test_access_t clock;
        clock.bind (repository);
        repository.configure_reclaim_margin (0ms);
        const auto old_owner =
          std::get<owner_lease_claimed_t> (
            repository.claim_owner_lease ("review-old", 60s).result ().value ())
            .token;
        const auto next_owner =
          std::get<owner_lease_claimed_t> (
            repository.claim_owner_lease ("review-next", 60s).result ().value ())
            .token;
        auto descriptor = review_mesh_descriptor (old_owner);
        ASSERT_EQ (
          repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
            .result ()
            .value ()
            .status,
          location_write_status_t::stored);
        repository.configure_reclaim_margin (5s);
        repository.release_owner_lease (old_owner).result ().value ();
        descriptor.owner_id = next_owner.owner_id;
        descriptor.lease_generation = next_owner.lease_generation;
        const auto waiting =
          repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
            .result ();
        ASSERT_TRUE (waiting);
        EXPECT_EQ (waiting.value ().status, location_write_status_t::waiting);
        EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
        EXPECT_EQ (repository.remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, old_owner)
                     .result ()
                     .value (),
                   location_write_status_t::stored);
        EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 0u);
        const auto missing_wait =
          repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
            .result ();
        ASSERT_TRUE (missing_wait);
        EXPECT_EQ (missing_wait.value ().status, location_write_status_t::waiting);
        EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
        EXPECT_EQ (repository.remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, old_owner)
                     .result ()
                     .value (),
                   location_write_status_t::ignored_stale);
        // Missing observation belongs to the next lifecycle, not the deleted token.
        EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
    }
}

TEST (CppFrameworkOpaqueLocationStore, DescriptorDeleteClearsObservationCreatedDuringWrite)
{
    reclaim_scan_store_t opaque;
    provider_location_repository_t repository (opaque);
    location_reclaim_test_access_t clock;
    clock.bind (repository);
    repository.configure_reclaim_margin (0ms);
    const auto old_owner = std::get<owner_lease_claimed_t> (
                             repository.claim_owner_lease ("delete-old", 60s).result ().value ())
                             .token;
    const auto next_owner = std::get<owner_lease_claimed_t> (
                              repository.claim_owner_lease ("delete-next", 60s).result ().value ())
                              .token;
    auto descriptor = review_mesh_descriptor (old_owner);
    ASSERT_EQ (
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ()
        .value ()
        .status,
      location_write_status_t::stored);
    repository.configure_reclaim_margin (5s);
    repository.release_owner_lease (old_owner).result ().value ();
    task_completion_source_t<void> held;
    task_completion_source_t<void> release;
    opaque.before_write = [&] () -> task_t<void> {
        held.complete (result_t<void>::success ());
        co_await release.task ();
    };
    auto deletion = repository.remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, old_owner);
    held.task ().result ().value ();
    descriptor.owner_id = next_owner.owner_id;
    descriptor.lease_generation = next_owner.lease_generation;
    const auto waiting =
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ();
    EXPECT_TRUE (waiting);
    if (waiting)
        EXPECT_EQ (waiting.value ().status, location_write_status_t::waiting);
    EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
    release.complete (result_t<void>::success ());
    EXPECT_EQ (deletion.result ().value (), location_write_status_t::stored);
    EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 0u);
    EXPECT_TRUE (
      repository.list_mesh_nodes (descriptor.mesh_name).result ().value ().items.empty ());
}

TEST (CppFrameworkOpaqueLocationStore, LateDescriptorPublishKeepsReplacementTokenObservation)
{
    reclaim_scan_store_t opaque;
    provider_location_repository_t repository (opaque);
    location_reclaim_test_access_t clock;
    clock.bind (repository);
    repository.configure_reclaim_margin (0ms);
    const auto old_owner = std::get<owner_lease_claimed_t> (
                             repository.claim_owner_lease ("publish-old", 60s).result ().value ())
                             .token;
    const auto next_owner = std::get<owner_lease_claimed_t> (
                              repository.claim_owner_lease ("publish-next", 60s).result ().value ())
                              .token;
    const auto contender =
      std::get<owner_lease_claimed_t> (
        repository.claim_owner_lease ("publish-contender", 60s).result ().value ())
        .token;
    auto descriptor = review_mesh_descriptor (old_owner);
    ASSERT_EQ (
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ()
        .value ()
        .status,
      location_write_status_t::stored);
    repository.configure_reclaim_margin (5s);
    repository.release_owner_lease (old_owner).result ().value ();
    descriptor.owner_id = contender.owner_id;
    descriptor.lease_generation = contender.lease_generation;
    ASSERT_EQ (
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ()
        .value ()
        .status,
      location_write_status_t::waiting);
    clock.advance (15s);
    task_completion_source_t<void> owner_read_held;
    task_completion_source_t<void> release_owner_read;
    opaque.after_read_key = {std::string ("owner-lease") + '\0' + old_owner.owner_id};
    opaque.after_read = [&] () -> task_t<void> {
        owner_read_held.complete (result_t<void>::success ());
        co_await release_owner_read.task ();
    };
    auto replacement = descriptor;
    replacement.owner_id = next_owner.owner_id;
    replacement.lease_generation = next_owner.lease_generation;
    auto publication =
      repository.update_mesh_node (replacement, location_write_intent_t::fixed_rid_new_claim);
    owner_read_held.task ().result ().value ();
    task_completion_source_t<void> read_held;
    task_completion_source_t<void> release_read;
    opaque.before_read_key = {std::string ("mesh-node") + '\0' + descriptor.mesh_name + '\0'
                              + descriptor.rid.to_hex ()};
    opaque.before_read = [&] () -> task_t<void> {
        read_held.complete (result_t<void>::success ());
        co_await release_read.task ();
    };
    auto contender_claim =
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim);
    read_held.task ().result ().value ();
    task_completion_source_t<void> write_held;
    task_completion_source_t<void> release_write;
    opaque.after_write = [&] () -> task_t<void> {
        write_held.complete (result_t<void>::success ());
        co_await release_write.task ();
    };
    release_owner_read.complete (result_t<void>::success ());
    write_held.task ().result ().value ();
    repository.release_owner_lease (next_owner).result ().value ();
    release_read.complete (result_t<void>::success ());
    EXPECT_EQ (contender_claim.result ().value ().status, location_write_status_t::waiting);
    EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
    release_write.complete (result_t<void>::success ());
    EXPECT_EQ (publication.result ().value ().status, location_write_status_t::stored);
    EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
    clock.advance (15s - 1ms);
    EXPECT_EQ (
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ()
        .value ()
        .status,
      location_write_status_t::waiting);
    clock.advance (1ms);
    EXPECT_EQ (
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ()
        .value ()
        .status,
      location_write_status_t::stored);
    EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 0u);
}

TEST (CppFrameworkOpaqueLocationStore, LateSameTokenRenewKeepsInvalidObservation)
{
    reclaim_scan_store_t opaque;
    provider_location_repository_t repository (opaque);
    location_reclaim_test_access_t clock;
    clock.bind (repository);
    repository.configure_reclaim_margin (0ms);
    const auto owner = std::get<owner_lease_claimed_t> (
                         repository.claim_owner_lease ("renew-owner", 60s).result ().value ())
                         .token;
    const auto contender =
      std::get<owner_lease_claimed_t> (
        repository.claim_owner_lease ("renew-contender", 60s).result ().value ())
        .token;
    auto descriptor = review_mesh_descriptor (owner);
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    repository.configure_reclaim_margin (5s);
    task_completion_source_t<void> applied;
    task_completion_source_t<void> release;
    release_descriptor_on_exit_t release_on_exit{release};
    opaque.after_write = [&] () -> task_t<void> {
        applied.complete (result_t<void>::success ());
        co_await release.task ();
    };
    ++descriptor.descriptor_revision;
    auto renewal = repository.update_mesh_node (descriptor, location_write_intent_t::renew);
    applied.task ().result ().value ();
    opaque.expired_lease_key = {std::string ("owner-lease") + '\0' + owner.owner_id};
    descriptor.owner_id = contender.owner_id;
    descriptor.lease_generation = contender.lease_generation;
    const auto claim = [&] {
        return repository
          .update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
          .result ()
          .value ()
          .status;
    };
    ASSERT_EQ (claim (), location_write_status_t::waiting);
    ASSERT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
    release.complete (result_t<void>::success ());
    EXPECT_EQ (renewal.result ().value ().status, location_write_status_t::stored);
    EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
    clock.advance (15s - 1ms);
    EXPECT_EQ (claim (), location_write_status_t::waiting);
    clock.advance (1ms);
    EXPECT_EQ (claim (), location_write_status_t::stored);
    EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 0u);
}

void verify_late_descriptor_live_read (bool observed_before)
{
    SCOPED_TRACE (observed_before ? "repeated invalid observation" : "first invalid observation");
    reclaim_scan_store_t opaque;
    provider_location_repository_t repository (opaque);
    location_reclaim_test_access_t clock;
    clock.bind (repository);
    repository.configure_reclaim_margin (0ms);
    const auto old_owner = std::get<owner_lease_claimed_t> (
                             repository.claim_owner_lease ("review-old", 60s).result ().value ())
                             .token;
    const auto next_owner = std::get<owner_lease_claimed_t> (
                              repository.claim_owner_lease ("review-next", 60s).result ().value ())
                              .token;
    auto descriptor = review_mesh_descriptor (old_owner);
    ASSERT_EQ (
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ()
        .value ()
        .status,
      location_write_status_t::stored);
    repository.configure_reclaim_margin (5s);
    descriptor.owner_id = next_owner.owner_id;
    descriptor.lease_generation = next_owner.lease_generation;
    if (observed_before) {
        const store_key_t owner_key{std::string ("owner-lease") + '\0' + old_owner.owner_id};
        const auto saved_owner =
          std::get<store_found_t> (opaque.inner.read (owner_key).result ().value ());
        repository.release_owner_lease (old_owner).result ().value ();
        EXPECT_EQ (
          repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
            .result ()
            .value ()
            .status,
          location_write_status_t::waiting);
        opaque.inner.write ({{}, {store_put_t{owner_key, saved_owner.value.bytes, 60s}}})
          .result ()
          .value ();
    }
    task_completion_source_t<void> held;
    task_completion_source_t<void> release;
    opaque.after_read_key = {std::string ("owner-lease") + '\0' + old_owner.owner_id};
    opaque.after_read = [&] () -> task_t<void> {
        held.complete (result_t<void>::success ());
        co_await release.task ();
    };
    auto older =
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim);
    held.task ().result ().value ();
    repository.release_owner_lease (old_owner).result ().value ();
    const auto waiting =
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ();
    ASSERT_TRUE (waiting);
    EXPECT_EQ (waiting.value ().status, location_write_status_t::waiting);
    clock.advance (15s - 1ms);
    release.complete (result_t<void>::success ());
    EXPECT_EQ (older.result ().value ().status, location_write_status_t::rejected_conflict);
    EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
    const auto before_boundary =
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ();
    ASSERT_TRUE (before_boundary);
    EXPECT_EQ (before_boundary.value ().status, location_write_status_t::waiting);
    clock.advance (1ms);
    const auto at_boundary =
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ();
    ASSERT_TRUE (at_boundary);
    EXPECT_EQ (at_boundary.value ().status, location_write_status_t::stored);
}

TEST (CppFrameworkOpaqueLocationStore, DescriptorLateLiveReadPreservesNewInvalidObservation)
{
    verify_late_descriptor_live_read (false);
    verify_late_descriptor_live_read (true);
}

TEST (CppFrameworkOpaqueLocationStore, DescriptorLateInvalidReadUsesEstablishedObservation)
{
    reclaim_scan_store_t opaque;
    provider_location_repository_t repository (opaque);
    repository.configure_reclaim_margin (0ms);
    location_reclaim_test_access_t clock;
    clock.bind (repository);
    const auto old_owner =
      std::get<owner_lease_claimed_t> (
        repository.claim_owner_lease ("invalid-read-old", 60s).result ().value ())
        .token;
    const auto next_owner =
      std::get<owner_lease_claimed_t> (
        repository.claim_owner_lease ("invalid-read-next", 60s).result ().value ())
        .token;
    auto descriptor = review_mesh_descriptor (old_owner);
    ASSERT_EQ (
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ()
        .value ()
        .status,
      location_write_status_t::stored);
    repository.release_owner_lease (old_owner).result ().value ();
    repository.configure_reclaim_margin (5s);
    descriptor.owner_id = next_owner.owner_id;
    descriptor.lease_generation = next_owner.lease_generation;
    ASSERT_EQ (
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ()
        .value ()
        .status,
      location_write_status_t::waiting);
    task_completion_source_t<void> held;
    task_completion_source_t<void> release;
    opaque.after_read_key = {std::string ("owner-lease") + '\0' + old_owner.owner_id};
    opaque.after_read = [&] () -> task_t<void> {
        held.complete (result_t<void>::success ());
        co_await release.task ();
    };
    auto older =
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim);
    held.task ().result ().value ();
    EXPECT_EQ (
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ()
        .value ()
        .status,
      location_write_status_t::waiting);
    clock.advance (15s);
    release.complete (result_t<void>::success ());
    EXPECT_EQ (older.result ().value ().status, location_write_status_t::stored);
    EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 0u);
}

TEST (CppFrameworkOpaqueLocationStore, LateDescriptorDeleteKeepsReplacementTokenObservation)
{
    reclaim_scan_store_t opaque;
    provider_location_repository_t repository (opaque);
    repository.configure_reclaim_margin (0ms);
    location_reclaim_test_access_t clock;
    clock.bind (repository);
    const auto old_owner = std::get<owner_lease_claimed_t> (
                             repository.claim_owner_lease ("delete-old", 60s).result ().value ())
                             .token;
    const auto next_owner = std::get<owner_lease_claimed_t> (
                              repository.claim_owner_lease ("delete-next", 60s).result ().value ())
                              .token;
    const auto contender =
      std::get<owner_lease_claimed_t> (
        repository.claim_owner_lease ("delete-contender", 60s).result ().value ())
        .token;
    auto descriptor = review_mesh_descriptor (old_owner);
    ASSERT_EQ (
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ()
        .value ()
        .status,
      location_write_status_t::stored);
    task_completion_source_t<void> held;
    task_completion_source_t<void> release;
    opaque.after_write = [&] () -> task_t<void> {
        held.complete (result_t<void>::success ());
        co_await release.task ();
    };
    auto deletion = repository.remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, old_owner);
    held.task ().result ().value ();
    descriptor.owner_id = next_owner.owner_id;
    descriptor.lease_generation = next_owner.lease_generation;
    EXPECT_EQ (
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ()
        .value ()
        .status,
      location_write_status_t::stored);
    repository.configure_reclaim_margin (5s);
    repository.release_owner_lease (next_owner).result ().value ();
    descriptor.owner_id = contender.owner_id;
    descriptor.lease_generation = contender.lease_generation;
    const auto waiting =
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ();
    EXPECT_TRUE (waiting);
    if (waiting)
        EXPECT_EQ (waiting.value ().status, location_write_status_t::waiting);
    EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
    release.complete (result_t<void>::success ());
    EXPECT_EQ (deletion.result ().value (), location_write_status_t::stored);
    EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
    clock.advance (15s);
    EXPECT_EQ (
      repository.update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
        .result ()
        .value ()
        .status,
      location_write_status_t::stored);
}
}
