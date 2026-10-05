/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "../support/owner_lease_time_store.hpp"

#include <gtest/gtest.h>
#include <zlink/framework/contracts/detail/handler_invocation.hpp>

#include <algorithm>
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

namespace
{

using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using zlink::framework::tests::owner_lease_time_store_t;

const store_key_t object_generation_counter_key{"zlink:v11:object-counter"};
const store_key_t authority_owner_generation_counter_key{"zlink:v11:authority-owner-counter"};

class deferred_owner_read_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t) override { return completion.task (); }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        return inner.write (std::move (request));
    }

    task_completion_source_t<store_read_result_t> completion;
    in_memory_location_store_t inner;
};

TEST (ProviderLocationRepositoryTest, OwnerLeaseReadContinuesAfterProviderCompletion)
{
    deferred_owner_read_store_t store;
    provider_location_repository_t repository (store);
    auto read = [&] {
        infrastructure_wait_guard::infrastructure_scope_t infrastructure (&repository);
        return repository.read_owner_lease ("owner");
    }();
    ASSERT_FALSE (read.await_ready ());
    store.completion.complete (result_t<store_read_result_t>::success (store_missing_t{}));
    ASSERT_TRUE (std::holds_alternative<owner_lease_missing_t> (read.result ().value ()));
}

class deferred_relocation_provider_t final : public relocation_store_t
{
  public:
    task_t<blob_put_result_t> put (blob_reference_t reference,
                                   std::span<const std::byte> payload,
                                   std::chrono::milliseconds retention) override
    {
        return inner.put (std::move (reference), payload, retention);
    }
    task_t<blob_read_result_t> read (blob_reference_t) override
    {
        entered.set_value ();
        return read_completion.task ();
    }
    task_t<blob_renew_result_t> renew (blob_reference_t, std::chrono::milliseconds) override
    {
        entered.set_value ();
        return renew_completion.task ();
    }
    task_t<void> erase (blob_reference_t) override
    {
        entered.set_value ();
        return erase_completion.task ();
    }
    in_memory_relocation_store_t inner;
    std::promise<void> entered;
    task_completion_source_t<blob_read_result_t> read_completion;
    task_completion_source_t<blob_renew_result_t> renew_completion;
    task_completion_source_t<void> erase_completion;
};

TEST (CppFrameworkOpaqueRelocationStore, ProviderTasksYieldHandlerWorkerUntilCompletion)
{
    configure_handler_coroutine_executor (1);
    install_host_context_hooks ();
    for (int operation = 0; operation < 3; ++operation) {
        SCOPED_TRACE (operation);
        deferred_relocation_provider_t provider;
        provider_relocation_repository_t repository (provider);
        auto entered = provider.entered.get_future ();
        std::promise<void> probe;
        auto probed = probe.get_future ();
        std::promise<bool> completion;
        auto completed = completion.get_future ();
        const auto observe = [&completion] (auto pending) {
            detail::observe_task_completion (pending, [&completion] (const auto &result) {
                completion.set_value (result.has_value ());
            });
        };
        handler_coroutine_executor ().post_native_continuation ([&] {
            if (operation == 0)
                observe (repository.get_relocation ("held"));
            else if (operation == 1)
                observe (repository.renew_relocation ("held", std::chrono::hours (1)));
            else
                observe (repository.delete_relocation ("held"));
        });
        entered.wait ();
        handler_coroutine_executor ().post_native_continuation ([&probe] { probe.set_value (); });
        const bool yielded = probed.wait_for (300ms) == std::future_status::ready;
        provider.read_completion.complete (
          result_t<blob_read_result_t>::success (blob_missing_t{}));
        provider.renew_completion.complete (
          result_t<blob_renew_result_t>::success (blob_missing_t{}));
        provider.erase_completion.complete (result_t<void>::success ());
        EXPECT_TRUE (completed.get ());
        probed.wait ();
        EXPECT_TRUE (yielded);
    }
    shutdown_handler_coroutine_executor ();
}

std::vector<std::byte> bytes (std::string_view value)
{
    std::vector<std::byte> result;
    result.reserve (value.size ());
    for (const auto character : value)
        result.push_back (static_cast<std::byte> (static_cast<unsigned char> (character)));
    return result;
}

enum class completion_kind_t
{
    created,
    rejected,
    failed
};

std::vector<std::byte> from_hex (std::string_view value)
{
    const auto digit = [] (char value) -> unsigned char {
        if (value >= '0' && value <= '9')
            return static_cast<unsigned char> (value - '0');
        return static_cast<unsigned char> (value - 'a' + 10);
    };
    std::vector<std::byte> result;
    result.reserve (value.size () / 2);
    for (std::size_t index = 0; index < value.size (); index += 2)
        result.push_back (
          static_cast<std::byte> ((digit (value[index]) << 4) | digit (value[index + 1])));
    return result;
}

std::string segment (std::string_view value)
{
    return std::to_string (value.size ()) + ":" + std::string (value) + ":";
}

store_key_t capacity_key (const mesh_node_descriptor_t &descriptor)
{
    return {"zlink:v11:capacity:" + segment (descriptor.mesh_name)
            + segment (descriptor.rid.to_hex ())
            + std::to_string (descriptor.lifecycle_generation)};
}

TEST (ZLinkFrameworkOpaqueStoreProviders, ValueConditionFencesBytesButNotVersion)
{
    in_memory_location_store_t store;
    const store_key_t lease{"lease"};
    const store_key_t mutation{"mutation"};
    const auto write = [&] (std::vector<std::byte> expected) {
        return store
          .write ({{store_value_condition_t{lease, std::move (expected)}},
                   {store_put_t{mutation, bytes ("changed"), std::nullopt}}})
          .result ()
          .value ();
    };
    const auto put = [&] (std::string_view value) {
        return store.write ({{}, {store_put_t{lease, bytes (value), 30s}}}).result ().value ();
    };
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (put ("owner")));
    const auto before = std::get<store_found_t> (store.read (lease).result ().value ());
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (put ("owner")));
    const auto after = std::get<store_found_t> (store.read (lease).result ().value ());
    EXPECT_NE (before.value.version.value, after.value.version.value);
    EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (write (bytes ("owner"))));
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (put ("replacement")));
    EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (write (bytes ("owner"))));
    const auto before_conflict = std::get<store_found_t> (store.read (mutation).result ().value ());
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      store.write ({{}, {store_delete_t{lease}}}).result ().value ()));
    EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (write (bytes ("owner"))));
    const auto after_conflict = std::get<store_found_t> (store.read (mutation).result ().value ());
    EXPECT_EQ (before_conflict.value.version.value, after_conflict.value.version.value);
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      store.write ({{}, {store_put_t{lease, bytes ("owner"), 1ms}}}).result ().value ()));
    bool expired = false;
    for (int attempt = 0; attempt < 100000 && !expired; ++attempt)
        expired = std::holds_alternative<store_missing_t> (store.read (lease).result ().value ());
    ASSERT_TRUE (expired);
    EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (write (bytes ("owner"))));
    const auto after_expiry = std::get<store_found_t> (store.read (mutation).result ().value ());
    EXPECT_EQ (before_conflict.value.version.value, after_expiry.value.version.value);
}

class delayed_renew_write_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        if (!delay_write)
            return inner.write (std::move (request));
        pending_request = std::move (request);
        write_called.set_value ();
        return pending.task ();
    }
    void finish_write ()
    {
        auto result = inner.write (std::move (*pending_request)).result ();
        pending.complete (result);
    }

    in_memory_location_store_t inner;
    bool delay_write = false;
    std::promise<void> write_called;
    task_completion_source_t<store_write_result_t> pending;
    std::optional<store_write_request_t> pending_request;
};

TEST (ZLinkFrameworkOpaqueStoreProviders, RenewReturnsBeforeDelayedProviderWriteCompletes)
{
    delayed_renew_write_store_t store;
    provider_location_repository_t repository (store);
    const auto claimed = repository.claim_owner_lease ("delayed-owner", 30s).result ().value ();
    const auto owner = std::get<owner_lease_claimed_t> (claimed).token;
    auto write_called = store.write_called.get_future ();
    store.delay_write = true;
    auto renewal = repository.renew_owner_lease (owner, 30s);
    write_called.wait ();
    EXPECT_FALSE (renewal.await_ready ());
    store.finish_write ();
    std::atomic_int completions = 0;
    detail::observe_task_completion (renewal, [&] (const auto &) { ++completions; });
    EXPECT_TRUE (std::holds_alternative<owner_lease_renewed_t> (renewal.result ().value ()));
    EXPECT_EQ (1, completions.load ());
}

class renew_before_conditional_commit_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        const auto descriptor_write = std::any_of (
          request.mutations.begin (), request.mutations.end (), [] (const auto &mutation) {
              const auto *put = std::get_if<store_put_t> (&mutation);
              return put && put->key.value.starts_with (std::string ("mesh-node") + '\0');
          });
        const auto reclaim_write = std::any_of (
          request.mutations.begin (), request.mutations.end (), [] (const auto &mutation) {
              const auto *erase = std::get_if<store_delete_t> (&mutation);
              return erase && erase->key.value.starts_with (std::string ("authority") + '\0');
          });
        if (reclaim_write)
            ++reclaim_writes;
        if ((renew_on_descriptor_write && descriptor_write)
            || (renew_on_reclaim_write && reclaim_write)) {
            renew_on_descriptor_write = false;
            renew_on_reclaim_write = false;
            const store_key_t lease_key{std::string ("owner-lease") + '\0' + owner_id};
            const auto lease = std::get<store_found_t> (co_await inner.read (lease_key));
            store_write_request_t renew_request{
              {store_version_condition_t{lease_key, lease.value.version}},
              {store_put_t{lease_key, lease.value.bytes, 30s}}};
            const auto renewed = co_await inner.write (std::move (renew_request));
            if (!std::holds_alternative<store_write_applied_t> (renewed))
                throw std::runtime_error ("test lease renewal failed");
        }
        co_return co_await inner.write (std::move (request));
    }

    in_memory_location_store_t inner;
    std::string owner_id = "renewing-owner";
    bool renew_on_descriptor_write = false;
    bool renew_on_reclaim_write = false;
    unsigned reclaim_writes = 0;
};

TEST (ZLinkFrameworkOpaqueStoreProviders, DescriptorCommitAcceptsConcurrentLeaseRenewal)
{
    renew_before_conditional_commit_store_t store;
    provider_location_repository_t repository (store);
    const auto claimed = repository.claim_owner_lease (store.owner_id, 30s).result ().value ();
    const auto *owner = std::get_if<owner_lease_claimed_t> (&claimed);
    ASSERT_NE (owner, nullptr);
    const store_key_t lease_key{std::string ("owner-lease") + '\0' + store.owner_id};
    const auto before = std::get<store_found_t> (store.read (lease_key).result ().value ());
    ASSERT_TRUE (std::holds_alternative<owner_lease_renewed_t> (
      repository.renew_owner_lease (owner->token, 30s).result ().value ()));
    const auto after = std::get<store_found_t> (store.read (lease_key).result ().value ());
    EXPECT_EQ (before.value.bytes, after.value.bytes);
    EXPECT_NE (before.value.version.value, after.value.version.value);
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "renewal-mesh";
    descriptor.rid = zlink::routing_id_t::from (std::string ("renewal-node"));
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner->token.owner_id;
    descriptor.lease_generation = owner->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    store.renew_on_descriptor_write = true;
    const auto result = repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                          .result ()
                          .value ();
    EXPECT_EQ (result.status, location_write_status_t::stored);
    EXPECT_FALSE (store.renew_on_descriptor_write);
}

TEST (ZLinkFrameworkOpaqueStoreProviders, StaleReservationReclaimIgnoresNewLeaseRenewal)
{
    renew_before_conditional_commit_store_t store;
    store.owner_id = "reclaim-source";
    provider_location_repository_t repository (store);
    const auto source_claim = repository.claim_owner_lease (store.owner_id, 30s).result ().value ();
    const auto target_claim =
      repository.claim_owner_lease ("reclaim-target", 30s).result ().value ();
    const auto *source = std::get_if<owner_lease_claimed_t> (&source_claim);
    const auto *target = std::get_if<owner_lease_claimed_t> (&target_claim);
    ASSERT_NE (source, nullptr);
    ASSERT_NE (target, nullptr);
    const auto publish = [&] (std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "reclaim-mesh";
        descriptor.rid = zlink::routing_id_t::from (rid);
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                                   maintenance_policy_kind_t::recreate, false, 0});
        descriptor.capacity.actors.limit = 1;
        ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
    };
    publish ("reclaim-source-node", source->token);
    publish ("reclaim-target-node", target->token);

    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "reclaim-actor"};
    request.intent.stable_type = "player";
    request.target = {"reclaim-mesh", node_rid_t::from_string ("reclaim-source-node"), 1,
                      source->token};
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;
    const auto original = repository.reserve (request).result ().value ();
    ASSERT_NE (std::get_if<object_reserved_t> (&original), nullptr);
    ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
      repository.release_owner_lease (source->token).result ().value ()));
    const auto successor = repository.claim_owner_lease (store.owner_id, 30s).result ().value ();
    const auto *new_owner = std::get_if<owner_lease_claimed_t> (&successor);
    ASSERT_NE (new_owner, nullptr);
    ASSERT_NE (source->token.lease_generation, new_owner->token.lease_generation);

    request.target = {"reclaim-mesh", node_rid_t::from_string ("reclaim-target-node"), 1,
                      target->token};
    store.renew_on_reclaim_write = true;
    const auto reserved = repository.reserve (request).result ().value ();
    ASSERT_NE (std::get_if<object_reserved_t> (&reserved), nullptr);
    EXPECT_EQ (store.reclaim_writes, 1u);
    EXPECT_FALSE (store.renew_on_reclaim_write);
}

nlohmann::json capacity_record (location_store_t &provider,
                                const mesh_node_descriptor_t &descriptor)
{
    const auto row = provider.read (capacity_key (descriptor)).result ().value ();
    const auto *found = std::get_if<store_found_t> (&row);
    EXPECT_NE (found, nullptr);
    return found
             ? nlohmann::json::parse (reinterpret_cast<const char *> (found->value.bytes.data ()),
                                      reinterpret_cast<const char *> (found->value.bytes.data ())
                                        + found->value.bytes.size ())
             : nlohmann::json{};
}

class creation_terminal_failure_store_t final : public location_store_t
{
  public:
    enum class fault_t
    {
        none,
        before_commit,
        capacity_conflict_once,
        reserve_counter_conflict,
        concurrent_terminal,
        after_commit,
        between_writes
    };

    task_t<store_read_result_t> read (store_key_t key) override
    {
        const auto terminal_key = key.value.starts_with (std::string ("creation-terminal") + '\0');
        auto result = co_await inner.read (std::move (key));
        if (terminal_key) {
            if (const auto *missing = std::get_if<store_missing_t> (&result))
                terminal_read_store_now = missing->store_now;
        }
        co_return result;
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        creation_writes.push_back (request);
        if (before_next_write) {
            auto action = std::exchange (before_next_write, {});
            co_await action ();
        }
        for (const auto &mutation : request.mutations) {
            const auto *put = std::get_if<store_put_t> (&mutation);
            if (!put || !put->key.value.starts_with (std::string ("authority") + '\0'))
                continue;
            const auto authority = nlohmann::json::parse (put->bytes.begin (), put->bytes.end ());
            const auto &pending = authority.at (location_record_fields::pendingCreation);
            if (!pending.is_null ())
                reservation_attempts.push_back (
                  pending.at (location_record_fields::reservationId).get<std::string> ());
        }
        if (fault == fault_t::reserve_counter_conflict) {
            fault = fault_t::none;
            const auto counter = std::get<store_put_t> (request.mutations.at (1));
            store_write_request_t advance{{request.conditions.at (1)}, {counter}};
            (void) co_await inner.write (std::move (advance));
            co_return store_write_result_t{
              store_write_conflict_t{std::chrono::system_clock::now ()}};
        }
        if (fault == fault_t::none) {
            const auto applied = co_await inner.write (request);
            if (attempted) {
                attempted = std::move (request);
                if (const auto *written = std::get_if<store_write_applied_t> (&applied))
                    terminal_write_store_now = written->store_now;
            }
            co_return applied;
        }
        ++writes;
        attempted = request;
        if (fault == fault_t::concurrent_terminal) {
            auto terminal = std::get<store_put_t> (request.mutations.back ());
            terminal.bytes = bytes ("winning-terminal");
            store_write_request_t publish{{store_missing_condition_t{terminal.key}}, {terminal}};
            (void) co_await inner.write (std::move (publish));
            co_return store_write_result_t{
              store_write_conflict_t{std::chrono::system_clock::now ()}};
        }
        if (fault == fault_t::before_commit) {
            const auto authority_key = std::visit (
              [] (const auto &mutation) { return mutation.key; }, request.mutations.front ());
            const auto row = co_await inner.read (authority_key);
            const auto &found = std::get<store_found_t> (row);
            store_write_request_t change{
              {store_version_condition_t{authority_key, found.value.version}},
              {store_put_t{authority_key, found.value.bytes, std::nullopt}}};
            (void) co_await inner.write (std::move (change));
            co_return store_write_result_t{
              store_write_conflict_t{std::chrono::system_clock::now ()}};
        }
        if (fault == fault_t::capacity_conflict_once) {
            if (std::none_of (request.conditions.begin (), request.conditions.end (),
                              [this] (const auto &condition) {
                                  return std::visit (
                                    [this] (const auto &value) {
                                        return value.key.value == conflicting_capacity_key.value;
                                    },
                                    condition);
                              }))
                co_return co_await inner.write (std::move (request));
            fault = fault_t::none;
            const auto row = co_await inner.read (conflicting_capacity_key);
            const auto &found = std::get<store_found_t> (row);
            store_write_request_t change{
              {store_version_condition_t{conflicting_capacity_key, found.value.version}},
              {store_put_t{conflicting_capacity_key, found.value.bytes, std::nullopt}}};
            (void) co_await inner.write (std::move (change));
            co_return store_write_result_t{
              store_write_conflict_t{std::chrono::system_clock::now ()}};
        }
        if (fault == fault_t::between_writes && writes > 1)
            co_return result_t<store_write_result_t>::failure (
              framework_error_kind_t::internal_failure, "provider failed between writes");
        auto applied = co_await inner.write (std::move (request));
        if (const auto *written = std::get_if<store_write_applied_t> (&applied))
            terminal_write_store_now = written->store_now;
        if (fault == fault_t::after_commit) {
            co_return result_t<store_write_result_t>::failure (
              framework_error_kind_t::internal_failure, "provider lost the atomic write reply");
        }
        co_return applied;
    }

    std::vector<store_write_request_t> creation_writes;
    std::function<task_t<void> ()> before_next_write;
    in_memory_location_store_t inner;
    fault_t fault = fault_t::none;
    unsigned writes = 0;
    std::vector<std::string> reservation_attempts;
    store_key_t conflicting_capacity_key;
    std::optional<store_write_request_t> attempted;
    std::optional<std::chrono::system_clock::time_point> terminal_read_store_now;
    std::optional<std::chrono::system_clock::time_point> terminal_write_store_now;
};

class CreationTerminalTest : public ::testing::TestWithParam<completion_kind_t>
{
  protected:
    void SetUp () override
    {
        const auto lease = repository.claim_owner_lease ("terminal-owner", 30s).result ().value ();
        const auto *owner = std::get_if<owner_lease_claimed_t> (&lease);
        ASSERT_NE (owner, nullptr);
        descriptor.mesh_name = "terminal-mesh";
        descriptor.rid = zlink::routing_id_t::from (std::string{"terminal-target"});
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner->token.owner_id;
        descriptor.lease_generation = owner->token.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                                   maintenance_policy_kind_t::recreate, false, 0});
        descriptor.capacity.actors.limit = 1;
        descriptor.object_capabilities.push_back ({placement_object_kind_t::user_spot, "player",
                                                   maintenance_policy_kind_t::disabled, false, 1});
        descriptor.capacity.spots.limit = 1;
        descriptor.capacity.spot_types.push_back (
          {placement_object_kind_t::user_spot, "player", {0, 0, 1}});

        provider.conflicting_capacity_key = capacity_key (descriptor);
        ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
        reserve_request.key = {placement_object_kind_t::actor, "terminal-actor"};
        reserve_request.intent.stable_type = "player";
        reserve_request.target = {"terminal-mesh", node_rid_t::from_string ("terminal-target"), 1,
                                  owner->token};
        reserve_request.creating_payload = bytes ("creating");
        reserve_request.capacity_bundle.actor_slots = 1;
        const auto reserved = repository.reserve (reserve_request).result ().value ();
        const auto *reservation = std::get_if<object_reserved_t> (&reserved);
        ASSERT_NE (reservation, nullptr);
        fence = reservation->fence;
        publication.operation = {
          node_rid_t::from_string (std::string{"\0\xff\x10", 3}), 7, {1, 0xabcdef}};
        publication.terminal_envelope =
          bytes (GetParam () == completion_kind_t::created
                   ? "created-terminal"
                   : (GetParam () == completion_kind_t::rejected ? "rejected-terminal"
                                                                 : "failed-terminal"));
        publication.operation_deadline = std::chrono::time_point_cast<std::chrono::milliseconds> (
                                           std::chrono::system_clock::now ())
                                         + 2s;
    }

    object_complete_creation_request_t request () const
    {
        object_creation_completion_t completion;
        switch (GetParam ()) {
            case completion_kind_t::created:
                completion = object_creation_completed_t{bytes ("ready"), publication};
                break;
            case completion_kind_t::rejected:
                completion = object_creation_rejected_t{publication};
                break;
            case completion_kind_t::failed:
                completion = object_creation_failed_t{publication};
                break;
        }
        return {reserve_request.key, fence, std::move (completion)};
    }

    void expect_published_terminal_and_replay ()
    {
        ASSERT_EQ (provider.writes, 1u);
        ASSERT_TRUE (provider.attempted);
        const std::string terminal_key = std::string ("creation-terminal") + '\0' + "00ff10" + '\0'
                                         + "7" + '\0' + "00000000000000010000000000abcdef";
        bool authority = false, capacity = false, terminal_condition = false;
        const store_put_t *terminal = nullptr;
        unsigned terminal_mutations = 0;
        for (const auto &mutation : provider.attempted->mutations) {
            const auto &key =
              std::visit ([] (const auto &value) -> const store_key_t & { return value.key; },
                          mutation)
                .value;
            authority |= key.starts_with (std::string ("authority") + '\0');
            capacity |= key.starts_with ("zlink:v11:capacity:");
            if (key.starts_with (std::string ("creation-terminal") + '\0')) {
                ++terminal_mutations;
                if (const auto *put = std::get_if<store_put_t> (&mutation);
                    put && key == terminal_key)
                    terminal = put;
            }
        }
        for (const auto &condition : provider.attempted->conditions)
            if (const auto *missing = std::get_if<store_missing_condition_t> (&condition))
                terminal_condition |= missing->key.value == terminal_key;
        ASSERT_NE (terminal, nullptr);
        EXPECT_EQ (terminal_mutations, 1u);
        EXPECT_EQ (terminal->bytes, publication.terminal_envelope);
        ASSERT_TRUE (provider.terminal_read_store_now);
        ASSERT_TRUE (terminal->retention);
        const auto target_expiry = std::chrono::time_point_cast<std::chrono::milliseconds> (
          publication.operation_deadline + 5min);
        EXPECT_EQ (*terminal->retention, std::chrono::ceil<std::chrono::milliseconds> (
                                           target_expiry - *provider.terminal_read_store_now));
        EXPECT_TRUE (authority && capacity && terminal_condition);
        const auto raw = provider.inner.read ({terminal_key}).result ().value ();
        const auto *raw_terminal = std::get_if<store_found_t> (&raw);
        ASSERT_NE (raw_terminal, nullptr);
        EXPECT_EQ (raw_terminal->value.bytes, publication.terminal_envelope);
        ASSERT_TRUE (raw_terminal->value.expires_at);
        provider_location_repository_t reopened (provider);
        const auto stored =
          reopened.read_creation_terminal (publication.operation).result ().value ();
        ASSERT_TRUE (stored);
        EXPECT_EQ (stored->terminal_envelope, publication.terminal_envelope);
        ASSERT_TRUE (provider.terminal_write_store_now);
        // Location runtime §7 retains through the target instant. Location Store §3
        // rounds retention up to milliseconds; the provider's later StoreNow
        // contributes only the read-to-write interval and less than 1 ms.
        EXPECT_GE (stored->expires_at, target_expiry);
        EXPECT_LT (stored->expires_at - target_expiry,
                   *provider.terminal_write_store_now - *provider.terminal_read_store_now + 1ms);
        const auto authority_result =
          reopened.read_authority (actor_authority_key (reserve_request.key.global_id))
            .result ()
            .value ();
        const auto counts = capacity_record (provider, descriptor);
        EXPECT_EQ (counts.at ("actorsPending"), 0);
        if (GetParam () == completion_kind_t::created) {
            const auto *ready = std::get_if<authority_snapshot_t> (&authority_result);
            ASSERT_NE (ready, nullptr);
            EXPECT_EQ (ready->allocation.state, placement_allocation_state_t::active);
            EXPECT_EQ (ready->payload, bytes ("ready"));
            EXPECT_FALSE (ready->pending_creation);
            EXPECT_EQ (counts.at ("actorsActive"), 1);
        } else {
            EXPECT_TRUE (std::holds_alternative<authority_missing_t> (authority_result));
            EXPECT_EQ (counts.at ("actorsActive"), 0);
        }
        // Replays cannot refresh retention or borrow another source's terminal.
        publication.operation_deadline += 1min;
        const auto replay = reopened.complete_creation (request ()).result ().value ();
        const auto *retained = std::get_if<object_creation_already_completed_result_t> (&replay);
        ASSERT_NE (retained, nullptr);
        EXPECT_EQ (retained->terminal.terminal_envelope, stored->terminal_envelope);
        EXPECT_EQ (retained->terminal.expires_at, stored->expires_at);
        EXPECT_EQ (provider.writes, 1u);
        for (const int changed : {0, 1, 2}) {
            auto other = publication.operation;
            if (changed == 0)
                other.source_node_rid = node_rid_t::from_string ("another-source");
            if (changed == 1)
                ++other.source_node_generation;
            if (changed == 2)
                ++other.operation_id.low;
            EXPECT_FALSE (reopened.read_creation_terminal (other).result ().value ());
        }
    }

    void prepare_user_spot ();
    void verify_creation_conditions (bool user_spot);
    void verify_creation_descriptor_conflict (bool user_spot);

    creation_terminal_failure_store_t provider;
    provider_location_repository_t repository{provider};
    mesh_node_descriptor_t descriptor;
    object_reserve_request_t reserve_request;
    object_reservation_fence_t fence;
    creation_terminal_publication_t publication;
};

void CreationTerminalTest::prepare_user_spot ()
{
    ASSERT_TRUE (std::holds_alternative<object_aborted_t> (
      repository.abort ({reserve_request.key, fence}).result ().value ()));
    const auto kind = placement_object_kind_t::user_spot;
    reserve_request.key.kind = kind;
    reserve_request.capacity_bundle = {0, 1, spot_type_capacity_delta_t{kind, "player", 1}};
    const auto reserved = repository.reserve (reserve_request).result ().value ();
    ASSERT_TRUE (std::holds_alternative<object_reserved_t> (reserved));
    fence = std::get<object_reserved_t> (reserved).fence;
}

void CreationTerminalTest::verify_creation_conditions (bool user_spot)
{
    if (user_spot)
        prepare_user_spot ();

    const store_key_t descriptor_key{std::string ("mesh-node") + '\0' + descriptor.mesh_name + '\0'
                                     + descriptor.rid.to_hex ()};
    const auto descriptor_read =
      std::get<store_found_t> (provider.inner.read (descriptor_key).result ().value ());
    for (int transition = 0; transition < 3; ++transition) {
        if (transition == 0) {
            ASSERT_TRUE (std::holds_alternative<object_aborted_t> (
              repository.abort ({reserve_request.key, fence}, {}, publication.operation_deadline)
                .result ()
                .value ()));
            provider.creation_writes.clear ();
            const auto reserved = repository.reserve (reserve_request).result ().value ();
            ASSERT_TRUE (std::holds_alternative<object_reserved_t> (reserved));
            fence = std::get<object_reserved_t> (reserved).fence;
        } else if (transition == 1) {
            provider.creation_writes.clear ();
            ASSERT_TRUE (std::holds_alternative<object_aborted_t> (
              repository.abort ({reserve_request.key, fence}, {}, publication.operation_deadline)
                .result ()
                .value ()));
        } else {
            reserve_request.key.global_id = "abort-actor";
            fence =
              std::get<object_reserved_t> (repository.reserve (reserve_request).result ().value ())
                .fence;
            provider.creation_writes.clear ();
            ASSERT_TRUE (std::holds_alternative<object_committed_t> (
              repository
                .commit ({reserve_request.key, fence, bytes ("ready")}, {},
                         publication.operation_deadline)
                .result ()
                .value ()));
        }
        ASSERT_EQ (provider.creation_writes.size (), 1u);
        const auto &conditions = provider.creation_writes.front ().conditions;
        EXPECT_EQ (conditions.size (), transition == 0 ? 6u : 4u);
        EXPECT_TRUE (std::any_of (conditions.begin (), conditions.end (), [&] (const auto &c) {
            const auto *v = std::get_if<store_value_condition_t> (&c);
            return v && v->key.value == std::string ("owner-lease") + '\0' + descriptor.owner_id;
        }));
        EXPECT_TRUE (std::any_of (conditions.begin (), conditions.end (), [&] (const auto &c) {
            const auto *v = std::get_if<store_version_condition_t> (&c);
            return v && v->key.value == descriptor_key.value
                   && v->expected.value == descriptor_read.value.version.value;
        }));
    }
}

void CreationTerminalTest::verify_creation_descriptor_conflict (bool user_spot)
{
    if (user_spot)
        prepare_user_spot ();

    reserve_request.operation_deadline = publication.operation_deadline;
    const store_key_t descriptor_key{std::string ("mesh-node") + '\0' + descriptor.mesh_name + '\0'
                                     + descriptor.rid.to_hex ()};
    for (int transition = 0; transition < 3; ++transition) {
        if (transition == 0)
            ASSERT_TRUE (std::holds_alternative<object_aborted_t> (
              repository.abort ({reserve_request.key, fence}, {}, publication.operation_deadline)
                .result ()
                .value ()));
        if (transition == 2) {
            reserve_request.key.global_id = "abort-actor";
            fence =
              std::get<object_reserved_t> (repository.reserve (reserve_request).result ().value ())
                .fence;
        }
        provider.creation_writes.clear ();
        provider.before_next_write = [&] () -> task_t<void> {
            const auto read = co_await provider.inner.read (descriptor_key);
            store_write_request_t republish;
            republish.mutations.emplace_back (store_put_t{
              descriptor_key, std::get<store_found_t> (read).value.bytes, std::nullopt});
            co_await provider.inner.write (std::move (republish));
        };
        if (transition == 0)
            fence =
              std::get<object_reserved_t> (repository.reserve (reserve_request).result ().value ())
                .fence;
        else if (transition == 1)
            ASSERT_TRUE (std::holds_alternative<object_aborted_t> (
              repository.abort ({reserve_request.key, fence}, {}, publication.operation_deadline)
                .result ()
                .value ()));
        else
            ASSERT_TRUE (std::holds_alternative<object_committed_t> (
              repository
                .commit ({reserve_request.key, fence, bytes ("ready")}, {},
                         publication.operation_deadline)
                .result ()
                .value ()));
        EXPECT_EQ (provider.creation_writes.size (), 2u);
    }
}

TEST_P (CreationTerminalTest, CreationTransitionsFenceOwnerLeaseAndDescriptorVersion)
{
    verify_creation_conditions (false);
}

TEST_P (CreationTerminalTest, UserSpotCreationTransitionsFenceOwnerLeaseAndDescriptorVersion)
{
    verify_creation_conditions (true);
}

TEST_P (CreationTerminalTest, SameLifecycleDescriptorRepublishRebuildsQualifiedConflict)
{
    verify_creation_descriptor_conflict (false);
}

TEST_P (CreationTerminalTest, UserSpotSameLifecycleDescriptorRepublishRebuildsQualifiedConflict)
{
    verify_creation_descriptor_conflict (true);
}

TEST_P (CreationTerminalTest, PreviousLifecycleCommitIsRejected)
{
    provider.creation_writes.clear ();
    provider.before_next_write = [&] () -> task_t<void> {
        const store_key_t key{std::string ("mesh-node") + '\0' + descriptor.mesh_name + '\0'
                              + descriptor.rid.to_hex ()};
        const auto read = co_await provider.inner.read (key);
        const auto &data = std::get<store_found_t> (read).value.bytes;
        auto record = nlohmann::json::parse (
          std::string (reinterpret_cast<const char *> (data.data ()), data.size ()));
        record["descriptor"]["lifecycleGeneration"] = "2";
        store_write_request_t republish;
        republish.mutations.emplace_back (store_put_t{key, bytes (record.dump ()), std::nullopt});
        co_await provider.inner.write (std::move (republish));
    };
    const auto result =
      repository
        .commit ({reserve_request.key, fence, bytes ("ready")}, {}, publication.operation_deadline)
        .result ()
        .value ();
    EXPECT_TRUE (std::holds_alternative<object_commit_conflict_t> (result));
    EXPECT_EQ (provider.creation_writes.size (), 1u);
}

TEST_P (CreationTerminalTest, AcceptedCreationCompletesWhileTargetDraining)
{
    descriptor.state = framework_runtime_state_t::draining;
    ++descriptor.descriptor_revision;
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::renew)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    const auto result = repository.complete_creation (request ()).result ().value ();
    EXPECT_TRUE (std::holds_alternative<object_creation_completed_result_t> (result));
}

TEST_P (CreationTerminalTest, FailureBetweenFormerWritesCannotSplitPublication)
{
    provider.fault = creation_terminal_failure_store_t::fault_t::between_writes;
    const auto result = repository.complete_creation (request ()).result ().value ();
    ASSERT_TRUE (std::holds_alternative<object_creation_completed_result_t> (result));
    expect_published_terminal_and_replay ();
}

TEST_P (CreationTerminalTest, LostAtomicWriteReplyReplaysStoredTerminal)
{
    provider.fault = creation_terminal_failure_store_t::fault_t::after_commit;
    const auto result = repository.complete_creation (request ()).result ().value ();
    ASSERT_TRUE (std::holds_alternative<object_creation_completed_result_t> (result));
    expect_published_terminal_and_replay ();
}

TEST_P (CreationTerminalTest, ConditionalAuthorityConflictLeavesReservationAndCapacityUnchanged)
{
    provider.fault = creation_terminal_failure_store_t::fault_t::before_commit;
    const auto result = repository.complete_creation (request ()).result ().value ();
    EXPECT_TRUE (std::holds_alternative<object_creation_completion_conflict_t> (result));
    EXPECT_EQ (provider.writes, 1u);
    EXPECT_FALSE (repository.read_creation_terminal (publication.operation).result ().value ());
    const auto authority =
      repository.read_authority (actor_authority_key (reserve_request.key.global_id))
        .result ()
        .value ();
    const auto *creating = std::get_if<authority_snapshot_t> (&authority);
    ASSERT_NE (creating, nullptr);
    EXPECT_EQ (creating->allocation.state, placement_allocation_state_t::reserved);
    EXPECT_EQ (creating->payload, bytes ("creating"));
    EXPECT_NE (creating->store_version, fence.expected_store_version);
    EXPECT_EQ (capacity_record (provider, descriptor).at ("actorsPending"), 1);
    EXPECT_EQ (capacity_record (provider, descriptor).at ("actorsActive"), 0);
}

TEST_P (CreationTerminalTest, SharedCapacityConflictReconstructsWithoutChangingReservation)
{
    provider.fault = creation_terminal_failure_store_t::fault_t::capacity_conflict_once;
    const auto result = repository.complete_creation (request ()).result ().value ();
    ASSERT_TRUE (std::holds_alternative<object_creation_completed_result_t> (result));
    expect_published_terminal_and_replay ();
}

TEST_P (CreationTerminalTest, ReserveCounterConflictKeepsOperationReservationIdentity)
{
    ASSERT_TRUE (std::holds_alternative<object_aborted_t> (
      repository.abort ({reserve_request.key, fence}).result ().value ()));
    provider.reservation_attempts.clear ();
    provider.fault = creation_terminal_failure_store_t::fault_t::reserve_counter_conflict;
    reserve_request.operation_deadline = publication.operation_deadline;
    const auto result = repository.reserve (reserve_request).result ().value ();
    const auto *reserved = std::get_if<object_reserved_t> (&result);
    ASSERT_NE (reserved, nullptr);
    ASSERT_EQ (provider.reservation_attempts.size (), 2u);
    EXPECT_EQ (provider.reservation_attempts.front (), provider.reservation_attempts.back ());
    EXPECT_EQ (reserved->fence.reservation_id, provider.reservation_attempts.front ());
}
TEST_P (CreationTerminalTest, ConcurrentTerminalEndsConflictReconstruction)
{
    provider.fault = creation_terminal_failure_store_t::fault_t::concurrent_terminal;
    const auto result = repository.complete_creation (request ()).result ().value ();
    const auto *completed = std::get_if<object_creation_already_completed_result_t> (&result);
    ASSERT_NE (completed, nullptr);
    EXPECT_EQ (completed->terminal.terminal_envelope, bytes ("winning-terminal"));
    EXPECT_EQ (provider.writes, 1u);
    const auto authority =
      repository.read_authority (actor_authority_key (reserve_request.key.global_id))
        .result ()
        .value ();
    const auto *reserved = std::get_if<authority_snapshot_t> (&authority);
    ASSERT_NE (reserved, nullptr);
    EXPECT_EQ (reserved->store_version, fence.expected_store_version);
    EXPECT_EQ (capacity_record (provider, descriptor).at ("actorsPending"), 1);
    EXPECT_EQ (capacity_record (provider, descriptor).at ("actorsActive"), 0);
}

INSTANTIATE_TEST_SUITE_P (CreationTerminalStates,
                          CreationTerminalTest,
                          ::testing::Values (completion_kind_t::created,
                                             completion_kind_t::rejected,
                                             completion_kind_t::failed));

TEST (CreationTerminalStoreRecord, ReadsNodeTerminalEnvelopeAtCanonicalKey)
{
    const creation_operation_identity_t operation{
      node_rid_t::from_string (std::string{"\0\xff\x10", 3}), 7, {1, 0xabcdef}};
    const std::string key = std::string ("creation-terminal") + '\0' + "00ff10" + '\0' + "7" + '\0'
                            + "00000000000000010000000000abcdef";
    // Generated by the Node production schema codec for a created terminal.
    const auto node_terminal =
      from_hex ("01000000250000000000000000010200180f6163746f722d63616e6f6e6963616c"
                "000000000000000100");
    in_memory_location_store_t provider;
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider
        .write ({.conditions = {store_missing_condition_t{{key}}},
                 .mutations = {store_put_t{{key}, node_terminal, 1min}}})
        .result ()
        .value ()));

    provider_location_repository_t repository{provider};
    const auto stored = repository.read_creation_terminal (operation).result ().value ();
    ASSERT_TRUE (stored);
    EXPECT_EQ (stored->terminal_envelope, node_terminal);
}

class post_commit_failure_location_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }

    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        auto committed = co_await inner.write (std::move (request));
        if (_fail_next_write) {
            _fail_next_write = false;
            co_return result_t<store_write_result_t>::failure (framework_error_kind_t::unavailable,
                                                               "reply was lost after commit");
        }
        co_return committed;
    }

    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }

    in_memory_location_store_t inner;

  private:
    bool _fail_next_write = true;
};

class reject_next_authority_capacity_write_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }

    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        if (reject_next) {
            std::size_t authority_mutations = 0;
            std::size_t capacity_mutations = 0;
            for (const auto &mutation : request.mutations) {
                const auto &key = std::visit (
                  [] (const auto &value) -> const store_key_t & { return value.key; }, mutation);
                if (key.value.starts_with (std::string ("authority") + '\0'))
                    ++authority_mutations;
                if (key.value.starts_with ("zlink:v11:capacity:"))
                    ++capacity_mutations;
            }
            if (authority_mutations != 0 && capacity_mutations != 0) {
                reject_next = false;
                rejected_atomic_batch = true;
                rejected_capacity_mutations = capacity_mutations;
                const auto authority_key = std::visit (
                  [] (const auto &mutation) { return mutation.key; }, request.mutations.front ());
                // Awaited: the repository may resume on the inner store's lane.
                const auto row = std::get<store_found_t> (co_await inner.read (authority_key));
                store_write_request_t concurrent{
                  {store_version_condition_t{authority_key, row.value.version}},
                  {store_put_t{authority_key, row.value.bytes, std::nullopt}}};
                (void) co_await inner.write (std::move (concurrent));
                co_return store_write_result_t{
                  store_write_conflict_t{std::chrono::system_clock::now ()}};
            }
        }
        co_return co_await inner.write (std::move (request));
    }

    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }

    in_memory_location_store_t inner;
    bool reject_next = false;
    bool rejected_atomic_batch = false;
    std::size_t rejected_capacity_mutations = 0;
};

class NewOwnerRecoveryTest : public ::testing::TestWithParam<std::tuple<bool, bool, bool>>
{
};

TEST_P (NewOwnerRecoveryTest, SourceDescriptorAndLeaseDoNotOwnTargetCommit)
{
    creation_terminal_failure_store_t store;
    provider_location_repository_t provider (store);
    in_memory_location_repository_t memory;
    location_repository_t &repository = std::get<0> (GetParam ())
                                          ? static_cast<location_repository_t &> (provider)
                                          : static_cast<location_repository_t &> (memory);
    const bool aggregate = std::get<1> (GetParam ());
    const bool target_live = std::get<2> (GetParam ());
    const auto source_claim = repository.claim_owner_lease ("source", 30s).result ().value ();
    const auto target_claim = repository.claim_owner_lease ("target", 30s).result ().value ();
    const auto source = std::get<owner_lease_claimed_t> (source_claim).token;
    const auto target = std::get<owner_lease_claimed_t> (target_claim).token;
    const auto publish = [&] (std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "new-owner-recovery";
        descriptor.rid = zlink::routing_id_t::from (rid);
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.activation_concurrency.limit = 2;
        descriptor.security_identity = "new-owner-recovery";
        descriptor.object_capabilities = {
          {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0},
          {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::snapshot, true,
           8}};
        descriptor.capacity.actors.limit = 1;
        descriptor.capacity.spots.limit = 1;
        descriptor.capacity.spot_types.push_back (
          {placement_object_kind_t::user_spot, "room", {0, 0, 1}});
        EXPECT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
        return descriptor;
    };
    const auto source_descriptor = publish ("source-node", source);
    publish ("target-node", target);
    const object_creation_target_t source_placement{
      "new-owner-recovery", node_rid_t::from_string ("source-node"), 1, source};
    const object_creation_target_t target_placement{
      "new-owner-recovery", node_rid_t::from_string ("target-node"), 1, target};
    const auto create = [&] (placement_object_kind_t kind, std::string id) {
        object_reserve_request_t request;
        request.key = {kind, std::move (id)};
        request.intent.stable_type = kind == placement_object_kind_t::actor ? "player" : "room";
        request.target = source_placement;
        if (kind == placement_object_kind_t::actor)
            request.capacity_bundle.actor_slots = 1;
        else {
            request.capacity_bundle.spot_slots = 1;
            request.capacity_bundle.spot_type =
              spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
        }
        const auto reserved = repository.reserve (request).result ().value ();
        const auto *reservation = std::get_if<object_reserved_t> (&reserved);
        EXPECT_NE (reservation, nullptr);
        if (!reservation)
            return authority_snapshot_t{};
        const auto committed =
          repository.commit ({request.key, reservation->fence, bytes ("ready")}).result ().value ();
        const auto *ready = std::get_if<object_committed_t> (&committed);
        EXPECT_NE (ready, nullptr);
        return ready ? ready->ready : authority_snapshot_t{};
    };
    const auto actor = create (placement_object_kind_t::actor, "actor");
    const auto spot =
      aggregate ? create (placement_object_kind_t::user_spot, "spot") : authority_snapshot_t{};
    ASSERT_EQ (
      repository
        .remove_mesh_node (
          {"new-owner-recovery", zlink::routing_id_t::from (std::string ("source-node"))}, source)
        .result ()
        .value (),
      location_write_status_t::stored);
    ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
      repository.release_owner_lease (source).result ().value ()));
    if (std::get<0> (GetParam ()) && target_live) {
        store.conflicting_capacity_key = capacity_key (source_descriptor);
        store.fault = creation_terminal_failure_store_t::fault_t::capacity_conflict_once;
    }
    if (aggregate) {
        aggregate_prepare_request_t request;
        request.aggregate_id.value[0] = std::byte{1};
        request.aggregate_generation = 1;
        request.participants = {
          {actor_authority_key ("actor"), actor.store_version,
           authority_generation_transition_t::new_owner, bytes ("moved-actor")},
          {spot_authority_key ("spot"), spot.store_version,
           authority_generation_transition_t::new_owner, bytes ("moved-spot")}};
        request.target_owner = target;
        request.target_descriptor = {"new-owner-recovery",
                                     zlink::routing_id_t::from (std::string ("target-node"))};
        request.target_descriptor_lifecycle_generation = 1;
        request.capacity_bundle.actor_slots = 1;
        request.capacity_bundle.spot_slots = 1;
        request.capacity_bundle.spot_type =
          spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
        const auto prepared = repository.prepare_aggregate (request).result ().value ();
        const auto *fence = std::get_if<aggregate_prepared_t> (&prepared);
        ASSERT_NE (fence, nullptr);
        if (!target_live)
            ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
              repository.release_owner_lease (target).result ().value ()));
        EXPECT_EQ (repository.commit_aggregate (fence->fence).result ().value (),
                   target_live ? aggregate_commit_result_t::committed
                               : aggregate_commit_result_t::stale);
    } else {
        if (!target_live)
            ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
              repository.release_owner_lease (target).result ().value ()));
        const auto moved =
          repository
            .compare_exchange_authority (actor_authority_key ("actor"), actor.store_version,
                                         authority_retarget_t{bytes ("moved"), target_placement})
            .result ()
            .value ();
        EXPECT_EQ (std::holds_alternative<authority_stored_t> (moved), target_live);
    }
    const auto observed =
      repository.read_authority (actor_authority_key ("actor")).result ().value ();
    const auto *current = std::get_if<authority_snapshot_t> (&observed);
    ASSERT_NE (current, nullptr);
    EXPECT_EQ (current->object_generation, actor.object_generation);
    EXPECT_EQ (current->owner.owner_id, target_live ? target.owner_id : source.owner_id);
    EXPECT_EQ (current->allocation.target.node_rid.value (),
               target_live ? "target-node" : "source-node");
    if (target_live)
        EXPECT_NE (current->store_version, actor.store_version);
    else
        EXPECT_EQ (current->store_version, actor.store_version);
}

INSTANTIATE_TEST_SUITE_P (RepositoryImplementations,
                          NewOwnerRecoveryTest,
                          ::testing::Combine (::testing::Bool (),
                                              ::testing::Bool (),
                                              ::testing::Bool ()));
class aggregate_commit_contention_store_t final : public location_store_t
{
  public:
    enum class phase_t
    {
        transition,
        page,
        terminal,
        lease_loss,
        counter_race
    };
    task_t<store_read_result_t> read (store_key_t key) override
    {
        if (on_counter_read && key.value == "zlink:v11:authority-owner-counter"
            && ++counter_reads == 2) {
            // The injected peer step runs on the lane that resumes the
            // repository, so it is awaited rather than blocking that lane.
            auto action = std::move (on_counter_read);
            co_await action ();
        }
        co_return co_await inner.read (std::move (key));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        bool counter = false;
        bool page = false;
        bool capacity = false;
        for (const auto &mutation : request.mutations) {
            const auto &key = std::visit (
              [] (const auto &value) -> const store_key_t & { return value.key; }, mutation);
            counter |= key.value == "zlink:v11:authority-owner-counter";
            page |= key.value.starts_with ("zlink:v11:aggregate-commit:");
            capacity |= key.value.starts_with ("zlink:v11:capacity:");
        }
        const bool matching = phase == phase_t::transition ? counter
                              : phase == phase_t::page     ? page
                                                           : capacity;
        if (remaining != 0 && matching) {
            --remaining;
            ++rejected;
            if (on_conflict) {
                auto action = std::move (on_conflict);
                co_await action ();
            }
            co_return store_write_result_t{
              store_write_conflict_t{std::chrono::system_clock::now ()}};
        }
        co_return co_await inner.write (std::move (request));
    }
    in_memory_location_store_t inner;
    phase_t phase = phase_t::transition;
    std::size_t remaining = 0;
    std::size_t rejected = 0;
    std::function<task_t<void> ()> on_conflict;
    std::function<task_t<void> ()> on_counter_read;
    std::size_t counter_reads = 0;
};

class aggregate_lock_contention_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }

    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        const auto publishes_lock = std::any_of (
          request.mutations.begin (), request.mutations.end (), [] (const auto &mutation) {
              const auto *put = std::get_if<store_put_t> (&mutation);
              return put && put->key.value.starts_with ("zlink:v11:aggregate-lock:");
          });
        if (!peer_marker_published && publishes_lock) {
            peer_marker_published = true;
            auto peer_request = request;
            for (auto &mutation : peer_request.mutations) {
                auto *put = std::get_if<store_put_t> (&mutation);
                if (!put || !put->key.value.starts_with ("zlink:v11:aggregate-lock:"))
                    continue;
                auto marker = nlohmann::json::parse (
                  reinterpret_cast<const char *> (put->bytes.data ()),
                  reinterpret_cast<const char *> (put->bytes.data ()) + put->bytes.size ());
                marker["peerMarker"] = true;
                put->bytes = bytes (marker.dump ());
            }
            // The repository may resume on the inner store's lane, so await
            // the peer write instead of blocking that lane.
            const auto published = co_await inner.write (std::move (peer_request));
            const auto now = std::holds_alternative<store_write_applied_t> (published)
                               ? std::get<store_write_applied_t> (published).store_now
                               : std::get<store_write_conflict_t> (published).store_now;
            co_return store_write_conflict_t{now};
        }
        co_return co_await inner.write (std::move (request));
    }

    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }

    in_memory_location_store_t inner;
    bool peer_marker_published = false;
};

class post_commit_failure_relocation_store_t final : public relocation_store_t
{
  public:
    task_t<blob_put_result_t> put (blob_reference_t reference,
                                   std::span<const std::byte> payload,
                                   std::chrono::milliseconds retention) override
    {
        auto committed = co_await inner.put (reference, payload, retention);
        if (_fail_next_put) {
            _fail_next_put = false;
            co_return result_t<blob_put_result_t>::failure (framework_error_kind_t::unavailable,
                                                            "reply was lost after commit");
        }
        co_return committed;
    }

    task_t<blob_read_result_t> read (blob_reference_t reference) override
    {
        return inner.read (std::move (reference));
    }

    task_t<blob_renew_result_t> renew (blob_reference_t reference,
                                       std::chrono::milliseconds retention) override
    {
        return inner.renew (std::move (reference), retention);
    }

    task_t<void> erase (blob_reference_t reference) override
    {
        return inner.erase (std::move (reference));
    }

    in_memory_relocation_store_t inner;

  private:
    bool _fail_next_put = true;
};

class uncertain_put_relocation_store_t final : public relocation_store_t
{
  public:
    explicit uncertain_put_relocation_store_t (
      framework_error_kind_t error_kind = framework_error_kind_t::unavailable) :
        _error_kind (error_kind)
    {
    }

    task_t<blob_put_result_t> put (blob_reference_t reference,
                                   std::span<const std::byte> payload,
                                   std::chrono::milliseconds retention) override
    {
        put_references.push_back (reference.value);
        if (put_references.size () < 3)
            return task_t<blob_put_result_t> (
              result_t<blob_put_result_t>::failure (_error_kind, "put response uncertain"));
        return inner.put (std::move (reference), payload, retention);
    }
    task_t<blob_read_result_t> read (blob_reference_t reference) override
    {
        read_references.push_back (reference.value);
        return inner.read (std::move (reference));
    }
    task_t<blob_renew_result_t> renew (blob_reference_t reference,
                                       std::chrono::milliseconds retention) override
    {
        return inner.renew (std::move (reference), retention);
    }
    task_t<void> erase (blob_reference_t reference) override
    {
        return inner.erase (std::move (reference));
    }
    in_memory_relocation_store_t inner;
    std::vector<std::string> put_references;
    std::vector<std::string> read_references;

  private:
    framework_error_kind_t _error_kind;
};

TEST (CppFrameworkOpaqueRelocationStore, MissingReadBackRetriesSameReferenceAfterProviderError)
{
    uncertain_put_relocation_store_t provider (framework_error_kind_t::protocol_error);
    provider_relocation_repository_t repository (provider);
    const auto result =
      repository
        .put_relocation (bytes ("uncommitted"), 1h, std::chrono::steady_clock::now () + 1min)
        .result ();
    ASSERT_TRUE (result);
    ASSERT_EQ (provider.put_references.size (), 3u);
    EXPECT_EQ (provider.put_references[0], provider.put_references[1]);
    EXPECT_EQ (provider.put_references[0], provider.put_references[2]);
}

TEST (CppFrameworkOpaqueRelocationStore, ReconciliationDoesNotStopAtFixedAttemptCount)
{
    uncertain_put_relocation_store_t provider;
    provider_relocation_repository_t repository (provider);
    const auto result =
      repository
        .put_relocation (bytes ("eventual-commit"), 1h, std::chrono::steady_clock::now () + 1min)
        .result ();
    ASSERT_TRUE (result);
    ASSERT_EQ (provider.put_references.size (), 3u);
    EXPECT_EQ (provider.put_references[0], provider.put_references[1]);
    EXPECT_EQ (provider.put_references[0], provider.put_references[2]);
    ASSERT_EQ (provider.read_references.size (), 2u);
    EXPECT_EQ (provider.read_references[0], provider.put_references[0]);
    EXPECT_EQ (provider.read_references[1], provider.put_references[0]);
}

TEST (CppFrameworkOpaqueRelocationStore, ExpiredOperationDoesNotStartProviderIo)
{
    uncertain_put_relocation_store_t provider;
    provider_relocation_repository_t repository (provider);
    const auto result =
      repository.put_relocation (bytes ("expired"), 1h, std::chrono::steady_clock::now ())
        .result ();
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::deadline_exceeded);
    EXPECT_TRUE (provider.put_references.empty ());
    EXPECT_TRUE (provider.read_references.empty ());
    try {
        (void) result.value ();
        FAIL () << "expired operation did not throw its typed failure";
    }
    catch (...) {
        const auto failure = detail::current_exception_result<void> ();
        EXPECT_EQ (failure.error_kind (), framework_error_kind_t::deadline_exceeded);
    }
}

class pending_relocation_store_t final : public relocation_store_t
{
  public:
    enum class stage_t
    {
        put,
        read
    };
    explicit pending_relocation_store_t (stage_t stage) : _stage (stage) {}
    std::future<void> started () { return _started.get_future (); }

    task_t<blob_put_result_t>
    put (blob_reference_t, std::span<const std::byte> payload, std::chrono::milliseconds) override
    {
        _payload.assign (payload.begin (), payload.end ());
        if (_stage == stage_t::read)
            return task_t<blob_put_result_t> (result_t<blob_put_result_t>::failure (
              framework_error_kind_t::unavailable, "put response uncertain"));
        _started.set_value ();
        return _put.task ();
    }
    task_t<blob_read_result_t> read (blob_reference_t) override
    {
        _started.set_value ();
        return _read.task ();
    }
    task_t<blob_renew_result_t> renew (blob_reference_t, std::chrono::milliseconds) override
    {
        return task_t<blob_renew_result_t> (
          result_t<blob_renew_result_t>::success (blob_missing_t{}));
    }
    task_t<void> erase (blob_reference_t) override
    {
        return task_t<void> (result_t<void>::success ());
    }
    void complete ()
    {
        const auto now = std::chrono::system_clock::now ();
        if (_stage == stage_t::put)
            _put.complete (result_t<blob_put_result_t>::success (blob_stored_t{now + 1h, now}));
        else
            _read.complete (
              result_t<blob_read_result_t>::success (blob_found_t{_payload, now + 1h, now}));
    }

  private:
    stage_t _stage;
    std::vector<std::byte> _payload;
    std::promise<void> _started;
    task_completion_source_t<blob_put_result_t> _put;
    task_completion_source_t<blob_read_result_t> _read;
};

void expect_pending_relocation_operation_deadline (pending_relocation_store_t::stage_t stage)
{
    pending_relocation_store_t provider (stage);
    provider_relocation_repository_t repository (provider);
    auto started = provider.started ();
    const auto timeout = 200ms;
    auto operation = std::async (std::launch::async, [&] {
        return repository
          .put_relocation (bytes ("pending"), 1h, std::chrono::steady_clock::now () + timeout)
          .result ();
    });
    const auto started_status = started.wait_for (5s);
    EXPECT_EQ (started_status, std::future_status::ready);
    EXPECT_EQ (operation.wait_for (timeout * 2), std::future_status::ready);
    provider.complete ();
    const auto result = operation.get ();
    EXPECT_FALSE (result);
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::deadline_exceeded);
}

TEST (CppFrameworkOpaqueRelocationStore, PendingPutEndsAtOperationDeadline)
{
    expect_pending_relocation_operation_deadline (pending_relocation_store_t::stage_t::put);
}

TEST (CppFrameworkOpaqueRelocationStore, PendingReadBackEndsAtOperationDeadline)
{
    expect_pending_relocation_operation_deadline (pending_relocation_store_t::stage_t::read);
}

TEST (CppFrameworkOpaqueLocationStore, AtomicWriteUsesExactVersions)
{
    in_memory_location_store_t store;
    const auto first =
      store
        .write ({.conditions = {store_missing_condition_t{{.value = "authority:a"}}},
                 .mutations = {store_put_t{{.value = "authority:a"}, bytes ("one"), std::nullopt},
                               store_put_t{{.value = "capacity:n"}, bytes ("reserved"), 30s}}})
        .result ()
        .value ();
    const auto *applied = std::get_if<store_write_applied_t> (&first);
    ASSERT_NE (applied, nullptr);
    ASSERT_EQ (applied->put_versions.size (), 2u);

    const auto read = store.read ({.value = "authority:a"}).result ().value ();
    const auto *found = std::get_if<store_found_t> (&read);
    ASSERT_NE (found, nullptr);
    EXPECT_EQ (found->value.bytes, bytes ("one"));

    const auto conflict =
      store
        .write (
          {.conditions = {store_version_condition_t{{.value = "authority:a"}, {.value = "stale"}}},
           .mutations = {store_put_t{{.value = "authority:a"}, bytes ("two"), std::nullopt},
                         store_delete_t{{.value = "capacity:n"}}}})
        .result ()
        .value ();
    EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (conflict));

    const auto unchanged =
      std::get<store_found_t> (store.read ({.value = "authority:a"}).result ().value ());
    EXPECT_EQ (unchanged.value.bytes, bytes ("one"));
    EXPECT_TRUE (std::holds_alternative<store_found_t> (
      store.read ({.value = "capacity:n"}).result ().value ()));
}

TEST (CppFrameworkOpaqueLocationStore, ScanKeepsTheFirstPageSnapshot)
{
    in_memory_location_store_t store;
    for (const auto *key : {"descriptor:a", "descriptor:b", "other:a"}) {
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          store
            .write ({.conditions = {store_missing_condition_t{{.value = key}}},
                     .mutations = {store_put_t{{.value = key}, bytes (key), std::nullopt}}})
            .result ()
            .value ()));
    }

    auto first = std::get<store_scan_page_t> (
      store.scan ({.prefix = "descriptor:", .cursor = std::nullopt, .limit = 1})
        .result ()
        .value ());
    ASSERT_EQ (first.items.size (), 1u);
    ASSERT_TRUE (first.next_cursor.has_value ());

    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      store
        .write (
          {.conditions = {store_missing_condition_t{{.value = "descriptor:c"}}},
           .mutations = {store_put_t{{.value = "descriptor:c"}, bytes ("late"), std::nullopt}}})
        .result ()
        .value ()));

    const auto second = std::get<store_scan_page_t> (
      store.scan ({.prefix = "descriptor:", .cursor = first.next_cursor, .limit = 10})
        .result ()
        .value ());
    ASSERT_EQ (second.items.size (), 1u);
    EXPECT_EQ (second.items.front ().key.value, "descriptor:b");
}

TEST (CppFrameworkOpaqueLocationStore, CursorFromAnotherStoreInstanceExpires)
{
    in_memory_location_store_t first;
    for (const auto *key : {"descriptor:a", "descriptor:b"}) {
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          first
            .write ({.conditions = {store_missing_condition_t{{.value = key}}},
                     .mutations = {store_put_t{{.value = key}, bytes (key), std::nullopt}}})
            .result ()
            .value ()));
    }
    const auto page = std::get<store_scan_page_t> (
      first.scan ({.prefix = "descriptor:", .cursor = std::nullopt, .limit = 1})
        .result ()
        .value ());
    ASSERT_TRUE (page.next_cursor.has_value ());

    in_memory_location_store_t restarted;
    EXPECT_TRUE (std::holds_alternative<store_scan_expired_t> (
      restarted.scan ({.prefix = "descriptor:", .cursor = page.next_cursor, .limit = 1})
        .result ()
        .value ()));
}

TEST (CppFrameworkOpaqueLocationStore, RepositoryReconcilesLostCommitReply)
{
    post_commit_failure_location_store_t provider;
    provider_location_repository_t repository (provider);

    const auto result = repository.claim_owner_lease ("owner-a", 30s).result ().value ();
    ASSERT_TRUE (std::holds_alternative<owner_lease_claimed_t> (result));

    provider_location_repository_t reopened (provider.inner);
    EXPECT_TRUE (std::holds_alternative<owner_lease_found_t> (
      reopened.read_owner_lease ("owner-a").result ().value ()));
}

TEST (CppFrameworkOpaqueLocationStore, PrivateRepositoryPersistsLeaseAndDescriptorThroughProvider)
{
    in_memory_location_store_t provider;
    provider_location_repository_t first (provider);
    const auto claim = first.claim_owner_lease ("owner-a", 30s).result ().value ();
    const auto *claimed = std::get_if<owner_lease_claimed_t> (&claim);
    ASSERT_NE (claimed, nullptr);

    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "play";
    descriptor.rid = zlink::routing_id_t::from (std::uint32_t{7});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = claimed->token.owner_id;
    descriptor.lease_generation = claimed->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;

    const auto stored =
      first.update_mesh_node (descriptor, location_write_intent_t::new_claim).result ().value ();
    ASSERT_EQ (stored.status, location_write_status_t::stored);

    provider_location_repository_t second (provider);
    const auto lease = second.read_owner_lease ("owner-a").result ().value ();
    ASSERT_TRUE (std::holds_alternative<owner_lease_found_t> (lease));
    const auto page = second.list_mesh_nodes ("play").result ().value ();
    ASSERT_EQ (page.items.size (), 1u);
    EXPECT_EQ (page.items.front ().rid.to_string (), descriptor.rid.to_string ());
    EXPECT_EQ (page.items.front ().owner_id, "owner-a");
}

TEST (CppFrameworkOpaqueLocationStore, MeshNewClaimUsesStoredOwnerLeaseExactRead)
{
    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
    const auto expired_claim =
      repository.claim_owner_lease ("expired-descriptor-owner", 30s).result ().value ();
    const auto *expired_owner = std::get_if<owner_lease_claimed_t> (&expired_claim);
    ASSERT_NE (expired_owner, nullptr);

    const auto descriptor = [] (std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t value;
        value.mesh_name = "descriptor-fence";
        value.rid = zlink::routing_id_t::from (std::move (rid));
        value.lifecycle_generation = 1;
        value.descriptor_revision = 1;
        value.endpoint = "tcp://127.0.0.1:7001";
        value.owner_id = owner.owner_id;
        value.lease_generation = owner.lease_generation;
        value.object_role = object_role_t::server;
        value.state = framework_runtime_state_t::serving;
        return value;
    };
    ASSERT_EQ (repository
                 .update_mesh_node (descriptor ("expired", expired_owner->token),
                                    location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    ASSERT_NE (std::get_if<owner_lease_released_t> (
                 &repository.release_owner_lease (expired_owner->token).result ().value ()),
               nullptr);
    ASSERT_TRUE (std::holds_alternative<owner_lease_missing_t> (
      repository.read_owner_lease (expired_owner->token.owner_id).result ().value ()));

    const auto successor_claim =
      repository.claim_owner_lease ("successor-descriptor-owner", 30s).result ().value ();
    const auto *successor = std::get_if<owner_lease_claimed_t> (&successor_claim);
    ASSERT_NE (successor, nullptr);
    ASSERT_GT (successor->token.lease_generation, expired_owner->token.lease_generation);
    EXPECT_EQ (repository
                 .update_mesh_node (descriptor ("expired", successor->token),
                                    location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    const auto live_claim =
      repository.claim_owner_lease ("live-descriptor-owner", 30s).result ().value ();
    const auto *live_owner = std::get_if<owner_lease_claimed_t> (&live_claim);
    ASSERT_NE (live_owner, nullptr);
    ASSERT_EQ (repository
                 .update_mesh_node (descriptor ("live", live_owner->token),
                                    location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    const auto contender_claim =
      repository.claim_owner_lease ("contending-descriptor-owner", 30s).result ().value ();
    const auto *contender = std::get_if<owner_lease_claimed_t> (&contender_claim);
    ASSERT_NE (contender, nullptr);
    EXPECT_EQ (repository
                 .update_mesh_node (descriptor ("live", contender->token),
                                    location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::rejected_conflict);
}

TEST (CppFrameworkOpaqueLocationStore, ExpiredOwnerLeaseReclaimsReservedAuthority)
{
    in_memory_location_store_t provider;
    provider_location_repository_t source (provider);
    const auto source_claim = source.claim_owner_lease ("expired-source", 30s).result ().value ();
    const auto target_claim = source.claim_owner_lease ("expired-target", 30s).result ().value ();
    const auto *source_owner = std::get_if<owner_lease_claimed_t> (&source_claim);
    const auto *target_owner = std::get_if<owner_lease_claimed_t> (&target_claim);
    ASSERT_NE (source_owner, nullptr);
    ASSERT_NE (target_owner, nullptr);

    const auto descriptor = [] (std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t value;
        value.mesh_name = "expired-mesh";
        value.rid = zlink::routing_id_t::from (rid);
        value.lifecycle_generation = 1;
        value.descriptor_revision = 1;
        value.endpoint = "tcp://127.0.0.1:7001";
        value.owner_id = owner.owner_id;
        value.lease_generation = owner.lease_generation;
        value.object_role = object_role_t::server;
        value.state = framework_runtime_state_t::serving;
        value.object_capabilities = {
          {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0},
          {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::recreate, false,
           0}};
        value.capacity.actors.limit = 1;
        value.capacity.spots.limit = 1;
        value.capacity.spot_types.push_back (
          {placement_object_kind_t::user_spot, "room", {0, 0, 1}});
        return value;
    };
    const auto source_descriptor = descriptor ("expired-source-node", source_owner->token);
    const auto target_descriptor = descriptor ("expired-target-node", target_owner->token);
    ASSERT_EQ (source.update_mesh_node (source_descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    ASSERT_EQ (source.update_mesh_node (target_descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    owner_lease_time_store_t expired (provider, source_owner->token.owner_id,
                                      owner_lease_time_store_t::lease_view_t::expired);
    provider_location_repository_t replacement (expired);
    for (const auto kind : {placement_object_kind_t::actor, placement_object_kind_t::user_spot}) {
        const auto stable_type = kind == placement_object_kind_t::actor ? "player" : "room";
        object_reserve_request_t request;
        request.key = {kind,
                       kind == placement_object_kind_t::actor ? "expired-actor" : "expired-spot"};
        request.intent.stable_type = stable_type;
        request.target = {"expired-mesh", node_rid_t::from_string ("expired-source-node"), 1,
                          source_owner->token};
        request.creating_payload = bytes ("creating");
        request.capacity_bundle =
          kind == placement_object_kind_t::actor
            ? placement_capacity_bundle_t{.actor_slots = 1}
            : placement_capacity_bundle_t{
                .spot_slots = 1, .spot_type = spot_type_capacity_delta_t{kind, stable_type, 1}};
        const auto original = source.reserve (request).result ().value ();
        ASSERT_NE (std::get_if<object_reserved_t> (&original), nullptr);

        request.target = {"expired-mesh", node_rid_t::from_string ("expired-target-node"), 1,
                          target_owner->token};
        const auto reserved = replacement.reserve (request).result ().value ();
        const auto *reclaimed = std::get_if<object_reserved_t> (&reserved);
        ASSERT_NE (reclaimed, nullptr);
        EXPECT_EQ (reclaimed->creating.owner.owner_id, target_owner->token.owner_id);
        EXPECT_GT (reclaimed->creating.object_generation, 1u);
    }
}

TEST (CppFrameworkOpaqueLocationStore, LiveOwnerLeaseProtectsReservedAuthority)
{
    in_memory_location_store_t provider;
    provider_location_repository_t source (provider);
    const auto source_claim = source.claim_owner_lease ("live-source", 30s).result ().value ();
    const auto target_claim = source.claim_owner_lease ("live-target", 30s).result ().value ();
    const auto *source_owner = std::get_if<owner_lease_claimed_t> (&source_claim);
    const auto *target_owner = std::get_if<owner_lease_claimed_t> (&target_claim);
    ASSERT_NE (source_owner, nullptr);
    ASSERT_NE (target_owner, nullptr);

    const auto publish = [&] (std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "live-mesh";
        descriptor.rid = zlink::routing_id_t::from (rid);
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                                   maintenance_policy_kind_t::recreate, false, 0});
        descriptor.capacity.actors.limit = 1;
        ASSERT_EQ (source.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
    };
    publish ("live-source-node", source_owner->token);
    publish ("live-target-node", target_owner->token);

    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "live-actor"};
    request.intent.stable_type = "player";
    request.target = {"live-mesh", node_rid_t::from_string ("live-source-node"), 1,
                      source_owner->token};
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;
    const auto first = source.reserve (request).result ().value ();
    const auto *original = std::get_if<object_reserved_t> (&first);
    ASSERT_NE (original, nullptr);

    owner_lease_time_store_t live (provider, source_owner->token.owner_id,
                                   owner_lease_time_store_t::lease_view_t::live);
    provider_location_repository_t replacement (live);
    request.target = {"live-mesh", node_rid_t::from_string ("live-target-node"), 1,
                      target_owner->token};
    EXPECT_TRUE (std::holds_alternative<object_reserve_conflict_t> (
      replacement.reserve (request).result ().value ()));
    const auto current = std::get<authority_snapshot_t> (
      replacement.read_authority (actor_authority_key ("live-actor")).result ().value ());
    EXPECT_EQ (current.store_version, original->creating.store_version);
    EXPECT_EQ (current.owner.owner_id, source_owner->token.owner_id);
}

TEST (CppFrameworkOpaqueLocationStore, MissingOwnerLeaseExpiryFailsClosedDuringReclaim)
{
    in_memory_location_store_t provider;
    provider_location_repository_t source (provider);
    const auto claim = source.claim_owner_lease ("corrupt-owner", 30s).result ().value ();
    const auto *owner = std::get_if<owner_lease_claimed_t> (&claim);
    ASSERT_NE (owner, nullptr);

    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "corrupt-mesh";
    descriptor.rid = zlink::routing_id_t::from ("corrupt-node");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner->token.owner_id;
    descriptor.lease_generation = owner->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0});
    descriptor.capacity.actors.limit = 1;
    ASSERT_EQ (source.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "corrupt-actor"};
    request.intent.stable_type = "player";
    request.target = {"corrupt-mesh", node_rid_t::from_string ("corrupt-node"), 1, owner->token};
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;
    const auto reserved = source.reserve (request).result ().value ();
    ASSERT_NE (std::get_if<object_reserved_t> (&reserved), nullptr);

    owner_lease_time_store_t corrupt (provider, owner->token.owner_id,
                                      owner_lease_time_store_t::lease_view_t::missing_expiry);
    provider_location_repository_t reopened (corrupt);
    const auto failure = reopened.reserve (request).result ();
    ASSERT_FALSE (failure);
    EXPECT_EQ (failure.error_kind (), framework_error_kind_t::internal_failure);
    ASSERT_NE (failure.error (), nullptr);
    EXPECT_STREQ (failure.error ()->what (), "Location Store owner lease record is invalid");
}

TEST (CppFrameworkOpaqueLocationStore, AbortedReservationCanBeReservedAgainThroughProvider)
{
    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
    const auto claim = repository.claim_owner_lease ("owner-retry", 30s).result ().value ();
    const auto *claimed = std::get_if<owner_lease_claimed_t> (&claim);
    ASSERT_NE (claimed, nullptr);

    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "retry";
    descriptor.rid = zlink::routing_id_t::from (std::string{"node-retry"});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = claimed->token.owner_id;
    descriptor.lease_generation = claimed->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0});
    descriptor.capacity.actors.limit = 1;
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "actor-retry"};
    request.intent.stable_type = "player";
    request.target = {"retry", node_rid_t::from_string ("node-retry"), 1, claimed->token};
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;

    const auto first = repository.reserve (request).result ().value ();
    const auto *first_reservation = std::get_if<object_reserved_t> (&first);
    ASSERT_NE (first_reservation, nullptr);
    ASSERT_TRUE (std::holds_alternative<object_aborted_t> (
      repository.abort ({request.key, first_reservation->fence}).result ().value ()));

    const auto second = repository.reserve (request).result ().value ();
    const auto *second_reservation = std::get_if<object_reserved_t> (&second);
    ASSERT_NE (second_reservation, nullptr);
    EXPECT_NE (second_reservation->fence.reservation_id, first_reservation->fence.reservation_id);
    const auto capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("actorsPending"), 1);
    EXPECT_EQ (capacity.at ("actorsActive"), 0);

    EXPECT_TRUE (std::holds_alternative<object_abort_stale_t> (
      repository.abort ({request.key, first_reservation->fence}).result ().value ()));
    EXPECT_TRUE (std::holds_alternative<object_aborted_t> (
      repository.abort ({request.key, second_reservation->fence}).result ().value ()));
    EXPECT_EQ (capacity_record (provider, descriptor).at ("actorsPending"), 0);
}

TEST (CppFrameworkOpaqueLocationStore, AggregatePrepareAdoptsPeerLockAfterConditionalWriteConflict)
{
    aggregate_lock_contention_store_t provider;
    provider_location_repository_t repository (provider);
    const auto source_claim =
      repository.claim_owner_lease ("aggregate-race-source-owner", 30s).result ().value ();
    const auto *source_owner = std::get_if<owner_lease_claimed_t> (&source_claim);
    ASSERT_NE (source_owner, nullptr);
    const auto target_claim =
      repository.claim_owner_lease ("aggregate-race-target-owner", 30s).result ().value ();
    const auto *target_owner = std::get_if<owner_lease_claimed_t> (&target_claim);
    ASSERT_NE (target_owner, nullptr);

    mesh_node_descriptor_t source_descriptor;
    source_descriptor.mesh_name = "aggregate-race";
    source_descriptor.rid = zlink::routing_id_t::from (std::string{"aggregate-race-source"});
    source_descriptor.lifecycle_generation = 1;
    source_descriptor.descriptor_revision = 1;
    source_descriptor.endpoint = "tcp://127.0.0.1:7001";
    source_descriptor.owner_id = source_owner->token.owner_id;
    source_descriptor.lease_generation = source_owner->token.lease_generation;
    source_descriptor.object_role = object_role_t::server;
    source_descriptor.state = framework_runtime_state_t::serving;
    source_descriptor.object_capabilities = {
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0},
      {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::snapshot, true, 1}};
    source_descriptor.capacity.actors.limit = 1;
    source_descriptor.capacity.spots.limit = 1;
    source_descriptor.capacity.spot_types.push_back (
      {placement_object_kind_t::user_spot, "room", {0, 0, 1}});
    ASSERT_EQ (repository.update_mesh_node (source_descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    auto target_descriptor = source_descriptor;
    target_descriptor.rid = zlink::routing_id_t::from (std::string{"aggregate-race-target"});
    target_descriptor.endpoint = "tcp://127.0.0.1:7002";
    target_descriptor.owner_id = target_owner->token.owner_id;
    target_descriptor.lease_generation = target_owner->token.lease_generation;
    ASSERT_EQ (repository.update_mesh_node (target_descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    const object_creation_target_t source_target{source_descriptor.mesh_name,
                                                 node_rid_t::from_string ("aggregate-race-source"),
                                                 1, source_owner->token};
    object_reserve_request_t actor_request;
    actor_request.key = {placement_object_kind_t::actor, "aggregate-race-actor"};
    actor_request.intent.stable_type = "player";
    actor_request.target = source_target;
    actor_request.creating_payload = bytes ("creating");
    actor_request.capacity_bundle.actor_slots = 1;
    const auto actor_reserved = repository.reserve (actor_request).result ().value ();
    const auto *actor_fence = std::get_if<object_reserved_t> (&actor_reserved);
    ASSERT_NE (actor_fence, nullptr);
    const auto actor_committed =
      repository.commit ({actor_request.key, actor_fence->fence, bytes ("ready")})
        .result ()
        .value ();
    const auto *actor = std::get_if<object_committed_t> (&actor_committed);
    ASSERT_NE (actor, nullptr);

    object_reserve_request_t spot_request;
    spot_request.key = {placement_object_kind_t::user_spot, "aggregate-race-spot"};
    spot_request.intent.stable_type = "room";
    spot_request.target = source_target;
    spot_request.creating_payload = bytes ("creating");
    spot_request.capacity_bundle.spot_slots = 1;
    spot_request.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    const auto spot_reserved = repository.reserve (spot_request).result ().value ();
    const auto *spot_fence = std::get_if<object_reserved_t> (&spot_reserved);
    ASSERT_NE (spot_fence, nullptr);
    const auto spot_committed =
      repository.commit ({spot_request.key, spot_fence->fence, bytes ("ready")}).result ().value ();
    const auto *spot = std::get_if<object_committed_t> (&spot_committed);
    ASSERT_NE (spot, nullptr);

    const auto source_capacity = capacity_record (provider, source_descriptor);
    EXPECT_EQ (source_capacity.at ("actorsActive"), 1);
    EXPECT_EQ (source_capacity.at ("spotsActive"), 1);
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (
      provider.read (capacity_key (target_descriptor)).result ().value ()));

    aggregate_prepare_request_t aggregate;
    aggregate.aggregate_id.value[15] = std::byte{0x44};
    aggregate.aggregate_generation = 1;
    aggregate.participants = {{actor_authority_key (actor_request.key.global_id),
                               actor->ready.store_version,
                               authority_generation_transition_t::new_owner,
                               bytes ("aggregate-actor"),
                               {}},
                              {spot_authority_key (spot_request.key.global_id),
                               spot->ready.store_version,
                               authority_generation_transition_t::new_owner,
                               bytes ("aggregate-spot"),
                               {}}};
    aggregate.target_descriptor = {target_descriptor.mesh_name, target_descriptor.rid};
    aggregate.target_descriptor_lifecycle_generation = 1;
    aggregate.capacity_bundle.actor_slots = 1;
    aggregate.capacity_bundle.spot_slots = 1;
    aggregate.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    aggregate.target_owner = target_owner->token;

    const auto prepared = repository.prepare_aggregate (aggregate).result ().value ();
    EXPECT_TRUE (provider.peer_marker_published);
    ASSERT_TRUE (std::holds_alternative<aggregate_prepared_t> (prepared));
    for (const auto &mutation : std::vector<authority_mutation_t>{
           authority_put_t{}, authority_reincarnate_t{}, authority_delete_t{}})
        EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (
          repository
            .compare_exchange_authority (actor_authority_key (actor_request.key.global_id),
                                         actor->ready.store_version, mutation)
            .result ()
            .value ()));
    EXPECT_EQ (source_capacity, capacity_record (provider, source_descriptor));
}

TEST (CppFrameworkOpaqueLocationStore, AggregateCommitRechecksFenceAfterTransientConflicts)
{
    using phase_t = aggregate_commit_contention_store_t::phase_t;
    for (const auto phase : {phase_t::transition, phase_t::page, phase_t::terminal,
                             phase_t::lease_loss, phase_t::counter_race}) {
        SCOPED_TRACE (static_cast<int> (phase));
        aggregate_commit_contention_store_t provider;
        provider_location_repository_t repository (provider);
        const auto claim =
          repository.claim_owner_lease ("aggregate-contention-owner", 30s).result ().value ();
        const auto *claimed = std::get_if<owner_lease_claimed_t> (&claim);
        ASSERT_NE (claimed, nullptr);
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "aggregate-contention";
        descriptor.rid = zlink::routing_id_t::from (std::string{"aggregate-contention-node"});
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = claimed->token.owner_id;
        descriptor.lease_generation = claimed->token.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                                   maintenance_policy_kind_t::recreate, false, 0});
        descriptor.object_capabilities.push_back ({placement_object_kind_t::user_spot, "room",
                                                   maintenance_policy_kind_t::snapshot, true, 10});
        descriptor.capacity.actors.limit = 10;
        descriptor.capacity.spots.limit = 10;
        descriptor.capacity.spot_types.push_back (
          {placement_object_kind_t::user_spot, "room", {0, 0, 10}});
        ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
        object_creation_target_t target{descriptor.mesh_name,
                                        node_rid_t::from_string ("aggregate-contention-node"), 1,
                                        claimed->token};
        aggregate_prepare_request_t aggregate;
        aggregate.aggregate_id.value[15] = std::byte{0x55};
        aggregate.aggregate_generation = 1;
        aggregate.target_descriptor = {descriptor.mesh_name, descriptor.rid};
        aggregate.target_descriptor_lifecycle_generation = 1;
        aggregate.target_owner = claimed->token;
        aggregate.capacity_bundle.actor_slots = 2;
        aggregate.capacity_bundle.spot_slots = 1;
        aggregate.capacity_bundle.spot_type =
          spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
        for (const auto &[kind, id] :
             {std::pair{placement_object_kind_t::actor, "aggregate-contention-actor-a"},
              std::pair{placement_object_kind_t::actor, "aggregate-contention-actor-b"},
              std::pair{placement_object_kind_t::user_spot, "aggregate-contention-spot"}}) {
            object_reserve_request_t request;
            request.key = {kind, id};
            request.intent.stable_type = kind == placement_object_kind_t::actor ? "player" : "room";
            request.target = target;
            request.creating_payload = bytes ("creating");
            if (kind == placement_object_kind_t::actor) {
                request.capacity_bundle.actor_slots = 1;
            } else {
                request.capacity_bundle.spot_slots = 1;
                request.capacity_bundle.spot_type = spot_type_capacity_delta_t{kind, "room", 1};
            }
            const auto reserved = repository.reserve (request).result ().value ();
            const auto *reservation = std::get_if<object_reserved_t> (&reserved);
            ASSERT_NE (reservation, nullptr);
            const auto committed =
              repository.commit ({request.key, reservation->fence, bytes ("ready")})
                .result ()
                .value ();
            const auto *ready = std::get_if<object_committed_t> (&committed);
            ASSERT_NE (ready, nullptr);
            aggregate.participants.push_back ({kind == placement_object_kind_t::actor
                                                 ? actor_authority_key (id)
                                                 : spot_authority_key (id),
                                               ready->ready.store_version,
                                               authority_generation_transition_t::new_owner,
                                               bytes ("aggregate-ready"),
                                               {}});
        }
        const auto prepared = repository.prepare_aggregate (aggregate).result ().value ();
        const auto *fence = std::get_if<aggregate_prepared_t> (&prepared);
        ASSERT_NE (fence, nullptr);
        provider.phase = phase;
        provider.remaining = phase == phase_t::counter_race ? 0 : 65;
        if (phase == phase_t::counter_race) {
            provider.on_counter_read = [&] () -> task_t<void> {
                object_reserve_request_t peer;
                peer.key = {placement_object_kind_t::actor, "aggregate-contention-peer"};
                peer.intent.stable_type = "player";
                peer.target = target;
                peer.creating_payload = bytes ("peer-creating");
                peer.capacity_bundle.actor_slots = 1;
                const auto reserved = co_await repository.reserve (peer);
                EXPECT_TRUE (std::holds_alternative<object_reserved_t> (reserved));
            };
        }
        if (phase == phase_t::lease_loss) {
            provider.on_conflict = [&] () -> task_t<void> {
                const auto released = co_await repository.release_owner_lease (claimed->token);
                EXPECT_TRUE (std::holds_alternative<owner_lease_released_t> (released));
            };
            EXPECT_EQ (repository.commit_aggregate (fence->fence).result ().value (),
                       aggregate_commit_result_t::stale);
            EXPECT_EQ (provider.rejected, 1u);
            provider.remaining = 0;
            EXPECT_EQ (repository.abort_aggregate (fence->fence).result ().value (),
                       aggregate_abort_result_t::aborted);
        } else {
            EXPECT_EQ (repository.commit_aggregate (fence->fence).result ().value (),
                       aggregate_commit_result_t::committed);
            EXPECT_EQ (provider.rejected, phase == phase_t::counter_race ? 0u : 65u);
            EXPECT_EQ (repository.commit_aggregate (fence->fence).result ().value (),
                       aggregate_commit_result_t::already_committed);
        }
    }
}

TEST (CppFrameworkOpaqueLocationStore, PrivateRepositoryPersistsAuthorityLifecycleThroughProvider)
{
    creation_terminal_failure_store_t provider;
    provider_location_repository_t repository (provider);
    const auto claim = repository.claim_owner_lease ("owner-a", 30s).result ().value ();
    const auto *claimed = std::get_if<owner_lease_claimed_t> (&claim);
    ASSERT_NE (claimed, nullptr);

    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "play";
    descriptor.rid = zlink::routing_id_t::from (std::string{"node-7"});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = claimed->token.owner_id;
    descriptor.lease_generation = claimed->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0});
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::snapshot, true, 10});
    descriptor.capacity.actors.limit = 10;
    descriptor.capacity.spots.limit = 10;
    descriptor.capacity.spot_types.push_back (
      {placement_object_kind_t::user_spot, "room", {0, 0, 10}});
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    object_creation_target_t target{"play", node_rid_t::from_string ("node-7"), 1, claimed->token};
    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "actor-1"};
    request.intent.stable_type = "player";
    request.target = target;
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;
    const auto reserved = repository.reserve (request).result ().value ();
    const auto *reservation = std::get_if<object_reserved_t> (&reserved);
    ASSERT_NE (reservation, nullptr);
    // Missing canonical counter rows bootstrap at issue 1 and are stored as
    // bare next-to-issue decimals in the same reserve batch.
    EXPECT_EQ (
      std::get<store_found_t> (provider.read (object_generation_counter_key).result ().value ())
        .value.bytes,
      bytes ("2"));
    EXPECT_EQ (std::get<store_found_t> (
                 provider.read (authority_owner_generation_counter_key).result ().value ())
                 .value.bytes,
               bytes ("2"));
    // store_version is the provider's own opaque per-key version
    // (checklist C-4d), not a per-record counter reset to "1" -- assert it
    // round-trips to a live re-read instead of pinning a literal that
    // depends on how many other keys this store instance already wrote.
    EXPECT_FALSE (reservation->creating.store_version.empty ());
    EXPECT_EQ (reservation->fence.expected_store_version, reservation->creating.store_version);
    auto nodes = repository.list_mesh_nodes ("play").result ().value ();
    ASSERT_EQ (nodes.items.size (), 1u);
    auto capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("actorsPending"), 1);
    EXPECT_EQ (capacity.at ("actorsActive"), 0);
    EXPECT_EQ (nodes.items.front ().capacity.actors.reserved, 0u);
    EXPECT_EQ (nodes.items.front ().capacity.actors.active, 0u);

    const auto ready =
      repository.commit ({request.key, reservation->fence, bytes ("ready")}).result ().value ();
    const auto *committed = std::get_if<object_committed_t> (&ready);
    ASSERT_NE (committed, nullptr);
    EXPECT_EQ (committed->ready.payload, bytes ("ready"));
    EXPECT_EQ (committed->ready.allocation.state, placement_allocation_state_t::active);
    nodes = repository.list_mesh_nodes ("play").result ().value ();
    ASSERT_EQ (nodes.items.size (), 1u);
    capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("actorsPending"), 0);
    EXPECT_EQ (capacity.at ("actorsActive"), 1);

    provider_location_repository_t reopened (provider);
    const auto actor_key = actor_authority_key ("actor-1");
    const auto spot_key = spot_authority_key ("spot-1");
    const auto found = reopened.read_authority (actor_key).result ().value ();
    const auto *snapshot = std::get_if<authority_snapshot_t> (&found);
    ASSERT_NE (snapshot, nullptr);
    EXPECT_EQ (snapshot->payload, bytes ("ready"));

    const auto restored =
      reopened
        .compare_exchange_authority (actor_key, snapshot->store_version,
                                     authority_restore_t{bytes ("restored"), claimed->token})
        .result ()
        .value ();
    const auto *stored = std::get_if<authority_stored_t> (&restored);
    ASSERT_NE (stored, nullptr);
    EXPECT_EQ (stored->snapshot.payload, bytes ("restored"));

    const auto moved = reopened
                         .compare_exchange_authority (actor_key, stored->snapshot.store_version,
                                                      authority_retarget_t{bytes ("moved"), target})
                         .result ()
                         .value ();
    const auto *moved_authority = std::get_if<authority_stored_t> (&moved);
    ASSERT_NE (moved_authority, nullptr);
    EXPECT_EQ (moved_authority->snapshot.payload, bytes ("moved"));
    capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("actorsPending"), 0);
    EXPECT_EQ (capacity.at ("actorsActive"), 1);

    object_reserve_request_t spot_request;
    spot_request.key = {placement_object_kind_t::user_spot, "spot-1"};
    spot_request.intent.stable_type = "room";
    spot_request.target = target;
    spot_request.creating_payload = bytes ("spot-creating");
    spot_request.capacity_bundle.spot_slots = 1;
    spot_request.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    const auto spot_reserved = reopened.reserve (spot_request).result ().value ();
    const auto *spot_reservation = std::get_if<object_reserved_t> (&spot_reserved);
    ASSERT_NE (spot_reservation, nullptr);
    const auto spot_committed =
      reopened.commit ({spot_request.key, spot_reservation->fence, bytes ("spot-ready")})
        .result ()
        .value ();
    const auto *spot_ready = std::get_if<object_committed_t> (&spot_committed);
    ASSERT_NE (spot_ready, nullptr);

    aggregate_prepare_request_t aggregate;
    aggregate.aggregate_id.value[15] = std::byte{1};
    aggregate.aggregate_generation = 1;
    aggregate.participants = {{actor_key,
                               moved_authority->snapshot.store_version,
                               authority_generation_transition_t::new_owner,
                               bytes ("actor-aggregate"),
                               {}},
                              {spot_key,
                               spot_ready->ready.store_version,
                               authority_generation_transition_t::new_owner,
                               bytes ("spot-aggregate"),
                               {}}};
    aggregate.target_descriptor = {"play", descriptor.rid};
    aggregate.target_descriptor_lifecycle_generation = 1;
    aggregate.capacity_bundle.actor_slots = 1;
    aggregate.capacity_bundle.spot_slots = 1;
    aggregate.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    aggregate.target_owner = claimed->token;
    const auto prepared = reopened.prepare_aggregate (aggregate).result ().value ();
    const auto *aggregate_fence = std::get_if<aggregate_prepared_t> (&prepared);
    ASSERT_NE (aggregate_fence, nullptr);
    provider.conflicting_capacity_key = capacity_key (descriptor);
    provider.fault = creation_terminal_failure_store_t::fault_t::capacity_conflict_once;
    EXPECT_EQ (reopened.commit_aggregate (aggregate_fence->fence).result ().value (),
               aggregate_commit_result_t::committed);
    // The two-participant aggregate issues 4 and 5 after reserve/retarget/
    // reserve, then stores the next-to-issue value 6 in one transition batch.
    EXPECT_EQ (std::get<store_found_t> (
                 provider.read (authority_owner_generation_counter_key).result ().value ())
                 .value.bytes,
               bytes ("6"));
    const auto aggregated_actor = reopened.read_authority (actor_key).result ().value ();
    ASSERT_TRUE (std::holds_alternative<authority_snapshot_t> (aggregated_actor));
    EXPECT_EQ (std::get<authority_snapshot_t> (aggregated_actor).payload,
               bytes ("actor-aggregate"));

    const auto page = reopened.list_authorities ("zla1:a:", std::nullopt, 10).result ().value ();
    const auto *items = std::get_if<authority_page_t> (&page);
    ASSERT_NE (items, nullptr);
    ASSERT_EQ (items->items.size (), 1u);
    EXPECT_EQ (items->items.front ().key.value, actor_key.value);

    const auto current_actor =
      std::get<authority_snapshot_t> (reopened.read_authority (actor_key).result ().value ());
    const auto current_spot =
      std::get<authority_snapshot_t> (reopened.read_authority (spot_key).result ().value ());
    aggregate_prepare_request_t fenced_aggregate;
    fenced_aggregate.aggregate_id.value[15] = std::byte{3};
    fenced_aggregate.aggregate_generation = 1;
    fenced_aggregate.participants = {{actor_key,
                                      current_actor.store_version,
                                      authority_generation_transition_t::new_owner,
                                      bytes ("fenced-actor"),
                                      {}},
                                     {spot_key,
                                      current_spot.store_version,
                                      authority_generation_transition_t::new_owner,
                                      bytes ("fenced-spot"),
                                      {}}};
    fenced_aggregate.inventory_digest = {};
    fenced_aggregate.target_descriptor = {descriptor.mesh_name, descriptor.rid};
    fenced_aggregate.target_descriptor_lifecycle_generation = 1;
    fenced_aggregate.capacity_bundle.actor_slots = 1;
    fenced_aggregate.capacity_bundle.spot_slots = 1;
    fenced_aggregate.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    fenced_aggregate.target_owner = claimed->token;
    auto unsupported_membership = fenced_aggregate;
    unsupported_membership.aggregate_id.value[14] = std::byte{0x33};
    unsupported_membership.participants.front ().membership_mutation = {std::byte{0x01}};
    EXPECT_TRUE (std::holds_alternative<aggregate_prepare_conflict_t> (
      reopened.prepare_aggregate (unsupported_membership).result ().value ()));
    const auto fenced_prepared = reopened.prepare_aggregate (fenced_aggregate).result ().value ();
    const auto *fenced = std::get_if<aggregate_prepared_t> (&fenced_prepared);
    ASSERT_NE (fenced, nullptr);
    capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("actorsPending"), 1);
    EXPECT_EQ (capacity.at ("spotsPending"), 1);
    EXPECT_EQ (reopened.commit_aggregate (fenced->fence).result ().value (),
               aggregate_commit_result_t::committed);
    capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("actorsPending"), 0);
    EXPECT_EQ (capacity.at ("spotsPending"), 0);

    const auto abort_actor =
      std::get<authority_snapshot_t> (reopened.read_authority (actor_key).result ().value ());
    const auto abort_spot =
      std::get<authority_snapshot_t> (reopened.read_authority (spot_key).result ().value ());
    aggregate_prepare_request_t abort_aggregate = fenced_aggregate;
    abort_aggregate.aggregate_id.value[15] = std::byte{4};
    abort_aggregate.participants[0].expected_store_version = abort_actor.store_version;
    abort_aggregate.participants[0].authority_payload = bytes ("abort-actor");
    abort_aggregate.participants[1].expected_store_version = abort_spot.store_version;
    abort_aggregate.participants[1].authority_payload = bytes ("abort-spot");
    const auto abort_prepared = reopened.prepare_aggregate (abort_aggregate).result ().value ();
    const auto *abort_fence = std::get_if<aggregate_prepared_t> (&abort_prepared);
    ASSERT_NE (abort_fence, nullptr);
    EXPECT_EQ (reopened.abort_aggregate (abort_fence->fence).result ().value (),
               aggregate_abort_result_t::aborted);
    capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("actorsPending"), 0);
    EXPECT_EQ (capacity.at ("spotsPending"), 0);

    // A committed creation keeps its reservation record until the authority
    // is deleted. Deletion must derive that record from the encoded authority
    // key so the same global actor ID can be created again.
    const auto deleted_actor = reopened.read_authority (actor_key).result ().value ();
    const auto *deleted_snapshot = std::get_if<authority_snapshot_t> (&deleted_actor);
    ASSERT_NE (deleted_snapshot, nullptr);
    const auto deleted = reopened
                           .compare_exchange_authority (actor_key, deleted_snapshot->store_version,
                                                        authority_delete_t{})
                           .result ()
                           .value ();
    ASSERT_TRUE (std::holds_alternative<authority_deleted_t> (deleted));
    const auto recreated = reopened.reserve (request).result ().value ();
    const auto *recreated_reservation = std::get_if<object_reserved_t> (&recreated);
    ASSERT_NE (recreated_reservation, nullptr);
    const auto recreated_commit =
      reopened.commit ({request.key, recreated_reservation->fence, bytes ("recreated")})
        .result ()
        .value ();
    ASSERT_TRUE (std::holds_alternative<object_committed_t> (recreated_commit));

    // A stored INT64_MAX is exhausted before an aggregate record transition;
    // neither its counter bytes nor the gated authority record may change.
    const auto exhausted_actor =
      std::get<authority_snapshot_t> (reopened.read_authority (actor_key).result ().value ());
    const auto exhausted_spot =
      std::get<authority_snapshot_t> (reopened.read_authority (spot_key).result ().value ());
    auto exhausted_aggregate = abort_aggregate;
    exhausted_aggregate.aggregate_id.value[15] = std::byte{5};
    exhausted_aggregate.participants[0].expected_store_version = exhausted_actor.store_version;
    exhausted_aggregate.participants[1].expected_store_version = exhausted_spot.store_version;
    const auto exhausted_prepared =
      reopened.prepare_aggregate (exhausted_aggregate).result ().value ();
    const auto *exhausted_fence = std::get_if<aggregate_prepared_t> (&exhausted_prepared);
    ASSERT_NE (exhausted_fence, nullptr);
    const auto &authority_counter_key = authority_owner_generation_counter_key;
    const auto counter_before =
      std::get<store_found_t> (provider.read (authority_counter_key).result ().value ());
    const auto maximum = std::to_string (std::numeric_limits<std::int64_t>::max ());
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider
        .write ({.conditions = {store_version_condition_t{authority_counter_key,
                                                          counter_before.value.version}},
                 .mutations = {store_put_t{authority_counter_key, bytes (maximum), std::nullopt}}})
        .result ()
        .value ()));
    EXPECT_EQ (reopened.commit_aggregate (exhausted_fence->fence).result ().value (),
               aggregate_commit_result_t::generation_exhausted);
    EXPECT_EQ (std::get<store_found_t> (provider.read (authority_counter_key).result ().value ())
                 .value.bytes,
               bytes (maximum));
    EXPECT_EQ (
      std::get<authority_snapshot_t> (reopened.read_authority (actor_key).result ().value ())
        .store_version,
      exhausted_actor.store_version);
}

class counter_conflict_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        if (conflict_key) {
            // Awaited: the repository may resume on the inner store's lane.
            const auto key = std::exchange (conflict_key, std::nullopt).value ();
            const auto current = std::get<store_found_t> (co_await inner.read (key));
            store_write_request_t concurrent{{},
                                             {store_put_t{key, current.value.bytes, std::nullopt}}};
            (void) co_await inner.write (std::move (concurrent));
        }
        if (remaining_conflicts != 0) {
            --remaining_conflicts;
            ++rejected;
            if (on_conflict) {
                auto action = std::move (on_conflict);
                co_await action ();
            }
            co_return store_write_result_t{
              store_write_conflict_t{std::chrono::system_clock::now ()}};
        }
        co_return co_await inner.write (std::move (request));
    }

    in_memory_location_store_t inner;
    std::optional<store_key_t> conflict_key;
    std::size_t remaining_conflicts = 0;
    std::size_t rejected = 0;
    std::function<task_t<void> ()> on_conflict;
};

TEST (CppFrameworkOpaqueLocationStore, EveryAuthorityMutationRechecksConflictQualification)
{
    for (int kind = 0; kind != 5; ++kind) {
        for (int loss = 0; loss != 3; ++loss) {
            SCOPED_TRACE (kind);
            SCOPED_TRACE (loss);
            counter_conflict_store_t provider;
            provider_location_repository_t repository (provider);
            const auto owner =
              std::get<owner_lease_claimed_t> (
                repository.claim_owner_lease ("mutation-owner", 30s).result ().value ())
                .token;
            mesh_node_descriptor_t descriptor;
            descriptor.mesh_name = "mutation-mesh";
            descriptor.rid = zlink::routing_id_t::from (std::string{"mutation-node"});
            descriptor.lifecycle_generation = 1;
            descriptor.descriptor_revision = 1;
            descriptor.endpoint = "tcp://127.0.0.1:7001";
            descriptor.owner_id = owner.owner_id;
            descriptor.lease_generation = owner.lease_generation;
            descriptor.object_role = object_role_t::server;
            descriptor.state = framework_runtime_state_t::serving;
            descriptor.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                                       maintenance_policy_kind_t::recreate, false,
                                                       0});
            descriptor.capacity.actors.limit = 2;
            ASSERT_EQ (location_write_status_t::stored,
                       repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                         .result ()
                         .value ()
                         .status);
            object_reserve_request_t request;
            request.key = {placement_object_kind_t::actor, "mutation-actor"};
            request.intent.stable_type = "player";
            request.target = {descriptor.mesh_name, node_rid_t::from_string ("mutation-node"), 1,
                              owner};
            request.capacity_bundle.actor_slots = 1;
            const auto reserved =
              std::get<object_reserved_t> (repository.reserve (request).result ().value ());
            const auto ready = std::get<object_committed_t> (
                                 repository.commit ({request.key, reserved.fence, bytes ("ready")})
                                   .result ()
                                   .value ())
                                 .ready;
            const auto key = actor_authority_key (request.key.global_id);
            const auto capacity_before = capacity_record (provider.inner, descriptor);
            const std::vector<authority_mutation_t> mutations{
              authority_put_t{bytes ("changed")}, authority_reincarnate_t{bytes ("changed")},
              authority_restore_t{bytes ("changed"), owner},
              authority_retarget_t{bytes ("changed"), request.target}, authority_delete_t{}};
            provider.remaining_conflicts = loss == 0 ? 65 : 1;
            provider.on_conflict = [&] () -> task_t<void> {
                if (loss == 2) {
                    EXPECT_TRUE (std::holds_alternative<owner_lease_released_t> (
                      co_await repository.release_owner_lease (owner)));
                } else if (loss == 1) {
                    authority_mutation_t concurrent = authority_put_t{bytes ("ready")};
                    EXPECT_TRUE (std::holds_alternative<authority_stored_t> (
                      co_await repository.compare_exchange_authority (key, ready.store_version,
                                                                      std::move (concurrent))));
                } else {
                    const auto row_key = object_generation_counter_key;
                    const auto row =
                      std::get<store_found_t> (co_await provider.inner.read (row_key));
                    store_write_request_t concurrent{
                      {}, {store_put_t{row_key, row.value.bytes, std::nullopt}}};
                    EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (
                      co_await provider.inner.write (std::move (concurrent))));
                }
            };
            const auto result =
              repository.compare_exchange_authority (key, ready.store_version, mutations[kind])
                .result ()
                .value ();
            if (loss != 0) {
                EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (result));
                EXPECT_EQ (provider.rejected, 1u);
                const auto current = std::get<authority_snapshot_t> (
                  repository.read_authority (key).result ().value ());
                EXPECT_EQ (current.payload, bytes ("ready"));
                EXPECT_EQ (capacity_before, capacity_record (provider.inner, descriptor));
            } else {
                EXPECT_EQ (provider.rejected, 65u);
                EXPECT_EQ (provider.remaining_conflicts, 0u);
                if (kind == 4) {
                    EXPECT_TRUE (std::holds_alternative<authority_deleted_t> (result));
                    EXPECT_TRUE (std::holds_alternative<authority_missing_t> (
                      repository.read_authority (key).result ().value ()));
                } else {
                    const auto *stored = std::get_if<authority_stored_t> (&result);
                    EXPECT_NE (stored, nullptr);
                    if (!stored)
                        continue;
                    EXPECT_EQ (stored->snapshot.payload, bytes ("changed"));
                    EXPECT_NE (stored->snapshot.store_version, ready.store_version);
                    EXPECT_EQ (capacity_before, capacity_record (provider.inner, descriptor));
                }
            }
        }
    }
}

TEST (CppFrameworkOpaqueLocationStore, ReincarnateAtomicallyIssuesPairAndPreservesCapacity)
{
    counter_conflict_store_t conflict_store;
    auto &provider = conflict_store.inner;
    provider_location_repository_t repository (conflict_store);
    const auto owner = std::get<owner_lease_claimed_t> (
                         repository.claim_owner_lease ("reincarnate-owner", 30s).result ().value ())
                         .token;
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "reincarnate-mesh";
    descriptor.rid = zlink::routing_id_t::from (std::string{"reincarnate-node"});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0});
    descriptor.capacity.actors.limit = 1;
    ASSERT_EQ (location_write_status_t::stored,
               repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);
    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "reincarnate-actor"};
    request.intent.stable_type = "player";
    request.target = {descriptor.mesh_name, node_rid_t::from_string ("reincarnate-node"), 1, owner};
    request.capacity_bundle.actor_slots = 1;
    const auto reservation =
      std::get<object_reserved_t> (repository.reserve (request).result ().value ());
    const auto key = actor_authority_key (request.key.global_id);
    const auto &object_counter = object_generation_counter_key;
    const auto &owner_counter = authority_owner_generation_counter_key;
    const auto counter = [&] (const store_key_t &counter_key) {
        return std::get<store_found_t> (provider.read (counter_key).result ().value ()).value;
    };
    const auto object_before = counter (object_counter);
    const auto owner_before = counter (owner_counter);
    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (
      repository
        .compare_exchange_authority (key, reservation.creating.store_version,
                                     authority_reincarnate_t{bytes ("new")})
        .result ()
        .value ()));
    EXPECT_EQ (object_before.version.value, counter (object_counter).version.value);
    EXPECT_EQ (owner_before.version.value, counter (owner_counter).version.value);
    const auto ready =
      std::get<object_committed_t> (
        repository.commit ({request.key, reservation.fence, bytes ("ready")}).result ().value ())
        .ready;
    const auto capacity_before = capacity_record (provider, descriptor);
    auto result = repository
                    .compare_exchange_authority (key, ready.store_version,
                                                 authority_reincarnate_t{bytes ("new")})
                    .result ()
                    .value ();
    auto *stored = std::get_if<authority_stored_t> (&result);
    ASSERT_NE (nullptr, stored);
    EXPECT_GT (stored->snapshot.object_generation, ready.object_generation);
    EXPECT_GT (stored->snapshot.authority_owner_generation, ready.authority_owner_generation);
    EXPECT_EQ (owner.owner_id, stored->snapshot.owner.owner_id);
    EXPECT_EQ (owner.lease_generation, stored->snapshot.owner.lease_generation);
    EXPECT_EQ (ready.allocation.target.node_rid.value (),
               stored->snapshot.allocation.target.node_rid.value ());
    EXPECT_EQ (capacity_before, capacity_record (provider, descriptor));
    EXPECT_EQ (bytes (std::to_string (stored->snapshot.object_generation + 1)),
               counter (object_counter).bytes);
    EXPECT_EQ (bytes (std::to_string (stored->snapshot.authority_owner_generation + 1)),
               counter (owner_counter).bytes);
    auto over_capacity = request;
    over_capacity.key.global_id = "reincarnate-over-capacity";
    EXPECT_TRUE (std::holds_alternative<object_placement_capacity_exhausted_t> (
      repository.reserve (over_capacity).result ().value ()));
    EXPECT_EQ (capacity_before, capacity_record (provider, descriptor));
    for (const auto &mutation :
         std::vector<authority_mutation_t>{authority_reincarnate_t{}, authority_delete_t{}})
        EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (
          repository.compare_exchange_authority (key, ready.store_version, mutation)
            .result ()
            .value ()));

    for (const auto &conflict_key : {object_counter, owner_counter}) {
        conflict_store.conflict_key = conflict_key;
        result = repository
                   .compare_exchange_authority (key, stored->snapshot.store_version,
                                                authority_reincarnate_t{bytes ("counter-conflict")})
                   .result ()
                   .value ();
        stored = std::get_if<authority_stored_t> (&result);
        ASSERT_NE (stored, nullptr);
        EXPECT_FALSE (conflict_store.conflict_key);
        EXPECT_EQ (bytes ("counter-conflict"), stored->snapshot.payload);
        EXPECT_EQ (bytes (std::to_string (stored->snapshot.object_generation + 1)),
                   counter (object_counter).bytes);
        EXPECT_EQ (bytes (std::to_string (stored->snapshot.authority_owner_generation + 1)),
                   counter (owner_counter).bytes);
        EXPECT_EQ (
          stored->snapshot.store_version,
          std::get<authority_snapshot_t> (repository.read_authority (key).result ().value ())
            .store_version);
        EXPECT_EQ (capacity_before, capacity_record (provider, descriptor));
    }
    const auto object_unexhausted = counter (object_counter);
    const auto owner_unexhausted = counter (owner_counter);
    for (const auto &exhausted_key : {object_counter, owner_counter}) {
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          provider
            .write (
              {{},
               {store_put_t{exhausted_key,
                            bytes (std::to_string (std::numeric_limits<std::int64_t>::max ())),
                            std::nullopt}}})
            .result ()
            .value ()));
        const auto object_exhausted = counter (object_counter);
        const auto owner_exhausted = counter (owner_counter);
        EXPECT_TRUE (std::holds_alternative<authority_generation_exhausted_t> (
          repository
            .compare_exchange_authority (key, stored->snapshot.store_version,
                                         authority_reincarnate_t{bytes ("exhausted")})
            .result ()
            .value ()));
        EXPECT_EQ (object_exhausted.version.value, counter (object_counter).version.value);
        EXPECT_EQ (owner_exhausted.version.value, counter (owner_counter).version.value);
        EXPECT_EQ (
          stored->snapshot.store_version,
          std::get<authority_snapshot_t> (repository.read_authority (key).result ().value ())
            .store_version);
        EXPECT_EQ (capacity_before, capacity_record (provider, descriptor));
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          provider
            .write ({{},
                     {store_put_t{object_counter, object_unexhausted.bytes, std::nullopt},
                      store_put_t{owner_counter, owner_unexhausted.bytes, std::nullopt}}})
            .result ()
            .value ()));
    }
    repository.release_owner_lease (owner).result ().value ();
    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (
      repository
        .compare_exchange_authority (key, stored->snapshot.store_version,
                                     authority_reincarnate_t{bytes ("expired")})
        .result ()
        .value ()));
    EXPECT_EQ (object_unexhausted.bytes, counter (object_counter).bytes);
    EXPECT_EQ (owner_unexhausted.bytes, counter (owner_counter).bytes);
    EXPECT_EQ (capacity_before, capacity_record (provider, descriptor));
}

TEST (CppFrameworkOpaqueLocationStore, ReservationLivesOnlyInReservedAuthorityRow)
{
    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
    const auto lease = repository.claim_owner_lease ("single-row-owner", 30s).result ().value ();
    const auto *owner = std::get_if<owner_lease_claimed_t> (&lease);
    ASSERT_NE (owner, nullptr);

    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "single-row-mesh";
    descriptor.rid = zlink::routing_id_t::from (std::string{"single-row-node"});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner->token.owner_id;
    descriptor.lease_generation = owner->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0});
    descriptor.capacity.actors.limit = 1;
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "single-row-actor"};
    request.intent.stable_type = "player";
    request.intent.request_content_reference = "inline-v1:cmVxdWVzdA";
    request.intent.request_sha256 = sha256 (bytes ("request"));
    request.intent.request_encoded_size = 7;
    request.target = {"single-row-mesh", node_rid_t::from_string ("single-row-node"), 1,
                      owner->token};
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;

    const auto reserved = repository.reserve (request).result ().value ();
    const auto *reservation = std::get_if<object_reserved_t> (&reserved);
    ASSERT_NE (reservation, nullptr);
    const auto authority =
      repository.read_authority (actor_authority_key (request.key.global_id)).result ().value ();
    const auto *snapshot = std::get_if<authority_snapshot_t> (&authority);
    ASSERT_NE (snapshot, nullptr);
    EXPECT_EQ (snapshot->allocation.state, placement_allocation_state_t::reserved);
    ASSERT_TRUE (snapshot->pending_creation);
    EXPECT_EQ (snapshot->pending_creation->reservation_id, reservation->fence.reservation_id);
    EXPECT_EQ (snapshot->pending_creation->request_content_reference,
               request.intent.request_content_reference);
    EXPECT_EQ (snapshot->pending_creation->request_sha256, request.intent.request_sha256);
    EXPECT_EQ (snapshot->pending_creation->request_encoded_size,
               request.intent.request_encoded_size);

    const auto reservation_rows =
      provider
        .scan ({.prefix = "zlink:v11:creation-reservation:", .cursor = std::nullopt, .limit = 10})
        .result ()
        .value ();
    const auto *reservation_page = std::get_if<store_scan_page_t> (&reservation_rows);
    ASSERT_NE (reservation_page, nullptr);
    EXPECT_TRUE (reservation_page->items.empty ());

    const auto committed =
      repository.commit ({request.key, reservation->fence, bytes ("ready")}).result ().value ();
    const auto *ready = std::get_if<object_committed_t> (&committed);
    ASSERT_NE (ready, nullptr);
    EXPECT_EQ (ready->ready.allocation.state, placement_allocation_state_t::active);
    EXPECT_FALSE (ready->ready.pending_creation);
}

TEST (CppFrameworkOpaqueLocationStore, RetargetUsesCapacityRowsAtomically)
{
    reject_next_authority_capacity_write_store_t provider;
    provider_location_repository_t repository (provider);
    const auto source_claim = repository.claim_owner_lease ("source-owner", 30s).result ().value ();
    const auto target_claim = repository.claim_owner_lease ("target-owner", 30s).result ().value ();
    const auto *source_owner = std::get_if<owner_lease_claimed_t> (&source_claim);
    const auto *target_owner = std::get_if<owner_lease_claimed_t> (&target_claim);
    ASSERT_NE (source_owner, nullptr);
    ASSERT_NE (target_owner, nullptr);

    const auto descriptor = [] (std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t value;
        value.mesh_name = "retarget";
        value.rid = zlink::routing_id_t::from (std::move (rid));
        value.lifecycle_generation = 1;
        value.descriptor_revision = 1;
        value.endpoint = "tcp://127.0.0.1:7001";
        value.owner_id = owner.owner_id;
        value.lease_generation = owner.lease_generation;
        value.object_role = object_role_t::server;
        value.state = framework_runtime_state_t::serving;
        value.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                              maintenance_policy_kind_t::recreate, false, 0});
        value.capacity.actors.limit = 10;
        return value;
    };
    auto source_descriptor = descriptor ("source-node", source_owner->token);
    auto target_descriptor = descriptor ("target-node", target_owner->token);
    ASSERT_EQ (repository.update_mesh_node (source_descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    ASSERT_EQ (repository.update_mesh_node (target_descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    const object_creation_target_t source_target{
      "retarget", node_rid_t::from_string ("source-node"), 1, source_owner->token};
    const object_creation_target_t target_target{
      "retarget", node_rid_t::from_string ("target-node"), 1, target_owner->token};

    const auto create_actor = [&] (std::string id) {
        object_reserve_request_t request;
        request.key = {placement_object_kind_t::actor, std::move (id)};
        request.intent.stable_type = "player";
        request.target = source_target;
        request.creating_payload = bytes ("creating");
        request.capacity_bundle.actor_slots = 1;
        const auto reserved = repository.reserve (request).result ().value ();
        const auto *reservation = std::get_if<object_reserved_t> (&reserved);
        EXPECT_NE (reservation, nullptr);
        if (!reservation)
            return authority_snapshot_t{};
        const auto committed =
          repository.commit ({request.key, reservation->fence, bytes ("ready")}).result ().value ();
        const auto *ready = std::get_if<object_committed_t> (&committed);
        EXPECT_NE (ready, nullptr);
        return ready ? ready->ready : authority_snapshot_t{};
    };

    auto moved_actor = create_actor ("actor-moved");
    const auto moved =
      repository
        .compare_exchange_authority (actor_authority_key ("actor-moved"), moved_actor.store_version,
                                     authority_retarget_t{bytes ("moved"), target_target})
        .result ()
        .value ();
    const auto *moved_snapshot = std::get_if<authority_stored_t> (&moved);
    ASSERT_NE (moved_snapshot, nullptr);
    EXPECT_EQ (moved_snapshot->snapshot.allocation.target.node_rid.value (), "target-node");
    auto source_capacity = capacity_record (provider, source_descriptor);
    auto target_capacity = capacity_record (provider, target_descriptor);
    EXPECT_EQ (source_capacity.at ("actorsActive"), 0);
    EXPECT_EQ (target_capacity.at ("actorsActive"), 1);

    auto atomic_actor = create_actor ("actor-atomic");
    const store_key_t node_source_capacity_key{"zlink:v11:capacity:retarget:source-node"};
    const auto canonical_source_row = std::get<store_found_t> (
      provider.inner.read (capacity_key (source_descriptor)).result ().value ());
    const auto node_source_capacity = nlohmann::json{
      {"active", {{"actors", 1}, {"spots", 0}, {"spotTypes", nlohmann::json::object ()}}},
      {"pending", {{"actors", 0}, {"spots", 0}, {"spotTypes", nlohmann::json::object ()}}}};
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider.inner
        .write ({.conditions = {store_version_condition_t{capacity_key (source_descriptor),
                                                          canonical_source_row.value.version},
                                store_missing_condition_t{node_source_capacity_key}},
                 .mutations = {store_delete_t{capacity_key (source_descriptor)},
                               store_put_t{node_source_capacity_key,
                                           bytes (node_source_capacity.dump ()), std::nullopt}}})
        .result ()
        .value ()));
    provider.reject_next = true;
    const auto rejected = repository
                            .compare_exchange_authority (
                              actor_authority_key ("actor-atomic"), atomic_actor.store_version,
                              authority_retarget_t{bytes ("must-not-commit"), target_target})
                            .result ()
                            .value ();
    ASSERT_TRUE (std::holds_alternative<authority_conflict_t> (rejected));
    EXPECT_TRUE (provider.rejected_atomic_batch);
    EXPECT_EQ (provider.rejected_capacity_mutations, 2u);
    const auto after_rejected = std::get<authority_snapshot_t> (
      repository.read_authority (actor_authority_key ("actor-atomic")).result ().value ());
    EXPECT_NE (after_rejected.store_version, atomic_actor.store_version);
    EXPECT_EQ (after_rejected.allocation.target.node_rid.value (), "source-node");
    auto node_source_row =
      std::get<store_found_t> (provider.inner.read (node_source_capacity_key).result ().value ());
    auto node_source =
      nlohmann::json::parse (reinterpret_cast<const char *> (node_source_row.value.bytes.data ()),
                             reinterpret_cast<const char *> (node_source_row.value.bytes.data ())
                               + node_source_row.value.bytes.size ());
    target_capacity = capacity_record (provider, target_descriptor);
    EXPECT_EQ (node_source.at ("active").at ("actors"), 1);
    EXPECT_EQ (target_capacity.at ("actorsActive"), 1);

    atomic_actor = after_rejected;
    node_source["active"]["actors"] = 0;
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider.inner
        .write ({.conditions = {store_version_condition_t{node_source_capacity_key,
                                                          node_source_row.value.version}},
                 .mutations = {store_put_t{node_source_capacity_key, bytes (node_source.dump ()),
                                           std::nullopt}}})
        .result ()
        .value ()));
    const auto underflow = repository
                             .compare_exchange_authority (
                               actor_authority_key ("actor-atomic"), atomic_actor.store_version,
                               authority_retarget_t{bytes ("underflow"), target_target})
                             .result ()
                             .value ();
    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (underflow));
    node_source_row =
      std::get<store_found_t> (provider.inner.read (node_source_capacity_key).result ().value ());
    node_source =
      nlohmann::json::parse (reinterpret_cast<const char *> (node_source_row.value.bytes.data ()),
                             reinterpret_cast<const char *> (node_source_row.value.bytes.data ())
                               + node_source_row.value.bytes.size ());
    target_capacity = capacity_record (provider, target_descriptor);
    EXPECT_EQ (node_source.at ("active").at ("actors"), 0);
    EXPECT_EQ (target_capacity.at ("actorsActive"), 1);
}

TEST (CppFrameworkOpaqueLocationStore, AggregateCommitUsesBoundedBatches)
{
    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
    // This bounded-batch stress test commits 2050 participants through the
    // in-memory fixture store, which takes tens of seconds; the owner-lease TTL
    // must outlast the whole prepare/commit flow so the terminal CAS still finds
    // the lease valid.
    const auto claim = repository.claim_owner_lease ("owner-large", 300s).result ().value ();
    const auto *claimed = std::get_if<owner_lease_claimed_t> (&claim);
    ASSERT_NE (claimed, nullptr);

    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "large";
    descriptor.rid = zlink::routing_id_t::from (std::string{"node-large"});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7101";
    descriptor.owner_id = claimed->token.owner_id;
    descriptor.lease_generation = claimed->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities = {
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0},
      {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::snapshot, true, 8}};
    descriptor.capacity.actors.limit = 8192;
    descriptor.capacity.spots.limit = 8;
    descriptor.capacity.spot_types.push_back (
      {placement_object_kind_t::user_spot, "room", {0, 0, 8}});
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    const object_creation_target_t target{"large", node_rid_t::from_string ("node-large"), 1,
                                          claimed->token};
    std::vector<aggregate_participant_t> participants;
    participants.reserve (2050);
    for (std::size_t index = 0; index < 2049; ++index) {
        const auto suffix = std::to_string (100000 + index).substr (1);
        const auto global_id = "actor-" + suffix;
        object_reserve_request_t request;
        request.key = {placement_object_kind_t::actor, global_id};
        request.intent.stable_type = "player";
        request.target = target;
        request.creating_payload = bytes ("creating");
        request.capacity_bundle.actor_slots = 1;
        const auto reserved = repository.reserve (request).result ().value ();
        const auto *fence = std::get_if<object_reserved_t> (&reserved);
        ASSERT_NE (fence, nullptr);
        const auto committed =
          repository.commit ({request.key, fence->fence, bytes ("ready")}).result ().value ();
        const auto *ready = std::get_if<object_committed_t> (&committed);
        ASSERT_NE (ready, nullptr);
        participants.push_back ({actor_authority_key (global_id),
                                 ready->ready.store_version,
                                 authority_generation_transition_t::new_owner,
                                 bytes ("aggregate-" + suffix),
                                 {}});
    }

    object_reserve_request_t spot_request;
    spot_request.key = {placement_object_kind_t::user_spot, "spot-large"};
    spot_request.intent.stable_type = "room";
    spot_request.target = target;
    spot_request.creating_payload = bytes ("creating");
    spot_request.capacity_bundle.spot_slots = 1;
    spot_request.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    const auto spot_reserved = repository.reserve (spot_request).result ().value ();
    const auto *spot_fence = std::get_if<object_reserved_t> (&spot_reserved);
    ASSERT_NE (spot_fence, nullptr);
    const auto spot_committed =
      repository.commit ({spot_request.key, spot_fence->fence, bytes ("ready")}).result ().value ();
    const auto *spot_ready = std::get_if<object_committed_t> (&spot_committed);
    ASSERT_NE (spot_ready, nullptr);
    const auto large_spot_key = spot_authority_key ("spot-large");
    participants.push_back ({large_spot_key,
                             spot_ready->ready.store_version,
                             authority_generation_transition_t::new_owner,
                             bytes ("aggregate-spot"),
                             {}});

    aggregate_prepare_request_t aggregate;
    aggregate.aggregate_id.value[0] = std::byte{0x22};
    aggregate.aggregate_generation = 1;
    aggregate.participants = std::move (participants);
    aggregate.target_descriptor = {descriptor.mesh_name, descriptor.rid};
    aggregate.target_descriptor_lifecycle_generation = 1;
    aggregate.capacity_bundle.actor_slots = 2049;
    aggregate.capacity_bundle.spot_slots = 1;
    aggregate.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    aggregate.target_owner = claimed->token;
    const auto prepared = repository.prepare_aggregate (aggregate).result ().value ();
    const auto *prepared_fence = std::get_if<aggregate_prepared_t> (&prepared);
    ASSERT_NE (prepared_fence, nullptr);
    EXPECT_EQ (repository.commit_aggregate (prepared_fence->fence).result ().value (),
               aggregate_commit_result_t::committed);

    const auto large_actor_0 = actor_authority_key ("actor-00000").value;
    const auto large_actor_1 = actor_authority_key ("actor-01024").value;
    const auto large_actor_2 = actor_authority_key ("actor-02048").value;
    for (const auto &key : {large_actor_0, large_actor_1, large_actor_2, large_spot_key.value}) {
        SCOPED_TRACE (key);
        const auto found = repository.read_authority ({key}).result ().value ();
        const auto *snapshot = std::get_if<authority_snapshot_t> (&found);
        ASSERT_NE (snapshot, nullptr);
        EXPECT_TRUE (snapshot->payload.size () >= 10);
        EXPECT_EQ (snapshot->owner.owner_id, "owner-large");
    }
}

TEST (CppFrameworkOpaqueRelocationStore, CallerIssuedReferenceSupportsExactReconcile)
{
    in_memory_relocation_store_t store;
    const blob_reference_t reference{"relocation:operation-0001:chunk-0000"};
    const auto payload = bytes ("immutable");

    const auto stored = store.put (reference, payload, 30s).result ().value ();
    EXPECT_TRUE (std::holds_alternative<blob_stored_t> (stored));

    const auto replay = store.put (reference, payload, 30s).result ().value ();
    EXPECT_TRUE (std::holds_alternative<blob_already_stored_t> (replay));

    const auto conflict = store.put (reference, bytes ("changed"), 30s).result ().value ();
    EXPECT_TRUE (std::holds_alternative<blob_conflict_t> (conflict));

    const auto exact = store.read (reference).result ().value ();
    const auto *found = std::get_if<blob_found_t> (&exact);
    ASSERT_NE (found, nullptr);
    EXPECT_EQ (found->bytes, payload);

    store.erase (reference).result ().value ();
    EXPECT_TRUE (
      std::holds_alternative<blob_missing_t> (store.read (reference).result ().value ()));
}

TEST (CppFrameworkOpaqueRelocationStore, PrivateRepositoryUsesTheRegisteredOpaqueProvider)
{
    in_memory_relocation_store_t provider;
    provider_relocation_repository_t repository (provider);
    const auto payload = bytes ("repository-payload");

    const auto stored =
      repository.put_relocation (payload, 1h, std::chrono::steady_clock::now () + 1min)
        .result ()
        .value ();
    EXPECT_FALSE (stored.reference.empty ());
    EXPECT_GT (stored.expires_at, stored.store_now);

    const auto provider_read =
      provider.read (blob_reference_t{stored.reference}).result ().value ();
    const auto *provider_found = std::get_if<blob_found_t> (&provider_read);
    ASSERT_NE (provider_found, nullptr);
    EXPECT_EQ (provider_found->bytes, payload);

    const auto repository_read = repository.get_relocation (stored.reference).result ().value ();
    const auto *repository_found = std::get_if<relocation_found_t> (&repository_read);
    ASSERT_NE (repository_found, nullptr);
    EXPECT_EQ (repository_found->payload, payload);

    const auto renewed = repository.renew_relocation (stored.reference, 2h).result ().value ();
    EXPECT_TRUE (std::holds_alternative<relocation_renewed_t> (renewed));

    EXPECT_EQ (repository.delete_relocation (stored.reference).result ().value (),
               relocation_delete_result_t::deleted);
    EXPECT_TRUE (std::holds_alternative<relocation_missing_t> (
      repository.get_relocation (stored.reference).result ().value ()));
}

TEST (CppFrameworkOpaqueRelocationStore, RepositoryReconcilesLostCommitReply)
{
    post_commit_failure_relocation_store_t provider;
    provider_relocation_repository_t repository (provider);
    const auto payload = bytes ("immutable-after-timeout");

    const auto stored =
      repository.put_relocation (payload, 1h, std::chrono::steady_clock::now () + 1min)
        .result ()
        .value ();
    const auto read = provider.inner.read (blob_reference_t{stored.reference}).result ().value ();
    const auto *found = std::get_if<blob_found_t> (&read);
    ASSERT_NE (found, nullptr);
    EXPECT_EQ (found->bytes, payload);
}

// Rewrites every stored canonical JSON row so its `recordVersion` field is
// ABSENT (not merely unknown), returning how many rows were stripped.
// Provider-private rows (counters, reservations, ...) either are not JSON
// objects or carry no recordVersion and pass through untouched.
std::size_t strip_record_version_fields (in_memory_location_store_t &store)
{
    std::size_t stripped = 0;
    std::optional<store_scan_cursor_t> cursor;
    do {
        auto result = store.scan ({"", cursor, 256}).result ().value ();
        auto *page = std::get_if<store_scan_page_t> (&result);
        if (page == nullptr)
            break;
        for (const auto &item : page->items) {
            const std::string text (reinterpret_cast<const char *> (item.value.bytes.data ()),
                                    item.value.bytes.size ());
            auto record = nlohmann::json::parse (text, nullptr, false);
            if (!record.is_object () || !record.contains ("recordVersion"))
                continue;
            record.erase ("recordVersion");
            (void) store
              .write ({.conditions = {},
                       .mutations = {store_put_t{item.key, bytes (record.dump ()),
                                                 std::chrono::hours (1)}}})
              .result ()
              .value ();
            ++stripped;
        }
        cursor = page->next_cursor;
    } while (cursor);
    return stripped;
}

// Spec 21 §2.4 fail-closed: a canonical row whose recordVersion is MISSING
// is just as unrecognized as one with a wrong value — the reader must fail
// explicitly instead of guessing how to read it (java is strict; dotnet
// went strict in f2dfa809e8; this pins the cpp readers: owner lease,
// descriptor envelope, and authority).
TEST (CppFrameworkOpaqueLocationStore, MissingRecordVersionFailsClosed)
{
    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
    const auto claim = repository.claim_owner_lease ("owner-a", 30s).result ().value ();
    const auto *claimed = std::get_if<owner_lease_claimed_t> (&claim);
    ASSERT_NE (claimed, nullptr);

    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "play";
    descriptor.rid = zlink::routing_id_t::from (std::string{"node-7"});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = claimed->token.owner_id;
    descriptor.lease_generation = claimed->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0});
    descriptor.capacity.actors.limit = 10;
    descriptor.capacity.spots.limit = 10;
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    object_creation_target_t target{"play", node_rid_t::from_string ("node-7"), 1, claimed->token};
    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "actor-1"};
    request.intent.stable_type = "player";
    request.target = target;
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;
    const auto reserved = repository.reserve (request).result ().value ();
    const auto *reservation = std::get_if<object_reserved_t> (&reserved);
    ASSERT_NE (reservation, nullptr);
    const auto ready =
      repository.commit ({request.key, reservation->fence, bytes ("ready")}).result ().value ();
    ASSERT_NE (std::get_if<object_committed_t> (&ready), nullptr);

    // Sanity: intact rows read fine before the corruption.
    const auto actor_key = actor_authority_key ("actor-1");
    ASSERT_TRUE (std::holds_alternative<owner_lease_found_t> (
      repository.read_owner_lease ("owner-a").result ().value ()));
    ASSERT_TRUE (std::holds_alternative<authority_snapshot_t> (
      repository.read_authority (actor_key).result ().value ()));
    ASSERT_EQ (repository.list_mesh_nodes ("play").result ().value ().items.size (), 1u);

    // Owner lease + MeshNode descriptor + authority at minimum.
    ASSERT_GE (strip_record_version_fields (provider), 3u);

    provider_location_repository_t reopened (provider);
    const auto owner_failure = reopened.read_owner_lease ("owner-a").result ();
    ASSERT_FALSE (owner_failure);
    EXPECT_NE (std::string (owner_failure.error ()->what ()).find ("recordVersion"),
               std::string::npos);
    try {
        (void) reopened.read_authority (actor_key).result ().value ();
        FAIL () << "Missing authority recordVersion must fail";
    }
    catch (const framework_exception_t &error) {
        EXPECT_EQ (error.kind (), framework_error_kind_t::internal_failure);
        EXPECT_STREQ (error.what (), "unrecognized authority recordVersion");
    }
    const auto descriptor_failure = reopened.list_mesh_nodes ("play").result ();
    ASSERT_FALSE (descriptor_failure);
    EXPECT_NE (std::string (descriptor_failure.error ()->what ()).find ("recordVersion"),
               std::string::npos);
}

} // namespace
