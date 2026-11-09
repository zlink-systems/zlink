/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "encoding.hpp"
#include "fake_etcd.hpp"

#include <google/protobuf/unknown_field_set.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iterator>
#include <locale>
#include <new>
#include <semaphore>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::etcd;
using namespace etcd_test;
namespace encoding = zlink::framework::etcd::detail;

thread_local bool fail_external_capture_once = false;
const zlink::framework::detail::ambient_context_hooks_t failing_capture_hooks{
  [] () -> std::shared_ptr<void> {
      if (fail_external_capture_once && !zlink::framework::detail::current_infrastructure_wait ()) {
          fail_external_capture_once = false;
          throw std::bad_alloc ();
      }
      return {};
  },
  [] (const std::shared_ptr<void> &state) { return state; }};

template <typename T> T await (task_t<T> task)
{
    const auto result = task.result_for (10s);
    if (!result)
        throw std::runtime_error ("etcd test operation did not complete within 10 seconds");
    return result->value ();
}

std::vector<std::byte> bytes (std::string_view input)
{
    const auto *begin = reinterpret_cast<const std::byte *> (input.data ());
    return {begin, begin + input.size ()};
}

std::string hex (std::string_view text)
{
    std::string result;
    for (std::size_t i = 0; i < text.size (); i += 2)
        result += static_cast<char> (std::stoul (std::string (text.substr (i, 2)), nullptr, 16));
    return result;
}

store_write_request_t put (std::string key,
                           std::string_view value = {},
                           std::optional<std::chrono::milliseconds> retention = {},
                           std::vector<store_condition_t> conditions = {})
{
    return {.conditions = std::move (conditions),
            .mutations = {store_put_t{{std::move (key)}, bytes (value), retention}}};
}

store_found_t found (etcd_location_store_t &store, const std::string &key = "k")
{
    return std::get<store_found_t> (await (store.read ({key})));
}

nlohmann::json golden ()
{
    std::ifstream input (ZLINK_ETCD_GOLDEN_PATH);
    if (!input)
        throw std::runtime_error ("etcd golden fixture was not found");
    return nlohmann::json::parse (input);
}

TEST (EtcdGolden, VersionEncodingDoesNotUseApplicationLocale)
{
    struct grouped_t : std::numpunct<char>
    {
        char do_thousands_sep () const override { return '_'; }
        std::string do_grouping () const override { return "\3"; }
    };
    struct locale_guard_t
    {
        std::locale previous = std::locale ();
        ~locale_guard_t () { std::locale::global (previous); }
    } guard;
    std::locale::global (std::locale (guard.previous, new grouped_t));
    EXPECT_EQ (encoding::encode_version (0xfedcba9876543210ULL, 1234567),
               "e1:fedcba9876543210:1234567");
}

std::string pem (const char *name)
{
    std::ifstream input (std::string (ZLINK_ETCD_TLS_DIR) + "/" + name);
    if (!input)
        throw std::runtime_error ("etcd TLS fixture was not found");
    return {std::istreambuf_iterator<char> (input), {}};
}

TEST (EtcdGolden, AllKeysClocksMetadataAndVersions)
{
    const auto vectors = golden ();
    for (const auto &item : vectors.at ("keyVectors")) {
        SCOPED_TRACE (item.at ("name").get<std::string> ());
        const auto ns = item.at ("namespace").get<std::string> ();
        const auto suffix = hex (item.at ("frameworkKeyHex").get<std::string> ());
        EXPECT_EQ (encoding::physical_key (ns, 'm', suffix),
                   hex (item.at ("metadataKeyHex").get<std::string> ()));
        EXPECT_EQ (encoding::physical_key (ns, 'p', suffix),
                   hex (item.at ("payloadKeyHex").get<std::string> ()));
        EXPECT_EQ (encoding::physical_key (ns, 'c'),
                   hex (item.at ("clockKeyHex").get<std::string> ()));
    }
    for (const auto &item : vectors.at ("clockVectors")) {
        const auto value = std::stoull (item.at ("value").get<std::string> ());
        const auto encoded = hex (item.at ("hex").get<std::string> ());
        EXPECT_EQ (encoding::encode_clock (value), encoded);
        EXPECT_EQ (encoding::decode_clock (encoded), value);
    }
    for (const auto &item : vectors.at ("metadataVectors")) {
        SCOPED_TRACE (item.at ("name").get<std::string> ());
        const encoding::metadata_t expected{
          item.at ("hasExpiry").get<bool> (),
          std::stoull (item.at ("expiresAtMs").get<std::string> ()),
          item.at ("payloadLength").get<std::uint32_t> ()};
        const auto encoded = hex (item.at ("metadataHex").get<std::string> ());
        EXPECT_EQ (encoding::encode_metadata (expected), encoded);
        const auto decoded = encoding::decode_metadata (encoded);
        EXPECT_EQ (decoded.has_expiry, expected.has_expiry);
        EXPECT_EQ (decoded.expires_at, expected.expires_at);
        EXPECT_EQ (decoded.payload_length, expected.payload_length);
    }
    for (const auto &item : vectors.at ("storeVersions").at ("accepted")) {
        const auto cluster = std::stoull (item.at ("clusterIdDecimal").get<std::string> ());
        const auto revision = std::stoll (item.at ("modRevision").get<std::string> ());
        const auto value = item.at ("value").get<std::string> ();
        EXPECT_EQ (encoding::encode_version (cluster, revision), value);
    }
}

TEST (EtcdGolden, EveryVersionVectorUsesOpaqueEqualityThroughPublicWrite)
{
    const auto vectors = golden ().at ("storeVersions");
    for (const auto &item : vectors.at ("accepted")) {
        const auto expected = item.at ("value").get<std::string> ();
        SCOPED_TRACE (expected);
        fake_etcd_t backend;
        backend.cluster = std::stoull (item.at ("clusterIdDecimal").get<std::string> ());
        const auto revision = std::stoll (item.at ("modRevision").get<std::string> ());
        // raw_record isolates version equality from revision exhaustion. Its
        // metadata revision may exceed this fake server's transaction revision.
        backend.raw_record (metadata (false, 0, 1), "a", revision, revision);
        etcd_location_store_t store (backend.options ());
        const auto result = await (
          store.write (put ("x", "applied", {}, {store_version_condition_t{{"k"}, {expected}}})));
        EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (result));
        EXPECT_TRUE (backend.get (key ("test", 'm', "x")));
        const auto payload = backend.get (key ("test", 'p', "x"));
        ASSERT_TRUE (payload);
        EXPECT_EQ (payload->value (), "applied");
    }

    fake_etcd_t backend;
    const auto &canonical = vectors.at ("accepted").at (1);
    backend.cluster = std::stoull (canonical.at ("clusterIdDecimal").get<std::string> ());
    const auto revision = std::stoll (canonical.at ("modRevision").get<std::string> ());
    backend.raw_record (metadata (false, 0, 1), "a", revision, revision);
    etcd_location_store_t store (backend.options ());
    for (const auto &item : vectors.at ("rejected")) {
        const auto expected = item.get<std::string> ();
        SCOPED_TRACE (expected);
        const auto before = backend.revision;
        const auto result = await (store.write (
          put ("x", "must-not-apply", {}, {store_version_condition_t{{"k"}, {expected}}})));
        EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (result));
        EXPECT_EQ (backend.revision, before);
        EXPECT_FALSE (backend.get (key ("test", 'm', "x")));
        EXPECT_FALSE (backend.get (key ("test", 'p', "x")));
    }
}

TEST (EtcdGolden, EveryCorruptRecordFailsThroughPublicRead)
{
    fake_etcd_t backend;
    etcd_location_store_t store (backend.options ());
    const auto vectors = golden ();
    for (const auto &item : vectors.at ("corruptRecords")) {
        SCOPED_TRACE (item.at ("name").get<std::string> ());
        backend.put (key ("test", 'c'), item.contains ("clockHex")
                                          ? hex (item.at ("clockHex").get<std::string> ())
                                          : clock_bytes (1000));
        const auto field = [&] (const char *name) -> std::optional<std::string> {
            return item.contains (name) && !item.at (name).is_null ()
                     ? std::optional<std::string> (hex (item.at (name).get<std::string> ()))
                     : std::nullopt;
        };
        backend.raw_record (field ("metadataHex"), field ("payloadHex"),
                            item.contains ("metadataModRevision")
                              ? std::stoll (item.at ("metadataModRevision").get<std::string> ())
                              : 2,
                            item.contains ("payloadModRevision")
                              ? std::stoll (item.at ("payloadModRevision").get<std::string> ())
                              : 2);
        EXPECT_THROW (await (store.read ({"k"})), framework_exception_t);
    }
}

TEST (EtcdProvider, PublicContractAndStoreClockConditions)
{
    static_assert (std::is_base_of_v<location_store_t, etcd_location_store_t>);
    static_assert (!std::is_default_constructible_v<etcd_location_store_t>);
    fake_etcd_t backend;
    etcd_location_store_t store (backend.options ());
    const auto missing = std::get<store_missing_t> (await (store.read ({"k"})));
    EXPECT_EQ (missing.store_now, std::chrono::system_clock::time_point (1000ms));
    const std::string data{"\0\xff", 2};
    const auto written = std::get<store_write_applied_t> (
      await (store.write (put ("k", data, 1ms, {store_missing_condition_t{{"k"}}}))));
    const auto value = found (store).value;
    EXPECT_EQ (value.bytes, bytes (data));
    EXPECT_EQ (value.store_now, missing.store_now);
    EXPECT_EQ (value.expires_at, std::chrono::system_clock::time_point (1001ms));
    ASSERT_EQ (written.put_versions.size (), 1U);
    EXPECT_EQ (written.put_versions[0].version.value, value.version.value);
    for (const auto &expected : {std::string ("xx"), std::string ("x")}) {
        EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (await (store.write (
          put ("other", {}, {}, {store_value_condition_t{{"k"}, bytes (expected)}})))));
    }
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (await (store.read ({"other"}))));
    EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (
      await (store.write (put ("other", {}, {}, {store_value_condition_t{{"k"}, value.bytes}})))));
    backend.clock (1001);
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (await (store.read ({"k"}))));
    EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (
      await (store.write (put ("k", {}, {}, {store_version_condition_t{{"k"}, value.version}})))));
    EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (
      await (store.write (put ("k", {}, {}, {store_missing_condition_t{{"k"}}})))));
}

TEST (EtcdProvider, MaximumSendLimitUsesStoreFailureBeforeServerDispatch)
{
    {
        SCOPED_TRACE ("15 MiB authentication request is within the send limit");
        fake_etcd_t backend;
        auto options = backend.options ();
        options.credentials = etcd_credentials_t{"user", std::string (15 * 1024 * 1024, 'p')};
        etcd_location_store_t store (std::move (options));
        const auto result = await (store.read ({"k"}));
        EXPECT_TRUE (std::holds_alternative<store_missing_t> (result));
        EXPECT_EQ (backend.auth_calls, 1);
        EXPECT_EQ (backend.txn_calls, 1);
    }
    fake_etcd_t backend;
    auto options = backend.options ();
    options.credentials = etcd_credentials_t{"user", std::string (17 * 1024 * 1024, 'p')};
    etcd_location_store_t store (std::move (options));
    const auto result = store.read ({"k"}).result_for (10s);
    ASSERT_TRUE (result) << "Oversized authentication RPC did not complete";
    EXPECT_EQ (backend.auth_calls, 0);
    EXPECT_EQ (backend.txn_calls, 0);
    ASSERT_FALSE (result->has_value ()) << "The provider's native send limit must reject the RPC";
    EXPECT_THROW (result->value (), framework_exception_t);
    ASSERT_NE (result->error (), nullptr);
    EXPECT_EQ (result->error_kind (), framework_error_kind_t::internal_failure);
}

TEST (EtcdProvider, ReceiveLimitAcceptsFiveMiBAndRejectsNineMiBAuthenticationResponses)
{
    for (const std::size_t padding_mib : {5U, 9U}) {
        SCOPED_TRACE ("authentication response padding MiB=" + std::to_string (padding_mib));
        fake_etcd_t backend;
        backend.auth_fault = [padding_mib] (auto *, AuthenticateResponse *response) {
            auto *unknown = response->GetReflection ()->MutableUnknownFields (response);
            const std::string padding (padding_mib * 1024 * 1024, 'p');
            unknown->AddLengthDelimited (1000, padding);
            return grpc::Status::OK;
        };
        auto options = backend.options ();
        options.credentials = etcd_credentials_t{"u", "p"};
        etcd_location_store_t store (std::move (options));
        const auto result = store.read ({"k"}).result_for (10s);
        ASSERT_TRUE (result) << "Padded authentication RPC did not complete";
        EXPECT_EQ (backend.auth_calls, 1);
        if (padding_mib == 5) {
            ASSERT_TRUE (result->has_value ()) << "The 8 MiB receive limit must accept 5 MiB";
            EXPECT_TRUE (std::holds_alternative<store_missing_t> (result->value ()));
            EXPECT_EQ (backend.txn_calls, 1);
        } else {
            ASSERT_FALSE (result->has_value ()) << "The 8 MiB receive limit must reject 9 MiB";
            EXPECT_THROW (result->value (), framework_exception_t);
            ASSERT_NE (result->error (), nullptr);
            EXPECT_EQ (result->error_kind (), framework_error_kind_t::internal_failure);
            EXPECT_EQ (backend.txn_calls, 0);
        }
    }
}

TEST (EtcdProvider, AuthenticationRefreshesOnlyOnceForRecognizedDescriptions)
{
    for (const auto code :
         {grpc::StatusCode::UNAUTHENTICATED, grpc::StatusCode::INVALID_ARGUMENT}) {
        fake_etcd_t backend;
        auto options = backend.options ();
        options.credentials = etcd_credentials_t{"user", "password"};
        etcd_location_store_t store (options);
        const auto description = code == grpc::StatusCode::UNAUTHENTICATED
                                   ? "etcdserver: invalid auth token"
                                   : "etcdserver: revision of auth store is old";
        backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
            return backend.txn_calls == 1 ? grpc::Status (code, description)
                                          : backend.transact (request, response);
        };
        EXPECT_TRUE (std::holds_alternative<store_missing_t> (await (store.read ({"k"}))));
        EXPECT_EQ (backend.auth_calls, 2);
        EXPECT_EQ (backend.txn_calls, 2);
        backend.txn_fault = [=] (auto *, const auto &, auto *) {
            return grpc::Status (code, description);
        };
        EXPECT_THROW (await (store.read ({"k"})), framework_exception_t);
        EXPECT_EQ (backend.auth_calls, 3);
        EXPECT_EQ (backend.txn_calls, 4);
    }
    fake_etcd_t backend;
    auto options = backend.options ();
    options.credentials = etcd_credentials_t{"user", "password"};
    etcd_location_store_t store (options);
    backend.txn_fault = [] (auto *, const auto &, auto *) {
        return grpc::Status (grpc::StatusCode::UNAUTHENTICATED, "permission denied");
    };
    EXPECT_THROW (await (store.read ({"k"})), framework_exception_t);
    EXPECT_EQ (backend.txn_calls, 1);
    EXPECT_EQ (backend.auth_calls, 1);
}

TEST (EtcdProvider, LostWriteIsNotReplayedAndNextOperationMovesEndpoint)
{
    fake_etcd_t first, second;
    auto options = first.options ();
    options.endpoints.push_back (second.endpoint ());
    etcd_location_store_t store (options);
    std::atomic<int> applies{0};
    first.txn_fault = [&] (auto *, const auto &request, auto *response) {
        const auto result = first.transact (request, response);
        if (applying (request)) {
            ++applies;
            return grpc::Status (grpc::StatusCode::UNAVAILABLE, "connection lost after commit");
        }
        return result;
    };
    EXPECT_THROW (await (store.write (put ("k", "written"))), framework_exception_t);
    EXPECT_EQ (applies, 1);
    ASSERT_TRUE (first.get (key ("test", 'p', "k")));
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (await (store.read ({"k"}))));
    EXPECT_EQ (second.txn_calls, 1);
}

TEST (EtcdProvider, NoResponseDeadlineMovesNextOperation)
{
    fake_etcd_t first, second;
    auto options = first.options ();
    options.endpoints.push_back (second.endpoint ());
    options.operation_timeout = 250ms;
    etcd_location_store_t store (options);
    first.txn_fault = [] (grpc::ServerContext *context, const auto &, auto *) {
        while (!context->IsCancelled ())
            std::this_thread::sleep_for (1ms);
        return grpc::Status (grpc::StatusCode::CANCELLED, "deadline passed");
    };
    EXPECT_THROW (await (store.read ({"k"})), framework_exception_t);
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (await (store.read ({"k"}))));
    EXPECT_EQ (first.txn_calls, 1);
    EXPECT_EQ (second.txn_calls, 1);
}

TEST (EtcdProvider, AuthenticationAndAllWriteCallsShareAbsoluteDeadline)
{
    fake_etcd_t backend;
    auto options = backend.options ();
    options.operation_timeout = 2s;
    options.credentials = etcd_credentials_t{"u", "p"};
    etcd_location_store_t store (options);
    backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
        if (backend.txn_calls == 1) {
            std::this_thread::sleep_for (500ms);
            return grpc::Status (grpc::StatusCode::INVALID_ARGUMENT,
                                 "revision of auth store is old");
        }
        return backend.transact (request, response);
    };
    EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (await (store.write (put ("k")))));
    const auto deadlines = backend.deadlines ();
    ASSERT_EQ (deadlines.size (), 5U);
    const auto [earliest, latest] = std::minmax_element (deadlines.begin (), deadlines.end ());
    EXPECT_LT (*latest - *earliest, 250ms);
}

TEST (EtcdProvider, NospaceAndUnrecognizedErrorsRemainStoreFailures)
{
    fake_etcd_t backend;
    etcd_location_store_t store (backend.options ());
    std::atomic<int> applies{0};
    backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
        if (applying (request)) {
            ++applies;
            return grpc::Status (grpc::StatusCode::RESOURCE_EXHAUSTED,
                                 "etcdserver: mvcc: database space exceeded");
        }
        return backend.transact (request, response);
    };
    EXPECT_THROW (await (store.write (put ("k"))), framework_exception_t);
    EXPECT_EQ (applies, 1);
    backend.txn_fault = [] (auto *, const auto &, auto *) {
        return grpc::Status (grpc::StatusCode::OUT_OF_RANGE, "some other range error");
    };
    EXPECT_THROW (await (store.read ({"k"})), framework_exception_t);
    EXPECT_THROW (await (store.scan ({.prefix = ""})), framework_exception_t);
}

TEST (EtcdProvider, CompactedConditionPayloadRestartsDecision)
{
    fake_etcd_t backend;
    backend.record ("k", "a");
    etcd_location_store_t store (backend.options ());
    bool compacted_once = false;
    backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
        if (historical (request) && !std::exchange (compacted_once, true))
            return grpc::Status (grpc::StatusCode::OUT_OF_RANGE,
                                 "required revision has been compacted");
        return backend.transact (request, response);
    };
    EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (
      await (store.write (put ("x", {}, {}, {store_value_condition_t{{"k"}, bytes ("a")}})))));
    EXPECT_EQ (backend.txn_calls, 5);
}

TEST (EtcdProvider, FailedApplyComparisonReevaluatesCondition)
{
    fake_etcd_t backend;
    etcd_location_store_t store (backend.options ());
    bool changed = false;
    backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
        if (applying (request) && !std::exchange (changed, true))
            backend.record ("k", "a");
        return backend.transact (request, response);
    };
    EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (
      await (store.write (put ("x", {}, {}, {store_missing_condition_t{{"k"}}})))));
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (await (store.read ({"x"}))));
    EXPECT_EQ (backend.txn_calls, 4);
}

TEST (EtcdProvider, ExpiryBetweenDecisionAndApplyReevaluatesCondition)
{
    fake_etcd_t backend;
    backend.record ("k", "a", 1001);
    etcd_location_store_t store (backend.options ());
    const auto version = found (store).value.version;
    bool changed = false;
    backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
        if (applying (request) && !std::exchange (changed, true))
            backend.clock (1001);
        return backend.transact (request, response);
    };
    const auto conflict = std::get<store_write_conflict_t> (
      await (store.write (put ("x", {}, {}, {store_version_condition_t{{"k"}, version}}))));
    EXPECT_EQ (conflict.store_now, std::chrono::system_clock::time_point (1001ms));
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (await (store.read ({"x"}))));
}

TEST (EtcdProvider, ConcurrentMissingConditionsHaveExactlyOneWinner)
{
    fake_etcd_t backend;
    etcd_location_store_t store (backend.options ());
    std::vector<task_t<store_write_result_t>> operations;
    for (int i = 0; i < 16; ++i)
        operations.push_back (store.write (put ("k", "a", {}, {store_missing_condition_t{{"k"}}})));
    int applied = 0, conflicts = 0;
    for (auto &operation : operations) {
        const auto result = await (std::move (operation));
        applied += std::holds_alternative<store_write_applied_t> (result);
        conflicts += std::holds_alternative<store_write_conflict_t> (result);
    }
    EXPECT_EQ (applied, 1);
    EXPECT_EQ (conflicts, 15);
}

TEST (EtcdProvider, SnapshotPreservesValuesClockAndExpiresAfterCompaction)
{
    fake_etcd_t backend;
    backend.record ("p/a", "a");
    backend.record ("p/b", "b", 1001);
    etcd_location_store_t store (backend.options ());
    const auto first =
      std::get<store_scan_page_t> (await (store.scan ({.prefix = "p/", .limit = 1})));
    ASSERT_EQ (first.items.size (), 1U);
    ASSERT_TRUE (first.next_cursor);
    backend.record ("p/b", "changed");
    backend.clock (2000);
    const auto second = std::get<store_scan_page_t> (
      await (store.scan ({.prefix = "p/", .cursor = first.next_cursor, .limit = 1})));
    ASSERT_EQ (second.items.size (), 1U);
    EXPECT_EQ (second.items[0].value.bytes, bytes ("b"));
    EXPECT_EQ (second.store_now, first.store_now);
    EXPECT_FALSE (second.next_cursor);
    backend.compacted = backend.revision;
    EXPECT_TRUE (std::holds_alternative<store_scan_expired_t> (
      await (store.scan ({.prefix = "p/", .cursor = first.next_cursor, .limit = 1}))));
}

TEST (EtcdProvider, ScanSkipsMoreThanOneMetadataBatchAndChecksCorruption)
{
    fake_etcd_t backend;
    for (int i = 0; i < 1001; ++i) {
        const auto number = std::to_string (i);
        backend.record ("p/" + std::string (4 - number.size (), '0') + number, {}, 1000);
    }
    backend.record ("p/z", "a");
    etcd_location_store_t store (backend.options ());
    const auto page = std::get<store_scan_page_t> (await (store.scan ({.prefix = "p/"})));
    ASSERT_EQ (page.items.size (), 1U);
    EXPECT_EQ (page.items[0].key.value, "p/z");
    EXPECT_GE (backend.range_calls, 2);
    backend.put (key ("test", 'p', "p/z"), {});
    EXPECT_THROW (await (store.scan ({.prefix = "p/"})), framework_exception_t);
    backend.put (key ("test", 'm', "p/z"), metadata (false, 0, 0));
    EXPECT_THROW (await (store.write (put ("x", {}, {}, {store_value_condition_t{{"p/z"}, {}}}))),
                  framework_exception_t);
}

TEST (EtcdProvider, ScanCompactionBetweenMetadataAndPayloadExpiresWholePage)
{
    fake_etcd_t backend;
    backend.record ("k", "a");
    etcd_location_store_t store (backend.options ());
    backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
        if (historical (request))
            backend.compacted = backend.revision;
        return backend.transact (request, response);
    };
    EXPECT_TRUE (
      std::holds_alternative<store_scan_expired_t> (await (store.scan ({.prefix = ""}))));
}

TEST (EtcdProvider, ScanPageBytesAndCursorScope)
{
    fake_etcd_t backend;
    const std::string payload (1024 * 1024, 'p');
    for (int i = 0; i < 5; ++i)
        backend.record ("p/" + std::to_string (i), payload);
    etcd_location_store_t store (backend.options ());
    const auto first =
      std::get<store_scan_page_t> (await (store.scan ({.prefix = "p/", .limit = 1000})));
    ASSERT_EQ (first.items.size (), 3U);
    ASSERT_TRUE (first.next_cursor);
    EXPECT_LE (first.next_cursor->value.size (), 4096U);
    EXPECT_TRUE (std::holds_alternative<store_scan_expired_t> (
      await (store.scan ({.prefix = "other/", .cursor = first.next_cursor, .limit = 1000}))));
    {
        etcd_location_store_t other (backend.options ());
        EXPECT_TRUE (std::holds_alternative<store_scan_expired_t> (
          await (other.scan ({.prefix = "p/", .cursor = first.next_cursor, .limit = 1000}))));
    }
    auto tampered = *first.next_cursor;
    tampered.value[tampered.value.size () / 2] ^= 1;
    EXPECT_TRUE (std::holds_alternative<store_scan_expired_t> (
      await (store.scan ({.prefix = "p/", .cursor = tampered, .limit = 1000}))));
    const auto second = std::get<store_scan_page_t> (
      await (store.scan ({.prefix = "p/", .cursor = first.next_cursor, .limit = 1000})));
    EXPECT_EQ (second.items.size (), 2U);
    EXPECT_FALSE (second.next_cursor);
    ++backend.cluster;
    EXPECT_TRUE (std::holds_alternative<store_scan_expired_t> (
      await (store.scan ({.prefix = "p/", .cursor = first.next_cursor, .limit = 1000}))));
}

TEST (EtcdProvider, MaximumPrefixAndKeyKeepCursorWithin4096Bytes)
{
    fake_etcd_t backend;
    auto options = backend.options ();
    options.key_namespace = std::string (64, 'n');
    backend.clock (1000, options.key_namespace);
    const std::string prefix (1023, 'p');
    backend.record (prefix + "a", {}, {}, options.key_namespace);
    backend.record (prefix + "b", {}, {}, options.key_namespace);
    etcd_location_store_t store (options);
    const auto first =
      std::get<store_scan_page_t> (await (store.scan ({.prefix = prefix, .limit = 1})));
    ASSERT_TRUE (first.next_cursor);
    EXPECT_LE (first.next_cursor->value.size (), 4096U);
    EXPECT_EQ (std::get<store_scan_page_t> (
                 await (store.scan ({.prefix = prefix, .cursor = first.next_cursor, .limit = 1})))
                 .items.size (),
               1U);
}

TEST (EtcdProvider, InvalidInputSendsNoRpc)
{
    fake_etcd_t backend;
    etcd_location_store_t store (backend.options ());
    EXPECT_THROW (await (store.read ({""})), std::invalid_argument);
    EXPECT_THROW (await (store.read ({std::string (1, static_cast<char> (0xff))})),
                  std::invalid_argument);
    EXPECT_THROW (await (store.scan ({.prefix = std::string ("\xc0\x80", 2)})),
                  std::invalid_argument);
    EXPECT_THROW (await (store.read ({std::string (1025, 'k')})), std::invalid_argument);
    EXPECT_THROW (await (store.write (put ("k", std::string (1048577, 'v')))),
                  std::invalid_argument);
    EXPECT_THROW (await (store.write (put ("k", {}, 0ms))), std::invalid_argument);
    EXPECT_THROW (
      await (store.write (
        put ("k", {}, {}, {store_missing_condition_t{{"k"}}, store_missing_condition_t{{"k"}}}))),
      std::invalid_argument);
    EXPECT_THROW (await (store.scan ({.prefix = "", .limit = 0})), std::invalid_argument);
    EXPECT_THROW (await (store.scan ({.prefix = "", .limit = 1001})), std::invalid_argument);
    EXPECT_THROW (
      await (store.scan (
        {.prefix = "", .cursor = store_scan_cursor_t{std::string (4097, 'a')}, .limit = 1})),
      std::invalid_argument);
    store_write_request_t too_many;
    for (int i = 0; i < 2049; ++i)
        too_many.mutations.push_back (store_delete_t{{"k" + std::to_string (i)}});
    EXPECT_THROW (await (store.write (std::move (too_many))), std::invalid_argument);
    store_write_request_t too_large;
    for (int i = 0; i < 4; ++i)
        too_large.mutations.push_back (
          store_put_t{{"k" + std::to_string (i)}, std::vector<std::byte> (1048576), {}});
    EXPECT_THROW (await (store.write (std::move (too_large))), std::invalid_argument);
    EXPECT_EQ (backend.txn_calls, 0);
    EXPECT_EQ (backend.range_calls, 0);
    EXPECT_EQ (backend.auth_calls, 0);
}

TEST (EtcdProvider, OptionsFailBeforeChannelIo)
{
    fake_etcd_t backend;
    for (const std::string endpoint : {"localhost:2379", "ftp://localhost", "http://localhost/path",
                                       "http://u:p@localhost", "http://localhost?x=1"}) {
        auto options = backend.options ();
        options.endpoints = {endpoint};
        EXPECT_THROW (etcd_location_store_t{options}, std::invalid_argument);
    }
    std::vector<etcd_location_options_t> invalid;
    auto options = backend.options ();
    options.endpoints.clear ();
    invalid.push_back (options);
    options = backend.options ();
    options.endpoints.push_back ("https://localhost:2379");
    invalid.push_back (options);
    options = backend.options ();
    options.tls = etcd_tls_options_t{};
    invalid.push_back (options);
    options = backend.options ();
    options.key_namespace = "";
    invalid.push_back (options);
    options.key_namespace = std::string (65, 'n');
    invalid.push_back (options);
    options.key_namespace = std::string{"n\0s", 3};
    invalid.push_back (options);
    options = backend.options ();
    options.operation_timeout = 0ms;
    invalid.push_back (options);
    options = backend.options ();
    options.credentials = etcd_credentials_t{"u", ""};
    invalid.push_back (options);
    options = backend.options ();
    options.endpoints = {"https://localhost:2379"};
    options.tls = etcd_tls_options_t{"", etcd_client_identity_t{"", "key"}};
    invalid.push_back (options);
    options.tls = etcd_tls_options_t{"not a PEM certificate", {}};
    invalid.push_back (options);
    options.tls =
      etcd_tls_options_t{"", etcd_client_identity_t{pem ("client.pem"), "not a PEM key"}};
    invalid.push_back (options);
    for (const auto &value : invalid)
        EXPECT_THROW (etcd_location_store_t{value}, std::invalid_argument);
    EXPECT_EQ (backend.txn_calls, 0);
    EXPECT_EQ (backend.auth_calls, 0);
}

TEST (EtcdProvider, MissingClockAndOverflowAreFailuresWithoutMutation)
{
    fake_etcd_t backend;
    backend.clock (encoding::max_time);
    etcd_location_store_t store (backend.options ());
    EXPECT_THROW (await (store.write (put ("k", {}, 1ms))), std::invalid_argument);
    EXPECT_FALSE (backend.get (key ("test", 'm', "k")));
    backend.erase (key ("test", 'c'));
    EXPECT_THROW (await (store.read ({"k"})), framework_exception_t);
    EXPECT_THROW (await (store.write (put ("k"))), framework_exception_t);
    EXPECT_THROW (await (store.scan ({.prefix = ""})), framework_exception_t);
}

TEST (EtcdProvider, MalformedReadResponsesNeverBecomeMissing)
{
    for (int fault = 0; fault < 4; ++fault) {
        fake_etcd_t backend;
        etcd_location_store_t store (backend.options ());
        backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
            const auto status = backend.transact (request, response);
            if (fault == 0)
                response->clear_header ();
            else if (fault == 1)
                response->mutable_responses ()->RemoveLast ();
            else if (fault == 2)
                response->mutable_responses (0)->mutable_response_range ()->add_kvs ();
            else
                response->mutable_responses (1)->mutable_response_put ();
            return status;
        };
        EXPECT_THROW (await (store.read ({"k"})), framework_exception_t)
          << "malformed response " << fault;
    }
}

TEST (EtcdProvider, UnknownMetadataTagFailsConditionAndSnapshot)
{
    fake_etcd_t backend;
    auto invalid_metadata = metadata (false, 0, 1);
    invalid_metadata[0] = '\x02';
    backend.raw_record (invalid_metadata, "a");
    etcd_location_store_t store (backend.options ());
    EXPECT_THROW (await (store.write (put ("x", {}, {}, {store_missing_condition_t{{"k"}}}))),
                  framework_exception_t);
    EXPECT_THROW (await (store.scan ({.prefix = ""})), framework_exception_t);
    EXPECT_FALSE (backend.get (key ("test", 'm', "x")));
}

TEST (EtcdProvider, StoreClockProgressDoesNotForceReapply)
{
    fake_etcd_t backend;
    backend.record ("k", "a", 2000);
    etcd_location_store_t store (backend.options ());
    const auto version = found (store).value.version;
    backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
        if (applying (request))
            backend.clock (1500);
        return backend.transact (request, response);
    };
    const auto result = std::get<store_write_applied_t> (
      await (store.write (put ("x", {}, {}, {store_version_condition_t{{"k"}, version}}))));
    EXPECT_EQ (result.store_now, std::chrono::system_clock::time_point (1000ms));
    EXPECT_EQ (backend.txn_calls, 3);
}

task_t<store_read_result_t> destroy_after_read (task_t<store_read_result_t> read,
                                                std::unique_ptr<etcd_location_store_t> &store)
{
    auto result = co_await read;
    store.reset ();
    co_return result;
}

TEST (EtcdProvider, DestructionFromCompletionCallbackDoesNotDeadlock)
{
    fake_etcd_t backend;
    auto store = std::make_unique<etcd_location_store_t> (backend.options ());
    std::binary_semaphore entered (0), release (0);
    backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
        entered.release ();
        release.acquire ();
        return backend.transact (request, response);
    };
    auto pending = store->read ({"k"});
    const bool arrived = entered.try_acquire_for (2s);
    if (!arrived) {
        release.release ();
        FAIL () << "Store read did not reach fake server";
    }
    auto chained = destroy_after_read (std::move (pending), store);
    release.release ();
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (await (std::move (chained))));
    EXPECT_FALSE (store);
}

TEST (EtcdProvider, DestructionOnOtherThreadDrainsInflightOperation)
{
    fake_etcd_t backend;
    auto store = std::make_unique<etcd_location_store_t> (backend.options ());
    std::binary_semaphore entered (0), release (0);
    backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
        entered.release ();
        release.acquire ();
        return backend.transact (request, response);
    };
    auto pending = store->read ({"k"});
    const bool arrived = entered.try_acquire_for (2s);
    if (!arrived) {
        release.release ();
        FAIL () << "Store read did not reach fake server";
    }
    auto closing = std::async (std::launch::async, [&] { store.reset (); });
    EXPECT_EQ (closing.wait_for (30ms), std::future_status::timeout);
    release.release ();
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (await (std::move (pending))));
    EXPECT_EQ (closing.wait_for (2s), std::future_status::ready);
    closing.get ();
}

TEST (EtcdProvider, ConsumerWaitCancellationDoesNotCancelOrRetirePhysicalOperation)
{
    fake_etcd_t backend;
    auto store = std::make_unique<etcd_location_store_t> (backend.options ());
    std::stop_source owner;
    std::binary_semaphore entered (0), release (0);
    backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
        entered.release ();
        release.acquire ();
        return backend.transact (request, response);
    };
    std::future<void> closing;
    struct release_rpc_t
    {
        std::binary_semaphore &semaphore;
        bool pending = true;
        void complete ()
        {
            if (std::exchange (pending, false))
                semaphore.release ();
        }
        ~release_rpc_t () { complete (); }
    } unblock{release};
    auto pending = [&] {
        struct wait_owner_scope_t
        {
            explicit wait_owner_scope_t (std::stop_token token) :
                previous (zlink::framework::detail::exchange_wait_owner (token))
            {
            }
            ~wait_owner_scope_t () { zlink::framework::detail::exchange_wait_owner (previous); }
            std::stop_token previous;
        } scope (owner.get_token ());
        return store->read ({"k"});
    }();
    ASSERT_TRUE (entered.try_acquire_for (2s)) << "Store read did not reach fake server";
    owner.request_stop ();
    EXPECT_FALSE (pending.result_for (30ms))
      << "Consumer wait cancellation must not complete a provider operation";
    closing = std::async (std::launch::async, [&] { store.reset (); });
    EXPECT_EQ (closing.wait_for (30ms), std::future_status::timeout)
      << "Provider destruction must retain the physical RPC after consumer cancellation";
    unblock.complete ();
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (await (std::move (pending))));
    EXPECT_EQ (closing.wait_for (2s), std::future_status::ready);
    closing.get ();
}

TEST (EtcdProvider, ObserverRegistrationFailureRetainsAndDrainsPhysicalWrite)
{
    fake_etcd_t backend;
    auto store = std::make_unique<etcd_location_store_t> (backend.options ());
    std::binary_semaphore entered (0), release (0);
    std::future<void> closing;
    struct cleanup_t
    {
        std::binary_semaphore &semaphore;
        std::future<void> &closing;
        std::unique_ptr<etcd_location_store_t> &store;
        bool pending = true;
        void release_rpc ()
        {
            if (std::exchange (pending, false))
                semaphore.release ();
        }
        ~cleanup_t ()
        {
            release_rpc ();
            if (closing.valid ())
                closing.wait ();
            store.reset ();
        }
    } cleanup{release, closing, store};
    backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
        if (backend.txn_calls == 1) {
            entered.release ();
            release.acquire ();
        }
        return backend.transact (request, response);
    };
    {
        struct hook_scope_t
        {
            const zlink::framework::detail::ambient_context_hooks_t *previous =
              zlink::framework::detail::current_ambient_context_hooks ();
            bool previous_failure = fail_external_capture_once;
            hook_scope_t ()
            {
                zlink::framework::detail::set_ambient_context_hooks (&failing_capture_hooks);
                fail_external_capture_once = true;
            }
            ~hook_scope_t ()
            {
                fail_external_capture_once = previous_failure;
                zlink::framework::detail::set_ambient_context_hooks (previous);
            }
        } hooks;
        EXPECT_THROW (store->write (put ("k", "written-after-observer-failure")), std::bad_alloc);
        EXPECT_FALSE (fail_external_capture_once)
          << "External observer capture must inject the failure";
    }
    ASSERT_TRUE (entered.try_acquire_for (2s)) << "Decision RPC did not reach the fake server";
    closing = std::async (std::launch::async, [&] { store.reset (); });
    EXPECT_EQ (closing.wait_for (30ms), std::future_status::timeout)
      << "Provider destruction must retain the RPC after observer registration fails";
    cleanup.release_rpc ();
    EXPECT_EQ (closing.wait_for (2s), std::future_status::ready);
    closing.get ();
    const auto payload = backend.get (key ("test", 'p', "k"));
    ASSERT_TRUE (payload) << "The submitted physical write must drain through apply";
    EXPECT_EQ (payload->value (), "written-after-observer-failure");
    EXPECT_EQ (backend.txn_calls, 2);
}

TEST (EtcdProvider, DestructionFromCompletionKeepsAnotherInflightOperationAlive)
{
    fake_etcd_t backend;
    auto store = std::make_unique<etcd_location_store_t> (backend.options ());
    std::counting_semaphore<2> entered (0);
    std::binary_semaphore first_release (0), second_release (0);
    backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
        entered.release ();
        if (request.success (1).request_range ().key () == key ("test", 'm', "first"))
            first_release.acquire ();
        else
            second_release.acquire ();
        return backend.transact (request, response);
    };
    auto first = store->read ({"first"});
    auto second = store->read ({"second"});
    const bool first_entered = entered.try_acquire_for (2s);
    const bool second_entered = entered.try_acquire_for (2s);
    if (!first_entered || !second_entered) {
        first_release.release ();
        second_release.release ();
        FAIL () << "Both Store reads must reach the fake server";
    }
    auto callback = destroy_after_read (std::move (first), store);
    first_release.release ();
    const auto completed = callback.result_for (2s);
    // Always unblock the second RPC, including when a destructor incorrectly
    // waits for it on the completion queue thread.
    second_release.release ();
    EXPECT_TRUE (completed) << "Completion callback destruction blocked another RPC";
    // Drain the callback after releasing the RPC even when the timing assertion
    // fails, so its reference to store cannot outlive this test's stack.
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (await (std::move (callback))));
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (await (std::move (second))));
    EXPECT_FALSE (store);
}

task_t<store_scan_result_t> chain_operations (etcd_location_store_t &store)
{
    const auto missing = co_await store.read ({"k"});
    if (!std::holds_alternative<store_missing_t> (missing))
        throw std::runtime_error ("Expected missing before chained write");
    const auto written = co_await store.write (put ("k", "chained"));
    if (!std::holds_alternative<store_write_applied_t> (written))
        throw std::runtime_error ("Expected applied chained write");
    const auto read = co_await store.read ({"k"});
    if (std::get<store_found_t> (read).value.bytes != bytes ("chained"))
        throw std::runtime_error ("Chained read did not observe the write");
    co_return co_await store.scan ({.prefix = "", .limit = 1});
}

TEST (EtcdProvider, PublicOperationsCanChainFromRpcCompletion)
{
    fake_etcd_t backend;
    etcd_location_store_t store (backend.options ());
    const auto result = std::get<store_scan_page_t> (await (chain_operations (store)));
    ASSERT_EQ (result.items.size (), 1U);
    EXPECT_EQ (result.items[0].key.value, "k");
    EXPECT_EQ (result.items[0].value.bytes, bytes ("chained"));
}

TEST (EtcdTls, MutualTlsCustomCaAndIdentityHostNameAndTrustValidation)
{
    grpc::SslServerCredentialsOptions server_options;
    server_options.pem_root_certs = pem ("ca.pem");
    server_options.pem_key_cert_pairs.push_back ({pem ("server.key"), pem ("server.pem")});
    server_options.client_certificate_request =
      GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
    fake_etcd_t backend (grpc::SslServerCredentials (server_options));
    auto options = backend.options ();
    options.endpoints = {backend.endpoint (true)};
    const etcd_client_identity_t identity{pem ("client.pem"), pem ("client.key")};
    options.tls = etcd_tls_options_t{pem ("ca.pem"), identity};
    {
        etcd_location_store_t store (options);
        EXPECT_TRUE (std::holds_alternative<store_missing_t> (await (store.read ({"k"}))));
    }
    const int calls = backend.txn_calls;
    options.operation_timeout = 500ms;
    {
        auto untrusted = options;
        untrusted.tls->trusted_ca_pem.clear ();
        etcd_location_store_t store (untrusted);
        EXPECT_THROW (await (store.read ({"k"})), framework_exception_t);
    }
    EXPECT_EQ (backend.txn_calls, calls);
    {
        auto wrong_host = options;
        wrong_host.endpoints = {backend.endpoint (true, false)};
        etcd_location_store_t store (wrong_host);
        EXPECT_THROW (await (store.read ({"k"})), framework_exception_t);
    }
    EXPECT_EQ (backend.txn_calls, calls);
}

TEST (EtcdLive, ConditionsExpirationMaximumBatchAndSnapshot)
{
    const auto *endpoints = std::getenv ("ZLINK_ETCD_ENDPOINTS");
    const auto *ns = std::getenv ("ZLINK_ETCD_NAMESPACE");
    if (!endpoints || !ns)
        GTEST_SKIP () << "Set ZLINK_ETCD_ENDPOINTS and ZLINK_ETCD_NAMESPACE; run Node admin clock";
    etcd_location_options_t options;
    options.key_namespace = ns;
    std::string remaining_endpoints (endpoints);
    while (!remaining_endpoints.empty ()) {
        const auto comma = remaining_endpoints.find (',');
        options.endpoints.push_back (remaining_endpoints.substr (0, comma));
        if (comma == std::string::npos)
            break;
        remaining_endpoints.erase (0, comma + 1);
    }
    etcd_location_store_t store (options);
    const auto prefix =
      "cpp-live/" + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ())
      + "/";
    const auto condition_key = prefix + "condition";
    std::vector<store_mutation_t> cleanup{store_delete_t{{condition_key}}};
    struct cleanup_t
    {
        etcd_location_store_t &store;
        std::vector<store_mutation_t> &mutations;
        ~cleanup_t ()
        {
            for (std::size_t first = 0; first < mutations.size (); first += 2048) {
                const auto last = std::min (first + 2048, mutations.size ());
                EXPECT_NO_THROW (await (
                  store.write ({{},
                                {mutations.begin () + static_cast<std::ptrdiff_t> (first),
                                 mutations.begin () + static_cast<std::ptrdiff_t> (last)}})));
            }
        }
    } clean{store, cleanup};
    const auto written = std::get<store_write_applied_t> (await (
      store.write (put (condition_key, "a", 1ms, {store_missing_condition_t{{condition_key}}}))));
    EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (await (store.write (
      put (condition_key, "b", {}, {store_value_condition_t{{condition_key}, bytes ("b")}})))));
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    bool expired = false;
    do {
        expired = std::holds_alternative<store_missing_t> (await (store.read ({condition_key})));
        if (expired)
            break;
        std::this_thread::sleep_for (50ms);
    } while (std::chrono::steady_clock::now () < deadline);
    ASSERT_TRUE (expired) << "Node Store clock must advance through expiry";
    ASSERT_EQ (written.put_versions.size (), 1U);
    EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (await (store.write (
      put (condition_key, {}, {},
           {store_version_condition_t{{condition_key}, written.put_versions[0].version}})))));

    const auto batch_prefix = prefix + "batch/";
    std::vector<store_key_t> keys;
    std::size_t remaining = 4 * 1024 * 1024;
    for (int i = 0; i < 2048; ++i) {
        const auto number = std::to_string (i);
        keys.push_back ({batch_prefix + std::string (4 - number.size (), '0') + number});
        remaining -= keys.back ().value.size ();
    }
    store_write_request_t request;
    for (std::size_t i = 0; i < keys.size (); ++i) {
        const auto length = remaining / (keys.size () - i);
        remaining -= length;
        request.mutations.push_back (store_put_t{keys[i], std::vector<std::byte> (length), {}});
        cleanup.push_back (store_delete_t{keys[i]});
    }
    EXPECT_EQ (remaining, 0U);
    const auto applied =
      std::get<store_write_applied_t> (await (store.write (std::move (request))));
    ASSERT_EQ (applied.put_versions.size (), 2048U);
    const auto first =
      std::get<store_scan_page_t> (await (store.scan ({.prefix = batch_prefix, .limit = 1000})));
    ASSERT_EQ (first.items.size (), 1000U);
    ASSERT_TRUE (first.next_cursor);
    const auto replaced = keys[1500];
    await (store.write (put (replaced.value, "changed")));
    const auto second = std::get<store_scan_page_t> (
      await (store.scan ({.prefix = batch_prefix, .cursor = first.next_cursor, .limit = 1000})));
    ASSERT_EQ (second.items.size (), 1000U);
    EXPECT_EQ (second.store_now, first.store_now);
    const auto historical =
      std::find_if (second.items.begin (), second.items.end (),
                    [&] (const auto &item) { return item.key.value == replaced.value; });
    ASSERT_NE (historical, second.items.end ());
    EXPECT_EQ (historical->value.version.value, applied.put_versions[1500].version.value);
    EXPECT_GT (historical->value.bytes.size (), 7U);
    ASSERT_TRUE (second.next_cursor);
    const auto last = std::get<store_scan_page_t> (
      await (store.scan ({.prefix = batch_prefix, .cursor = second.next_cursor, .limit = 1000})));
    EXPECT_EQ (last.items.size (), 48U);
    EXPECT_FALSE (last.next_cursor);
    EXPECT_EQ (last.store_now, first.store_now);
}
} // namespace
