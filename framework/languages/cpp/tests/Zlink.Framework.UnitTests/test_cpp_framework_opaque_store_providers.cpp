/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "../support/owner_lease_time_store.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using zlink::framework::tests::owner_lease_time_store_t;

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
        after_commit,
        between_writes
    };

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
        if (fault == fault_t::none)
            return inner.write (std::move (request));
        ++writes;
        attempted = request;
        if (fault == fault_t::before_commit)
            return task_t<store_write_result_t> (result_t<store_write_result_t>::success (
              store_write_conflict_t{std::chrono::system_clock::now ()}));
        if (fault == fault_t::between_writes && writes > 1)
            return task_t<store_write_result_t> (result_t<store_write_result_t>::failure (
              framework_error_kind_t::internal_failure, "provider failed between writes"));
        auto applied = inner.write (std::move (request));
        if (fault == fault_t::after_commit) {
            applied.result ().value ();
            return task_t<store_write_result_t> (result_t<store_write_result_t>::failure (
              framework_error_kind_t::internal_failure, "provider lost the atomic write reply"));
        }
        return applied;
    }

    in_memory_location_store_t inner;
    fault_t fault = fault_t::none;
    unsigned writes = 0;
    std::optional<store_write_request_t> attempted;
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
        EXPECT_EQ (stored->expires_at, std::chrono::time_point_cast<std::chrono::milliseconds> (
                                         *raw_terminal->value.expires_at));
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

    creation_terminal_failure_store_t provider;
    provider_location_repository_t repository{provider};
    mesh_node_descriptor_t descriptor;
    object_reserve_request_t reserve_request;
    object_reservation_fence_t fence;
    creation_terminal_publication_t publication;
};

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

TEST_P (CreationTerminalTest, ConditionalConflictLeavesReservationAndCapacityUnchanged)
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
    EXPECT_EQ (creating->store_version, fence.expected_store_version);
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
        auto committed = inner.write (std::move (request));
        if (_fail_next_write) {
            _fail_next_write = false;
            committed.result ().value ();
            return task_t<store_write_result_t> (result_t<store_write_result_t>::failure (
              framework_error_kind_t::unavailable, "reply was lost after commit"));
        }
        return committed;
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
                return task_t<store_write_result_t> (result_t<store_write_result_t>::success (
                  store_write_result_t{store_write_conflict_t{std::chrono::system_clock::now ()}}));
            }
        }
        return inner.write (std::move (request));
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
            const auto published = inner.write (std::move (peer_request)).result ().value ();
            const auto now = std::holds_alternative<store_write_applied_t> (published)
                               ? std::get<store_write_applied_t> (published).store_now
                               : std::get<store_write_conflict_t> (published).store_now;
            return task_t<store_write_result_t> (
              result_t<store_write_result_t>::success (store_write_conflict_t{now}));
        }
        return inner.write (std::move (request));
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
        auto committed = inner.put (reference, payload, retention);
        if (_fail_next_put) {
            _fail_next_put = false;
            committed.result ().value ();
            return task_t<blob_put_result_t> (result_t<blob_put_result_t>::failure (
              framework_error_kind_t::unavailable, "reply was lost after commit"));
        }
        return committed;
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
    EXPECT_THROW ((void) reopened.reserve (request), std::invalid_argument);
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
}

TEST (CppFrameworkOpaqueLocationStore, PrivateRepositoryPersistsAuthorityLifecycleThroughProvider)
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
      std::get<store_found_t> (provider.read ({"zlink:v11:object-counter"}).result ().value ())
        .value.bytes,
      bytes ("2"));
    EXPECT_EQ (std::get<store_found_t> (
                 provider.read ({"zlink:v11:authority-owner-counter"}).result ().value ())
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
    EXPECT_EQ (reopened.commit_aggregate (aggregate_fence->fence).result ().value (),
               aggregate_commit_result_t::committed);
    // The two-participant aggregate issues 4 and 5 after reserve/retarget/
    // reserve, then stores the next-to-issue value 6 in one transition batch.
    EXPECT_EQ (std::get<store_found_t> (
                 provider.read ({"zlink:v11:authority-owner-counter"}).result ().value ())
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
    const store_key_t authority_counter_key{"zlink:v11:authority-owner-counter"};
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
    EXPECT_EQ (after_rejected.store_version, atomic_actor.store_version);
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

    const auto stored = repository.put_relocation (payload, 1h).result ().value ();
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

    const auto stored = repository.put_relocation (payload, 1h).result ().value ();
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
    EXPECT_THROW ((void) reopened.read_owner_lease ("owner-a"), std::invalid_argument);
    EXPECT_THROW ((void) reopened.read_authority (actor_key), std::invalid_argument);
    EXPECT_THROW ((void) reopened.list_mesh_nodes ("play"), std::invalid_argument);
}

} // namespace
