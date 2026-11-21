/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/mesh_node_vertical_cases.hpp"
#include <zlink.hpp>
#include <atomic>
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/route_mesh_runtime_options_service.hpp"
#include "runtime/mesh/route_mesh_runtime_service.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/actors/actor_ref_access.hpp"
#include "../support/loopback_tcp_endpoint.hpp"
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/configuration/framework_options.hpp>
#include <zlink/stream_connector.hpp>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <latch>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include "../support/mesh_node_vertical_transport_fixture.hpp"
#include "../support/mesh_node_vertical_location_resolvers.hpp"

using namespace framework_mesh_node_vertical_fixture;

using namespace std::chrono_literals;

namespace
{
class expired_actor_bind_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    expired_actor_bind_session_t (zlink::framework::session_actor_manager_t &manager,
                                  zlink::framework::actor_ref_t actor,
                                  std::promise<zlink::framework::framework_error_kind_t> &result) :
        _manager (manager), _actor (std::move (actor)), _result (result)
    {
    }

    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        try {
            (void) co_await _manager.bind (_actor).async ();
            _result.set_value (zlink::framework::framework_error_kind_t::internal_failure);
        }
        catch (const zlink::framework::framework_exception_t &error) {
            _result.set_value (error.kind ());
        }
        co_return;
    }
    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &) override
    {
        co_return;
    }

  private:
    zlink::framework::session_actor_manager_t &_manager;
    zlink::framework::actor_ref_t _actor;
    std::promise<zlink::framework::framework_error_kind_t> &_result;
};
} // namespace

namespace framework_mesh_node_vertical_tests
{
void verify_actor_route_resolver_preserves_unavailable ()
{
    zlink::framework::completion_owner_t completion_owner;
    using namespace zlink::framework;
    auto registration = make_named_node ("actor-route-unavailable", "actor-route-unavailable-node");
    const auto actor = detail::actor_ref_access_t::make (
      node_rid_t::from_string ("expired-actor-target"), "player", "expired-actor", 1);
    std::promise<framework_error_kind_t> bind_result;
    auto bind_terminal = bind_result.get_future ();
    serializer_registry_t serializers;
    service_collection_t services;
    services.add_singleton<detail::actor_gateway_runtime_t> ();
    services.add_factory<session_actor_manager_t> (
      [] (service_provider_t &provider) {
          return std::make_unique<session_actor_manager_t> (
            provider.get_required<detail::actor_gateway_runtime_t> ().manager ());
      },
      service_lifetime_t::scoped);
    services.add_factory<expired_actor_bind_session_t> (
      [actor, &bind_result] (service_provider_t &provider) {
          return std::make_unique<expired_actor_bind_session_t> (
            provider.get_required<session_actor_manager_t> (), actor, bind_result);
      },
      service_lifetime_t::scoped);
    auto owned_store = std::make_unique<runtime::in_memory_location_repository_t> ();
    auto &store = *owned_store;
    services.add_singleton<location_repository_t> (
      std::unique_ptr<location_repository_t> (owned_store.release ()));
    services.add_singleton<runtime::location_runtime_t> (
      std::make_unique<runtime::location_runtime_t> (store));
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    provider.get_required<runtime::location_runtime_t> ().start (*registration->routing_id);
    runtime::mesh_node_host_service_t service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (), {registration},
      serializers);
    service.start (provider).result ().value ();
    const auto claimed = store.claim_owner_lease ("expired-actor-owner", 200ms).result ().value ();
    const auto *owner = std::get_if<owner_lease_claimed_t> (&claimed);
    assert (owner);
    const auto target_rid = zlink::routing_id_t::from ("expired-actor-target");
    mesh_node_descriptor_t descriptor{
      .mesh_name = "actor-route-unavailable",
      .rid = target_rid,
      .lifecycle_generation = 1,
      .descriptor_revision = 1,
      .endpoint = "tcp://127.0.0.1:5001",
      .application_version = 1,
      .object_capabilities = {{.object_kind = placement_object_kind_t::actor,
                               .stable_type = "player",
                               .policy = maintenance_policy_kind_t::recreate}},
      .object_role = object_role_t::server,
      .capacity = {.actors = {.limit = 1}},
      .activation_concurrency = {.limit = 1},
      .state = framework_runtime_state_t::serving,
      .security_identity = "test",
      .owner_id = owner->token.owner_id,
      .lease_generation = owner->token.lease_generation};
    assert (store.update_mesh_node (descriptor, location_write_intent_t::new_claim)
              .result ()
              .value ()
              .status
            == location_write_status_t::stored);
    const object_reserve_request_t reservation{
      .key = {placement_object_kind_t::actor, "expired-actor"},
      .intent = {.stable_type = "player",
                 .request_content_reference = "test",
                 .request_encoded_size = 1},
      .target = {.mesh_name = "actor-route-unavailable",
                 .node_rid = node_rid_t::from_string ("expired-actor-target"),
                 .node_lifecycle_generation = 1,
                 .owner = owner->token},
      .creating_payload = {std::byte{1}},
      .capacity_bundle = {.actor_slots = 1}};
    const auto reserved = store.reserve (reservation).result ().value ();
    const auto *pending = std::get_if<object_reserved_t> (&reserved);
    assert (pending);
    const auto payload =
      runtime::encode_actor_authority_payload (runtime::actor_authority_payload_t{
        .stable_type = "player",
        .actor_id = "expired-actor",
        .current_spot_id = detail::new_entry_spot_id ("expired-actor-target"),
        .current_spot_generation = descriptor.lifecycle_generation,
        .current_spot_kind = runtime::actor_authority_spot_kind_t::entry,
        .owner_id = owner->token.owner_id,
        .owner_lease_generation = static_cast<std::uint64_t> (owner->token.lease_generation),
        .mesh_name = descriptor.mesh_name,
        .node_rid = reservation.target.node_rid,
        .node_generation = descriptor.lifecycle_generation});
    const auto committed =
      store.commit ({reservation.key, pending->fence, payload}).result ().value ();
    assert (std::get_if<object_committed_t> (&committed));
    std::this_thread::sleep_for (220ms);
    const auto retained =
      store.read_authority (runtime::actor_authority_key ("expired-actor")).result ().value ();
    assert (std::get_if<authority_snapshot_t> (&retained));
    const auto expired_lease = store.read_owner_lease (owner->token.owner_id).result ().value ();
    assert (std::get_if<owner_lease_missing_t> (&expired_lease));
    handler_registry_t handlers;
    zlink_builder_t stream_zlink = zlink::framework::test::runtime_failure_builder ();
    detail::configure_stream_dispatch_executor ();
    zlink_framework_options_t stream_options (services, handlers, serializers, stream_zlink);
    stream_options.add_stream_node ("expired-actor-stream")
      .bind (zlink::framework::tests::loopback_tcp_any_port)
      .register_session ("expired-actor-session");
    stream_options.apply ();
    auto stream_runtime = detail::stream_runtime_t::from (stream_zlink);
    const auto stream_snapshots = stream_runtime.snapshots ();
    assert (stream_snapshots.size () == 1);
    assert (stream_snapshots.front ().packet_session_name == "expired-actor-session");
    auto stream_listeners = std::make_shared<runtime::listener_status_registry_t> ();
    runtime::stream_host_service_t stream_host (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (), stream_runtime,
      stream_snapshots,
      {{"expired-actor-session",
        [] (service_provider_t &scope) -> packet_stream_session_t & {
            return scope.get_required<expired_actor_bind_session_t> ();
        }}},
      30s, service.nodes ().front (), {}, stream_listeners);
    assert (stream_host.start (provider).result ());
    const auto stream_endpoint =
      stream_listeners->find (zlink::framework::listener_kind_t::stream, "expired-actor-stream")
        ->endpoint;
    zlink::stream_connector::connector_options_t connector_options;
    connector_options.endpoint = zlink::framework::tests::listener_endpoint (
      *stream_listeners, listener_kind_t::stream, "expired-actor-stream");
    connector_options.connect_timeout = 2s;
    connector_options.reconnect.enabled = false;
    auto client = zlink::stream_connector::connector_factory_t::create (connector_options);
    assert (client.connect ());
    assert (bind_terminal.wait_for (2s) == std::future_status::ready);
    const auto terminal = bind_terminal.get ();
    if (terminal != framework_error_kind_t::unavailable)
        std::cerr << "expired authority STREAM bind kind=" << static_cast<int> (terminal)
                  << std::endl;
    assert (terminal == framework_error_kind_t::unavailable);
    (void) client.close ();
    stream_host.stop ();
    service.stop ();
}
} // namespace framework_mesh_node_vertical_tests
