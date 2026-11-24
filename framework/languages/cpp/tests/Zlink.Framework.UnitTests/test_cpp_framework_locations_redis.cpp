/* SPDX-License-Identifier: Elastic-2.0 */

#include <zlink/locations/redis.hpp>

#include "../support/aggregate_page_read_fixture.hpp"
#include "../support/redis_location_options.hpp"

#include <sw/redis++/redis++.h>

#include <gtest/gtest.h>

#include <chrono>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace
{

using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::redis;
using zlink::framework::test::find_redis_options;
using zlink::framework::test::unique_redis_prefix;

std::vector<std::byte> bytes (std::string_view value)
{
    const auto *first = reinterpret_cast<const std::byte *> (value.data ());
    return {first, first + value.size ()};
}

long long command_calls (const std::string &stats, std::string_view command)
{
    const auto prefix = "cmdstat_" + std::string (command) + ":calls=";
    const auto offset = stats.find (prefix);
    return offset == std::string::npos ? 0LL : std::stoll (stats.substr (offset + prefix.size ()));
}

TEST (ZLinkFrameworkLocationsRedis, AggregatePageReadKeepsSourceVersionUntilCommit)
{
    const auto options = find_redis_options ();
    if (!options)
        GTEST_SKIP () << "Redis is not reachable at 127.0.0.1:6379; set ZLINK_REDIS_TEST_ENDPOINT";
    redis_location_store_t provider (*options);
    sw::redis::Redis raw (
      zlink::framework::redis::detail::normalize_connection_string (options->connection_string));
    zlink::framework::test::scope_exit_t cleanup ([&] {
        EXPECT_NO_THROW ({
            std::vector<std::string> keys;
            raw.keys (options->key_prefix + ":*", std::back_inserter (keys));
            if (!keys.empty ())
                raw.del (keys.begin (), keys.end ());
        });
    });
    zlink::framework::test::aggregate_page_read_keeps_source_version_until_commit (provider, bytes);
}

TEST (ZLinkFrameworkLocationsRedis, PublicTypesImplementOnlyTheOpaqueStoreContracts)
{
    static_assert (std::is_base_of_v<location_store_t, redis_location_store_t>);
    static_assert (std::is_base_of_v<relocation_store_t, redis_relocation_store_t>);
    static_assert (!std::is_default_constructible_v<redis_location_store_t>);
    static_assert (!std::is_default_constructible_v<redis_relocation_store_t>);

    EXPECT_NO_THROW (redis_location_store_t (
      {.connection_string = "tcp://127.0.0.1:6379", .key_prefix = "zlink:test:location"}));
    EXPECT_NO_THROW (redis_relocation_store_t (
      {.connection_string = "tcp://127.0.0.1:6379", .key_prefix = "zlink:test:relocation"}));
}

TEST (ZLinkFrameworkLocationsRedis, InvalidConfigurationIsRejected)
{
    EXPECT_THROW (redis_location_store_t ({.connection_string = "", .key_prefix = "zlink:test"}),
                  std::invalid_argument);
    EXPECT_THROW (
      redis_relocation_store_t ({.connection_string = "tcp://127.0.0.1:6379", .key_prefix = ""}),
      std::invalid_argument);
    EXPECT_THROW (redis_location_store_t ({.connection_string = "tcp://127.0.0.1:6379",
                                           .key_prefix = "zlink:test",
                                           .operation_timeout = 0ms}),
                  std::invalid_argument);
    EXPECT_THROW (redis_relocation_store_t ({.connection_string = "tcp://127.0.0.1:6379",
                                             .key_prefix = "zlink:test",
                                             .operation_timeout = 0ms}),
                  std::invalid_argument);
}

TEST (ZLinkFrameworkLocationsRedis, ScriptsReuseShaAndRecoverAfterFlush)
{
    const auto options = find_redis_options ();
    if (!options)
        GTEST_SKIP () << "Redis is not reachable at 127.0.0.1:6379; set ZLINK_REDIS_TEST_ENDPOINT";

    sw::redis::Redis raw (
      zlink::framework::redis::detail::normalize_connection_string (options->connection_string));
    zlink::framework::test::scope_exit_t cleanup ([&] {
        EXPECT_NO_THROW ({
            std::vector<std::string> keys;
            raw.keys (options->key_prefix + ":*", std::back_inserter (keys));
            if (!keys.empty ())
                raw.del (keys.begin (), keys.end ());
        });
    });
    redis_location_store_t location_provider (*options);
    redis_relocation_store_t relocation_provider (
      {.connection_string = options->connection_string, .key_prefix = options->key_prefix});
    location_store_t &location = location_provider;
    relocation_store_t &relocation = relocation_provider;
    const store_key_t key{"authority:script-cache"};
    const auto payload = bytes ("payload");

    const auto exercise_scripts = [&] (int expected_version) {
        const auto written =
          location.write ({{}, {store_put_t{key, payload, 30s}}}).result ().value ();
        const auto *applied = std::get_if<store_write_applied_t> (&written);
        ASSERT_NE (applied, nullptr);
        ASSERT_EQ (applied->put_versions.size (), 1u);
        EXPECT_EQ (applied->put_versions.front ().version.value, std::to_string (expected_version));

        const auto read = location.read (key).result ().value ();
        const auto *found = std::get_if<store_found_t> (&read);
        ASSERT_NE (found, nullptr);
        EXPECT_EQ (found->value.bytes, payload);
        const auto scan =
          location.scan ({.prefix = "authority:", .cursor = std::nullopt, .limit = 10})
            .result ()
            .value ();
        const auto *page = std::get_if<store_scan_page_t> (&scan);
        ASSERT_NE (page, nullptr);
        ASSERT_EQ (page->items.size (), 1u);
        EXPECT_EQ (page->items.front ().value.version.value, std::to_string (expected_version));

        const auto put = relocation.put ({"script-cache"}, payload, 30s).result ().value ();
        EXPECT_TRUE (expected_version == 1 ? std::holds_alternative<blob_stored_t> (put)
                                           : std::holds_alternative<blob_already_stored_t> (put));
    };
    const auto check_command_calls = [&] (const char *phase, int first_version, int count,
                                          long long expected_eval_calls) {
        SCOPED_TRACE (phase);
        const auto before = raw.info ("commandstats");
        for (int version = first_version; version < first_version + count; ++version)
            exercise_scripts (version);
        const auto after = raw.info ("commandstats");
        const auto evalsha_calls =
          command_calls (after, "evalsha") - command_calls (before, "evalsha");
        const auto eval_calls = command_calls (after, "eval") - command_calls (before, "eval");
        RecordProperty (std::string (phase) + "_evalsha", static_cast<int> (evalsha_calls));
        RecordProperty (std::string (phase) + "_eval", static_cast<int> (eval_calls));
        EXPECT_EQ (evalsha_calls, 4 * count);
        EXPECT_EQ (eval_calls, expected_eval_calls);
    };

    raw.script_flush ();
    check_command_calls ("cold", 1, 1, 4);
    check_command_calls ("warm", 2, 3, 0);
    raw.script_flush ();
    check_command_calls ("flushed", 5, 1, 4);
}

TEST (ZLinkFrameworkLocationsRedis, OpaqueLocationStoreProvidesAtomicCasAndScan)
{
    const auto options = find_redis_options ();
    if (!options)
        GTEST_SKIP () << "Redis is not reachable at 127.0.0.1:6379; set ZLINK_REDIS_TEST_ENDPOINT";

    redis_location_store_t store (*options);
    location_store_t &provider = store;
    const store_key_t key{"authority:actor-1"};
    const auto first = provider
                         .write ({.conditions = {store_missing_condition_t{key}},
                                  .mutations = {store_put_t{key, bytes ("ready"), 30s}}})
                         .result ()
                         .value ();
    const auto *applied = std::get_if<store_write_applied_t> (&first);
    ASSERT_NE (applied, nullptr);
    ASSERT_EQ (applied->put_versions.size (), 1u);

    const auto found = provider.read (key).result ().value ();
    const auto *value = std::get_if<store_found_t> (&found);
    ASSERT_NE (value, nullptr);
    EXPECT_EQ (value->value.bytes, bytes ("ready"));

    const auto conflict =
      provider
        .write ({.conditions = {store_version_condition_t{key, {.value = "stale"}}},
                 .mutations = {store_put_t{key, bytes ("wrong"), 30s}}})
        .result ()
        .value ();
    EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (conflict));

    const auto page = provider.scan ({.prefix = "authority:", .cursor = std::nullopt, .limit = 10})
                        .result ()
                        .value ();
    const auto *items = std::get_if<store_scan_page_t> (&page);
    ASSERT_NE (items, nullptr);
    ASSERT_EQ (items->items.size (), 1u);
    EXPECT_EQ (items->items.front ().key.value, key.value);

    const auto erased =
      provider
        .write ({.conditions = {store_version_condition_t{key, value->value.version}},
                 .mutations = {store_delete_t{key}}})
        .result ()
        .value ();
    EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (erased));
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (provider.read (key).result ().value ()));
}

TEST (ZLinkFrameworkLocationsRedis, ValueConditionUsesLiveBytesAtCommit)
{
    const auto options = find_redis_options ();
    if (!options)
        GTEST_SKIP () << "Redis is not reachable at 127.0.0.1:6379; set ZLINK_REDIS_TEST_ENDPOINT";

    redis_location_store_t store (*options);
    location_store_t &provider = store;
    const store_key_t key{"lease"};
    const store_key_t changed{"changed"};
    const auto put = [&] (std::string_view value) {
        return provider.write ({{}, {store_put_t{key, bytes (value), 30s}}}).result ().value ();
    };
    const auto conditional = [&] {
        return provider
          .write ({{store_value_condition_t{key, bytes ("owner")}},
                   {store_put_t{changed, bytes ("committed"), std::nullopt}}})
          .result ()
          .value ();
    };
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (put ("owner")));
    const auto initial = std::get<store_found_t> (provider.read (key).result ().value ());
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (put ("owner")));
    const auto renewed = std::get<store_found_t> (provider.read (key).result ().value ());
    EXPECT_NE (initial.value.version.value, renewed.value.version.value);
    EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (conditional ()));
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (put ("other")));
    EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (conditional ()));
    const auto before = std::get<store_found_t> (provider.read (changed).result ().value ());
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider.write ({{}, {store_delete_t{key}}}).result ().value ()));
    EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (conditional ()));
    const auto after = std::get<store_found_t> (provider.read (changed).result ().value ());
    EXPECT_EQ (before.value.version.value, after.value.version.value);
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider.write ({{}, {store_put_t{key, bytes ("owner"), 1ms}}}).result ().value ()));
    bool expired = false;
    for (int attempt = 0; attempt < 1000 && !expired; ++attempt)
        expired = std::holds_alternative<store_missing_t> (provider.read (key).result ().value ());
    ASSERT_TRUE (expired);
    EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (conditional ()));
    const auto after_expiry = std::get<store_found_t> (provider.read (changed).result ().value ());
    EXPECT_EQ (before.value.version.value, after_expiry.value.version.value);
}

// Sol review [M] (cpp-store-record-convergence e14bce0297): the production
// Lua point-read/CAS scripts must reject a member whose format tag isn't
// 0x01 explicitly (22-location-store-redis.md#7's clean-break requirement --
// see the C++-side equivalent check in test_cpp_framework_store_record_
// golden.cpp), not silently decode it as if it were tag-0x01. This drives
// that check against real Redis Lua (cmsgpack.unpack, string.byte), not just
// the C++ decode_opaque_value helper -- the two are independent decoders
// over the same wire format.
TEST (ZLinkFrameworkLocationsRedis, PointReadRejectsCorruptedFormatTag)
{
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
    const auto options = find_redis_options ();
    if (!options)
        GTEST_SKIP () << "Redis is not reachable at 127.0.0.1:6379; set ZLINK_REDIS_TEST_ENDPOINT";

    redis_location_store_t store (*options);
    location_store_t &provider = store;
    const store_key_t key{"authority:corrupted-tag"};

    // Establish the row through the real write path first (tag 0x01,
    // well-formed cmsgpack), then corrupt only the format-tag byte directly
    // in Redis -- exactly what a bit-flip or a foreign writer would produce.
    const auto put = provider
                       .write ({.conditions = {store_missing_condition_t{key}},
                                .mutations = {store_put_t{key, bytes ("payload"), std::nullopt}}})
                       .result ()
                       .value ();
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (put));
    ASSERT_TRUE (std::holds_alternative<store_found_t> (provider.read (key).result ().value ()));

    sw::redis::Redis raw (
      zlink::framework::redis::detail::normalize_connection_string (options->connection_string));
    const auto physical_key = options->key_prefix + ":{zlink-location-v3}:opaque:"
                              + zlink::framework::redis::detail::sha256_hex (key.value);
    std::vector<std::pair<std::string, double>> scored;
    raw.zrevrange (physical_key, 0, 0, std::back_inserter (scored));
    ASSERT_EQ (scored.size (), 1u);
    ASSERT_EQ (static_cast<std::uint8_t> (scored[0].first[0]), 0x01u);
    std::string corrupted = scored[0].first;
    corrupted[0] = static_cast<char> (0x02);
    raw.zrem (physical_key, scored[0].first);
    raw.zadd (physical_key, corrupted, scored[0].second + 1.0);

    const auto before = raw.info ("commandstats");
    const auto reread = provider.read (key).result ();
    const auto after = raw.info ("commandstats");
    EXPECT_FALSE (reread.has_value ())
      << "a corrupted (non-0x01) format tag must fail explicitly, not be "
         "silently decoded";
    EXPECT_EQ (command_calls (after, "evalsha") - command_calls (before, "evalsha"), 1);
    EXPECT_EQ (command_calls (after, "eval") - command_calls (before, "eval"), 0);
#else
    GTEST_SKIP () << "built without the async redis client";
#endif
}

TEST (ZLinkFrameworkLocationsRedis, OpaqueRelocationStoreKeepsPayloadImmutable)
{
    const auto location_options = find_redis_options ();
    if (!location_options)
        GTEST_SKIP () << "Redis is not reachable at 127.0.0.1:6379; set ZLINK_REDIS_TEST_ENDPOINT";

    redis_relocation_store_t store ({.connection_string = location_options->connection_string,
                                     .key_prefix = location_options->key_prefix + ":relocation"});
    relocation_store_t &provider = store;
    const blob_reference_t reference{"root-1"};
    const auto payload = bytes ("immutable-payload");

    EXPECT_TRUE (std::holds_alternative<blob_stored_t> (
      provider.put (reference, payload, 30s).result ().value ()));
    EXPECT_TRUE (std::holds_alternative<blob_already_stored_t> (
      provider.put (reference, payload, 30s).result ().value ()));
    EXPECT_TRUE (std::holds_alternative<blob_conflict_t> (
      provider.put (reference, bytes ("different"), 30s).result ().value ()));

    const auto read = provider.read (reference).result ().value ();
    const auto *found = std::get_if<blob_found_t> (&read);
    ASSERT_NE (found, nullptr);
    EXPECT_EQ (found->bytes, payload);
    EXPECT_TRUE (
      std::holds_alternative<blob_renewed_t> (provider.renew (reference, 60s).result ().value ()));

    ASSERT_TRUE (provider.erase (reference).result ().has_value ());
    EXPECT_TRUE (
      std::holds_alternative<blob_missing_t> (provider.read (reference).result ().value ()));
}

TEST (ZLinkFrameworkLocationsRedis, PrefixScanIncludesExactAndUtf8SuffixesOnly)
{
    const auto options = find_redis_options ();
    if (!options)
        GTEST_SKIP () << "Redis is not reachable at 127.0.0.1:6379; set ZLINK_REDIS_TEST_ENDPOINT";

    redis_location_store_t store (*options);
    location_store_t &provider = store;
    const std::vector<std::string> keys{
      "a", "mesh:", "mesh:a", "mesh:\xc3\xa9", "mesh:\xf4\x8f\xbf\xbf", "mesh;", "z"};
    store_write_request_t write;
    for (const auto &key : keys)
        write.mutations.emplace_back (store_put_t{{key}, bytes (key), std::nullopt});
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider.write (std::move (write)).result ().value ()));

    const auto scan = [&] (std::string prefix) {
        std::vector<std::string> found;
        store_scan_request_t request{.prefix = std::move (prefix), .limit = 1};
        do {
            const auto result = provider.scan (request).result ().value ();
            const auto &page = std::get<store_scan_page_t> (result);
            for (const auto &item : page.items)
                found.push_back (item.key.value);
            request.cursor = page.next_cursor;
        } while (request.cursor);
        return found;
    };
    EXPECT_EQ (scan ("mesh:"), (std::vector<std::string>{keys[1], keys[2], keys[3], keys[4]}));
    EXPECT_EQ (scan ("mesh:\xc3\xa9"), (std::vector<std::string>{keys[3]}));
    EXPECT_TRUE (scan ("missing:").empty ());
    EXPECT_EQ (scan (""), keys);
}

TEST (ZLinkFrameworkLocationsRedis, SnapshotRetainsValuesAcrossImmediateVersionPruning)
{
    const auto options = find_redis_options ();
    if (!options)
        GTEST_SKIP () << "Redis is not reachable at 127.0.0.1:6379; set ZLINK_REDIS_TEST_ENDPOINT";

    redis_location_store_t store (*options);
    location_store_t &provider = store;
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider
        .write ({{},
                 {store_put_t{{"mesh:a"}, bytes ("old-a"), std::nullopt},
                  store_put_t{{"mesh:b"}, bytes ("old-b"), std::nullopt}}})
        .result ()
        .value ()));
    const auto first = std::get<store_scan_page_t> (
      provider.scan ({.prefix = "mesh:", .limit = 1}).result ().value ());
    ASSERT_TRUE (first.next_cursor);
    for (int version = 0; version < 128; ++version) {
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          provider
            .write ({{}, {store_put_t{{"mesh:b"}, bytes (std::to_string (version)), std::nullopt}}})
            .result ()
            .value ()));
    }
    const auto next = std::get<store_scan_page_t> (
      provider.scan ({.prefix = "mesh:", .cursor = first.next_cursor, .limit = 1})
        .result ()
        .value ());
    ASSERT_EQ (next.items.size (), 1u);
    EXPECT_EQ (next.items[0].value.bytes, bytes ("old-b"));
    EXPECT_FALSE (next.next_cursor);
    const auto current = std::get<store_found_t> (provider.read ({"mesh:b"}).result ().value ());
    EXPECT_EQ (current.value.bytes, bytes ("127"));
}

TEST (ZLinkFrameworkLocationsRedis, SnapshotUsesRedisClockAndKeepsItAcrossPages)
{
    const auto options = find_redis_options ();
    if (!options)
        GTEST_SKIP () << "Redis is not reachable at 127.0.0.1:6379; set ZLINK_REDIS_TEST_ENDPOINT";

    redis_location_store_t store (*options);
    location_store_t &provider = store;
    const store_key_t expires_later{"mesh:a"};
    const store_key_t never_expires{"mesh:b"};
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider
        .write ({{},
                 {store_put_t{expires_later, bytes ("a"), 5min},
                  store_put_t{never_expires, bytes ("b"), std::nullopt}}})
        .result ()
        .value ()));

    const auto first = std::get<store_scan_page_t> (
      provider.scan ({.prefix = "mesh:", .limit = 1}).result ().value ());
    ASSERT_EQ (first.items.size (), 1u);
    EXPECT_EQ (first.items.front ().key.value, expires_later.value);
    ASSERT_TRUE (first.next_cursor);

    const auto second = std::get<store_scan_page_t> (
      provider.scan ({.prefix = "mesh:", .cursor = first.next_cursor, .limit = 1})
        .result ()
        .value ());
    ASSERT_EQ (second.items.size (), 1u);
    EXPECT_EQ (second.items.front ().key.value, never_expires.value);
    EXPECT_EQ (second.store_now, first.store_now);
}

} // namespace
