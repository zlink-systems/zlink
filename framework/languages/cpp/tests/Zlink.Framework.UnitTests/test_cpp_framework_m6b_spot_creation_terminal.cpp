/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/telemetry_observation.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "../support/m6b_cases.hpp"
#include <zlink/json.hpp>
#include "../support/m6b_counting_location_repository.hpp"
#include "../support/m6b_relocation_repository.hpp"
#include "../support/m6b_mesh_fixture.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/locations/source_creation_cleanup.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <nlohmann/json.hpp>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace foundation = zlink::framework::runtime::foundation;
namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;
namespace host = zlink::framework::runtime::host;
using namespace std::chrono_literals;
using zlink::framework::test::begin_reserved_object;

namespace zlink::framework::test::m6b
{
// The public Instance call payload of the Spot Close ready-route cases.
struct quest_probe_t
{
    static constexpr const char *packet_name = "quest.start";
};
}

template <> struct zlink::json::dto_traits<zlink::framework::test::m6b::quest_probe_t>
{
    static constexpr auto fields = std::tuple{};
};

namespace zlink::framework::test::m6b
{
bool verify_remote_user_spot_create_close_terminal_once (bool cleanup_only)
{
    using namespace zlink::framework;
    auto store = std::make_shared<counting_location_repository_t> ();
    const auto claimed = store->claim_owner_lease ("target-owner", 30s).result ().value ();
    const auto *owner = std::get_if<owner_lease_claimed_t> (&claimed);
    assert (owner);
    mesh_node_descriptor_t target_location{
      .mesh_name = "m6b-mesh",
      .rid = zlink::routing_id_t::from ("user-target"),
      .lifecycle_generation = 1,
      .descriptor_revision = 1,
      .endpoint = "tcp://127.0.0.1:1",
      .application_version = 1,
      .object_capabilities = {{.object_kind = placement_object_kind_t::user_spot,
                               .stable_type = "room",
                               .policy = maintenance_policy_kind_t::recreate},
                              {.object_kind = placement_object_kind_t::instance_spot,
                               .stable_type = "quest",
                               .policy = maintenance_policy_kind_t::recreate}},
      .object_role = object_role_t::server,
      .capacity = {.spots = {.limit = 100},
                   .spot_types = {{.object_kind = placement_object_kind_t::instance_spot,
                                   .stable_type = "quest",
                                   .usage = {.limit = 100}}}},
      .state = framework_runtime_state_t::serving,
      .security_identity = "test",
      .owner_id = owner->token.owner_id,
      .lease_generation = owner->token.lease_generation};
    assert (store->update_mesh_node (target_location, location_write_intent_t::new_claim)
              .result ()
              .value ()
              .status
            == location_write_status_t::stored);

    const std::string spot_id = "remote-room";
    const std::vector<std::byte> creation_payload{std::byte{0x41}, std::byte{0x42}};
    const object_reserve_request_t reserve{
      .key = {placement_object_kind_t::user_spot, spot_id},
      .intent = {.stable_type = "room",
                 .request_content_reference = "inline-v1:QUI",
                 .request_sha256 = zlink::framework::runtime::sha256 (creation_payload),
                 .request_encoded_size = 2},
      .target = {.mesh_name = "m6b-mesh",
                 .node_rid = node_rid_t::from_string ("user-target"),
                 .node_lifecycle_generation = 1,
                 .owner = owner->token},
      .creating_payload = {std::byte{0x7f}},
      .capacity_bundle = {
        .spot_slots = 1,
        .spot_type = spot_type_capacity_delta_t{
          .object_kind = placement_object_kind_t::user_spot, .stable_type = "room", .slots = 1}}};
    const auto reserved = store->reserve (reserve).result ().value ();
    const auto *reservation = std::get_if<object_reserved_t> (&reserved);
    assert (reservation);
    assert (reservation->creating.pending_creation);
    assert (reservation->creating.pending_creation->reservation_id
            == reservation->fence.reservation_id);
    assert (reservation->creating.pending_creation->request_content_reference
            == reserve.intent.request_content_reference);
    const std::string invalid_spot_id = "remote-room-invalid";
    auto invalid_reserve = reserve;
    invalid_reserve.key.global_id = invalid_spot_id;
    invalid_reserve.intent.request_encoded_size = 3;
    const auto invalid_reserved = store->reserve (invalid_reserve).result ().value ();
    const auto *invalid_reservation = std::get_if<object_reserved_t> (&invalid_reserved);
    assert (invalid_reservation);
    const std::string cleanup_spot_id = "remote-room-cleanup";
    auto cleanup_reserve = reserve;
    cleanup_reserve.key.global_id = cleanup_spot_id;
    const auto cleanup_reserved = store->reserve (cleanup_reserve).result ().value ();
    const auto *cleanup_reservation = std::get_if<object_reserved_t> (&cleanup_reserved);
    assert (cleanup_reservation);
    assert (zlink::framework::runtime::cleanup_source_created_reservation (
              store, cleanup_reserve.key, cleanup_reservation->fence, false)
            == zlink::framework::runtime::source_creation_cleanup_t::not_owned);
    auto wrong_cleanup_fence = cleanup_reservation->fence;
    wrong_cleanup_fence.reservation_id += "-other";
    assert (zlink::framework::runtime::cleanup_source_created_reservation (
              store, cleanup_reserve.key, wrong_cleanup_fence, true)
            == zlink::framework::runtime::source_creation_cleanup_t::stale);
    assert (zlink::framework::runtime::cleanup_source_created_reservation (
              store, cleanup_reserve.key, cleanup_reservation->fence, true)
            == zlink::framework::runtime::source_creation_cleanup_t::aborted);
    assert (std::holds_alternative<authority_missing_t> (
      store->read_authority (zlink::framework::runtime::spot_authority_key (cleanup_spot_id))
        .result ()
        .value ()));

    auto source = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("user-source"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    auto instance_dispatch_diagnostics =
      std::make_shared<test::telemetry_observations_t<message_dispatch_error_event_t>> ();
    const auto dispatch_diagnostic_snapshot = [instance_dispatch_diagnostics] {
        return instance_dispatch_diagnostics->snapshot ();
    };
    auto target_options = host::host_options_t{
      .mesh =
        mesh::raw_mesh_node_options_t{
          .descriptor = descriptor ("user-target"),
          .runtime_failures =
            std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      .user_spot_operation_replay_retention = 50ms};
    target_options.mesh.dispatch.message_flow (message_flow_log_mode_t::normal);
    detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      target_options.mesh.dispatch,
      [instance_dispatch_diagnostics] (const message_dispatch_error_event_t &event) {
          if (event.surface == dispatch_error_surface_t::instance_spot) {
              assert (event.action
                      == (event.message_kind == dispatch_message_kind_t::request
                            ? dispatch_error_action_t::reply_error
                            : dispatch_error_action_t::drop));
              assert (event.error_message);
              instance_dispatch_diagnostics->record (event);
          }
      });
    auto target = std::make_shared<host::public_host_runtime_t> (std::move (target_options));
    std::size_t materialize_count = 0;
    target->configure_user_spot_operations (
      store,
      [&materialize_count] (const stateful::object_ref_t &object, const std::string &stable_type,
                            const std::vector<std::byte> &creation)
        -> zlink::framework::task_t<
          zlink::framework::runtime::host::user_spot_materialize_result_t> {
          ++materialize_count;
          assert (object.object_generation != 0);
          assert (stable_type == "room");
          assert (creation == std::vector<std::byte> ({std::byte{0x41}, std::byte{0x42}}));
          co_return host::user_spot_materialize_result_t{true, std::nullopt};
      });
    if (cleanup_only) {
        task_completion_source_t<void> cleanup_release;
        bool cleanup_started = false;
        target->configure_user_spot_operations (
          store,
          [&] (const stateful::object_ref_t &, const std::string &, const std::vector<std::byte> &)
            -> zlink::framework::task_t<
              zlink::framework::runtime::host::user_spot_materialize_result_t> {
              // Revoke this reservation after materialization so publication fails.
              (void) store->abort ({reserve.key, reservation->fence}).result ().value ();
              co_return host::user_spot_materialize_result_t{true, std::nullopt,
                                                             [&] (bool ready) -> task_t<void> {
                                                                 assert (!ready);
                                                                 cleanup_started = true;
                                                                 return cleanup_release.task ();
                                                             }};
          });
        target->start ();
        std::promise<
          std::pair<foundation::operation_terminal_t, protocol::user_spot_create_reply_t>>
          reply_completion;
        auto local_reply = reply_completion.get_future ();
        const auto deadline_ms =
          static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                        std::chrono::system_clock::now ().time_since_epoch () + 5s)
                                        .count ());
        const protocol::user_spot_create_header_t request{
          1,
          {99, 1},
          target->status ().routing_id ().to_bytes (),
          target->status ().lifecycle_generation (),
          spot_id,
          "room",
          {reservation->fence.reservation_id, reservation->fence.expected_store_version,
           reservation->fence.object_generation, reservation->fence.authority_owner_generation,
           target->status ().routing_id ().to_bytes (), target->status ().lifecycle_generation (),
           reservation->fence.target.owner.owner_id,
           static_cast<std::uint64_t> (reservation->fence.target.owner.lease_generation),
           reservation->fence.capacity_bundle.spot_slots},
          deadline_ms};
        const auto submitted =
          target
            ->create_user_spot_remote (
              target->status ().routing_id (), request, 5s,
              [&] (foundation::operation_terminal_t terminal,
                   result_t<protocol::user_spot_create_reply_t> completion_reply,
                   std::optional<protocol::application_payload_t>) {
                  auto reply = std::move (completion_reply.value ());
                  reply_completion.set_value ({terminal, std::move (reply)});
              })
            .result ();
        assert (submitted && submitted.value ());
        auto dispatched =
          target->dispatch_ready ([] (const host::ready_record_t &, const host::receive_record_t &,
                                      std::vector<zlink::message_t>, auto) {});
        const auto pending_before_cleanup = target->pending_operation_count ();
        cleanup_release.complete (result_t<void>::success ());
        const auto result = dispatched.result ();
        const auto [reply_terminal, reply] = local_reply.get ();
        const auto pending_after_cleanup = target->pending_operation_count ();
        test::acknowledge_delivery (std::chrono::steady_clock::now () + 5s);
        target->close ();
        std::cout << "cleanup_started=" << cleanup_started
                  << " pending_before_cleanup=" << pending_before_cleanup
                  << " pending_after_cleanup=" << pending_after_cleanup
                  << " rejected=" << (reply.result == protocol::user_spot_create_result_t::rejected)
                  << std::endl;
        return result && result.value () == 1 && cleanup_started && pending_before_cleanup == 1
               && pending_after_cleanup == 0
               && reply_terminal == foundation::operation_terminal_t::completed
               && reply.result == protocol::user_spot_create_result_t::rejected;
    }
    std::size_t instance_activation_count = 0;
    std::size_t instance_prepare_count = 0;
    bool fail_recovery_dispatch_once = false;
    std::optional<framework_error_kind_t> injected_activation_failure;
    dispatch_error_reason_t injected_failure_reason = dispatch_error_reason_t::handler_exception;
    bool close_during_instance_turn = false;
    std::size_t recovery_barrier_finishes = 0;
    std::size_t recovery_accepted_finishes = 0;
    auto instance_relocations = std::make_shared<memory_relocation_repository_t> ();
    const std::weak_ptr<host::public_host_runtime_t> weak_target = target;
    target->configure_instance_spot_operations (
      store, instance_relocations, [owner] { return owner->token; },
      host::instance_spot_activation_materializer_t{
        [&instance_prepare_count, &store, &recovery_barrier_finishes] (
          const protocol::instance_spot_activation_header_t &activation,
          const authority_snapshot_t &, std::function<task_t<void> (bool)> &preparation_terminal)
          -> zlink::framework::task_t<bool> {
            if (store->activation_recovery_write_observer)
                preparation_terminal = [&recovery_barrier_finishes] (bool) {
                    ++recovery_barrier_finishes;
                    return task_t<void> (result_t<void>::success ());
                };
            ++instance_prepare_count;
            assert (!activation.target.spot_id.empty ());
            co_return true;
        },
        [&instance_activation_count, &store, &fail_recovery_dispatch_once,
         &close_during_instance_turn, &recovery_accepted_finishes, &injected_activation_failure,
         weak_target] (std::shared_ptr<const protocol::instance_activation_recovery_t> command,
                       std::function<task_t<zlink::message_t> (std::function<void ()> &)>,
                       std::function<void ()> accept_activation) {
            const auto &activation = command->activation;
            const auto &metadata = command->metadata;
            const auto &application = command->application_payload;
            if (injected_activation_failure) {
                const auto failure =
                  runtime::messaging::request_failure_mapper_t{}.target_failure_reply (
                    *injected_activation_failure);
                assert (failure);
                return task_t<host::instance_spot_activation_result_t> (
                  result_t<host::instance_spot_activation_result_t>::success (
                    {failure->terminal_result, failure->failure_code, std::nullopt}));
            }
            accept_activation ();
            ++instance_activation_count;
            assert (activation.target.stable_type == "quest");
            assert (activation.request);
            const auto authority =
              store
                ->read_authority (
                  zlink::framework::runtime::spot_authority_key (activation.target.spot_id))
                .result ()
                .value ();
            const auto *authority_snapshot = std::get_if<authority_snapshot_t> (&authority);
            assert (authority_snapshot);
            assert (metadata
                    == std::optional<std::vector<std::uint8_t>> (
                      {{1, 1, 5, 't', 'r', 'a', 'c', 'e', 0, 3, 'a', 'b', 'c'}}));
            assert (application.packet_name () == "quest.start");
            if (activation.target.spot_id == "instance-recover" && fail_recovery_dispatch_once) {
                fail_recovery_dispatch_once = false;
                throw std::runtime_error ("simulated process failure after Ready publication");
            }
            std::function<void ()> accepted_turn_terminal;
            if (store->activation_recovery_write_observer) {
                accepted_turn_terminal = [&recovery_accepted_finishes] {
                    ++recovery_accepted_finishes;
                };
            }
            if (close_during_instance_turn) {
                const auto runtime = weak_target.lock ();
                assert (runtime);
                auto close = runtime
                               ->begin_instance_spot_close (
                                 activation.target.stable_type, activation.target.spot_id,
                                 authority_snapshot->object_generation,
                                 authority_snapshot->authority_owner_generation)
                               .result ();
                assert (close && close.value ().release);
                accepted_turn_terminal = [release = std::move (close.value ().release)] () mutable {
                    const auto released = release ().result ();
                    assert (released && released.value ());
                };
            }
            return task_t<host::instance_spot_activation_result_t> (
              result_t<host::instance_spot_activation_result_t>::success (
                {0, 0,
                 protocol::application_payload_t{"quest.reply", "application/json", {'{', '}'}},
                 std::move (accepted_turn_terminal)}));
        }});
    source->start ();
    target->start ();
    assert (
      source->connect_peer (target->status ().local_endpoint (), target->status ().routing_id ()));
    auto deadline = std::chrono::steady_clock::now () + 5s;
    auto dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                        std::vector<zlink::message_t>, auto) {};
    while (
      (!ready_peer (source->transport ().topology (), target->status ().routing_id ().to_bytes ())
       || !ready_peer (target->transport ().topology (),
                       source->status ().routing_id ().to_bytes ()))
      && std::chrono::steady_clock::now () < deadline) {
        (void) source->dispatch_ready (dispatch);
        (void) target->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (source->transport ().topology ().peer (target->status ().routing_id ().to_bytes ()));

    std::optional<protocol::reply_header_t> instance_reply_header;
    std::optional<protocol::application_payload_t> instance_reply_payload;
    protocol::instance_spot_activation_header_t instance_request{
      {target->status ().routing_id ().to_bytes (), target->status ().lifecycle_generation (),
       "instance-1", "m6b-mesh", "quest", "descriptor-1",
       static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                     std::chrono::system_clock::now ().time_since_epoch () + 5s)
                                     .count ())},
      source->status ().lifecycle_generation (),
      source->status ().routing_id ().to_bytes (),
      std::string ("entry"),
      true,
      {123, 456},
      0,
      true};
    const auto replay_instance_request = instance_request;
    assert (source
              ->activate_instance_spot_remote (
                target->status ().routing_id (), std::move (instance_request),
                std::vector<std::uint8_t>{1, 1, 5, 't', 'r', 'a', 'c', 'e', 0, 3, 'a', 'b', 'c'},
                {"quest.start", "application/json", {'{', '}'}}, 5s,
                [&] (foundation::operation_terminal_t terminal, protocol::reply_header_t header,
                     std::optional<protocol::application_payload_t> reply_payload) {
                    assert (terminal == foundation::operation_terminal_t::completed);
                    instance_reply_header = header;
                    instance_reply_payload = std::move (reply_payload);
                })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!instance_reply_header && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (instance_reply_header);
    assert (instance_reply_header->terminal_result == 0);
    assert (instance_reply_payload);
    assert (instance_reply_payload->packet_name () == "quest.reply");
    assert (instance_prepare_count == 1);
    assert (instance_activation_count == 1);

    assert (instance_relocations->size () == 0);

    const auto ready_authority = store
                                   ->read_authority (zlink::framework::runtime::spot_authority_key (
                                     replay_instance_request.target.spot_id))
                                   .result ()
                                   .value ();
    const auto *instance_ready = std::get_if<authority_snapshot_t> (&ready_authority);
    assert (instance_ready
            && instance_ready->allocation.state == placement_allocation_state_t::active);
    std::ifstream close_fixture_input (ZLINK_SPOT_CLOSE_CONFORMANCE_PATH);
    assert (close_fixture_input);
    const auto close_fixture = nlohmann::json::parse (close_fixture_input);
    std::uint64_t ready_case_operation = 1000;
    // The public Instance call path: a Store resolver whose route cache keeps the
    // Ready route it resolved before Close, and the source host as the activator.
    location_options_t route_cache_options;
    route_cache_options.route_cache_max_age = 30s;
    const auto route_resolver_owner =
      std::make_shared<runtime::store_location_resolvers_t> (*store, route_cache_options);
    auto &route_resolver = *route_resolver_owner;
    serializer_registry_t serializers;
    zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto channels = detail::channel_runtime_t::from (builder.message_bus ());
    channels.bind_serializers (serializers);
    channels.bind_spot_address_resolver (route_resolver_owner);
    channels.bind_instance_spot_activator (
      [] (const spot_id_t &, const detail::spot_activation_intent_t &,
          const std::optional<runtime::spot_address_t> &, const std::string &, std::type_index,
          std::function<serialized_payload_t (serializer_registry_t &)>,
          const std::map<std::string, std::string> &,
          std::chrono::system_clock::time_point) -> task_t<result_t<void>> {
          co_return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                             "unused one-way activation");
      },
      [&] (const spot_id_t &spot_id, const detail::spot_activation_intent_t &intent,
           const std::optional<runtime::spot_address_t> &cached_route, std::string, std::type_index,
           std::function<serialized_payload_t (serializer_registry_t &)>,
           std::chrono::milliseconds timeout,
           std::map<std::string, std::string>) -> request_task_t<zlink::message_t> {
          // The same activation header the app builds for a cached or a cold route.
          auto header = replay_instance_request;
          header.operation = {123, ready_case_operation++};
          header.target.spot_id = std::string (spot_id);
          header.target.deadline_unix_ms = static_cast<std::uint64_t> (
            std::chrono::duration_cast<std::chrono::milliseconds> (
              std::chrono::system_clock::now ().time_since_epoch () + timeout)
              .count ());
          if (cached_route) {
              header.target.object_generation = cached_route->object_generation;
              header.target.authority_owner_generation = cached_route->authority_owner_generation;
              header.target.owner_id = cached_route->owner.owner_id;
              header.target.owner_lease_generation =
                static_cast<std::uint64_t> (cached_route->owner.lease_generation);
              header.target.store_version = cached_route->store_version;
              header.target.instance_intent = intent.instance;
              header.target.deadline_unix_ms = 0;
          }
          auto completion = std::make_shared<task_completion_source_t<zlink::message_t>> ();
          auto output = completion->task ();
          const auto submitted = co_await source->activate_instance_spot_remote (
            target->status ().routing_id (), std::move (header),
            std::vector<std::uint8_t>{1, 1, 5, 't', 'r', 'a', 'c', 'e', 0, 3, 'a', 'b', 'c'},
            {"quest.start", "application/json", {'{', '}'}}, timeout,
            [completion] (foundation::operation_terminal_t terminal, protocol::reply_header_t reply,
                          std::optional<protocol::application_payload_t> payload) {
                if (terminal != foundation::operation_terminal_t::completed) {
                    completion->complete (result_t<zlink::message_t>::failure (
                      framework_error_kind_t::unavailable, "activation transport failed"));
                } else if (reply.terminal_result != 0) {
                    completion->complete (detail::result_access_t::failure<zlink::message_t> (
                      runtime::messaging::request_failure_mapper_t{}.reply_header_exception (
                        reply.terminal_result, reply.failure_code, "Instance Spot request")));
                } else {
                    assert (payload);
                    completion->complete (result_t<zlink::message_t>::success (
                      zlink::message_t::from (std::move (payload->payload_bytes ()))));
                }
            });
          if (!submitted)
              completion->complete (result_t<zlink::message_t>::failure (
                framework_error_kind_t::unavailable, "activation was not admitted"));
          if (submitted)
              co_await detail::request_admitted_t{};
          co_return co_await output;
      });
    auto instance_route = builder.route_client (serializers);
    std::size_t ready_route_cold_activations = 0;
    const auto verify_ready_route_cases = [&] (const std::string &authority,
                                               const authority_snapshot_t &ready) {
        for (const auto &scenario : close_fixture.at ("readyRouteCases")) {
            const auto &given = scenario.at ("given");
            const auto &expected = scenario.at ("expect");
            if (given.at ("authority") != authority)
                continue;
            assert (given.at ("ownerFence") == "mismatch");
            for (const auto &intent : given.at ("instanceIntent")) {
                auto request = replay_instance_request;
                request.operation = {123, ready_case_operation++};
                request.has_metadata = false;
                if (injected_activation_failure)
                    request.target.spot_id = "instance-recover";
                request.target.object_generation = ready.object_generation;
                request.target.authority_owner_generation =
                  ready.authority_owner_generation + (injected_activation_failure ? 0 : 1);
                // Keep the fixture's ownerFence=mismatch across a public cold
                // activation too: an authority token from an older incarnation
                // alone no longer represents a current owner mismatch.
                request.target.owner_id =
                  ready.owner.owner_id + (injected_activation_failure ? "" : "-stale");
                request.target.owner_lease_generation =
                  static_cast<std::uint64_t> (ready.owner.lease_generation);
                request.target.store_version = ready.store_version;
                request.target.instance_intent = intent.get<bool> ();
                request.request = given.at ("messageKind") == "request";
                const auto prepares_before = instance_prepare_count;
                const auto handlers_before = instance_activation_count;
                const auto reservations_before = store->reservation_calls.load ();
                const auto reads_before = store->authority_reads.load ();
                instance_reply_header.reset ();
                instance_reply_payload.reset ();
                deadline = std::chrono::steady_clock::now () + 5s;
                // The preceding operation finished before resetting its owned observations.
                test::acknowledge_delivery (deadline);
                instance_dispatch_diagnostics->clear ();
                std::size_t terminal_count = 0;
                // Failover policy §4.4: the public Instance intent call still uses
                // the Ready route its resolver cached before Close released authority.
                const bool public_call =
                  request.request && request.target.instance_intent && authority == "Missing";
                std::optional<task_t<quest_probe_t>> public_reply;
                if (public_call) {
                    public_reply.emplace (
                      instance_route
                        .request_to_spot (spot_id_t (replay_instance_request.target.spot_id),
                                          quest_probe_t{})
                        .instance_spot ("quest")
                        .timeout (5s)
                        .async<quest_probe_t> ());
                } else if (request.request) {
                    assert (source
                              ->activate_instance_spot_remote (
                                target->status ().routing_id (), request, std::nullopt,
                                {"quest.start", "application/json", {'{', '}'}}, 5s,
                                [&] (foundation::operation_terminal_t terminal,
                                     protocol::reply_header_t header,
                                     std::optional<protocol::application_payload_t> payload) {
                                    assert (terminal
                                            == foundation::operation_terminal_t::completed);
                                    ++terminal_count;
                                    instance_reply_header = header;
                                    instance_reply_payload = std::move (payload);
                                })
                              .result ()
                              .value ());
                } else {
                    assert (source
                              ->send_instance_spot_activation_remote (
                                target->status ().routing_id (), request, std::nullopt,
                                {"quest.start", "application/json", {'{', '}'}})
                              .result ()
                              .value ());
                }
                while ((public_call       ? !public_reply->await_ready ()
                        : request.request ? !instance_reply_header
                                          : dispatch_diagnostic_snapshot ().empty ())
                       && std::chrono::steady_clock::now () < deadline) {
                    (void) target->dispatch_ready (dispatch);
                    (void) source->dispatch_ready (dispatch);
                    std::this_thread::sleep_for (1ms);
                }
                // dispatch_ready/actual request completion has crossed the producer boundary.
                test::acknowledge_delivery (deadline);
                assert (store->authority_reads.load () > reads_before);
                if (injected_activation_failure) {
                    const auto diagnostics = dispatch_diagnostic_snapshot ();
                    assert (diagnostics.size () == 1);
                    const auto &event = diagnostics.front ();
                    assert (event.message_kind
                            == (request.request ? dispatch_message_kind_t::request
                                                : dispatch_message_kind_t::send));
                    const auto expected_failure =
                      runtime::messaging::request_failure_mapper_t{}.target_failure_reply (
                        *injected_activation_failure);
                    const auto expected_error =
                      runtime::messaging::request_failure_mapper_t{}.reply_header_exception (
                        expected_failure->terminal_result, expected_failure->failure_code,
                        "Instance Spot activation");
                    assert (event.error_type == typeid (framework_exception_t).name ());
                    assert (event.reason == injected_failure_reason);
                    assert (event.error_message == expected_error.what ());
                    if (request.request) {
                        assert (terminal_count == 1 && instance_reply_header);
                        assert (instance_reply_header->terminal_result
                                == expected_failure->terminal_result);
                        assert (instance_reply_header->failure_code
                                == expected_failure->failure_code);
                    } else {
                        assert (!instance_reply_header);
                    }
                    continue;
                }
                if (public_call) {
                    // The cached route reached the owner, whose Ready owner fence
                    // refused it before admission; the request then cold-activated.
                    assert (public_reply->await_ready () && public_reply->result ());
                    assert (expected.at ("messageTerminal") == "reply");
                    assert (expected.at ("messageTerminalCount") == 1);
                    const auto diagnostics = dispatch_diagnostic_snapshot ();
                    assert (diagnostics.size () == 1);
                    assert (diagnostics.front ().reason == dispatch_error_reason_t::stale_target);
                    ++ready_route_cold_activations;
                } else if (request.request) {
                    assert (instance_reply_header);
                    const auto kind = runtime::messaging::request_failure_mapper_t{}
                                        .reply_header_exception (
                                          instance_reply_header->terminal_result,
                                          instance_reply_header->failure_code, "Ready owner fence")
                                        .kind ();
                    const auto terminal_name =
                      kind == framework_error_kind_t::unavailable ? "Unavailable" : "unexpected";
                    assert (expected.at ("messageTerminal") == terminal_name);
                    assert (expected.at ("messageTerminalCount") == terminal_count);
                    assert (!instance_reply_payload);
                    const auto diagnostics = dispatch_diagnostic_snapshot ();
                    assert (diagnostics.size () == 1);
                    assert (diagnostics.front ().message_kind == dispatch_message_kind_t::request);
                    assert (diagnostics.front ().action == dispatch_error_action_t::reply_error);
                    assert (diagnostics.front ().reason == dispatch_error_reason_t::stale_target);
                } else {
                    assert (!instance_reply_header);
                    std::vector<std::string> expected_diagnostics;
                    for (const auto &expected_kind : expected.at ("diagnostics")) {
                        assert (expected_kind == "Unavailable");
                        // The owner refuses a stale Ready owner fence before admission.
                        const auto wire =
                          runtime::messaging::request_failure_mapper_t{}.target_failure_reply (
                            framework_error_kind_t::unavailable,
                            static_cast<std::uint32_t> (
                              protocol::framework_error_code::spotMoving));
                        assert (wire);
                        expected_diagnostics.emplace_back (
                          runtime::messaging::request_failure_mapper_t{}
                            .reply_header_exception (wire->terminal_result, wire->failure_code,
                                                     "Instance Spot activation")
                            .what ());
                    }
                    const auto diagnostics = dispatch_diagnostic_snapshot ();
                    assert (diagnostics.size () == expected_diagnostics.size ());
                    for (std::size_t index = 0; index < diagnostics.size (); ++index) {
                        const auto &event = diagnostics[index];
                        assert (event.error_message == expected_diagnostics[index]);
                        assert (detail::enum_name (event.surface)
                                == expected.at ("surface").get<std::string> ());
                        assert (detail::enum_name (event.reason)
                                == expected.at ("reason").get<std::string> ());
                        assert (event.exception);
                        try {
                            std::rethrow_exception (event.exception);
                        }
                        catch (const framework_exception_t &error) {
                            assert (error.kind () == framework_error_kind_t::unavailable);
                        }
                    }
                }
                assert (expected.at ("factoryCalls") == instance_prepare_count - prepares_before);
                assert (expected.at ("handlerCalls")
                        == instance_activation_count - handlers_before);
                assert (expected.at ("missingPlacementCalls")
                        == store->reservation_calls.load () - reservations_before);
            }
        }
    };
    verify_ready_route_cases ("Ready", *instance_ready);
    const auto after_stale_ready =
      store
        ->read_authority (
          zlink::framework::runtime::spot_authority_key (replay_instance_request.target.spot_id))
        .result ()
        .value ();
    const auto *unchanged_ready = std::get_if<authority_snapshot_t> (&after_stale_ready);
    assert (unchanged_ready && unchanged_ready->store_version == instance_ready->store_version);
    assert (unchanged_ready->object_generation == instance_ready->object_generation);
    assert (unchanged_ready->authority_owner_generation
            == instance_ready->authority_owner_generation);
    assert (instance_relocations->size () == 0);

    instance_reply_header.reset ();
    instance_reply_payload.reset ();
    assert (source
              ->activate_instance_spot_remote (
                target->status ().routing_id (), replay_instance_request,
                std::vector<std::uint8_t>{1, 1, 5, 't', 'r', 'a', 'c', 'e', 0, 3, 'a', 'b', 'c'},
                {"quest.start", "application/json", {'{', '}'}}, 5s,
                [&] (foundation::operation_terminal_t terminal, protocol::reply_header_t header,
                     std::optional<protocol::application_payload_t> reply_payload) {
                    assert (terminal == foundation::operation_terminal_t::completed);
                    instance_reply_header = header;
                    instance_reply_payload = std::move (reply_payload);
                })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!instance_reply_header && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (instance_reply_header && instance_reply_header->terminal_result == 0);
    assert (instance_reply_payload && instance_reply_payload->packet_name () == "quest.reply");
    assert (instance_prepare_count == 2);
    assert (instance_activation_count == 2);

    instance_reply_header.reset ();
    instance_reply_payload.reset ();
    assert (source
              ->activate_instance_spot_remote (
                target->status ().routing_id (), replay_instance_request,
                std::vector<std::uint8_t>{1, 1, 5, 't', 'r', 'a', 'c', 'e', 0, 3, 'a', 'b', 'c'},
                {"quest.start", "application/json", {'[', ']'}}, 5s,
                [&] (foundation::operation_terminal_t terminal, protocol::reply_header_t header,
                     std::optional<protocol::application_payload_t> reply_payload) {
                    assert (terminal == foundation::operation_terminal_t::completed);
                    instance_reply_header = header;
                    instance_reply_payload = std::move (reply_payload);
                })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!instance_reply_header && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (instance_reply_header && instance_reply_header->terminal_result == 0);
    assert (instance_reply_payload && instance_reply_payload->packet_name () == "quest.reply");
    assert (instance_prepare_count == 3);
    assert (instance_activation_count == 3);

    auto recovery_request = replay_instance_request;
    recovery_request.target.spot_id = "instance-recover";
    recovery_request.operation = {123, 457};
    recovery_request.target.deadline_unix_ms =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () + 5s)
                                    .count ());
    fail_recovery_dispatch_once = true;
    instance_reply_header.reset ();
    instance_reply_payload.reset ();
    assert (source
              ->activate_instance_spot_remote (
                target->status ().routing_id (), recovery_request,
                std::vector<std::uint8_t>{1, 1, 5, 't', 'r', 'a', 'c', 'e', 0, 3, 'a', 'b', 'c'},
                {"quest.start", "application/json", {'{', '}'}}, 5s,
                [&] (foundation::operation_terminal_t terminal, protocol::reply_header_t header,
                     std::optional<protocol::application_payload_t> reply_payload) {
                    assert (terminal == foundation::operation_terminal_t::completed);
                    instance_reply_header = header;
                    instance_reply_payload = std::move (reply_payload);
                })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!instance_reply_header && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (instance_reply_header && instance_reply_header->terminal_result == 105);
    assert (instance_relocations->size () == 1);
    assert (instance_prepare_count == 4);
    assert (instance_activation_count == 4);

    const auto recovery_authority_key =
      zlink::framework::runtime::spot_authority_key ("instance-recover");
    const auto before_recovery = store->read_authority (recovery_authority_key).result ().value ();
    const auto *before_recovery_snapshot = std::get_if<authority_snapshot_t> (&before_recovery);
    assert (before_recovery_snapshot);
    const auto before_recovery_payload =
      zlink::framework::runtime::decode_instance_spot_authority_payload (
        before_recovery_snapshot->payload);
    assert (before_recovery_payload && before_recovery_payload->activation_recovery
            && before_recovery_payload->activation_recovery->inbox_sequence == 1
            && before_recovery_payload->activation_recovery->replay_cursor == 0);
    store->activation_recovery_write_observer = [&] (const authority_key_t &key, const auto &) {
        assert (key.value == recovery_authority_key.value);
        assert (recovery_barrier_finishes == 0 && recovery_accepted_finishes == 0);
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "injected startup recovery cursor Store failure");
    };
    try {
        (void) target->recover_instance_spot_activations ().result ().value ();
        assert (false);
    }
    catch (const framework_exception_t &error) {
        assert (error.kind () == framework_error_kind_t::internal_failure);
    }
    assert (recovery_barrier_finishes == 1 && recovery_accepted_finishes == 1);
    assert (instance_relocations->size () == 1);
    recovery_barrier_finishes = 0;
    recovery_accepted_finishes = 0;
    store->activation_recovery_write_observer = [&] (const authority_key_t &key, const auto &) {
        assert (key.value == recovery_authority_key.value);
        assert (recovery_barrier_finishes == 0 && recovery_accepted_finishes == 0);
    };
    store->clear_activation_recovery_writes ();
    assert (target->recover_instance_spot_activations ().result ().value () == 1);
    store->activation_recovery_write_observer = {};
    assert (recovery_barrier_finishes == 1 && recovery_accepted_finishes == 1);
    assert (instance_relocations->size () == 0);
    assert (instance_prepare_count == 6);
    assert (instance_activation_count == 6);
    const auto recovery_writes = store->activation_recovery_write_snapshot ();
    assert (recovery_writes.size () == 2);
    assert (recovery_writes[0].first == recovery_authority_key.value
            && recovery_writes[0].second == std::optional<std::uint64_t>{1});
    assert (recovery_writes[1].first == recovery_authority_key.value && !recovery_writes[1].second);
    const auto after_recovery = store->read_authority (recovery_authority_key).result ().value ();
    const auto *after_recovery_snapshot = std::get_if<authority_snapshot_t> (&after_recovery);
    assert (after_recovery_snapshot);
    const auto after_recovery_payload =
      zlink::framework::runtime::decode_instance_spot_authority_payload (
        after_recovery_snapshot->payload);
    assert (after_recovery_payload && !after_recovery_payload->activation_recovery);

    instance_reply_header.reset ();
    instance_reply_payload.reset ();
    assert (source
              ->activate_instance_spot_remote (
                target->status ().routing_id (), recovery_request,
                std::vector<std::uint8_t>{1, 1, 5, 't', 'r', 'a', 'c', 'e', 0, 3, 'a', 'b', 'c'},
                {"quest.start", "application/json", {'{', '}'}}, 5s,
                [&] (foundation::operation_terminal_t terminal, protocol::reply_header_t header,
                     std::optional<protocol::application_payload_t> reply_payload) {
                    assert (terminal == foundation::operation_terminal_t::completed);
                    instance_reply_header = header;
                    instance_reply_payload = std::move (reply_payload);
                })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!instance_reply_header && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (instance_reply_header && instance_reply_header->terminal_result == 0);
    assert (instance_reply_payload && instance_reply_payload->packet_name () == "quest.reply");
    assert (instance_prepare_count == 7);
    assert (instance_activation_count == 7);

    // 이 Ready authority는 아래 turn 안의 Close가 정상 Release한다.
    const auto released_read =
      store->read_authority (runtime::spot_authority_key (replay_instance_request.target.spot_id))
        .result ()
        .value ();
    const auto *released = std::get_if<authority_snapshot_t> (&released_read);
    assert (released);
    // The source resolves the Ready route now; its route cache keeps it across the Close.
    const auto cached_ready_route =
      route_resolver.resolve_spot_address ({}, replay_instance_request.target.spot_id)
        .result ()
        .value ();
    assert (cached_ready_route
            && cached_ready_route->authority_owner_generation
                 == released->authority_owner_generation);

    auto closing_instance_request = replay_instance_request;
    closing_instance_request.operation = {123, 458};
    closing_instance_request.target.deadline_unix_ms =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () + 5s)
                                    .count ());
    close_during_instance_turn = true;
    instance_reply_header.reset ();
    instance_reply_payload.reset ();
    assert (source
              ->activate_instance_spot_remote (
                target->status ().routing_id (), closing_instance_request,
                std::vector<std::uint8_t>{1, 1, 5, 't', 'r', 'a', 'c', 'e', 0, 3, 'a', 'b', 'c'},
                {"quest.start", "application/json", {'{', '}'}}, 5s,
                [&] (foundation::operation_terminal_t terminal, protocol::reply_header_t header,
                     std::optional<protocol::application_payload_t> reply_payload) {
                    assert (terminal == foundation::operation_terminal_t::completed);
                    instance_reply_header = header;
                    instance_reply_payload = std::move (reply_payload);
                })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!instance_reply_header && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (instance_reply_header && instance_reply_header->terminal_result == 0);
    assert (instance_reply_payload && instance_reply_payload->packet_name () == "quest.reply");
    assert (std::holds_alternative<authority_missing_t> (
      store->read_authority (zlink::framework::runtime::spot_authority_key ("instance-1"))
        .result ()
        .value ()));
    close_during_instance_turn = false;

    verify_ready_route_cases ("Missing", *released);
    // Only a cold activation through the public Instance call publishes a new authority.
    assert (
      std::holds_alternative<authority_missing_t> (
        store->read_authority (runtime::spot_authority_key (replay_instance_request.target.spot_id))
          .result ()
          .value ())
      == (ready_route_cold_activations == 0));

    // Inject the materializer results from missing activation, sealed admission,
    // and handler failure at the terminal owner's existing request/send route.
    for (const auto &[kind, reason] :
         {std::pair{framework_error_kind_t::not_found, dispatch_error_reason_t::handler_missing},
          std::pair{framework_error_kind_t::rejected, dispatch_error_reason_t::handler_exception},
          std::pair{framework_error_kind_t::shutting_down, dispatch_error_reason_t::shutdown},
          std::pair{framework_error_kind_t::internal_failure,
                    dispatch_error_reason_t::handler_exception}}) {
        injected_activation_failure = kind;
        injected_failure_reason = reason;
        verify_ready_route_cases ("Ready", *after_recovery_snapshot);
    }
    injected_activation_failure.reset ();

    // Reincarnate issues a new object/authority pair while keeping the same owner.
    // A Ready route resolved before that transition must reach the new incarnation.
    const auto request_recreated = [&] (protocol::instance_spot_activation_header_t request) {
        request.operation = {123, ready_case_operation++};
        instance_reply_header.reset ();
        instance_reply_payload.reset ();
        assert (
          source
            ->activate_instance_spot_remote (
              target->status ().routing_id (), request,
              std::vector<std::uint8_t>{1, 1, 5, 't', 'r', 'a', 'c', 'e', 0, 3, 'a', 'b', 'c'},
              {"quest.start", "application/json", {'{', '}'}}, 5s,
              [&] (foundation::operation_terminal_t terminal, protocol::reply_header_t header,
                   std::optional<protocol::application_payload_t> payload) {
                  assert (terminal == foundation::operation_terminal_t::completed);
                  instance_reply_header = header;
                  instance_reply_payload = std::move (payload);
              })
            .result ()
            .value ());
        const auto until = std::chrono::steady_clock::now () + 5s;
        while (!instance_reply_header && std::chrono::steady_clock::now () < until) {
            (void) target->dispatch_ready (dispatch);
            (void) source->dispatch_ready (dispatch);
            std::this_thread::sleep_for (1ms);
        }
        assert (instance_reply_header);
        return *instance_reply_header;
    };
    auto recreated_request = replay_instance_request;
    recreated_request.target.spot_id = "instance-recreated";
    recreated_request.target.deadline_unix_ms =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () + 5s)
                                    .count ());
    assert (request_recreated (recreated_request).terminal_result == 0);
    const auto recreated_key = runtime::spot_authority_key (recreated_request.target.spot_id);
    const auto old_read = store->read_authority (recreated_key).result ().value ();
    const auto old_ready = std::get<authority_snapshot_t> (old_read);
    const auto close_recreated =
      target
        ->begin_instance_spot_close (recreated_request.target.stable_type,
                                     recreated_request.target.spot_id, old_ready.object_generation,
                                     old_ready.authority_owner_generation)
        .result ();
    assert (close_recreated && close_recreated.value ().reincarnate);
    const auto fresh_ready = close_recreated.value ().reincarnate ().result ().value ();
    assert (fresh_ready.object_generation > old_ready.object_generation);
    assert (fresh_ready.authority_owner_generation != old_ready.authority_owner_generation);
    assert (fresh_ready.owner.owner_id == old_ready.owner.owner_id);
    recreated_request.target.object_generation = old_ready.object_generation;
    recreated_request.target.authority_owner_generation = old_ready.authority_owner_generation;
    recreated_request.target.owner_id = old_ready.owner.owner_id;
    recreated_request.target.owner_lease_generation = old_ready.owner.lease_generation;
    recreated_request.target.store_version = old_ready.store_version;
    recreated_request.target.deadline_unix_ms = 0;
    recreated_request.target.instance_intent = true;
    const auto recreated_handlers_before = instance_activation_count;
    assert (request_recreated (recreated_request).terminal_result == 0);
    assert (instance_activation_count == recreated_handlers_before + 1);

    // A direct existing-only call uses the current Ready incarnation as well.
    recreated_request.target.instance_intent = false;
    assert (request_recreated (recreated_request).terminal_result == 0);
    assert (instance_activation_count == recreated_handlers_before + 2);
    const auto delivered_before_refusals = instance_activation_count;
    enum class route_mismatch_t
    {
        owner,
        lease,
        authority,
        same_incarnation_owner,
        future
    };
    for (const auto mismatch :
         {route_mismatch_t::owner, route_mismatch_t::lease, route_mismatch_t::authority,
          route_mismatch_t::same_incarnation_owner, route_mismatch_t::future}) {
        auto refused = recreated_request;
        switch (mismatch) {
            case route_mismatch_t::owner:
                refused.target.owner_id += "-other";
                break;
            case route_mismatch_t::lease:
                ++refused.target.owner_lease_generation;
                break;
            case route_mismatch_t::authority:
                refused.target.object_generation = fresh_ready.object_generation;
                // Same incarnation with an old authority generation must still be refused.
                break;
            case route_mismatch_t::same_incarnation_owner:
                refused.target.object_generation = fresh_ready.object_generation;
                refused.target.authority_owner_generation = fresh_ready.authority_owner_generation;
                refused.target.owner_id += "-other";
                break;
            case route_mismatch_t::future:
                refused.target.object_generation = fresh_ready.object_generation + 1;
                break;
        }
        const auto refusal = request_recreated (refused);
        assert (refusal.terminal_result
                == static_cast<std::uint32_t> (protocol::request_terminal_result::conflict));
        assert (
          refusal.failure_code
          == static_cast<std::uint32_t> (mismatch == route_mismatch_t::future
                                           ? protocol::framework_error_code::spotGenerationStale
                                           : protocol::framework_error_code::spotMoving));
        assert (instance_activation_count == delivered_before_refusals);
    }
    const auto final_recreated_read = store->read_authority (recreated_key).result ().value ();
    const auto final_recreated = std::get<authority_snapshot_t> (final_recreated_read);
    const auto release_recreated =
      target
        ->begin_instance_spot_close (
          recreated_request.target.stable_type, recreated_request.target.spot_id,
          final_recreated.object_generation, final_recreated.authority_owner_generation)
        .result ();
    assert (release_recreated && release_recreated.value ().release);
    assert (release_recreated.value ().release ().result ().value ());
    const auto released_refusal = request_recreated (recreated_request);
    assert (released_refusal.failure_code
            == static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving));
    assert (instance_activation_count == delivered_before_refusals);

    const auto unix_deadline =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () + 100ms)
                                    .count ());
    protocol::user_spot_create_header_t create{
      1,
      {99, 1},
      source->status ().routing_id ().to_bytes (),
      source->status ().lifecycle_generation (),
      spot_id,
      "room",
      {reservation->fence.reservation_id, reservation->fence.expected_store_version,
       reservation->fence.object_generation, reservation->fence.authority_owner_generation,
       target->status ().routing_id ().to_bytes (), target->status ().lifecycle_generation (),
       reservation->fence.target.owner.owner_id,
       static_cast<std::uint64_t> (reservation->fence.target.owner.lease_generation),
       reservation->fence.capacity_bundle.spot_slots},
      unix_deadline};
    const auto send_create_and_wait = [&] (const protocol::user_spot_create_header_t &request) {
        std::optional<protocol::user_spot_create_reply_t> reply;
        assert (source
                  ->create_user_spot_remote (
                    target->status ().routing_id (), request, 5s,
                    [&] (foundation::operation_terminal_t terminal,
                         result_t<protocol::user_spot_create_reply_t> completion_reply,
                         std::optional<protocol::application_payload_t>) {
                        auto result = std::move (completion_reply.value ());
                        assert (terminal == foundation::operation_terminal_t::completed);
                        reply = std::move (result);
                    })
                  .result ()
                  .value ());
        const auto wait_deadline = std::chrono::steady_clock::now () + 5s;
        while (!reply && std::chrono::steady_clock::now () < wait_deadline) {
            (void) target->dispatch_ready (dispatch);
            (void) source->dispatch_ready (dispatch);
            std::this_thread::sleep_for (1ms);
        }
        assert (reply);
        return std::move (*reply);
    };
    auto invalid_create = create;
    invalid_create.operation = {98, 1};
    invalid_create.spot_id = invalid_spot_id;
    invalid_create.deadline_unix_ms =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () + 100ms)
                                    .count ());
    invalid_create.reservation = {
      invalid_reservation->fence.reservation_id,
      invalid_reservation->fence.expected_store_version,
      invalid_reservation->fence.object_generation,
      invalid_reservation->fence.authority_owner_generation,
      target->status ().routing_id ().to_bytes (),
      target->status ().lifecycle_generation (),
      invalid_reservation->fence.target.owner.owner_id,
      static_cast<std::uint64_t> (invalid_reservation->fence.target.owner.lease_generation),
      invalid_reservation->fence.capacity_bundle.spot_slots};
    std::optional<protocol::user_spot_create_reply_t> invalid_reply;
    assert (source
              ->create_user_spot_remote (
                target->status ().routing_id (), invalid_create, 5s,
                [&] (foundation::operation_terminal_t terminal,
                     result_t<protocol::user_spot_create_reply_t> completion_reply,
                     std::optional<protocol::application_payload_t>) {
                    auto reply = std::move (completion_reply.value ());
                    assert (terminal == foundation::operation_terminal_t::completed);
                    invalid_reply = std::move (reply);
                })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!invalid_reply && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (invalid_reply);
    assert (invalid_reply->header.terminal_result == 105);
    assert (materialize_count == 0);
    std::this_thread::sleep_for (80ms);
    auto mismatch_create = create;
    mismatch_create.operation = {97, 1};
    mismatch_create.stable_type = "other-room";
    mismatch_create.deadline_unix_ms =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () + 100ms)
                                    .count ());
    std::optional<protocol::user_spot_create_reply_t> mismatch_reply;
    assert (source
              ->create_user_spot_remote (
                target->status ().routing_id (), mismatch_create, 5s,
                [&] (foundation::operation_terminal_t terminal,
                     result_t<protocol::user_spot_create_reply_t> completion_reply,
                     std::optional<protocol::application_payload_t>) {
                    auto reply = std::move (completion_reply.value ());
                    assert (terminal == foundation::operation_terminal_t::completed);
                    mismatch_reply = std::move (reply);
                })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!mismatch_reply && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (mismatch_reply);
    assert (mismatch_reply->header.terminal_result == 107);
    assert (mismatch_reply->header.failure_code
            == static_cast<std::uint32_t> (protocol::framework_error_code::spotTypeMismatch));
    std::optional<protocol::user_spot_create_reply_t> replayed_mismatch_reply;
    assert (source
              ->create_user_spot_remote (
                target->status ().routing_id (), mismatch_create, 5s,
                [&] (foundation::operation_terminal_t terminal,
                     result_t<protocol::user_spot_create_reply_t> completion_reply,
                     std::optional<protocol::application_payload_t>) {
                    auto reply = std::move (completion_reply.value ());
                    assert (terminal == foundation::operation_terminal_t::completed);
                    replayed_mismatch_reply = std::move (reply);
                })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!replayed_mismatch_reply && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (replayed_mismatch_reply);
    assert (replayed_mismatch_reply->header.failure_code
            == static_cast<std::uint32_t> (protocol::framework_error_code::spotTypeMismatch));
    assert (materialize_count == 0);

    auto moving_create = create;
    moving_create.operation = {99, 2};
    moving_create.deadline_unix_ms =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () + 5s)
                                    .count ());
    const auto moving_inventory =
      target->objects ().try_begin_maintenance_inventory ().result ().value ();
    assert (moving_inventory);
    const auto moving_reply = send_create_and_wait (moving_create);
    assert (moving_reply.header.terminal_result
            == static_cast<std::uint32_t> (protocol::request_terminal_result::conflict));
    assert (moving_reply.header.failure_code
            == static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving));
    target->objects ().end_maintenance_inventory ().result ().value ();

    const stateful::object_ref_t reserved_ref{
      stateful::object_kind_t::user_spot,
      spot_id,
      reservation->fence.object_generation,
      reservation->fence.authority_owner_generation,
      reservation->fence.target.mesh_name,
      std::string (reservation->fence.target.node_rid.value ())};
    const auto local_reservation =
      begin_reserved_object (target->objects (), reserved_ref, "room", {0x41, 0x42});
    assert (local_reservation.status == stateful::create_status_t::reserved);
    assert (local_reservation.factory_owner);
    auto joined_create = create;
    joined_create.operation = {99, 9};
    joined_create.deadline_unix_ms =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () + 5s)
                                    .count ());
    const auto joined_reply = send_create_and_wait (joined_create);
    assert (joined_reply.header.terminal_result
            == static_cast<std::uint32_t> (protocol::request_terminal_result::internalError));
    assert (joined_reply.header.failure_code
            == static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed));
    assert (target->objects ().abort_create (local_reservation.attempt)
            == stateful::stateful_error_t::none);
    assert (materialize_count == 0);

    const auto replay_expiry_unix_ms = mismatch_create.deadline_unix_ms + 51;
    while (static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                         std::chrono::system_clock::now ().time_since_epoch ())
                                         .count ())
           <= replay_expiry_unix_ms)
        std::this_thread::sleep_for (1ms);
    create.deadline_unix_ms =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () + 500ms)
                                    .count ());
    std::optional<protocol::user_spot_create_reply_t> create_reply;
    std::optional<foundation::operation_terminal_t> create_operation_terminal;
    bool create_has_application_reply = false;
    std::size_t create_terminal_count = 0;
    const auto record_create =
      [&] (foundation::operation_terminal_t terminal,
           result_t<protocol::user_spot_create_reply_t> completion_reply,
           std::optional<protocol::application_payload_t> application_reply) {
          auto reply = std::move (completion_reply.value ());
          create_operation_terminal = terminal;
          create_reply = std::move (reply);
          create_has_application_reply = application_reply.has_value ();
          ++create_terminal_count;
      };
    assert (
      source->create_user_spot_remote (target->status ().routing_id (), create, 5s, record_create)
        .result ()
        .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!create_reply && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (create_reply);
    assert (create_operation_terminal == foundation::operation_terminal_t::completed);
    assert (create_reply->header.terminal_result == 0);
    assert (create_reply->result == protocol::user_spot_create_result_t::created);
    assert (create_terminal_count == 1);
    assert (materialize_count == 1);
    assert (zlink::framework::runtime::cleanup_source_created_reservation (store, reserve.key,
                                                                           reservation->fence, true)
            == zlink::framework::runtime::source_creation_cleanup_t::stale);
    const auto committed_authority =
      store->read_authority (zlink::framework::runtime::spot_authority_key (spot_id))
        .result ()
        .value ();
    const auto *committed_snapshot = std::get_if<authority_snapshot_t> (&committed_authority);
    assert (committed_snapshot
            && committed_snapshot->allocation.state == placement_allocation_state_t::active);
    std::optional<protocol::user_spot_create_reply_t> replayed_create_reply;
    assert (source
              ->create_user_spot_remote (
                target->status ().routing_id (), create, 5s,
                [&] (foundation::operation_terminal_t terminal,
                     result_t<protocol::user_spot_create_reply_t> completion_reply,
                     std::optional<protocol::application_payload_t>) {
                    auto reply = std::move (completion_reply.value ());
                    assert (terminal == foundation::operation_terminal_t::completed);
                    replayed_create_reply = std::move (reply);
                })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!replayed_create_reply && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (replayed_create_reply);
    assert (replayed_create_reply->result == protocol::user_spot_create_result_t::created);
    assert (materialize_count == 1);

    auto distinct_create = create;
    distinct_create.operation = {99, 3};
    std::optional<protocol::user_spot_create_reply_t> distinct_reply;
    assert (source
              ->create_user_spot_remote (
                target->status ().routing_id (), distinct_create, 5s,
                [&] (foundation::operation_terminal_t terminal,
                     result_t<protocol::user_spot_create_reply_t> completion_reply,
                     std::optional<protocol::application_payload_t>) {
                    auto reply = std::move (completion_reply.value ());
                    assert (terminal == foundation::operation_terminal_t::completed);
                    distinct_reply = std::move (reply);
                })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!distinct_reply && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (distinct_reply);
    assert (distinct_reply->header.terminal_result == 107);
    assert (distinct_reply->header.failure_code
            == static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving));
    assert (materialize_count == 1);
    std::this_thread::sleep_for (200ms);
    auto expired_create = create;
    expired_create.operation = {99, 4};
    expired_create.deadline_unix_ms =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () - 1ms)
                                    .count ());
    std::optional<protocol::user_spot_create_reply_t> expired_reply;
    assert (source
              ->create_user_spot_remote (
                target->status ().routing_id (), expired_create, 5s,
                [&] (foundation::operation_terminal_t terminal,
                     result_t<protocol::user_spot_create_reply_t> completion_reply,
                     std::optional<protocol::application_payload_t>) {
                    auto reply = std::move (completion_reply.value ());
                    assert (terminal == foundation::operation_terminal_t::completed);
                    expired_reply = std::move (reply);
                })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!expired_reply && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (expired_reply);
    assert (expired_reply->header.terminal_result == 101);
    assert (materialize_count == 1);

    const auto authority =
      store->read_authority (zlink::framework::runtime::spot_authority_key (spot_id))
        .result ()
        .value ();
    const auto *ready = std::get_if<authority_snapshot_t> (&authority);
    assert (ready);
    assert (ready->allocation.state == placement_allocation_state_t::active);
    const auto wait_until_wire_time = [] (std::uint64_t target_unix_ms) {
        const auto bound = std::chrono::steady_clock::now () + 5s;
        while (std::chrono::steady_clock::now () < bound) {
            const auto now =
              static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                            std::chrono::system_clock::now ().time_since_epoch ())
                                            .count ());
            if (now > target_unix_ms)
                return true;
            std::this_thread::sleep_for (1ms);
        }
        return false;
    };
    assert (wait_until_wire_time (create.deadline_unix_ms + 50));

    auto capacity_filler = create;
    capacity_filler.operation = {99, 5};
    capacity_filler.deadline_unix_ms =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () + 200ms)
                                    .count ());
    std::optional<protocol::user_spot_create_reply_t> capacity_filler_reply;
    assert (source
              ->create_user_spot_remote (
                target->status ().routing_id (), capacity_filler, 5s,
                [&] (foundation::operation_terminal_t terminal,
                     result_t<protocol::user_spot_create_reply_t> completion_reply,
                     std::optional<protocol::application_payload_t>) {
                    auto reply = std::move (completion_reply.value ());
                    assert (terminal == foundation::operation_terminal_t::completed);
                    capacity_filler_reply = std::move (reply);
                })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!capacity_filler_reply && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (capacity_filler_reply);

    protocol::user_spot_close_header_t expired_close{
      1,
      {99, 6},
      source->status ().routing_id ().to_bytes (),
      source->status ().lifecycle_generation (),
      {spot_id, ready->object_generation, target->status ().routing_id ().to_bytes (),
       target->status ().lifecycle_generation (), ready->authority_owner_generation,
       ready->store_version},
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () - 1ms)
                                    .count ())};
    std::optional<protocol::user_spot_close_reply_t> expired_close_reply;
    assert (
      source
        ->close_user_spot_remote (
          target->status ().routing_id (), expired_close, std::chrono::steady_clock::now () + 5s,
          [&] (foundation::operation_terminal_t terminal, protocol::user_spot_close_reply_t reply) {
              assert (terminal == foundation::operation_terminal_t::completed);
              expired_close_reply = std::move (reply);
          })
        .result ()
        .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!expired_close_reply && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (expired_close_reply && expired_close_reply->header.terminal_result == 101
            && !expired_close_reply->closed);
    const auto after_expired_close =
      store->read_authority (zlink::framework::runtime::spot_authority_key (spot_id))
        .result ()
        .value ();
    const auto *after_expired_snapshot = std::get_if<authority_snapshot_t> (&after_expired_close);
    assert (after_expired_snapshot
            && after_expired_snapshot->store_version == ready->store_version);

    // The target classifies each Close request once (§7.1, §9): another
    // ObjectGeneration is SpotGenerationStale (33, source InvalidOperation);
    // any other owner fence field is SpotMoving (34, source Unavailable). A
    // wrong target node lifecycle ends at service admission before this step.
    // The authority is unchanged in every case.
    const auto classify_close =
      [&] (std::uint64_t operation_low,
           const std::function<void (protocol::user_spot_close_fence_t &)> &alter) {
          auto rejected = expired_close;
          rejected.operation = {99, operation_low};
          rejected.deadline_unix_ms = static_cast<std::uint64_t> (
            std::chrono::duration_cast<std::chrono::milliseconds> (
              std::chrono::system_clock::now ().time_since_epoch () + 5s)
              .count ());
          alter (rejected.target);
          std::optional<protocol::user_spot_close_reply_t> reply;
          assert (source
                    ->close_user_spot_remote (
                      target->status ().routing_id (), rejected,
                      std::chrono::steady_clock::now () + 5s,
                      [&] (foundation::operation_terminal_t terminal,
                           protocol::user_spot_close_reply_t value) {
                          assert (terminal == foundation::operation_terminal_t::completed);
                          reply = std::move (value);
                      })
                    .result ()
                    .value ());
          const auto bound = std::chrono::steady_clock::now () + 5s;
          while (!reply && std::chrono::steady_clock::now () < bound) {
              (void) target->dispatch_ready (dispatch);
              (void) source->dispatch_ready (dispatch);
              std::this_thread::sleep_for (1ms);
          }
          assert (reply && !reply->closed && reply->header.terminal_result == 107);
          const auto unchanged =
            store->read_authority (zlink::framework::runtime::spot_authority_key (spot_id))
              .result ()
              .value ();
          const auto *unchanged_snapshot = std::get_if<authority_snapshot_t> (&unchanged);
          assert (unchanged_snapshot && unchanged_snapshot->store_version == ready->store_version);
          return reply->header.failure_code;
      };
    const auto stale =
      static_cast<std::uint32_t> (protocol::framework_error_code::spotGenerationStale);
    const auto moving = static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving);
    assert (stale == 33 && moving == 34);
    assert (classify_close (20, [] (auto &fence) { ++fence.object_generation; }) == stale);
    assert (classify_close (21, [] (auto &fence) { fence.expected_store_version += "-other"; })
            == moving);
    assert (classify_close (22, [] (auto &fence) { ++fence.authority_owner_generation; })
            == moving);
    assert (
      zlink::framework::runtime::user_spot_terminal::map_user_spot_operation_failure (
        foundation::operation_terminal_t::completed, protocol::reply_header_t{1, 107, stale}, false)
      == zlink::framework::framework_error_kind_t::invalid_operation);
    assert (zlink::framework::runtime::user_spot_terminal::map_user_spot_operation_failure (
              foundation::operation_terminal_t::completed, protocol::reply_header_t{1, 107, moving},
              false)
            == zlink::framework::framework_error_kind_t::unavailable);

    // The expired cache miss must not create a terminal record. Once the
    // unrelated capacity filler expires, the same operation identity with a
    // live deadline must execute instead of failing fingerprint validation.
    assert (wait_until_wire_time (capacity_filler.deadline_unix_ms + 50));
    auto close = expired_close;
    close.deadline_unix_ms =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () + 5s)
                                    .count ());
    std::optional<protocol::user_spot_close_reply_t> close_reply;
    assert (
      source
        ->close_user_spot_remote (
          target->status ().routing_id (), close, std::chrono::steady_clock::now () + 5s,
          [&] (foundation::operation_terminal_t terminal, protocol::user_spot_close_reply_t reply) {
              assert (terminal == foundation::operation_terminal_t::completed);
              close_reply = std::move (reply);
          })
        .result ()
        .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!close_reply && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (close_reply && close_reply->closed);
    assert (std::holds_alternative<authority_missing_t> (
      store->read_authority (zlink::framework::runtime::spot_authority_key (spot_id))
        .result ()
        .value ()));

    create.operation = {99, 7};
    create.spot_id = "transport-failure-room";
    create_operation_terminal.reset ();
    create_reply.reset ();
    create_has_application_reply = true;
    assert (
      source->create_user_spot_remote (target->status ().routing_id (), create, 5s, record_create)
        .result ()
        .value ());
    assert (target->transport ().mailbox ().pending_messages (
              mesh::service_mailbox_domain_t::infrastructure)
            == 0);
    deadline = std::chrono::steady_clock::now () + 5s;
    while (target->transport ().mailbox ().pending_messages (
             mesh::service_mailbox_domain_t::infrastructure)
             == 0
           && std::chrono::steady_clock::now () < deadline) {
        (void) source->dispatch_ready (dispatch);
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) target->transport ().observe_routes ();
        assert (target->transport ().pump_one (now).result ().value ()
                != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (target->transport ().mailbox ().pending_messages (
              mesh::service_mailbox_domain_t::infrastructure)
            == 1);
    assert (!create_operation_terminal);
    test::acknowledge_delivery (deadline);
    target->close ();
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!create_operation_terminal && std::chrono::steady_clock::now () < deadline) {
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (create_operation_terminal == foundation::operation_terminal_t::transport_failed);
    assert (create_terminal_count == 2);
    assert (create_reply);
    assert (!create_has_application_reply);
    assert (create_reply->header.correlation == 0);
    assert (create_reply->header.terminal_result == 0);
    assert (create_reply->header.failure_code == 0);

    source->close ();
    return true;
}
}
