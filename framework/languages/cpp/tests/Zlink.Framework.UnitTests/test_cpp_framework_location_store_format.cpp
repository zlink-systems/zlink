/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "../support/opaque_store_creation_fixture.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <fstream>
#include <future>

namespace
{
using namespace zlink::framework::test::opaque_store;
using zlink::framework::runtime::location_runtime_t;
using json_t = nlohmann::json;

const store_key_t format_key{"zlink:v12:format"};
const store_key_t floor_key{"zlink:v12:generation-floor"};

json_t fixture ()
{
    std::ifstream input (ZLINK_LOCATION_CONTENTION_FIXTURE_PATH);
    return json_t::parse (input);
}

class startup_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        const bool first_marker_read =
          key.value == format_key.value
          && marker_reads.fetch_add (1) < static_cast<unsigned> (startup_count)
          && first_marker_reads;
        auto result = co_await store.read (std::move (key));
        if (first_marker_read)
            first_marker_reads->arrive_and_wait ();
        co_return result;
    }

    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return store.scan (std::move (request));
    }

    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        for (const auto &mutation : request.mutations) {
            if (const auto *put = std::get_if<store_put_t> (&mutation);
                put && put->key.value.starts_with (std::string ("owner-lease") + '\0')) {
                const auto marker = co_await store.read (format_key);
                if (std::holds_alternative<store_missing_t> (marker))
                    ++writes_before_conversion;
            }
        }
        co_return co_await store.write (std::move (request));
    }

    void seed (std::string key, std::string_view value)
    {
        EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (
          store.write ({{}, {store_put_t{{std::move (key)}, bytes (value), std::nullopt}}})
            .result ()
            .value ()));
    }

    std::optional<std::string> value (store_key_t key)
    {
        const auto result = store.read (std::move (key)).result ().value ();
        if (const auto *found = std::get_if<store_found_t> (&result))
            return std::string (reinterpret_cast<const char *> (found->value.bytes.data ()),
                                found->value.bytes.size ());
        return std::nullopt;
    }

    void seed_descriptor (const std::string &mesh, const std::string &rid)
    {
        seed (std::string ("mesh-node") + '\0' + mesh + '\0' + rid,
              json_t{{"descriptor", {{"routingIdHex", rid}}}}.dump ());
    }

    in_memory_location_store_t store;
    std::barrier<> *first_marker_reads = nullptr;
    int startup_count = 0;
    std::atomic<unsigned> marker_reads{0};
    std::atomic<unsigned> writes_before_conversion{0};
};

TEST (LocationStoreFormat, SharedV11ConversionCases)
{
    const auto contract = fixture ().at ("v11ConversionCases");
    const auto mesh = contract.at ("mesh").get<std::string> ();
    for (const auto &[name, settings] : contract.at ("cases").items ()) {
        SCOPED_TRACE (name);
        startup_store_t store;
        for (const auto &rid : settings.at ("descriptorRoutingIdHexes"))
            store.seed_descriptor (mesh, rid.get<std::string> ());
        for (const auto &row : settings.at ("rows"))
            store.seed (row.at ("key"), row.at ("value").get<std::string> ());

        provider_location_repository_t repository (store);
        location_runtime_t runtime (repository, {}, "conversion-owner");
        const auto &expected = settings.at ("expected");
        if (expected.at ("startup") == "configurationError") {
            try {
                runtime.start (zlink::routing_id_t::from ("startup"));
                ADD_FAILURE () << "An invalid v11 namespace allowed startup";
            }
            catch (const framework_exception_t &error) {
                EXPECT_EQ (error.kind (), framework_error_kind_t::protocol_error);
            }
            EXPECT_EQ (store.value (format_key), std::nullopt);
            if (name == "pendingV11Aggregate")
                EXPECT_EQ (store.value (floor_key), std::nullopt);
            for (const auto &row : settings.at ("rows"))
                EXPECT_EQ (store.value ({row.at ("key")}), row.at ("value").get<std::string> ());
        } else {
            runtime.start (zlink::routing_id_t::from ("startup"));
            EXPECT_EQ (store.value (format_key), expected.at ("startup").get<std::string> ());
            for (const auto &row : settings.at ("rows"))
                EXPECT_EQ (store.value ({row.at ("key")}), std::nullopt);
            for (const auto &[key, value] : expected.at ("counters").items ())
                EXPECT_EQ (store.value ({key}), value.get<std::string> ());
            for (const auto &key : expected.at ("absentCounters"))
                EXPECT_EQ (store.value ({key}), std::nullopt);
        }
        EXPECT_EQ (store.writes_before_conversion.load (), 0u);
    }
}

TEST (LocationStoreFormat, SharedConcurrentStartupConversion)
{
    const auto contract = fixture ().at ("concurrentStartupConversion");
    const auto mesh = contract.at ("mesh").get<std::string> ();
    const auto expected = contract.at ("expected");
    startup_store_t store;
    store.seed ("zlink:v11:object-counter", contract.at ("legacyNextObjectGeneration").dump ());
    store.seed ("zlink:v11:authority-owner-counter",
                contract.at ("legacyNextAuthorityOwnerGeneration").dump ());
    for (const auto &row : contract.at ("records")) {
        const auto &descriptor = row.at ("descriptorRoutingIdHex");
        if (!descriptor.is_null ())
            store.seed_descriptor (mesh, descriptor.get<std::string> ());
        auto rid = row.at ("legacyRid").get<std::string> ();
        if (const auto separator = rid.find (':'); separator != std::string::npos)
            rid.replace (separator, 1, "%3A");
        const json_t value{{"active",
                            {{"actors", row.at ("activeActorSlots")},
                             {"spots", 0},
                             {"spotTypes", json_t::object ()}}},
                           {"pending",
                            {{"actors", row.at ("pendingActorSlots")},
                             {"spots", 0},
                             {"spotTypes", json_t::object ()}}}};
        store.seed ("zlink:v11:capacity:" + mesh + ":" + rid, value.dump ());
    }

    const int startups = contract.at ("startupCount");
    std::barrier start (startups);
    std::barrier marker_reads (startups);
    store.first_marker_reads = &marker_reads;
    store.startup_count = startups;
    std::vector<std::future<void>> jobs;
    for (int i = 0; i < startups; ++i) {
        jobs.push_back (std::async (std::launch::async, [&, i] {
            provider_location_repository_t repository (store);
            location_runtime_t runtime (repository, {}, "owner-" + std::to_string (i));
            start.arrive_and_wait ();
            runtime.start (zlink::routing_id_t::from ("startup-" + std::to_string (i)));
            EXPECT_TRUE (runtime.owner_lease_healthy ());
        }));
    }
    for (auto &job : jobs)
        ASSERT_NO_THROW (job.get ());
    EXPECT_EQ (store.value (format_key), expected.at ("format").get<std::string> ());
    EXPECT_EQ (store.value (floor_key), expected.at ("generationFloor").dump ());
    const auto remaining = store.scan ({"zlink:v11:capacity:", {}, 100}).result ().value ();
    EXPECT_EQ (std::get<store_scan_page_t> (remaining).items.size (),
               expected.at ("remainingLegacyRecords").get<std::size_t> ());
    EXPECT_EQ (store.writes_before_conversion.load (),
               expected.at ("writesBeforeConversion").get<unsigned> ());
    for (const auto &row : contract.at ("records")) {
        const auto prefix = std::string ("zlink:v12:capacity") + '\0' + mesh + '\0'
                            + row.at ("expectedRoutingIdHex").get<std::string> () + '\0' + "actors"
                            + '\0';
        EXPECT_EQ (store.value ({prefix + "active"}), row.at ("activeActorSlots").dump ());
        const auto pending = row.at ("pendingActorSlots");
        EXPECT_EQ (store.value ({prefix + "pending"}),
                   pending == 0 ? std::nullopt : std::optional{pending.dump ()});
    }
}

TEST (LocationStoreFormat, LeaseRenewalKeepsCompletedFormat)
{
    startup_store_t store;
    provider_location_repository_t repository (store);
    location_runtime_t runtime (repository, {}, "conversion-owner");
    runtime.start (zlink::routing_id_t::from ("startup"));
    EXPECT_EQ (store.value (format_key), "converted");
    const auto marker_reads_after_startup = store.marker_reads.load ();
    EXPECT_GT (marker_reads_after_startup, 0u);
    EXPECT_TRUE (std::holds_alternative<owner_lease_renewed_t> (runtime.renew_owner_lease_once ()));
    EXPECT_EQ (store.marker_reads.load (), marker_reads_after_startup);
}
}
