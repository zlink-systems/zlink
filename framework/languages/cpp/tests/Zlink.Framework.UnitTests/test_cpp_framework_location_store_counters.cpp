/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/in_memory_store_providers.hpp"

#include <gtest/gtest.h>

#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
#include "../support/redis_location_options.hpp"
#endif
#if defined(ZLINK_TEST_LOCATION_COUNTERS_ETCD)
#include <zlink/locations/etcd.hpp>
#endif

#include <array>
#include <barrier>
#include <cstdlib>
#include <limits>
#include <thread>

namespace
{
using namespace zlink::framework;
using zlink::framework::runtime::in_memory_location_store_t;
using namespace std::chrono_literals;

enum class provider_t
{
    memory,
    redis,
    etcd
};

std::vector<std::byte> bytes (std::string_view text)
{
    const auto data = std::as_bytes (std::span (text));
    return {data.begin (), data.end ()};
}

class LocationStoreCounters : public testing::TestWithParam<provider_t>
{
  protected:
    void SetUp () override
    {
        switch (GetParam ()) {
            case provider_t::memory:
                store = std::make_unique<in_memory_location_store_t> ();
                return;
            case provider_t::redis:
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
                if (const auto options = test::find_redis_options ()) {
                    store = std::make_unique<redis::redis_location_store_t> (*options);
                    return;
                }
#endif
                GTEST_SKIP () << "Set ZLINK_REDIS_TEST_ENDPOINT with Redis support enabled";
            case provider_t::etcd:
#if defined(ZLINK_TEST_LOCATION_COUNTERS_ETCD)
                if (const auto *endpoint = std::getenv ("ZLINK_ETCD_ENDPOINTS");
                    endpoint && std::getenv ("ZLINK_ETCD_NAMESPACE")) {
                    etcd::etcd_location_options_t options;
                    options.key_namespace = std::getenv ("ZLINK_ETCD_NAMESPACE");
                    std::string_view remaining (endpoint);
                    while (!remaining.empty ()) {
                        const auto comma = remaining.find (',');
                        options.endpoints.emplace_back (remaining.substr (0, comma));
                        remaining.remove_prefix (comma == std::string_view::npos ? remaining.size ()
                                                                                 : comma + 1);
                    }
                    store = std::make_unique<etcd::etcd_location_store_t> (options);
                    return;
                }
#endif
                GTEST_SKIP () << "Set ZLINK_ETCD_ENDPOINTS and ZLINK_ETCD_NAMESPACE";
        }
    }

    store_key_t key (const std::string &name) const { return {prefix + name}; }

    store_write_result_t write (store_write_request_t request)
    {
        return store->write (std::move (request)).result ().value ();
    }

    store_read_result_t read (const store_key_t &key)
    {
        return store->read (key).result ().value ();
    }

    void put (const store_key_t &key, std::string_view value)
    {
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          write ({{}, {store_put_t{key, bytes (value), std::nullopt}}})));
    }

    std::unique_ptr<location_store_t> store;
    const std::string prefix =
      "cpp-counters/"
      + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ()) + "/";
};

TEST_P (LocationStoreCounters, SumAtMostIncludesMissingKeysAndUsesExactArithmetic)
{
    const auto active = key ("active");
    const auto pending = key ("pending");
    const auto maximum = std::numeric_limits<std::int64_t>::max ();
    put (active, "9223372036854775806");
    auto result = write (
      {{store_sum_at_most_condition_t{{active, pending}, 1, maximum}}, {store_add_t{pending, 1}}});
    const auto *applied = std::get_if<store_write_applied_t> (&result);
    ASSERT_NE (applied, nullptr);
    ASSERT_EQ (applied->put_versions.size (), 1u);
    const auto before = std::get<store_found_t> (read (pending)).value;
    EXPECT_EQ (before.bytes, bytes ("1"));
    EXPECT_EQ (before.version.value, applied->put_versions.front ().version.value);
    EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (
      write ({{store_sum_at_most_condition_t{{active, pending}, 1, maximum}},
              {store_add_t{pending, 1}}})));
    const auto after = std::get<store_found_t> (read (pending)).value;
    EXPECT_EQ (after.bytes, before.bytes);
    EXPECT_EQ (after.version.value, before.version.value);

    put (active, "9223372036854775807");
    put (pending, "9223372036854775807");
    EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (
      write ({{store_sum_at_most_condition_t{{active, pending}, -maximum, maximum}}, {}})));
}

TEST_P (LocationStoreCounters, CorruptBatchLeavesEveryValueAndVersionUntouched)
{
    const auto counter = key ("counter");
    const auto opaque = key ("opaque");
    const auto created = key ("created");
    put (counter, "1");
    put (opaque, "opaque bytes");
    const auto before = std::get<store_found_t> (read (counter)).value;
    const auto opaque_before = std::get<store_found_t> (read (opaque)).value;
    EXPECT_TRUE (std::holds_alternative<store_write_corrupt_t> (
      write ({{},
              {store_put_t{created, bytes ("new"), std::nullopt}, store_delete_t{opaque},
               store_add_t{counter, -2}}})));
    const auto after = std::get<store_found_t> (read (counter)).value;
    EXPECT_EQ (after.bytes, before.bytes);
    EXPECT_EQ (after.version.value, before.version.value);
    const auto opaque_after = std::get<store_found_t> (read (opaque)).value;
    EXPECT_EQ (opaque_after.bytes, opaque_before.bytes);
    EXPECT_EQ (opaque_after.version.value, opaque_before.version.value);
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (read (created)));

    auto result = write ({{}, {store_add_t{counter, -1}}});
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (result));
    const auto zero = std::get<store_found_t> (read (counter)).value;
    EXPECT_EQ (zero.bytes, bytes ("0"));
    EXPECT_NE (zero.version.value, before.version.value);
    // Isolated namespaces give both Redis and memory consecutive write versions.
    if (GetParam () != provider_t::etcd)
        EXPECT_EQ (std::stoull (zero.version.value), std::stoull (opaque_before.version.value) + 1);
}

TEST_P (LocationStoreCounters, RejectsNonCanonicalValuesOnlyForCounterOperations)
{
    const auto counter = key ("counter");
    for (const auto invalid : {"text", "", "01", "+1", "-1", "9223372036854775808"}) {
        SCOPED_TRACE (invalid);
        put (counter, invalid);
        EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (
          write ({{store_value_condition_t{counter, bytes (invalid)}}, {}})));
        EXPECT_TRUE (std::holds_alternative<store_write_corrupt_t> (
          write ({{store_sum_at_most_condition_t{{counter}, 0, 10}}, {}})));
        EXPECT_TRUE (
          std::holds_alternative<store_write_corrupt_t> (write ({{}, {store_add_t{counter, 0}}})));
    }
    put (counter, "9223372036854775807");
    EXPECT_TRUE (
      std::holds_alternative<store_write_corrupt_t> (write ({{}, {store_add_t{counter, 1}}})));
    EXPECT_TRUE (std::holds_alternative<store_write_corrupt_t> (
      write ({{}, {store_add_t{counter, std::numeric_limits<std::int64_t>::min ()}}})));
}

TEST_P (LocationStoreCounters, ConcurrentAddOnlyWritesBothApply)
{
    const auto counter = key ("counter");
    std::barrier ready (2);
    using task_result_t = result_t<store_write_result_t>;
    std::array<std::optional<task_result_t>, 2> results;
    auto add = [&] (std::size_t index) {
        ready.arrive_and_wait ();
        results[index] = store->write ({{}, {store_add_t{counter, 1}}}).result ();
    };
    std::jthread first (add, 0);
    std::jthread second (add, 1);
    first.join ();
    second.join ();
    for (const auto &result : results) {
        ASSERT_TRUE (result && result->has_value ());
        EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (result->value ()));
    }
    EXPECT_EQ (std::get<store_found_t> (read (counter)).value.bytes, bytes ("2"));
}

TEST_P (LocationStoreCounters, CountsUniqueKeysAndRejectsRepeatedConditionKeys)
{
    store_sum_at_most_condition_t sum{{}, 0, 1};
    for (std::size_t index = 0; index != 2048; ++index)
        sum.keys.push_back (key (std::to_string (index)));
    EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (
      write ({{sum}, {store_add_t{sum.keys.front (), 1}}})));
    EXPECT_THROW ((void) write ({{sum}, {store_add_t{key ("extra"), 1}}}), std::invalid_argument);
    EXPECT_THROW ((void) write ({{sum, store_missing_condition_t{sum.keys.back ()}}, {}}),
                  std::invalid_argument);
    sum.keys.push_back (sum.keys.front ());
    EXPECT_THROW ((void) write ({{sum}, {}}), std::invalid_argument);
    sum.keys.back () = key ("extra");
    EXPECT_THROW ((void) write ({{sum}, {}}), std::invalid_argument);
}

TEST_P (LocationStoreCounters, CountsEncodedCounterOperands)
{
    constexpr std::size_t mebibyte = 1024 * 1024;
    const auto counter = key ("counter");
    store_write_request_t request{{store_sum_at_most_condition_t{{counter}, 0, 1}},
                                  {store_add_t{counter, 1}}};
    std::size_t overhead = 2 * counter.value.size () + 3;
    for (int index = 0; index < 4; ++index) {
        auto opaque = key (std::to_string (index));
        overhead += opaque.value.size ();
        request.mutations.emplace_back (
          store_put_t{std::move (opaque), std::vector<std::byte> (mebibyte), std::nullopt});
    }
    auto &last = std::get<store_put_t> (request.mutations.back ()).bytes;
    last.resize (mebibyte - overhead);
    EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (write (request)));
    last.push_back (std::byte{0});
    EXPECT_THROW ((void) write (request), std::invalid_argument);
}

INSTANTIATE_TEST_SUITE_P (InMemory, LocationStoreCounters, testing::Values (provider_t::memory));
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
INSTANTIATE_TEST_SUITE_P (Redis, LocationStoreCounters, testing::Values (provider_t::redis));
#endif
#if defined(ZLINK_TEST_LOCATION_COUNTERS_ETCD)
INSTANTIATE_TEST_SUITE_P (Etcd, LocationStoreCounters, testing::Values (provider_t::etcd));
#endif
} // namespace
