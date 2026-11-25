/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/telemetry_observation.hpp"
#include "../support/m6b_cases.hpp"
#include "../support/m6b_host_access.hpp"
#include "../support/m6b_task.hpp"
#include "../support/m6b_mesh_fixture.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace foundation = zlink::framework::runtime::foundation;
namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;
namespace host = zlink::framework::runtime::host;
using namespace std::chrono_literals;
using zlink::framework::test::begin_reserved_object;

namespace zlink::framework::test::m6b
{
void verify_remote_bound_session_bind_returns_terminal_fences ()
{
    using namespace zlink::framework;

    auto state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> ("m6b-mesh");
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from ("bound-session-reply-source");
    const auto core_context = std::make_shared<zlink::context_t> ();
    state->core_context = core_context;

    std::size_t invalidations = 0;
    std::size_t route_resolutions = 0;
    std::optional<protocol::actor_route_fence_t> invalidated;
    detail::mesh_node_runtime_t source (state);

    auto target_state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> ("m6b-mesh");
    target_state->listen_endpoint = "tcp://127.0.0.1:0";
    target_state->routing_id = zlink::routing_id_t::from ("bound-session-reply-target");
    target_state->core_context = core_context;
    detail::mesh_node_runtime_t target (target_state);
    target.start ().result ().value ();
    const auto target_descriptor =
      target.native_node ().transport ().topology ().local_descriptor ();
    const auto target_rid = target.status ().routing_id ();
    const actor_ref_t actor = detail::actor_ref_access_t::make (
      node_rid_t::from_string (target_rid.to_string ()), "bound.actor", "bound-actor", 7);
    const runtime::spot_address_t route{"m6b-mesh",
                                        target_rid,
                                        {},
                                        0,
                                        {},
                                        7,
                                        11,
                                        {"bound-owner", 13},
                                        target_descriptor.lifecycle_generation};
    const runtime::spot_address_t current_route{
      "m6b-mesh",
      zlink::routing_id_t::from ("current-actor-owner"),
      {},
      0,
      {},
      7,
      12,
      {"current-owner", 14},
      target_descriptor.lifecycle_generation == 18 ? std::uint64_t{17} : std::uint64_t{18}};
    assert (current_route.node_generation != route.node_generation);
    source.configure_actor_route_resolver (
      [&] (actor_ref_t) -> task_t<std::optional<runtime::spot_address_t>> {
          ++route_resolutions;
          co_return invalidations == 0 ? route : current_route;
      },
      [&] (const protocol::actor_route_fence_t &route) {
          ++invalidations;
          invalidated = route;
      });
    source.start ().result ().value ();
    source.connect_peer (target_rid, target.status ().local_endpoint (),
                         target.status ().lifecycle_generation ());

    const auto peer_deadline = std::chrono::steady_clock::now () + 5s;
    while ((!source.has_admitted_peer (target_rid, target.status ().lifecycle_generation ())
            || !target.has_admitted_peer (source.status ().routing_id (),
                                          source.status ().lifecycle_generation ()))
           && std::chrono::steady_clock::now () < peer_deadline) {
        (void) source.dispatch_ready ([] (const auto &, const auto &, auto, auto) {});
        (void) target.dispatch_ready ([] (const auto &, const auto &, auto, auto) {});
        std::this_thread::sleep_for (1ms);
    }
    assert (source.has_admitted_peer (target_rid, target.status ().lifecycle_generation ()));
    assert (target.has_admitted_peer (source.status ().routing_id (),
                                      source.status ().lifecycle_generation ()));

    const auto initial_route_resolutions = route_resolutions;

    const auto request_and_reply = [&] (protocol::request_terminal_result terminal,
                                        protocol::framework_error_code failure,
                                        std::optional<protocol::stream_bind_reply_tail_t> accepted =
                                          std::nullopt) {
        auto result = std::async (std::launch::async, [&] {
            return source
              .bind_application_actor_session (
                actor, zlink::routing_id_t::from ("bound-session-reply-session"), 1, route, 2s)
              .result ();
        });
        const auto deadline = std::chrono::steady_clock::now () + 2s;
        bool replied = false;
        while (result.wait_for (0ms) != std::future_status::ready
               && std::chrono::steady_clock::now () < deadline) {
            const auto pump = target.native_node ()
                                .transport ()
                                .pump_one (mesh::service_liveness_registry_t::clock_t::now ())
                                .result ()
                                .value ();
            assert (pump != mesh::raw_mesh_pump_result_t::protocol_error);
            if (!replied && pump == mesh::raw_mesh_pump_result_t::infrastructure) {
                auto claim = target.native_node ().transport ().mailbox ().try_claim (
                  mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
                if (claim && claim->records.size () == 1) {
                    assert (protocol::decode_header (
                              protocol::message_bytes (claim->records.front ()->parts ().front ()))
                              .kind
                            == protocol::command::boundSessionBind);
                    assert (target.native_node ().transport ().reply_bound_session_bind (
                      (*claim->records.front ()), static_cast<std::uint32_t> (terminal),
                      static_cast<std::uint32_t> (failure), accepted));
                    assert (target.native_node ().transport ().mailbox ().release (*claim));
                    replied = true;
                }
            }
            (void) source.dispatch_ready ([] (const auto &, const auto &, auto, auto) {});
            std::this_thread::sleep_for (1ms);
        }
        assert (replied);
        assert (result.wait_for (0ms) == std::future_status::ready);
        return result.get ();
    };

    const auto accepted = request_and_reply (protocol::request_terminal_result::ok,
                                             protocol::framework_error_code::none,
                                             protocol::stream_bind_reply_tail_t{19, 11});
    assert (accepted);
    assert (accepted.value ().binding_generation == 19);
    assert (accepted.value ().authority_owner_generation == 11);
    assert (route_resolutions - initial_route_resolutions == 0);
    const auto success_bytes =
      protocol::encode_stream_bind_reply (9, 0, 0, protocol::stream_bind_reply_tail_t{19, 11});
    assert (protocol::decode_stream_bind_reply (success_bytes).binding
            == (protocol::stream_bind_reply_tail_t{19, 11}));
    const auto expect_invalid_reply = [] (const auto &bytes) {
        bool rejected = false;
        try {
            (void) protocol::decode_stream_bind_reply (bytes);
        }
        catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    };
    expect_invalid_reply (protocol::encode_reply_header (9, 0, 0));
    auto extra = success_bytes;
    extra.push_back (0);
    expect_invalid_reply (extra);
    auto failure_tail = protocol::encode_reply_header (
      9, static_cast<std::uint32_t> (protocol::request_terminal_result::internalError),
      static_cast<std::uint32_t> (protocol::framework_error_code::routeNotConnected));
    failure_tail.push_back (0);
    expect_invalid_reply (failure_tail);

    const auto stale = request_and_reply (protocol::request_terminal_result::conflict,
                                          protocol::framework_error_code::actorLocationStale);
    assert (!stale);
    assert (stale.error_kind () == framework_error_kind_t::unavailable);
    assert (invalidations == 1);
    assert (invalidated);
    assert (invalidated->actor_id == "bound-actor");
    assert (invalidated->object_generation == 7);
    assert (invalidated->target_node_routing_id == target_descriptor.node_routing_id);
    assert (invalidated->target_node_generation == route.node_generation);
    assert (invalidated->authority_owner_generation == 11);
    assert (invalidated->owner_lease_generation == 13);
    assert (route_resolutions - initial_route_resolutions == 0);

    const auto actor_not_ready = request_and_reply (protocol::request_terminal_result::busy,
                                                    protocol::framework_error_code::none);
    assert (!actor_not_ready);
    assert (actor_not_ready.error_kind () == framework_error_kind_t::unavailable);
    assert (invalidations == 1);
    assert (route_resolutions - initial_route_resolutions == 0);

    const auto ordinary_conflict =
      request_and_reply (protocol::request_terminal_result::conflict,
                         protocol::framework_error_code::actorAlreadyExists);
    assert (!ordinary_conflict);
    assert (ordinary_conflict.error_kind () == framework_error_kind_t::unavailable);
    assert (invalidations == 1);
    assert (route_resolutions - initial_route_resolutions == 0);

    // Session-Actor binding §4: the Actor owner rejects a bind whose Session owner has no
    // admitted, lease-verified descriptor with the Unavailable representative.
    const auto session_owner_unavailable =
      request_and_reply (protocol::request_terminal_result::internalError,
                         protocol::framework_error_code::routeNotConnected);
    assert (!session_owner_unavailable);
    assert (session_owner_unavailable.error_kind () == framework_error_kind_t::unavailable);
    assert (invalidations == 1);
    assert (route_resolutions - initial_route_resolutions == 0);

    // A known owner descriptor may wait for ADMIT without an ingress pump.
    auto pending_descriptor = target_descriptor;
    pending_descriptor.node_routing_id =
      zlink::routing_id_t::from ("pending-bound-session-target").to_bytes ();
    source.native_node ().transport ().expect_peer (pending_descriptor);
    auto pending_route = route;
    pending_route.node_rid = zlink::routing_id_t::from ("pending-bound-session-target");
    const auto preadmit_deadline =
      source
        .bind_application_actor_session (
          actor, zlink::routing_id_t::from ("bound-session-before-admit"), 1, pending_route, 20ms)
        .result ();
    assert (!preadmit_deadline);
    assert (preadmit_deadline.error_kind () == framework_error_kind_t::deadline_exceeded);

    // Submit/completion §5: an authority-selected owner without its lifecycle
    // descriptor waits within the original request deadline.
    const runtime::spot_address_t disconnected_route{
      "m6b-mesh", zlink::routing_id_t::from ("unconnected-bound-session-target"),
      {},         0,
      {},         7,
      11,         {"bound-owner", 13},
      1};
    const auto transport_failure =
      source
        .bind_application_actor_session (actor,
                                         zlink::routing_id_t::from ("bound-session-reply-session"),
                                         1, disconnected_route, 20ms)
        .result ();
    assert (!transport_failure);
    assert (transport_failure.error_kind () == framework_error_kind_t::deadline_exceeded);
    assert (invalidations == 1);
    assert (route_resolutions - initial_route_resolutions == 0);

    // The connected route admits this request, but the target withholds its
    // reply. Its post-admission deadline expiry is also DeadlineExceeded.
    auto withheld = std::async (std::launch::async, [&] {
        return source
          .bind_application_actor_session (
            actor, zlink::routing_id_t::from ("bound-session-withheld-reply"), 2, route, 20ms)
          .result ();
    });
    const auto withheld_deadline = std::chrono::steady_clock::now () + 2s;
    bool admitted_without_reply = false;
    while (withheld.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < withheld_deadline) {
        const auto pump = target.native_node ()
                            .transport ()
                            .pump_one (mesh::service_liveness_registry_t::clock_t::now ())
                            .result ()
                            .value ();
        assert (pump != mesh::raw_mesh_pump_result_t::protocol_error);
        if (!admitted_without_reply && pump == mesh::raw_mesh_pump_result_t::infrastructure) {
            auto claim = target.native_node ().transport ().mailbox ().try_claim (
              mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
            if (claim && claim->records.size () == 1) {
                assert (protocol::decode_header (
                          protocol::message_bytes (claim->records.front ()->parts ().front ()))
                          .kind
                        == protocol::command::boundSessionBind);
                assert (target.native_node ().transport ().mailbox ().release (*claim));
                admitted_without_reply = true;
            }
        }
        (void) source.dispatch_ready ([] (const auto &, const auto &, auto, auto) {});
        std::this_thread::sleep_for (1ms);
    }
    assert (admitted_without_reply);
    assert (withheld.wait_for (0ms) == std::future_status::ready);
    const auto reply_timeout = withheld.get ();
    assert (!reply_timeout);
    assert (reply_timeout.error_kind () == framework_error_kind_t::deadline_exceeded);
    assert (invalidations == 1);
    assert (route_resolutions - initial_route_resolutions == 0);

    source.stop ();
    target.stop ();
}

void verify_local_session_binding_uses_location_authority ()
{
    using namespace zlink::framework;

    const auto actor = detail::actor_ref_access_t::make (node_rid_t::from_string ("target-node"),
                                                         "PlayerActor", "reconnected-player", 7);
    const runtime::stateful::object_ref_t materialized{
      runtime::stateful::object_kind_t::actor, "reconnected-player", 7, 3, "mesh", "target-node"};
    const runtime::spot_address_t route{
      "mesh", zlink::routing_id_t::from ("target-node"), "room", 11, "version", 7, 5, {"owner", 13},
      17};

    const auto verified =
      detail::make_local_application_actor_session_ref (materialized, actor, route);
    assert (verified);
    assert (verified.value ().authority_owner_generation == 5);
    assert (verified.value ().mesh_name == "mesh");
    assert (verified.value ().node_id == "target-node");

    stateful::stream_session_registry_t sessions (
      [materialized] (const std::string &) { return std::make_optional (materialized); });
    const auto connection = sessions.open ("reconnected-session");
    assert (sessions
              .bind (connection, verified.value (), route.node_generation,
                     static_cast<std::uint64_t> (route.owner.lease_generation))
              .first
            == stateful::stateful_error_t::generation_stale);
    const auto [bind_error, binding] =
      sessions.bind_remote (connection, verified.value (), route.node_generation,
                            static_cast<std::uint64_t> (route.owner.lease_generation));
    assert (bind_error == stateful::stateful_error_t::none);
    assert (binding.actor == verified.value ());
    assert (binding.actor.authority_owner_generation == 5);
    assert (binding.target_node_generation == route.node_generation);
    assert (binding.owner_lease_generation
            == static_cast<std::uint64_t> (route.owner.lease_generation));

    auto stale_route = route;
    stale_route.object_generation = 8;
    assert (!detail::make_local_application_actor_session_ref (materialized, actor, stale_route));
    auto wrong_actor = materialized;
    wrong_actor.key = "other-actor";
    assert (!detail::make_local_application_actor_session_ref (wrong_actor, actor, route));
}

protocol::actor_route_fence_t same_node_bound_session_route ()
{
    return protocol::actor_route_fence_t{
      "same-node-actor", 7, zlink::routing_id_t::from ("same-node-owner").to_bytes (), 11, 13, 17};
}

stateful::object_ref_t same_node_bound_session_actor ()
{
    return stateful::object_ref_t{
      stateful::object_kind_t::actor, "same-node-actor", 7, 13, "m6b-mesh", "same-node-owner"};
}

host::bound_session_bind_admission_t
classify_same_node_bound_session_bind (const protocol::actor_route_fence_t &requested,
                                       const std::optional<stateful::object_ref_t> &local_actor)
{
    const auto local_routing_id = zlink::routing_id_t::from ("same-node-owner");
    return host::classify_bound_session_bind_admission (
      host::bound_session_bind_actor_matches (requested, local_actor, local_routing_id, 11));
}

void verify_bound_session_bind_uses_only_three_value_actor_fence ()
{
    const auto requested = same_node_bound_session_route ();
    const auto actor = same_node_bound_session_actor ();

    // The authoritative lease is 23 while the command-38 envelope preserves
    // 17. OwnerLeaseGeneration is not one of the three bind approval values.
    assert (classify_same_node_bound_session_bind (requested, actor)
            == host::bound_session_bind_admission_t::ready);

    auto wrong_object = actor;
    ++wrong_object.object_generation;
    assert (classify_same_node_bound_session_bind (requested, wrong_object)
            == host::bound_session_bind_admission_t::actor_not_ready);

    auto wrong_node = requested;
    ++wrong_node.target_node_generation;
    assert (classify_same_node_bound_session_bind (wrong_node, actor)
            == host::bound_session_bind_admission_t::actor_not_ready);

    auto wrong_authority = actor;
    ++wrong_authority.authority_owner_generation;
    assert (classify_same_node_bound_session_bind (requested, wrong_authority)
            == host::bound_session_bind_admission_t::actor_not_ready);
}

void verify_bind_handler_rejects_missing_session_owner_descriptor (bool test_seal_drop)
{
    using namespace zlink::framework;
    auto source = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("descriptor-bind-source"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("descriptor-bind-target"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto drops_owner = std::make_shared<std::atomic_size_t> (0);
    auto &drops = *drops_owner;
    dispatch_options_t options;
    options.message_flow (message_flow_log_mode_t::normal);
    detail::dispatch_options_access_t::set_observer_for_tests (
      options, [drops_owner] (const message_flow_event_t &event) {
          if (event.packet_name == std::optional<std::string> ("sessionRelocationSeal")
              && event.outcome == message_flow_outcome_t::dropped
              && event.reason == message_flow_reason_t::location_unavailable)
              ++*drops_owner;
      });
    host::public_host_runtime_test_access_t::set_dispatch (*target, std::move (options));
    const auto local = target->status ();
    const stateful::object_ref_t actor{
      stateful::object_kind_t::actor,  "descriptor-actor", 7, 11, "m6b-mesh",
      local.routing_id ().to_string ()};
    const auto creation = begin_reserved_object (target->objects (), actor, "descriptor.actor", {});
    assert (creation.error == stateful::stateful_error_t::none);
    assert (target->objects ().commit_create (creation.attempt)
            == stateful::stateful_error_t::none);
    int binds = 0;
    target->configure_bound_session_operations (
      {.session_owner = [] (const zlink::routing_id_t &, std::uint64_t)
         -> task_t<std::optional<location_owner_token_t>> { co_return std::nullopt; },
       .bind =
         [&] (const auto &, const auto &, std::uint64_t, const auto &, auto) {
             ++binds;
             using bind_result_t = host::bound_session_bind_operation_result_t;
             return task_t<bind_result_t> (result_t<bind_result_t>::success (bind_result_t{}));
         },
       .send = [] (const auto &, auto) { return stateful::stateful_error_t::conflict; },
       .replaced = [] (const auto &) {},
       .commit_relocation_route = [] (const auto &, const auto &, const auto &) { return false; },
       .prepare_relocation_target_route = [] (const auto &, std::uint64_t) { return false; }});
    source->start ();
    target->start ();
    assert (source->connect_peer (target->transport ().endpoint (), local.routing_id ()));
    const auto discard = [] (const host::ready_record_t &, const host::receive_record_t &, auto,
                             auto) {};
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while ((!ready_peer (source->transport ().topology (), local.routing_id ().to_bytes ())
            || !ready_peer (target->transport ().topology (),
                            source->status ().routing_id ().to_bytes ()))
           && std::chrono::steady_clock::now () < deadline) {
        await_task (source->dispatch_ready (discard));
        await_task (target->dispatch_ready (discard));
    }
    std::optional<protocol::reply_header_t> reply;
    const protocol::bound_session_bind_t bind{
      0,
      {actor.key, 7, local.routing_id ().to_bytes (), local.lifecycle_generation (), 11, 13},
      zlink::routing_id_t::from ("descriptor-session").to_bytes (),
      {protocol::bound_session_binding_state_t::active, 17}};
    assert (await_task (source->transport ().request_bound_session_bind (
      local.routing_id ().to_bytes (), bind, 2s,
      [&] (foundation::operation_terminal_t,
           ::zlink::framework::result_t<zlink::message_t> completion_payload) {
          auto payload = std::move (completion_payload.value ());
          reply = protocol::decode_reply_header (protocol::message_bytes (payload));
      })));
    while (!reply && std::chrono::steady_clock::now () < deadline) {
        await_task (target->dispatch_ready (discard));
        await_task (source->dispatch_ready (discard));
    }
    assert (reply
            && reply->terminal_result
                 == static_cast<std::uint32_t> (protocol::request_terminal_result::internalError)
            && reply->failure_code
                 == static_cast<std::uint32_t> (protocol::framework_error_code::routeNotConnected));
    assert (binds == 0);
    if (test_seal_drop) {
        const protocol::session_relocation_seal_t seal{
          .relocation = {1, 2},
          .coordinator = {"coordinator", 3, source->status ().routing_id ().to_bytes (),
                          source->status ().lifecycle_generation (), "store"},
          .actor = {actor.key, 7, source->status ().routing_id ().to_bytes (),
                    source->status ().lifecycle_generation (), 11, 13},
          .session_owner_node_routing_id = local.routing_id ().to_bytes (),
          .session_owner_node_generation = local.lifecycle_generation (),
          .session_owner_id = "session-owner",
          .session_owner_lease_generation = 19,
          .session_routing_id = bind.session_routing_id,
          .binding_generation = 17,
          .target_node_routing_id = source->status ().routing_id ().to_bytes (),
          .target_node_generation = source->status ().lifecycle_generation ()};
        assert (
          target->transport ().mailbox ().try_enqueue (mesh::service_mailbox_record_t::from_parts (
            "seal", mesh::service_mailbox_domain_t::infrastructure,
            {zlink::message_t::from (protocol::encode_session_relocation_seal (seal))},
            source->status ().routing_id ().to_bytes (), std::nullopt,
            source->status ().lifecycle_generation ())));
        await_task (target->dispatch_ready (discard));
        test::acknowledge_delivery (deadline);
        assert (drops == 1);
    }
    source->close ();
    target->close ();
}
}
