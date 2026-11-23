/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "../support/opaque_store_record_fixture.hpp"
#include "../support/aggregate_page_read_fixture.hpp"

#include <fstream>

namespace
{
using namespace zlink::framework::test::opaque_store;
using namespace zlink::framework::runtime::stateful;

TEST (AuthorityAggregateRelocation, SharedRecordsRoundTripAndMalformedRecordsFailClosed)
{
    std::ifstream fixture (ZLINK_AUTHORITY_AGGREGATE_RELOCATION_GOLDEN_PATH);
    const auto golden = nlohmann::ordered_json::parse (fixture);
    for (const auto &item : golden.at ("cases")) {
        SCOPED_TRACE (item.at ("name").get<std::string> ());
        const auto &aggregate = item.at ("aggregate");
        if (item.at ("expect") == "reject") {
            EXPECT_THROW ((void) authority_aggregate_codec::read (aggregate),
                          std::invalid_argument);
        } else {
            const auto decoded = authority_aggregate_codec::read (aggregate);
            EXPECT_EQ (decoded->encoded.dump (), aggregate.dump ());
            if (decoded->relocation) {
                const auto rewritten =
                  authority_aggregate_codec::replace (decoded, aggregate.at ("relocation"));
                EXPECT_EQ (rewritten->encoded.dump (), aggregate.dump ());
            }
        }
    }
}

TEST (AuthorityAggregateRelocation, ForeignCaptureHoldsItsOriginalSourceFence)
{
    std::ifstream fixture (ZLINK_AUTHORITY_AGGREGATE_RELOCATION_GOLDEN_PATH);
    const auto golden = nlohmann::json::parse (fixture);
    const auto &cases = golden.at ("cases");
    const auto captured = std::find_if (cases.begin (), cases.end (), [] (const auto &item) {
        return item.at ("name") == "relocation-only-captured";
    });
    ASSERT_NE (captured, cases.end ());

    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
    public_authority_store_adapter_t adapter (repository);
    const store_key_t key{std::string{"authority\0actor\0golden-actor", 28}};
    nlohmann::json row = {{"recordVersion", 1},
                          {"payload", ""},
                          {"objectGeneration", "1"},
                          {"authorityOwnerGeneration", "1"},
                          {"ownerId", "owner-src"},
                          {"ownerLeaseGeneration", "7"},
                          {"allocation",
                           {{"state", "active"},
                            {"objectKind", "actor"},
                            {"stableType", "Game.Actor"},
                            {"descriptor", {{"meshName", "mesh"}, {"routingIdHex", "0a0b"}}},
                            {"descriptorLifecycleGeneration", "3"},
                            {"capacity", {{"actors", 1}, {"spots", 0}, {"spotType", nullptr}}}}},
                          {"pendingCreation", nullptr},
                          {"aggregate", captured->at ("aggregate")}};
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider.write ({{}, {store_put_t{key, bytes (row.dump ()), std::nullopt}}})
        .result ()
        .value ()));
    relocation_authority_fence_t fence{
      object_kind_t::actor, "golden-actor",   "opaque-v1",
      {"owner-src", 7},     {"owner-dst", 1}, {0x0f1e2d3c4b5a4968, 0x8778a1b2c3d4e5f6}};
    EXPECT_EQ (adapter.observe_relocation (fence), relocation_authority_t::unsettled);
    const auto authority_key = actor_authority_key ("golden-actor");
    const auto read = repository.read_authority (authority_key).result ().value ();
    const auto *authority = std::get_if<authority_snapshot_t> (&read);
    ASSERT_NE (authority, nullptr);
    const auto rewritten = repository
                             .compare_exchange_authority (authority_key, authority->store_version,
                                                          authority_restore_t{{}, {"owner-src", 7}})
                             .result ()
                             .value ();
    EXPECT_TRUE (std::holds_alternative<authority_stored_t> (rewritten));
    const auto persisted = std::get<store_found_t> (provider.read (key).result ().value ());
    const auto &persisted_bytes = persisted.value.bytes;
    const auto *persisted_data = reinterpret_cast<const char *> (persisted_bytes.data ());
    const auto persisted_row =
      nlohmann::json::parse (persisted_data, persisted_data + persisted_bytes.size ());
    EXPECT_EQ (persisted_row.at ("aggregate"), captured->at ("aggregate"));
    fence.source_node_generation = 4;
    EXPECT_EQ (adapter.observe_relocation (fence), relocation_authority_t::source_preserved);
    fence.source_node_generation = 3;
    EXPECT_EQ (adapter.observe_relocation (fence), relocation_authority_t::unsettled);
    fence.expected_store_version = "wrong-version";
    EXPECT_EQ (adapter.observe_relocation (fence), relocation_authority_t::source_preserved);
}
TEST (AuthorityAggregateRelocation, ForeignCommittedRecordFencesItsTargetAttempt)
{
    std::ifstream fixture (ZLINK_AUTHORITY_AGGREGATE_RELOCATION_GOLDEN_PATH);
    const auto golden = nlohmann::json::parse (fixture);
    const auto &cases = golden.at ("cases");
    const auto committed = std::find_if (cases.begin (), cases.end (), [] (const auto &item) {
        return item.at ("name") == "marker-and-committed";
    });
    ASSERT_NE (committed, cases.end ());

    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
    public_authority_store_adapter_t adapter (repository);
    const store_key_t key{std::string{"authority\0actor\0golden-actor", 28}};
    auto aggregate = committed->at ("aggregate");
    // The storage codec accepts a full-range record token. The repository's
    // live owner lease counter is bounded, so use a live counter value here.
    aggregate["relocation"]["target"]["ownerLeaseGeneration"] = "8";
    const nlohmann::json row = {
      {"recordVersion", 1},
      {"payload", ""},
      {"objectGeneration", "1"},
      {"authorityOwnerGeneration", "12"},
      {"ownerId", "owner-tgt"},
      {"ownerLeaseGeneration", "8"},
      {"allocation",
       {{"state", "active"},
        {"objectKind", "actor"},
        {"stableType", "Game.Actor"},
        {"descriptor", {{"meshName", "mesh"}, {"routingIdHex", "0c0d"}}},
        {"descriptorLifecycleGeneration", "5"},
        {"capacity", {{"actors", 1}, {"spots", 0}, {"spotType", nullptr}}}}},
      {"pendingCreation", nullptr},
      {"aggregate", aggregate}};
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider.write ({{}, {store_put_t{key, bytes (row.dump ()), std::nullopt}}})
        .result ()
        .value ()));
    relocation_authority_fence_t fence{
      object_kind_t::actor, "golden-actor",   "opaque-v1",
      {"owner-src", 7},     {"owner-tgt", 8}, {0x0f1e2d3c4b5a4968, 0x8778a1b2c3d4e5f6}};
    EXPECT_EQ (adapter.observe_relocation (fence), relocation_authority_t::target_committed);
    ++fence.relocation.low;
    EXPECT_EQ (adapter.observe_relocation (fence), relocation_authority_t::source_preserved);
}

TEST (AuthorityAggregateRelocation, SpotWideCommitConsumesForeignCapture)
{
    std::ifstream fixture (ZLINK_AUTHORITY_AGGREGATE_RELOCATION_GOLDEN_PATH);
    const auto golden = nlohmann::ordered_json::parse (fixture);
    const auto &cases = golden.at ("cases");
    const auto captured = std::find_if (cases.begin (), cases.end (), [] (const auto &item) {
        return item.at ("name") == "relocation-only-captured";
    });
    ASSERT_NE (captured, cases.end ());
    in_memory_location_store_t provider;
    zlink::framework::test::aggregate_page_read_keeps_source_version_until_commit (
      provider, [] (std::string_view value) { return bytes (value); }, captured->at ("aggregate"));
}

}
