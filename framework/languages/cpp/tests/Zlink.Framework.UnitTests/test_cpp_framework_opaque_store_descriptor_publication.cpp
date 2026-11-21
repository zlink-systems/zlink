/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/opaque_store_descriptor_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "../support/manual_deadlines.hpp"
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

#include "../support/location_runtime_test_access.hpp"

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace zlink::framework::test::opaque_store;

class lost_descriptor_reply_store_t final : public location_store_t
{
  public:
    in_memory_location_store_t inner;
    bool lose_reply = false;
    bool lose_confirmation = false;
    bool hide_confirmation_on_loss = false;
    int descriptor_reads = 0;
    int descriptor_writes = 0;
    int reads_before_lost_reply = 0;
    std::optional<store_key_t> lost_write_key;
    std::optional<store_key_t> pending_confirmation_key;
    std::vector<store_key_t> confirmation_read_keys;
    task_t<store_read_result_t> read (store_key_t key) override
    {
        if (key.value.starts_with (std::string ("mesh-node") + '\0')) {
            ++descriptor_reads;
            if (pending_confirmation_key) {
                confirmation_read_keys.push_back (key);
                pending_confirmation_key.reset ();
            }
            if (lose_confirmation) {
                lose_confirmation = false;
                co_return result_t<store_read_result_t>::failure (
                  framework_error_kind_t::internal_failure, "injected confirmation read failure");
            }
        }
        co_return co_await inner.read (std::move (key));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        for (const auto &mutation : request.mutations) {
            std::visit (
              [&] (const auto &value) {
                  if (value.key.value.starts_with (std::string ("mesh-node") + '\0')) {
                      ++descriptor_writes;
                      if (lose_reply)
                          lost_write_key = value.key;
                  }
              },
              mutation);
        }
        auto result = co_await inner.write (std::move (request));
        if (lose_reply) {
            lose_reply = false;
            reads_before_lost_reply = descriptor_reads;
            pending_confirmation_key = lost_write_key;
            lose_confirmation = hide_confirmation_on_loss;
            co_return result_t<store_write_result_t>::failure (
              framework_error_kind_t::internal_failure, "injected lost descriptor reply");
        }
        co_return result;
    }
};

TEST (ZLinkFrameworkOpaqueStoreProviders, DescriptorLostReplyIsConfirmedBySameKeyRead)
{
    lost_descriptor_reply_store_t store;
    provider_location_repository_t repository (store);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto owner = std::get<owner_lease_claimed_t> (
                         repository.claim_owner_lease ("lost-reply-owner", 30s).result ().value ())
                         .token;
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "lost-reply-mesh";
    descriptor.rid = zlink::routing_id_t::from ("lost-reply-node");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    store.lose_reply = true;
    const auto result = repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                          .result ()
                          .value ();
    EXPECT_EQ (location_write_status_t::stored, result.status);
    EXPECT_EQ (1, store.reads_before_lost_reply);
    ASSERT_TRUE (store.lost_write_key);
    ASSERT_EQ (1u, store.confirmation_read_keys.size ());
    EXPECT_EQ (store.lost_write_key->value, store.confirmation_read_keys.front ().value);
    EXPECT_EQ (store.reads_before_lost_reply + store.confirmation_read_keys.size (),
               store.descriptor_reads);
    EXPECT_EQ (1, store.descriptor_writes);
    descriptor.updated_at += 1ms;
    EXPECT_EQ (location_write_status_t::stored,
               repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);
    EXPECT_EQ (1, store.descriptor_writes);
    descriptor.endpoint = "tcp://127.0.0.1:7002";
    EXPECT_EQ (location_write_status_t::rejected_conflict,
               repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);
}

TEST (ZLinkFrameworkOpaqueStoreProviders, NewClaimLostReplyAndReadFailureIsIdempotent)
{
    lost_descriptor_reply_store_t store;
    provider_location_repository_t repository (store);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto owner = std::get<owner_lease_claimed_t> (
                         repository.claim_owner_lease ("lost-claim-owner", 30s).result ().value ())
                         .token;
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "lost-claim-mesh";
    descriptor.rid = zlink::routing_id_t::from ("lost-claim-node");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    store.lose_reply = true;
    store.hide_confirmation_on_loss = true;
    EXPECT_FALSE (
      repository.update_mesh_node (descriptor, location_write_intent_t::new_claim).result ());
    descriptor.updated_at += 1ms;
    EXPECT_EQ (location_write_status_t::stored,
               repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);
    EXPECT_EQ (1, store.descriptor_writes);
    descriptor.lease_generation += 1;
    EXPECT_EQ (location_write_status_t::ignored_stale,
               repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);
}

TEST (ZLinkFrameworkOpaqueStoreProviders, NewClaimReplayPreservesDescriptorAcrossRepositories)
{
    in_memory_location_store_t provider;
    provider_location_repository_t provider_repository (provider);
    provider_repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    in_memory_location_repository_t memory_repository;
    memory_repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    for (location_repository_t *repository :
         {static_cast<location_repository_t *> (&provider_repository),
          static_cast<location_repository_t *> (&memory_repository)}) {
        SCOPED_TRACE (repository == &provider_repository ? "provider" : "in-memory");
        const auto owner = std::get<owner_lease_claimed_t> (
                             repository->claim_owner_lease ("replay-owner", 30s).result ().value ())
                             .token;
        const auto other = std::get<owner_lease_claimed_t> (
                             repository->claim_owner_lease ("replay-other", 30s).result ().value ())
                             .token;
        auto verify = [&] (auto descriptor, auto update) {
            descriptor.owner_id = owner.owner_id;
            descriptor.lease_generation = owner.lease_generation;
            descriptor.lifecycle_generation = 1;
            descriptor.descriptor_revision = 1;
            descriptor.endpoint = "tcp://127.0.0.1:7001";
            descriptor.security_identity = "default";
            EXPECT_EQ (location_write_status_t::stored,
                       update (descriptor).result ().value ().status);
            descriptor.updated_at += 1ms;
            EXPECT_EQ (location_write_status_t::stored,
                       update (descriptor).result ().value ().status);
            descriptor.endpoint = "tcp://127.0.0.1:7002";
            EXPECT_EQ (location_write_status_t::rejected_conflict,
                       update (descriptor).result ().value ().status);
            descriptor.endpoint = "tcp://127.0.0.1:7001";
            descriptor.owner_id = other.owner_id;
            descriptor.lease_generation = other.lease_generation;
            EXPECT_EQ (location_write_status_t::rejected_conflict,
                       update (descriptor).result ().value ().status);
        };
        mesh_node_descriptor_t mesh;
        mesh.mesh_name = "replay-mesh";
        mesh.rid = zlink::routing_id_t::from ("replay-node");
        verify (mesh, [&] (auto descriptor) {
            return repository->update_mesh_node (std::move (descriptor),
                                                 location_write_intent_t::fixed_rid_new_claim);
        });
        repository->configure_reclaim_margin (5s);
        client_server_server_descriptor_t server;
        server.channel_name = "replay-server";
        server.server_rid = zlink::routing_id_t::from ("replay-server");
        verify (server, [&] (auto descriptor) {
            return repository->update_client_server (std::move (descriptor),
                                                     location_write_intent_t::new_claim);
        });
        fanout_publisher_descriptor_t fanout;
        fanout.channel_name = "replay-fanout";
        fanout.publisher_rid = zlink::routing_id_t::from ("replay-publisher");
        verify (fanout, [&] (auto descriptor) {
            return repository->update_fanout_publisher (std::move (descriptor),
                                                        location_write_intent_t::new_claim);
        });
        EXPECT_EQ (location_reclaim_test_access_t::observations (*repository), 0u);
    }
}

TEST (ZLinkFrameworkOpaqueStoreProviders, RepeatedMeshWriteConfirmsEitherIntentWithoutMutation)
{
    lost_descriptor_reply_store_t provider;
    provider_location_repository_t provider_repository (provider);
    provider_repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    in_memory_location_repository_t memory_repository;
    memory_repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    for (location_repository_t *repository :
         {static_cast<location_repository_t *> (&provider_repository),
          static_cast<location_repository_t *> (&memory_repository)}) {
        SCOPED_TRACE (repository == &provider_repository ? "provider" : "in-memory");
        const auto owner =
          std::get<owner_lease_claimed_t> (
            repository->claim_owner_lease ("repeated-owner", 30s).result ().value ())
            .token;
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "repeated-mesh";
        descriptor.rid = zlink::routing_id_t::from ("repeated-node");
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.security_identity = "default";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        ASSERT_EQ (location_write_status_t::stored,
                   repository->update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status);
        // Confirm both the initial claim and an applied higher-revision renewal.
        for (const auto revision : {1u, 2u}) {
            descriptor.descriptor_revision = revision;
            if (revision == 2) {
                descriptor.placement_weight += 1;
                ASSERT_EQ (location_write_status_t::stored,
                           repository->update_mesh_node (descriptor, location_write_intent_t::renew)
                             .result ()
                             .value ()
                             .status);
            }
            const auto before =
              repository->list_mesh_nodes (descriptor.mesh_name).result ().value ().items.at (0);
            const auto writes = provider.descriptor_writes;
            for (const auto intent :
                 {location_write_intent_t::new_claim, location_write_intent_t::fixed_rid_new_claim,
                  location_write_intent_t::renew}) {
                SCOPED_TRACE (static_cast<int> (intent));
                descriptor.updated_at = before.updated_at + 1ms;
                EXPECT_EQ (
                  location_write_status_t::stored,
                  repository->update_mesh_node (descriptor, intent).result ().value ().status);
            }
            EXPECT_EQ (writes, provider.descriptor_writes);
            EXPECT_EQ (before.updated_at, repository->list_mesh_nodes (descriptor.mesh_name)
                                            .result ()
                                            .value ()
                                            .items.at (0)
                                            .updated_at);
        }
        ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
          repository->release_owner_lease (owner).result ().value ()));
        for (const auto intent :
             {location_write_intent_t::new_claim, location_write_intent_t::fixed_rid_new_claim,
              location_write_intent_t::renew})
            EXPECT_EQ (location_write_status_t::ignored_stale,
                       repository->update_mesh_node (descriptor, intent).result ().value ().status);
    }
}

TEST (ZLinkFrameworkOpaqueStoreProviders, RepeatedMeshWriteRejectsOtherDescriptorChanges)
{
    lost_descriptor_reply_store_t provider;
    provider_location_repository_t provider_repository (provider);
    provider_repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    in_memory_location_repository_t memory_repository;
    memory_repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    for (location_repository_t *repository :
         {static_cast<location_repository_t *> (&provider_repository),
          static_cast<location_repository_t *> (&memory_repository)}) {
        SCOPED_TRACE (repository == &provider_repository ? "provider" : "in-memory");
        const auto owner =
          std::get<owner_lease_claimed_t> (
            repository->claim_owner_lease ("changed-owner", 30s).result ().value ())
            .token;
        const auto other = std::get<owner_lease_claimed_t> (
                             repository->claim_owner_lease ("other-owner", 30s).result ().value ())
                             .token;
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "changed-mesh";
        descriptor.rid = zlink::routing_id_t::from ("changed-node");
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 2;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.security_identity = "default";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        ASSERT_EQ (location_write_status_t::stored,
                   repository->update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status);
        const auto before =
          repository->list_mesh_nodes (descriptor.mesh_name).result ().value ().items.at (0);
        const std::vector<std::pair<std::string, std::function<void (mesh_node_descriptor_t &)>>>
          changes{{"owner",
                   [&] (auto &next) {
                       next.owner_id = other.owner_id;
                       next.lease_generation = other.lease_generation;
                   }},
                  {"lease", [] (auto &next) { ++next.lease_generation; }},
                  {"lifecycle", [] (auto &next) { ++next.lifecycle_generation; }},
                  {"revision", [] (auto &next) { --next.descriptor_revision; }},
                  {"endpoint", [] (auto &next) { next.endpoint = "tcp://127.0.0.1:7002"; }},
                  {"security", [] (auto &next) { next.security_identity = "other"; }},
                  {"weight", [] (auto &next) { ++next.placement_weight; }},
                  {"state", [] (auto &next) { next.state = framework_runtime_state_t::serving; }},
                  {"maintenance", [] (auto &next) { next.maintenance_wave = "wave"; }}};
        const auto writes = provider.descriptor_writes;
        for (const auto &[name, change] : changes) {
            SCOPED_TRACE (name);
            auto next = descriptor;
            next.updated_at = before.updated_at + 1ms;
            change (next);
            for (const auto intent :
                 {location_write_intent_t::new_claim, location_write_intent_t::fixed_rid_new_claim,
                  location_write_intent_t::renew})
                EXPECT_NE (location_write_status_t::stored,
                           repository->update_mesh_node (next, intent).result ().value ().status);
        }
        EXPECT_EQ (writes, provider.descriptor_writes);
        const auto after =
          repository->list_mesh_nodes (descriptor.mesh_name).result ().value ().items.at (0);
        EXPECT_EQ (before.updated_at, after.updated_at);
        EXPECT_EQ (before.descriptor_revision, after.descriptor_revision);
        EXPECT_EQ (before.endpoint, after.endpoint);
        EXPECT_EQ (before.placement_weight, after.placement_weight);
        EXPECT_EQ (before.state, after.state);
        EXPECT_EQ (before.maintenance_wave, after.maintenance_wave);
    }
}

TEST (ZLinkFrameworkOpaqueStoreProviders, NewClaimWaitsAfterPreviousOwnerLeaseGenerationChanges)
{
    in_memory_location_store_t provider;
    provider_location_repository_t provider_repository (provider);
    provider_repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    in_memory_location_repository_t memory_repository;
    memory_repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    for (location_repository_t *repository :
         {static_cast<location_repository_t *> (&provider_repository),
          static_cast<location_repository_t *> (&memory_repository)}) {
        SCOPED_TRACE (repository == &provider_repository ? "provider" : "in-memory");
        location_reclaim_test_access_t clock;
        clock.bind (*repository);
        const auto first = std::get<owner_lease_claimed_t> (
                             repository->claim_owner_lease ("reused-owner", 30s).result ().value ())
                             .token;
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "reused-owner-mesh";
        descriptor.rid = zlink::routing_id_t::from ("reused-owner-node");
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.security_identity = "default";
        descriptor.owner_id = first.owner_id;
        descriptor.lease_generation = first.lease_generation;
        ASSERT_EQ (
          location_write_status_t::stored,
          repository->update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
            .result ()
            .value ()
            .status);
        repository->configure_reclaim_margin (5s);
        ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
          repository->release_owner_lease (first).result ().value ()));
        const auto replacement =
          std::get<owner_lease_claimed_t> (
            repository->claim_owner_lease (first.owner_id, 30s).result ().value ())
            .token;
        ASSERT_GT (replacement.lease_generation, first.lease_generation);
        descriptor.lease_generation = replacement.lease_generation;
        descriptor.lifecycle_generation = 2;
        const auto waiting =
          repository->update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
            .result ();
        ASSERT_TRUE (waiting);
        EXPECT_EQ (waiting.value ().status, location_write_status_t::waiting);
        clock.advance (15s);
        EXPECT_EQ (
          location_write_status_t::stored,
          repository->update_mesh_node (descriptor, location_write_intent_t::fixed_rid_new_claim)
            .result ()
            .value ()
            .status);
    }
}

TEST (ZLinkFrameworkOpaqueStoreProviders, UnansweredDescriptorPublicationEndsAtRenewDeadline)
{
    for (auto phase : {unanswered_descriptor_store_t::phase_t::initial_read,
                       unanswered_descriptor_store_t::phase_t::write,
                       unanswered_descriptor_store_t::phase_t::confirmation_read}) {
        SCOPED_TRACE (static_cast<int> (phase));
        unanswered_descriptor_store_t provider (phase);
        provider_location_repository_t repository (provider);
        location_runtime_t runtime (repository, {}, "publication-deadline-owner");
        repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
        runtime.renew_owner_lease_once ();
        const auto owner = runtime.current_owner_token ();
        ASSERT_TRUE (owner.has_value ());
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "publication-deadline-mesh";
        descriptor.rid = zlink::routing_id_t::from ("publication-deadline-node");
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner->owner_id;
        descriptor.lease_generation = owner->lease_generation;
        descriptor.state = framework_runtime_state_t::serving;
        provider.armed = true;
        auto actual = std::make_shared<task_completion_source_t<location_write_result_t>> ();
        release_descriptor_on_exit_t release_on_exit{provider.release};
        auto ambient = detail::capture_ambient_context ();
        ambient.infrastructure_wait = true;
        auto startup_publication = [&] {
            const auto context = detail::enter_ambient_context (ambient);
            return runtime.descriptor_write_confirmed ([&, descriptor, actual] {
                auto pending =
                  repository.update_mesh_node (descriptor, location_write_intent_t::new_claim);
                detail::observe_task_terminal (
                  pending, [actual] (const auto &result) { actual->complete (result); });
                return pending;
            });
        }();
        ASSERT_TRUE (provider.held);
        EXPECT_FALSE (startup_publication.await_ready ());
        detail::deadline_scheduler_test_access_t::advance_to (
          std::chrono::steady_clock::now () + runtime.options ().owner_lease_renew_timeout);
        location_runtime_test_access_t::settle_publication (runtime);
        // A startup publisher must return unknown while the provider remains unanswered.
        const bool completed = startup_publication.await_ready ();
        EXPECT_TRUE (completed);
        if (completed)
            EXPECT_FALSE (startup_publication.result ().value ());
        EXPECT_TRUE (runtime.owner_lease_usable ());
        EXPECT_FALSE (actual->task ().await_ready ());
        provider.release.complete (result_t<void>::success ());
        EXPECT_EQ (location_write_status_t::stored, actual->task ().result ().value ().status);
        startup_publication.result ();
        EXPECT_TRUE (provider.input_stable);
        // Late success does not turn the expired observation into confirmed publication.
        if (completed)
            EXPECT_FALSE (startup_publication.result ().value ());
        provider.armed = false;
        EXPECT_TRUE (infrastructure_result ([&] {
                         return runtime.descriptor_write_confirmed ([&] {
                             return repository.update_mesh_node (
                               descriptor, location_write_intent_t::new_claim);
                         });
                     }).value ());
    }
}

TEST (ZLinkFrameworkOpaqueStoreProviders, ProviderCompletionAfterDeadlineCannotConfirmPublication)
{
    unanswered_descriptor_store_t provider (unanswered_descriptor_store_t::phase_t::write);
    provider_location_repository_t repository (provider);
    location_runtime_t runtime (repository, {}, "late-publication-owner");
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    release_descriptor_on_exit_t release_on_exit{provider.release};
    runtime.renew_owner_lease_once ();
    const auto owner = runtime.current_owner_token ();
    ASSERT_TRUE (owner.has_value ());
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "late-publication-mesh";
    descriptor.rid = zlink::routing_id_t::from ("late-publication-node");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner->owner_id;
    descriptor.lease_generation = owner->lease_generation;
    descriptor.state = framework_runtime_state_t::serving;
    provider.armed = true;
    auto clock = std::make_shared<std::atomic<std::chrono::steady_clock::time_point>> (
      std::chrono::steady_clock::now ());
    auto ambient = detail::capture_ambient_context ();
    ambient.infrastructure_wait = true;
    auto publication = [&] {
        const auto context = detail::enter_ambient_context (ambient);
        return runtime.descriptor_write_confirmed (
          [&, descriptor] {
              return repository.update_mesh_node (descriptor, location_write_intent_t::new_claim);
          },
          [clock] { return clock->load (std::memory_order_acquire); });
    }();
    EXPECT_TRUE (provider.held);
    EXPECT_FALSE (publication.await_ready ());
    clock->store (clock->load () + runtime.options ().owner_lease_renew_timeout,
                  std::memory_order_release);
    // Provider completes before the due timer is dispatched: the operation's
    // one deadline still prevents a late success from confirming startup.
    provider.release.complete (result_t<void>::success ());
    EXPECT_FALSE (publication.result ().value ());
    EXPECT_TRUE (provider.input_stable);
    EXPECT_TRUE (runtime.owner_lease_usable ());
    detail::deadline_scheduler_test_access_t::advance_to (clock->load ());
    EXPECT_FALSE (publication.result ().value ());
}

TEST (ZLinkFrameworkOpaqueStoreProviders, ExpiredPublicationDoesNotStartProvider)
{
    in_memory_location_repository_t repository;
    location_runtime_t runtime (repository);
    struct clock_state_t
    {
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ();
        std::atomic_int calls{0};
    };
    auto clock = std::make_shared<clock_state_t> ();
    int submissions = 0;
    const auto timeout = runtime.options ().owner_lease_renew_timeout;
    EXPECT_FALSE (
      infrastructure_result ([&] {
          return runtime.descriptor_write_confirmed (
            [&] {
                ++submissions;
                return task_t<location_write_result_t> (result_t<location_write_result_t>::success (
                  location_write_result_t{location_write_status_t::stored, 1, {}}));
            },
            [clock, timeout] {
                return clock->now + (clock->calls.fetch_add (1) == 0 ? timeout.zero () : timeout);
            });
      }).value ());
    EXPECT_EQ (0, submissions);
}

TEST (ZLinkFrameworkOpaqueStoreProviders, StopDrainsPublicationWithoutStartedHeartbeat)
{
    in_memory_location_repository_t repository;
    location_runtime_t runtime (repository);
    task_completion_source_t<location_write_result_t> provider;
    bool actual_terminal = false;
    auto actual = provider.task ();
    detail::observe_task_terminal (actual, [&] (const auto &) { actual_terminal = true; });
    auto ambient = detail::capture_ambient_context ();
    ambient.infrastructure_wait = true;
    auto publication = [&] {
        const auto context = detail::enter_ambient_context (ambient);
        return runtime.descriptor_write_confirmed ([&] { return provider.task (); });
    }();
    bool stopped_observed = false;
    detail::observe_task_terminal (publication, [&] (const auto &) {
        stopped_observed = true;
        provider.complete (result_t<location_write_result_t>::success (
          location_write_result_t{location_write_status_t::stored, 1, {}}));
    });
    runtime.stop ();
    EXPECT_TRUE (stopped_observed);
    EXPECT_TRUE (actual_terminal);
    EXPECT_TRUE (publication.await_ready ());
    // Failure cleanup also completes itself if stop incorrectly returned early.
    provider.complete (result_t<location_write_result_t>::success (
      location_write_result_t{location_write_status_t::stored, 1, {}}));
    EXPECT_FALSE (publication.result ());
}

TEST (ZLinkFrameworkOpaqueStoreProviders, PublicationDrainWaitsForActualProviderTerminal)
{
    in_memory_location_repository_t repository;
    location_runtime_t runtime (repository);
    task_completion_source_t<location_write_result_t> provider;
    auto ambient = detail::capture_ambient_context ();
    ambient.infrastructure_wait = true;
    auto publication = [&] {
        const auto context = detail::enter_ambient_context (ambient);
        return runtime.descriptor_write_confirmed ([&] { return provider.task (); });
    }();
    auto drained = location_runtime_test_access_t::drain_repository_operations (runtime);
    const bool cancelled = publication.await_ready ();
    EXPECT_TRUE (cancelled);
    if (cancelled)
        EXPECT_FALSE (publication.result ());
    EXPECT_FALSE (drained.await_ready ());
    EXPECT_FALSE (provider.task ().await_ready ());
    provider.complete (result_t<location_write_result_t>::success (
      location_write_result_t{location_write_status_t::stored, 1, {}}));
    drained.result ().value ();
    EXPECT_TRUE (drained.await_ready ());
    EXPECT_FALSE (publication.result ());
}

TEST (ZLinkFrameworkOpaqueStoreProviders, NativeCancellationIsPreservedBeforeDeadlineDispatch)
{
    in_memory_location_repository_t repository;
    location_runtime_t runtime (repository);
    task_completion_source_t<location_write_result_t> provider;
    auto clock = std::make_shared<std::atomic<std::chrono::steady_clock::time_point>> (
      std::chrono::steady_clock::now ());
    auto ambient = detail::capture_ambient_context ();
    ambient.infrastructure_wait = true;
    auto publication = [&] {
        const auto context = detail::enter_ambient_context (ambient);
        return runtime.descriptor_write_confirmed (
          [&] { return provider.task (); },
          [clock] { return clock->load (std::memory_order_acquire); });
    }();
    clock->store (clock->load () + runtime.options ().owner_lease_renew_timeout,
                  std::memory_order_release);
    const auto cancelled = detail::make_cancellation_exception ("publication cancelled by caller");
    provider.complete (detail::result_access_t::failure<location_write_result_t> (cancelled));
    EXPECT_FALSE (publication.result ());
    EXPECT_EQ (cancelled, publication.result ().exception ());
    detail::deadline_scheduler_test_access_t::advance_to (clock->load ());
    EXPECT_EQ (cancelled, publication.result ().exception ());
}
}
