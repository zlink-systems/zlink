/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/relocation_completion_store.hpp"
#include "runtime/locations/base64.hpp"

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "../support/opaque_store_record_fixture.hpp"

#include <fstream>
#include <zlink/framework/detail/sha256.hpp>

namespace
{
using namespace zlink::framework::test::opaque_store;
using namespace zlink::framework::runtime::relocation_completion;

nlohmann::json golden ()
{
    std::ifstream fixture (ZLINK_RELOCATION_COMPLETION_GOLDEN_PATH);
    return nlohmann::json::parse (fixture).at ("cases");
}

TEST (RelocationCompletion, SharedGolden)
{
    for (const auto &item : golden ()) {
        SCOPED_TRACE (item.at ("name").get<std::string> ());
        const auto raw = base64_decode (item.at ("bytesBase64").get<std::string> ());
        const std::string_view encoded (reinterpret_cast<const char *> (raw.data ()), raw.size ());
        if (item.at ("expect") == "reject") {
            EXPECT_THROW ((void) decode (encoded), framework_exception_t);
            continue;
        }
        const auto record = decode (encoded);
        EXPECT_EQ (encode (record), encoded);
        EXPECT_EQ (detail::encode_hex (std::span<const std::byte> (detail::sha256 (raw))),
                   item.at ("sha256").get<std::string> ());
        EXPECT_EQ (std::to_string (record.entries.size ()),
                   item.at ("completedResults").get<std::string> ());
        EXPECT_EQ (std::to_string (record.pending_deliveries ()),
                   item.at ("pendingDeliveries").get<std::string> ());
    }
}

record_t golden_record ()
{
    const auto raw = base64_decode (golden ().front ().at ("bytesBase64").get<std::string> ());
    return decode ({reinterpret_cast<const char *> (raw.data ()), raw.size ()});
}

TEST (RelocationCompletion, FinalSizeIncludesLongestDeliveryAndRemainingFailureReplies)
{
    auto record = golden_record ();
    auto remaining = record.entries.back ();
    record.entries.pop_back ();
    remaining.reply = bytes ("failure reply");
    const auto failures = std::span{&remaining, 1};
    auto final = record;
    final.entries.push_back (remaining);
    for (auto &entry : final.entries)
        entry.delivery = delivery_t::source_lease_expired;
    const auto bound = encode (final).size ();
    EXPECT_NO_THROW (require_fits (record, failures, bound));
    EXPECT_THROW (require_fits (record, failures, bound - 1), framework_exception_t);
}

const store_key_t authority_row{std::string{"authority\0actor\0actor-1", 23}};

class location_fixture_t final : public location_store_t
{
  public:
    enum class failure_t
    {
        none,
        before_write,
        after_write,
        applied_then_conflict,
        replaced_then_conflict,
        corrupt
    };
    unsigned attempts = 0;
    unsigned authority_reads = 0;
    unsigned replay_conflicts = 0;
    failure_t failure = failure_t::none;
    std::vector<std::string> *events = nullptr;

    task_t<store_read_result_t> read (store_key_t key) override
    {
        if (key.value == authority_row.value) {
            ++authority_reads;
            if (authority_reads == 2 && failure == failure_t::applied_then_conflict)
                throw framework_exception_t (framework_error_kind_t::unavailable,
                                             "Reconciliation read failed");
        }
        co_return co_await store.read (std::move (key));
    }

    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        ++attempts;
        if (events)
            events->push_back ("cas");
        if (failure == failure_t::before_write)
            throw framework_exception_t (framework_error_kind_t::unavailable, "CAS response lost");
        if (failure == failure_t::corrupt)
            co_return store_write_corrupt_t{};
        if (attempts == 1 && failure == failure_t::replaced_then_conflict) {
            const auto current = co_await store.read (authority_row);
            const auto &row = std::get<store_found_t> (current).value;
            store_write_request_t replacement{
              {}, {store_put_t{authority_row, row.bytes, std::nullopt}}};
            const auto replaced = co_await store.write (std::move (replacement));
            if (!std::holds_alternative<store_write_applied_t> (replaced))
                throw std::logic_error ("Concurrent authority write did not apply");
            throw framework_exception_t (framework_error_kind_t::unavailable, "CAS response lost");
        }
        auto result = co_await store.write (std::move (request));
        if (failure == failure_t::after_write
            || (attempts == 1 && failure == failure_t::applied_then_conflict))
            throw framework_exception_t (framework_error_kind_t::unavailable, "CAS response lost");
        if (attempts > 1 && std::holds_alternative<store_write_conflict_t> (result))
            ++replay_conflicts;
        co_return result;
    }

    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return store.scan (std::move (request));
    }

  private:
    in_memory_location_store_t store;
};

class blob_fixture_t final : public relocation_store_t
{
  public:
    unsigned puts = 0;
    unsigned reads = 0;
    unsigned fail_put = 0;
    unsigned fail_read = 0;
    unsigned corrupt_read = 0;
    std::vector<std::string> events;

    task_t<blob_put_result_t> put (blob_reference_t reference,
                                   std::span<const std::byte> payload,
                                   std::chrono::milliseconds retention) override
    {
        events.push_back ("put");
        if (++puts == fail_put)
            throw framework_exception_t (framework_error_kind_t::unavailable, "Put failed");
        return store.put (std::move (reference), payload, retention);
    }

    task_t<blob_read_result_t> read (blob_reference_t reference) override
    {
        events.push_back ("read");
        if (++reads == fail_read)
            throw framework_exception_t (framework_error_kind_t::unavailable, "Read failed");
        if (reads == corrupt_read)
            return task_t<blob_read_result_t> (
              result_t<blob_read_result_t>::success (blob_found_t{bytes ("corrupt"), {}, {}}));
        return store.read (std::move (reference));
    }

    task_t<blob_renew_result_t> renew (blob_reference_t reference,
                                       std::chrono::milliseconds retention) override
    {
        return store.renew (std::move (reference), retention);
    }

    task_t<void> erase (blob_reference_t reference) override
    {
        return store.erase (std::move (reference));
    }

  private:
    in_memory_relocation_store_t store;
};

class RelocationCompletionStore : public ::testing::Test
{
  protected:
    location_fixture_t location;
    provider_location_repository_t authorities{location};
    blob_fixture_t blob;
    provider_relocation_repository_t blobs{blob};
    store_t store{authorities, blobs};
    const authority_key_t key = actor_authority_key ("actor-1");
    record_t record = golden_record ();
    authority_snapshot_t original;

    void SetUp () override
    {
        const auto owner = std::get<owner_lease_claimed_t> (
                             authorities.claim_owner_lease ("owner-target", 30s).result ().value ())
                             .token;
        const auto fence =
          nlohmann::ordered_json{{"nodeRid", "ab"},
                                 {"nodeGeneration", "4"},
                                 {"ownerId", owner.owner_id},
                                 {"ownerLeaseGeneration", std::to_string (owner.lease_generation)}};
        auto coordinator = fence;
        coordinator["expectedStoreVersion"] = "source-version";
        auto target = fence;
        target["attemptGeneration"] = "1";
        const nlohmann::ordered_json aggregate = {
          {"relocation",
           {{"relocationId", record.relocation_id},
            {"phase", "committed"},
            {"source", fence},
            {"coordinator", coordinator},
            {"target", target},
            {"sourceInventorySha256", std::string (64, 'a')},
            {"applicationVersion", "1"},
            {"completion",
             {{"acceptedRequests", "3"},
              {"completedResults", "0"},
              {"pendingDeliveries", "0"},
              {"reference", nullptr},
              {"sha256", nullptr}}}}}};
        const nlohmann::ordered_json row = {
          {"recordVersion", 1},
          {"payload", "AQI="},
          {"objectGeneration", "1"},
          {"authorityOwnerGeneration", "2"},
          {"ownerId", owner.owner_id},
          {"ownerLeaseGeneration", std::to_string (owner.lease_generation)},
          {"allocation",
           {{"state", "active"},
            {"objectKind", "actor"},
            {"stableType", "Game.Actor"},
            {"descriptor", {{"meshName", "mesh"}, {"routingIdHex", "ab"}}},
            {"descriptorLifecycleGeneration", "4"},
            {"capacity", {{"actors", 1}, {"spots", 0}, {"spotType", nullptr}}}}},
          {"pendingCreation", nullptr},
          {"aggregate", aggregate}};
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          location.write ({{}, {store_put_t{authority_row, bytes (row.dump ()), std::nullopt}}})
            .result ()
            .value ()));
        original = current ();
        location.attempts = 0;
        location.authority_reads = 0;
        location.events = &blob.events;
    }

    authority_snapshot_t current ()
    {
        return std::get<authority_snapshot_t> (authorities.read_authority (key).result ().value ());
    }

    result_t<authority_compare_exchange_result_t> publish ()
    {
        return store.publish (key, original, record, {}).result ();
    }
};

TEST_F (RelocationCompletionStore, VerifiesChunkAndManifestBeforeSingleCas)
{
    const auto result = publish ();
    ASSERT_TRUE (result) << (result.error () ? result.error ()->what () : "");
    ASSERT_TRUE (std::holds_alternative<authority_stored_t> (result.value ()));
    EXPECT_EQ (blob.events, (std::vector<std::string>{"put", "read", "put", "read", "cas"}));
    EXPECT_EQ (location.attempts, 1u);
    const auto observed = current ();
    EXPECT_EQ (observed.payload, original.payload);
    EXPECT_TRUE (observed.same_incarnation (original));
    const auto read = store.read (observed).result ();
    ASSERT_TRUE (read);
    EXPECT_EQ (encode (read.value ()), encode (record));
}

TEST_F (RelocationCompletionStore, PutOrReadBackFailureLeavesAuthorityUnchanged)
{
    for (const auto &fault :
         {&blob_fixture_t::fail_put, &blob_fixture_t::fail_read, &blob_fixture_t::corrupt_read}) {
        for (unsigned stage : {1u, 2u}) {
            blob.puts = blob.reads = 0;
            blob.*fault = stage;
            EXPECT_FALSE (publish ());
            EXPECT_EQ (current ().store_version, original.store_version);
            EXPECT_EQ (location.attempts, 0u);
            EXPECT_LE (blob.puts, stage);
            blob.*fault = 0;
        }
    }
}

TEST_F (RelocationCompletionStore, StaleAuthorityRemainsConflictAfterReferenceCheck)
{
    ASSERT_TRUE (std::holds_alternative<authority_stored_t> (publish ().value ()));
    const auto committed = current ();
    location.authority_reads = 0;
    ASSERT_TRUE (std::holds_alternative<authority_conflict_t> (publish ().value ()));
    EXPECT_EQ (location.authority_reads, 2u);
    EXPECT_EQ (current ().store_version, committed.store_version);
    EXPECT_EQ (location.attempts, 1u);
}

TEST_F (RelocationCompletionStore, LostResponseConfirmsAppliedReference)
{
    location.failure = location_fixture_t::failure_t::after_write;
    const auto result = publish ();
    ASSERT_TRUE (result);
    EXPECT_TRUE (std::holds_alternative<authority_stored_t> (result.value ()));
    EXPECT_EQ (location.attempts, 1u);
    EXPECT_EQ (location.authority_reads, 2u);
}

TEST_F (RelocationCompletionStore, LostResponseWithoutApplyFails)
{
    location.failure = location_fixture_t::failure_t::before_write;
    const auto result = publish ();
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::unavailable);
    EXPECT_EQ (location.attempts, 2u);
    EXPECT_EQ (location.authority_reads, 4u);
    EXPECT_EQ (current ().store_version, original.store_version);
}

TEST_F (RelocationCompletionStore, LostResponseThenReplayConflictConfirmsAppliedReference)
{
    location.failure = location_fixture_t::failure_t::applied_then_conflict;
    const auto result = publish ();
    ASSERT_TRUE (result);
    ASSERT_TRUE (std::holds_alternative<authority_stored_t> (result.value ()));
    EXPECT_EQ (location.attempts, 2u);
    EXPECT_EQ (location.replay_conflicts, 1u);
    EXPECT_EQ (location.authority_reads, 5u);
    EXPECT_EQ (blob.reads, 4u);
}

TEST_F (RelocationCompletionStore, LostResponseThenReplayConflictRejectsOtherWrite)
{
    location.failure = location_fixture_t::failure_t::replaced_then_conflict;
    const auto result = publish ();
    ASSERT_TRUE (result);
    const auto *conflict = std::get_if<authority_conflict_t> (&result.value ());
    ASSERT_NE (conflict, nullptr);
    const auto &observed = std::get<authority_snapshot_t> (conflict->current);
    EXPECT_NE (observed.store_version, original.store_version);
    const auto completion = store.read (observed).result ();
    ASSERT_TRUE (completion);
    EXPECT_TRUE (completion.value ().entries.empty ());
    EXPECT_EQ (location.attempts, 2u);
    EXPECT_EQ (location.replay_conflicts, 1u);
    EXPECT_EQ (location.authority_reads, 5u);
    EXPECT_EQ (blob.reads, 2u);
}

TEST_F (RelocationCompletionStore, CorruptWriteKeepsOriginalError)
{
    location.failure = location_fixture_t::failure_t::corrupt;
    const auto result = publish ();
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::internal_failure);
    EXPECT_EQ (location.attempts, 1u);
    EXPECT_EQ (location.authority_reads, 1u);
    EXPECT_EQ (blob.reads, 2u);
    EXPECT_EQ (current ().store_version, original.store_version);
}

TEST_F (RelocationCompletionStore, RejectsInconsistentCounts)
{
    const auto entry = record.entries.back ();
    record.entries.pop_back ();
    EXPECT_FALSE (publish ());
    EXPECT_EQ (blob.puts, 0u);
    EXPECT_EQ (location.attempts, 0u);

    record.entries.push_back (entry);
    const auto published = publish ();
    ASSERT_TRUE (published);
    auto observed = std::get<authority_stored_t> (published.value ()).snapshot;
    auto relocation = observed.aggregate->encoded.at ("relocation");
    relocation["completion"]["pendingDeliveries"] = "0";
    observed.aggregate =
      authority_aggregate_codec::replace (observed.aggregate, std::move (relocation));
    const auto read = store.read (observed).result ();
    ASSERT_FALSE (read);
    EXPECT_EQ (read.error_kind (), framework_error_kind_t::data_lost);
}
}
