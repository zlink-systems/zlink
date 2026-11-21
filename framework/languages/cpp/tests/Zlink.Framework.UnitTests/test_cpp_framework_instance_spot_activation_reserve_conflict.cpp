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
#include "../support/instance_spot_activation_deadline_fixture.hpp"

using namespace framework_instance_spot_activation_fixture;

namespace
{
class reserve_loser_repository_t final
    : public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    zlink::framework::task_t<zlink::framework::authority_read_result_t>
    read_authority_core (zlink::framework::authority_key_t, std::stop_token = {}) override
    {
        ++reads;
        co_return zlink::framework::authority_missing_t{std::chrono::system_clock::now ()};
    }

    zlink::framework::task_t<zlink::framework::object_reserve_result_t>
    reserve_core (zlink::framework::object_reserve_request_t request, std::stop_token = {}) override
    {
        ++reserves;
        observed = std::move (request);
        captured = zlink::framework::runtime::protocol::decode_instance_activation_recovery (
          relocations->get (observed->intent.request_content_reference).value (), false);
        if (expire_deadline) {
            advance_activation_deadline (captured->activation.target.deadline_unix_ms).get ();
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::deadline_exceeded,
              "Object creation deadline expired");
        }
        co_return zlink::framework::object_reserve_conflict_t{
          zlink::framework::authority_missing_t{std::chrono::system_clock::now ()}};
    }

    int reads = 0;
    int reserves = 0;
    bool expire_deadline = false;
    std::optional<zlink::framework::object_reserve_request_t> observed;
    std::shared_ptr<zlink::framework::runtime::stateful::relocation_store_port_t> relocations;
    std::optional<zlink::framework::runtime::protocol::instance_activation_recovery_t> captured;
};

void verify_reserve_loser (bool expire_deadline, bool one_way)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    namespace host = rt::host;
    auto store = std::make_shared<reserve_loser_repository_t> ();
    store->expire_deadline = expire_deadline;
    auto blob_store = std::make_shared<rt::in_memory_relocation_store_t> ();
    auto blobs = std::make_shared<rt::provider_relocation_repository_t> (*blob_store);
    auto relocations = std::make_shared<rt::stateful::public_relocation_store_adapter_t> (blobs);
    store->relocations = relocations;
    auto options = host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "reserve-loser",
                              .node_routing_id = zlink::routing_id_t::from ("target").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures = std::make_shared<rt::runtime_failure_collector_t> ()}};
    auto diagnostics =
      std::make_shared<fw::test::telemetry_observations_t<fw::message_dispatch_error_event_t>> ();
    if (one_way)
        fw::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
          options.mesh.dispatch, [diagnostics] (const fw::message_dispatch_error_event_t &event) {
              diagnostics->record (event);
          });
    // Declared before `target` so it outlives the terminal that close() delivers.
    int terminals = 0;
    int prepares = 0;
    int handlers = 0;
    std::promise<rt::protocol::reply_header_t> reply_promise;
    auto reply_future = reply_promise.get_future ();
    auto target = std::make_shared<host::public_host_runtime_t> (std::move (options));
    target->configure_instance_spot_operations (
      store, relocations, [] { return fw::location_owner_token_t{"target-owner", 1}; },
      host::instance_spot_activation_materializer_t{
        [&] (const auto &, const auto &, auto &) -> zlink::framework::task_t<bool> {
            ++prepares;
            co_return true;
        },
        [&] (auto, auto,
             auto accept_activation) -> fw::task_t<host::instance_spot_activation_result_t> {
            accept_activation ();
            ++handlers;
            co_return host::instance_spot_activation_result_t{};
        }});
    target->start ();
    const auto deadline = std::chrono::system_clock::now () + (expire_deadline ? 150ms : 3s);
    rt::protocol::instance_spot_activation_header_t request{
      {target->status ().routing_id ().to_bytes (), target->status ().lifecycle_generation (),
       "contested", "reserve-loser", "room", "descriptor-1",
       static_cast<std::uint64_t> (
         std::chrono::duration_cast<std::chrono::milliseconds> (deadline.time_since_epoch ())
           .count ())},
      target->status ().lifecycle_generation (),
      target->status ().routing_id ().to_bytes (),
      std::nullopt,
      !one_way,
      {11, 13},
      one_way ? 0u : 17u,
      false};
    rt::protocol::instance_activation_recovery_t command{
      request, std::nullopt,
      rt::protocol::application_payload_t{"probe", "application/json", {1, 2, 3}}};
    std::uint64_t reply_correlation = 0;
    const auto submitted =
      one_way
        ? target
            ->send_instance_spot_activation_remote (target->status ().routing_id (), request,
                                                    command.metadata, command.application_payload)
            .result ()
        : target
            ->activate_instance_spot_remote (
              target->status ().routing_id (), request, command.metadata,
              command.application_payload, 3s,
              [&] (auto terminal, auto header, auto) {
                  EXPECT_EQ (rt::foundation::operation_terminal_t::completed, terminal);
                  ++terminals;
                  reply_promise.set_value (header);
              })
            .result ();
    ASSERT_TRUE (submitted);
    ASSERT_TRUE (submitted.value ());
    const auto pump_deadline = std::chrono::steady_clock::now () + 3s;
    while ((one_way ? diagnostics->snapshot ().empty ()
                    : reply_future.wait_for (0ms) != std::future_status::ready)
           && std::chrono::steady_clock::now () < pump_deadline) {
        ASSERT_TRUE (
          target->dispatch_ready ([] (const auto &, const auto &, auto, auto) {}, false).result ());
        std::this_thread::yield ();
    }
    fw::test::acknowledge_delivery (pump_deadline);
    const auto observed_diagnostics = diagnostics->snapshot ();
    if (one_way) {
        ASSERT_EQ (1u, observed_diagnostics.size ());
        const auto &event = observed_diagnostics.front ();
        EXPECT_EQ (fw::dispatch_error_action_t::drop, event.action);
        EXPECT_EQ (fw::dispatch_message_kind_t::send, event.message_kind);
        EXPECT_EQ (fw::dispatch_error_surface_t::instance_spot, event.surface);
        EXPECT_EQ (fw::dispatch_error_reason_t::stale_target, event.reason);
        EXPECT_EQ (typeid (fw::framework_exception_t).name (), event.error_type);
        EXPECT_EQ ("Instance Spot activation failed because the target route is not connected.",
                   event.error_message);
        ASSERT_TRUE (event.exception);
        try {
            std::rethrow_exception (event.exception);
        }
        catch (const fw::framework_exception_t &error) {
            EXPECT_EQ (fw::framework_error_kind_t::unavailable, error.kind ());
        }
        EXPECT_EQ (0, terminals);
    } else {
        ASSERT_EQ (std::future_status::ready, reply_future.wait_for (0ms));
        const auto reply = reply_future.get ();
        reply_correlation = reply.correlation;
        const auto error = rt::messaging::request_failure_mapper_t{}.reply_header_exception (
          reply.terminal_result, reply.failure_code, "Reserve loser");
        EXPECT_EQ (expire_deadline ? fw::framework_error_kind_t::deadline_exceeded
                                   : fw::framework_error_kind_t::unavailable,
                   error.kind ());
        EXPECT_EQ (1, terminals);
        EXPECT_TRUE (observed_diagnostics.empty ());
    }
    EXPECT_EQ (1, store->reads);
    EXPECT_EQ (1, store->reserves);
    EXPECT_EQ (0, prepares);
    EXPECT_EQ (0, handlers);
    ASSERT_TRUE (store->observed);
    EXPECT_EQ (std::chrono::system_clock::time_point (
                 std::chrono::milliseconds (request.target.deadline_unix_ms)),
               store->observed->operation_deadline);
    ASSERT_TRUE (store->captured);
    auto admitted_request = request;
    admitted_request.reply_route_id = reply_correlation;
    EXPECT_EQ (admitted_request, store->captured->activation);
    EXPECT_EQ (command.application_payload.payload_bytes (),
               store->captured->application_payload.payload_bytes ());
    EXPECT_EQ (command.metadata, store->captured->metadata);
    target->close ();
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      ReserveLoserReturnsUnavailableWithoutReadingCurrentOwner)
{
    verify_reserve_loser (false, false);
}

TEST (ZLinkFrameworkInstanceSpotActivation, ExpiredReserveLoserKeepsTimeoutTerminal)
{
    verify_reserve_loser (true, false);
}

TEST (ZLinkFrameworkInstanceSpotActivation, OneWayReserveLoserReportsUnavailableOnce)
{
    verify_reserve_loser (false, true);
}
} // namespace

TEST (ZLinkFrameworkInstanceSpotActivation, ExpiredAuthorityReservePreservesDeadlineAndCancellation)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    auto opaque = std::make_shared<rt::in_memory_location_store_t> ();
    std::vector<std::shared_ptr<fw::location_repository_t>> repositories{
      std::make_shared<rt::provider_location_repository_t> (*opaque),
      std::make_shared<rt::in_memory_location_repository_t> ()};
    for (const auto &repository : repositories) {
        fw::object_reserve_request_t request;
        request.key = {fw::placement_object_kind_t::instance_spot, "expired-room"};
        request.intent.stable_type = "room-type";
        request.capacity_bundle = {0, 1,
                                   fw::spot_type_capacity_delta_t{
                                     fw::placement_object_kind_t::instance_spot, "room-type", 1}};
        request.operation_deadline =
          std::chrono::system_clock::now () - std::chrono::milliseconds (1);
        const auto expired = repository->reserve (request).result ();
        ASSERT_FALSE (expired);
        EXPECT_EQ (fw::framework_error_kind_t::deadline_exceeded, expired.error_kind ());
        std::stop_source shutdown;
        shutdown.request_stop ();
        const auto cancelled =
          repository->commit ({}, shutdown.get_token (), request.operation_deadline).result ();
        ASSERT_FALSE (cancelled);
        ASSERT_TRUE (cancelled.exception ());
        try {
            std::rethrow_exception (cancelled.exception ());
        }
        catch (const std::exception &error) {
            EXPECT_TRUE (fw::detail::is_cancellation_exception (error));
        }
    }
}
