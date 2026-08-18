/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/locations/redis.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

namespace
{

using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::redis;

std::vector<std::byte> bytes (std::string_view value)
{
    const auto *first =
      reinterpret_cast<const std::byte *> (value.data ());
    return {first, first + value.size ()};
}

std::string unique_prefix ()
{
    return "zlink:cpp:opaque-redis-test:"
           + std::to_string (
             std::chrono::steady_clock::now ()
               .time_since_epoch ()
               .count ());
}

std::vector<std::string> redis_test_endpoints ()
{
    std::vector<std::string> endpoints;
    if (const auto *endpoint =
          std::getenv ("ZLINK_REDIS_TEST_ENDPOINT"))
        endpoints.emplace_back (endpoint);
    endpoints.emplace_back ("tcp://127.0.0.1:16379");
    endpoints.emplace_back ("tcp://127.0.0.1:6379");
    return endpoints;
}

std::optional<redis_location_options_t>
find_redis_options ()
{
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
    for (const auto &endpoint : redis_test_endpoints ()) {
        redis_location_options_t options{
          .connection_string = endpoint,
          .key_prefix = unique_prefix ()};
        redis_location_store_t store (options);
        location_store_t &provider = store;
        const auto probe =
          provider.read ({.value = "probe"}).result ();
        if (probe.has_value ())
            return options;
    }
#endif
    return std::nullopt;
}

TEST (ZLinkFrameworkLocationsRedis,
      PublicTypesImplementOnlyTheOpaqueStoreContracts)
{
    static_assert (
      std::is_base_of_v<location_store_t,
                        redis_location_store_t>);
    static_assert (
      std::is_base_of_v<relocation_store_t,
                        redis_relocation_store_t>);
    static_assert (
      !std::is_default_constructible_v<
        redis_location_store_t>);
    static_assert (
      !std::is_default_constructible_v<
        redis_relocation_store_t>);

    EXPECT_NO_THROW (redis_location_store_t (
      {.connection_string = "tcp://127.0.0.1:6379",
       .key_prefix = "zlink:test:location"}));
    EXPECT_NO_THROW (redis_relocation_store_t (
      {.connection_string = "tcp://127.0.0.1:6379",
       .key_prefix = "zlink:test:relocation"}));
}

TEST (ZLinkFrameworkLocationsRedis,
      InvalidConfigurationIsRejected)
{
    EXPECT_THROW (
      redis_location_store_t ({
        .connection_string = "",
        .key_prefix = "zlink:test"}),
      std::invalid_argument);
    EXPECT_THROW (
      redis_relocation_store_t ({
        .connection_string = "tcp://127.0.0.1:6379",
        .key_prefix = ""}),
      std::invalid_argument);
    EXPECT_THROW (
      redis_location_store_t ({
        .connection_string = "tcp://127.0.0.1:6379",
        .key_prefix = "zlink:test",
        .operation_timeout = 0ms}),
      std::invalid_argument);
    EXPECT_THROW (
      redis_relocation_store_t ({
        .connection_string = "tcp://127.0.0.1:6379",
        .key_prefix = "zlink:test",
        .operation_timeout = 0ms}),
      std::invalid_argument);
}

TEST (ZLinkFrameworkLocationsRedis,
      OpaqueLocationStoreProvidesAtomicCasAndScan)
{
    const auto options = find_redis_options ();
    if (!options)
        GTEST_SKIP ()
          << "Redis is not reachable; set ZLINK_REDIS_TEST_ENDPOINT";

    redis_location_store_t store (*options);
    location_store_t &provider = store;
    const store_key_t key{"authority:actor-1"};
    const auto first =
      provider
        .write ({
          .conditions = {
            store_missing_condition_t{key}},
          .mutations = {
            store_put_t{key, bytes ("ready"), 30s}}})
        .result ()
        .value ();
    const auto *applied =
      std::get_if<store_write_applied_t> (&first);
    ASSERT_NE (applied, nullptr);
    ASSERT_EQ (applied->put_versions.size (), 1u);

    const auto found =
      provider.read (key).result ().value ();
    const auto *value =
      std::get_if<store_found_t> (&found);
    ASSERT_NE (value, nullptr);
    EXPECT_EQ (value->value.bytes, bytes ("ready"));

    const auto conflict =
      provider
        .write ({
          .conditions = {
            store_version_condition_t{
              key, {.value = "stale"}}},
          .mutations = {
            store_put_t{key, bytes ("wrong"), 30s}}})
        .result ()
        .value ();
    EXPECT_TRUE (
      std::holds_alternative<store_write_conflict_t> (
        conflict));

    const auto page =
      provider
        .scan ({
          .prefix = "authority:",
          .cursor = std::nullopt,
          .limit = 10})
        .result ()
        .value ();
    const auto *items =
      std::get_if<store_scan_page_t> (&page);
    ASSERT_NE (items, nullptr);
    ASSERT_EQ (items->items.size (), 1u);
    EXPECT_EQ (items->items.front ().key.value,
               key.value);

    const auto erased =
      provider
        .write ({
          .conditions = {
            store_version_condition_t{
              key, value->value.version}},
          .mutations = {store_delete_t{key}}})
        .result ()
        .value ();
    EXPECT_TRUE (
      std::holds_alternative<store_write_applied_t> (
        erased));
    EXPECT_TRUE (
      std::holds_alternative<store_missing_t> (
        provider.read (key).result ().value ()));
}

// Sol review [M] (cpp-store-record-convergence e14bce0297): the production
// Lua point-read/CAS scripts must reject a member whose format tag isn't
// 0x01 explicitly (22-location-store-redis.md#7's clean-break requirement --
// see the C++-side equivalent check in test_cpp_framework_store_record_
// golden.cpp), not silently decode it as if it were tag-0x01. This drives
// that check against real Redis Lua (cmsgpack.unpack, string.byte), not just
// the C++ decode_opaque_value helper -- the two are independent decoders
// over the same wire format.
TEST (ZLinkFrameworkLocationsRedis,
      PointReadRejectsCorruptedFormatTag)
{
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
    const auto options = find_redis_options ();
    if (!options)
        GTEST_SKIP ()
          << "Redis is not reachable; set ZLINK_REDIS_TEST_ENDPOINT";

    redis_location_store_t store (*options);
    location_store_t &provider = store;
    const store_key_t key{"authority:corrupted-tag"};

    // Establish the row through the real write path first (tag 0x01,
    // well-formed cmsgpack), then corrupt only the format-tag byte directly
    // in Redis -- exactly what a bit-flip or a foreign writer would produce.
    const auto put =
      provider
        .write ({.conditions = {store_missing_condition_t{key}},
                 .mutations = {store_put_t{key, bytes ("payload"), std::nullopt}}})
        .result ()
        .value ();
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (put));

    sw::redis::Redis raw (
      zlink::framework::redis::detail::normalize_connection_string (
        options->connection_string));
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

    const auto reread = provider.read (key).result ();
    EXPECT_FALSE (reread.has_value ())
      << "a corrupted (non-0x01) format tag must fail explicitly, not be "
         "silently decoded";
#else
    GTEST_SKIP () << "built without the async redis client";
#endif
}

TEST (ZLinkFrameworkLocationsRedis,
      OpaqueRelocationStoreKeepsPayloadImmutable)
{
    const auto location_options = find_redis_options ();
    if (!location_options)
        GTEST_SKIP ()
          << "Redis is not reachable; set ZLINK_REDIS_TEST_ENDPOINT";

    redis_relocation_store_t store ({
      .connection_string =
        location_options->connection_string,
      .key_prefix =
        location_options->key_prefix + ":relocation"});
    relocation_store_t &provider = store;
    const blob_reference_t reference{"root-1"};
    const auto payload = bytes ("immutable-payload");

    EXPECT_TRUE (
      std::holds_alternative<blob_stored_t> (
        provider.put (reference, payload, 30s)
          .result ()
          .value ()));
    EXPECT_TRUE (
      std::holds_alternative<blob_already_stored_t> (
        provider.put (reference, payload, 30s)
          .result ()
          .value ()));
    EXPECT_TRUE (
      std::holds_alternative<blob_conflict_t> (
        provider.put (reference, bytes ("different"), 30s)
          .result ()
          .value ()));

    const auto read =
      provider.read (reference).result ().value ();
    const auto *found =
      std::get_if<blob_found_t> (&read);
    ASSERT_NE (found, nullptr);
    EXPECT_EQ (found->bytes, payload);
    EXPECT_TRUE (
      std::holds_alternative<blob_renewed_t> (
        provider.renew (reference, 60s)
          .result ()
          .value ()));

    ASSERT_TRUE (
      provider.erase (reference).result ().has_value ());
    EXPECT_TRUE (
      std::holds_alternative<blob_missing_t> (
        provider.read (reference).result ().value ()));
}

} // namespace
