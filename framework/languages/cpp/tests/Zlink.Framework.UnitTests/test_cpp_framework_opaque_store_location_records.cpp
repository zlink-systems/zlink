/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_runtime.hpp"
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
#include "../support/opaque_store_startup_fixture.hpp"

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace zlink::framework::test::opaque_store;

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
    first.configure_reclaim_margin (std::chrono::milliseconds::zero ());
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
    second.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto lease = second.read_owner_lease ("owner-a").result ().value ();
    ASSERT_TRUE (std::holds_alternative<owner_lease_found_t> (lease));
    const auto page = second.list_mesh_nodes ("play").result ().value ();
    ASSERT_EQ (page.items.size (), 1u);
    EXPECT_EQ (page.items.front ().rid.to_string (), descriptor.rid.to_string ());
    EXPECT_EQ (page.items.front ().owner_id, "owner-a");
}

TEST (CppFrameworkOpaqueLocationStore, DefaultOwnerLeaseTtlIsThirtySeconds)
{
    EXPECT_EQ (location_options_t{}.owner_lease_ttl, 30s);
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
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
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
    reopened.configure_reclaim_margin (std::chrono::milliseconds::zero ());
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

TEST (ProviderLocationRepositoryTest, AuthorityKindPrefixIsPassedToProvider)
{
    startup_review_store_t store;
    provider_location_repository_t repository (store);
    const auto check = [&] (std::string logical, std::string physical) {
        unsigned scans = 0;
        store.before_scan = [&] (const store_scan_request_t &request) -> task_t<void> {
            ++scans;
            EXPECT_EQ (request.prefix, physical);
            EXPECT_FALSE (request.cursor);
            EXPECT_EQ (request.limit, 100u);
            co_return;
        };
        auto read = repository.list_authorities (logical, std::nullopt, 100).result ().value ();
        const auto *page = std::get_if<authority_page_t> (&read);
        ASSERT_NE (page, nullptr);
        EXPECT_TRUE (page->items.empty ());
        EXPECT_FALSE (page->next_cursor);
        EXPECT_EQ (scans, 1u);
        store.before_scan = {};
    };
    check ("", std::string ("authority\0", 10));
    check ("zla1:a:", std::string ("authority\0actor\0", 16));
    check ("zla1:s:", std::string ("authority\0spot\0", 15));
    EXPECT_THROW (repository.list_authorities ("invalid", std::nullopt, 100).result ().value (),
                  std::invalid_argument);
}
}
