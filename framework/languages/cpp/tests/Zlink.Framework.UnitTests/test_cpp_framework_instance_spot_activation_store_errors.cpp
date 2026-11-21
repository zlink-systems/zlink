/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "../support/location_reclaim_clock.hpp"
#include "../support/manual_deadlines.hpp"
#include "../support/telemetry_observation.hpp"
#include "metric_test_reader.hpp"
#include <zlink/framework.hpp>
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/failure_origin_wire.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/request_deadline.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <gtest/gtest.h>
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime_context_hooks_environment.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <future>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <type_traits>
#include <typeinfo>
#include <vector>
#include "../support/instance_spot_activation_messages.hpp"

using namespace framework_instance_spot_activation_fixture;

namespace
{
TEST (ZLinkFrameworkInstanceSpotActivation, StoreReadFailureHasUnavailablePublicKind)
{
    namespace fw = zlink::framework;
    class failing_store_t final : public fw::location_store_t
    {
      public:
        fw::task_t<fw::store_read_result_t> read (fw::store_key_t) override
        {
            ++reads;
            throw std::runtime_error ("private provider command/key details");
        }
        fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t) override
        {
            throw std::logic_error ("unexpected Store write");
        }
        fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t) override
        {
            throw std::logic_error ("unexpected Store scan");
        }
        int reads = 0;
    } store;
    fw::runtime::provider_location_repository_t repository (store);
    const auto resolver_owner =
      std::make_shared<fw::runtime::store_location_resolvers_t> (repository);
    auto &resolver = *resolver_owner;
    fw::serializer_registry_t serializers;
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    runtime.bind_spot_address_resolver (resolver_owner);
    auto client = builder.route_client (serializers);
    const auto send = client.send_to_spot ("uncached-spot", event_t{1}).async ().result ();
    EXPECT_FALSE (send.has_value ());
    EXPECT_NE (nullptr, send.error ());
    if (send.error ()) {
        EXPECT_EQ (fw::framework_error_kind_t::unavailable, send.error_kind ());
        EXPECT_EQ (std::string::npos, std::string (send.error ()->what ()).find ("command"));
    }
    const auto request = client.request_to_spot ("uncached-spot", request_t{1})
                           .timeout (std::chrono::minutes (1))
                           .async<reply_t> ()
                           .result ();
    EXPECT_FALSE (request.has_value ());
    EXPECT_NE (nullptr, request.error ());
    if (request.error ()) {
        EXPECT_EQ (fw::framework_error_kind_t::unavailable, request.error_kind ());
        EXPECT_EQ (std::string::npos, std::string (request.error ()->what ()).find ("command"));
    }
    EXPECT_EQ (2, store.reads);
}

TEST (ZLinkFrameworkInstanceSpotActivation, RepositoryClassifiesEveryProviderOperation)
{
    namespace fw = zlink::framework;
    class fault_store_t final : public fw::location_store_t
    {
      public:
        explicit fault_store_t (std::string operation) : operation (std::move (operation)) {}
        fw::task_t<fw::store_read_result_t> read (fw::store_key_t) override
        {
            if (operation == "read")
                throw std::runtime_error ("private provider failure");
            return fw::task_t<fw::store_read_result_t> (
              fw::result_t<fw::store_read_result_t>::success (
                fw::store_read_result_t{fw::store_missing_t{std::chrono::system_clock::now ()}}));
        }
        fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t) override
        {
            throw std::runtime_error ("private provider failure");
        }
        fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t) override
        {
            throw std::runtime_error ("private provider failure");
        }
        std::string operation;
    };
    const auto verify = [] (const auto &result) {
        ASSERT_FALSE (result.has_value ());
        ASSERT_NE (nullptr, result.error ());
        EXPECT_EQ (fw::framework_error_kind_t::unavailable, result.error_kind ());
        try {
            std::rethrow_exception (result.exception ());
        }
        catch (const fw::framework_exception_t &error) {
            EXPECT_EQ (std::string::npos, std::string (error.what ()).find ("private"));
            try {
                std::rethrow_if_nested (error);
                FAIL () << "provider cause was lost";
            }
            catch (const std::runtime_error &cause) {
                EXPECT_STREQ ("private provider failure", cause.what ());
            }
        }
    };
    fault_store_t read_store ("read");
    fw::runtime::provider_location_repository_t read_repository (read_store);
    verify (read_repository.read_owner_lease ("owner").result ());
    fault_store_t write_store ("write");
    fw::runtime::provider_location_repository_t write_repository (write_store);
    verify (write_repository.claim_owner_lease ("owner", std::chrono::seconds (30)).result ());
    fault_store_t scan_store ("scan");
    fw::runtime::provider_location_repository_t scan_repository (scan_store);
    verify (scan_repository.list_mesh_nodes ("mesh").result ());
}

TEST (ZLinkFrameworkInstanceSpotActivation, RepositoryPreservesExcludedProviderFailures)
{
    namespace fw = zlink::framework;
    class fault_store_t final : public fw::location_store_t
    {
      public:
        fw::task_t<fw::store_read_result_t> read (fw::store_key_t) override
        {
            std::rethrow_exception (failure);
        }
        fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t) override
        {
            throw std::logic_error ("unexpected write");
        }
        fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t) override
        {
            throw std::logic_error ("unexpected scan");
        }
        std::exception_ptr failure;
    } store;
    fw::runtime::provider_location_repository_t repository (store);
    const auto verify = [&] (const auto &failure) {
        using exception_t = std::decay_t<decltype (failure)>;
        store.failure = std::make_exception_ptr (failure);
        const auto result = repository.read_owner_lease ("owner").result ();
        EXPECT_FALSE (result.has_value ());
        ASSERT_NE (nullptr, result.exception ());
        // MSVC can copy exceptions during rethrow; preserve their type and contents.
        try {
            result.value ();
            FAIL () << "provider failure was lost";
        }
        catch (const exception_t &error) {
            EXPECT_EQ (typeid (failure), typeid (error));
            EXPECT_STREQ (failure.what (), error.what ());
            if constexpr (std::is_same_v<exception_t, fw::framework_exception_t>) {
                EXPECT_EQ (failure.kind (), error.kind ());
                EXPECT_EQ (failure.code (), error.code ());
            } else if constexpr (std::is_same_v<exception_t, std::system_error>) {
                EXPECT_EQ (failure.code (), error.code ());
            }
        }
        catch (...) {
            FAIL () << "provider failure type changed";
        }
    };
    verify (
      fw::framework_exception_t (fw::framework_error_kind_t::protocol_error, "typed failure"));
    verify (std::invalid_argument ("caller validation"));
    verify (std::out_of_range ("caller range"));
    verify (std::system_error (std::make_error_code (std::errc::operation_canceled)));
}
} // namespace
