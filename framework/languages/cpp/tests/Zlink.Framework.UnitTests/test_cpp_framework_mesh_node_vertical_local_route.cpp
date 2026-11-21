/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/mesh_node_vertical_cases.hpp"
#include <zlink.hpp>
#include <atomic>
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
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "../support/loopback_tcp_endpoint.hpp"
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
struct local_route_probe_message_t
{
    std::string value;
};
} // namespace

template <> struct zlink::json::dto_traits<local_route_probe_message_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("value", &local_route_probe_message_t::value)};
};

namespace
{
struct local_route_probe_state_t
{
    std::mutex mutex;
    std::condition_variable changed;
    bool gate_open = false;
    int entered = 0;
    int completed = 0;
    std::vector<std::string> values;
    std::string last_mesh_name;
    std::string last_channel_name;
    std::string last_packet_name;
    std::string last_content_type;
    std::string last_source_node_rid;
};

class local_route_probe_handler_t
{
  public:
    explicit local_route_probe_handler_t (local_route_probe_state_t &state) : _state (state) {}

    void handle (const local_route_probe_message_t &message,
                 const zlink::framework::route_message_context_t &context)
    {
        std::unique_lock lock (_state.mutex);
        ++_state.entered;
        // v11 MessageContext: node-direct dispatch has no ChannelName, so the router channel
        // identity travels in the Mesh name and only the source node RID is added.
        _state.last_mesh_name = context.mesh_name.value_or ("<none>");
        _state.last_channel_name = context.channel_name.value_or ("<none>");
        _state.last_packet_name = context.packet_name;
        _state.last_content_type = context.content_type.value_or ("<none>");
        _state.last_source_node_rid = context.source_node_rid.to_string ();
        _state.values.push_back (message.value);
        _state.changed.notify_all ();
        if (message.value == "throw")
            throw std::runtime_error ("local route probe failure");
        _state.changed.wait (lock, [this] { return _state.gate_open; });
        ++_state.completed;
        _state.changed.notify_all ();
    }

  private:
    local_route_probe_state_t &_state;
};
} // namespace

namespace framework_mesh_node_vertical_tests
{
void verify_local_node_submit_bridge ()
{
    zlink::framework::completion_owner_t completion_owner;
    auto registration = make_node ("tcp://127.0.0.1:0", "local-route-node");
    registration->handlers.on_send<local_route_probe_handler_t, local_route_probe_message_t> (
      "vertical-mesh", "LocalRouteProbe", &local_route_probe_handler_t::handle);

    auto independent_registration = make_named_node ("independent-mesh", "independent-route-node");
    independent_registration->handlers
      .on_send<local_route_probe_handler_t, local_route_probe_message_t> (
        "independent-mesh", "LocalRouteProbe", &local_route_probe_handler_t::handle);

    zlink::framework::serializer_registry_t serializers;
    serializers.add<local_route_probe_message_t> (
      [] (const local_route_probe_message_t &message) {
          return zlink::framework::encoded_payload_t::from_string (message.value);
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return local_route_probe_message_t{payload.to_string ()};
      });
    auto probe = std::make_shared<local_route_probe_state_t> ();
    zlink::framework::service_collection_t services;
    services.add_factory<local_route_probe_state_t> (
      [probe] (zlink::framework::service_provider_t &) { return probe; },
      zlink::framework::service_lifetime_t::singleton);
    // v11: the MeshNode host resolves the Location store from the provider, so
    // a vertical check registers the in-memory store like any application.
    auto owned_store =
      std::make_unique<zlink::framework::runtime::in_memory_location_repository_t> ();
    auto &location_store = *owned_store;
    services.add_singleton<zlink::framework::location_repository_t> (
      std::unique_ptr<zlink::framework::location_repository_t> (owned_store.release ()));
    services.add_singleton<zlink::framework::runtime::location_runtime_t> (
      std::make_unique<zlink::framework::runtime::location_runtime_t> (location_store));
    location_store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    // The MeshNode publishes its descriptor under an owner lease, so the
    // Location runtime starts first just as the host does in production.
    auto &location_runtime =
      provider.get_required<zlink::framework::runtime::location_runtime_t> ();
    location_runtime.start (*registration->routing_id);
    auto application_jobs = std::make_shared<zlink::framework::runtime::application_job_queue_t> (
      zlink::framework::runtime::application_job_queue_configuration_t{
        zlink::framework::application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    zlink::framework::runtime::mesh_node_host_service_t service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      {registration, independent_registration}, serializers, {}, {}, application_jobs);
    service.start (provider).result ().value ();
    const auto node = service.nodes ().front ();
    const auto independent_node = service.nodes ().back ();

    auto encode = [&serializers] (std::string value, std::string mesh_name = "vertical-mesh") {
        zlink::framework::runtime::messaging::client_call_codec_t codec;
        const auto header =
          codec.create_envelope (zlink::framework::runtime::messaging::message_kind_t::command,
                                 std::move (mesh_name), "LocalRouteProbe");
        return codec.encode_envelope_parts (header, local_route_probe_message_t{std::move (value)},
                                            serializers);
    };

    {
        auto parts = encode ("owned-after-return");
        assert (service.submit_local_node_send (node, parts.items ()).result ().value ()
                == zlink::submit_result_t::ok);
    }
    {
        std::unique_lock lock (probe->mutex);
        assert (probe->changed.wait_for (lock, 1s, [&] { return probe->entered == 1; }));
        assert (probe->completed == 0);
    }
    const auto verify_released_capacity = [&] {
        // The receive pump shares this queue and can reserve the released
        // permit. Acquire it through the same FIFO to prove that the running
        // handler returned capacity at entry, independent of pump scheduling.
        using permit_t = zlink::framework::runtime::application_job_queue_t::permit_t;
        auto released_capacity = std::make_shared<std::promise<std::optional<permit_t>>> ();
        auto available = released_capacity->get_future ();
        auto waiter = application_jobs->wait_for_supply (
          [released_capacity] (std::optional<permit_t> permit) {
              released_capacity->set_value (std::move (permit));
          },
          zlink::framework::runtime::application_job_queue_t::origin_t::remote);
        assert (available.wait_for (1s) == std::future_status::ready);
        auto permit = available.get ();
        assert (permit);
        const auto capacity_while_handlers_are_pending = application_jobs->snapshot ();
        assert (capacity_while_handlers_are_pending.effective_max_queued_application_jobs == 1);
        assert (capacity_while_handlers_are_pending.reserved_supply_permits == 1);
        assert (capacity_while_handlers_are_pending.queued_application_jobs == 0);
        assert (capacity_while_handlers_are_pending.permits_in_use == 1);
    };
    verify_released_capacity ();

    // Node pipelines have no gate. Both another Node and this Node's successor
    // enter before the first handler completes (02 §2).
    auto independent = encode ("independent-owner", "independent-mesh");
    assert (
      service.submit_local_node_send (independent_node, independent.items ()).result ().value ()
      == zlink::submit_result_t::ok);
    {
        std::unique_lock lock (probe->mutex);
        assert (probe->changed.wait_for (lock, 1s, [&] { return probe->entered == 2; }));
        assert (probe->completed == 0);
    }
    auto progresses = encode ("progress-after-entry");
    assert (service.submit_local_node_send (node, progresses.items ()).result ().value ()
            == zlink::submit_result_t::ok);
    {
        std::unique_lock lock (probe->mutex);
        assert (probe->changed.wait_for (lock, 1s, [&] { return probe->entered == 3; }));
        assert (probe->completed == 0);
        assert (node->pending_application_callbacks () == 0);
        assert (node->active_application_callbacks () == 2);
        assert (independent_node->active_application_callbacks () == 1);
    }
    verify_released_capacity ();

    {
        std::lock_guard lock (probe->mutex);
        probe->gate_open = true;
    }
    probe->changed.notify_all ();
    {
        std::unique_lock lock (probe->mutex);
        assert (probe->changed.wait_for (lock, 1s, [&] { return probe->completed == 3; }));
        assert ((probe->values
                 == std::vector<std::string>{"owned-after-return", "independent-owner",
                                             "progress-after-entry"}));
        assert (probe->last_mesh_name == "vertical-mesh");
        assert (probe->last_channel_name == "<none>");
        assert (probe->last_packet_name == "LocalRouteProbe");
        // The wire content type, not the struct default, so the value really travels.
        assert (probe->last_content_type == "application/octet-stream");
        assert (!probe->last_source_node_rid.empty ());
    }
    assert (service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 1s));

    auto throwing = encode ("throw");
    assert (service.submit_local_node_send (node, throwing.items ()).result ().value ()
            == zlink::submit_result_t::ok);
    assert (service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 1s));
    auto after_throw = encode ("after-throw");
    assert (service.submit_local_node_send (node, after_throw.items ()).result ().value ()
            == zlink::submit_result_t::ok);
    {
        std::unique_lock lock (probe->mutex);
        assert (probe->changed.wait_for (lock, 1s, [&] { return probe->completed == 4; }));
    }
    assert (service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 1s));
    assert (node->pending_application_callbacks () == 0);
    assert (node->active_application_callbacks () == 0);

    service.seal_application_dispatch ();
    auto after_seal = encode ("after-seal");
    assert (service.submit_local_node_send (node, after_seal.items ()).result ().value ()
            == zlink::submit_result_t::terminated);
    service.stop ();
    location_runtime.stop ();
    assert (!location_runtime.current_owner_token ());
}

// Regression pin for mesh_node_runtime_t::classify_node_direct_target's
// Location-Store gate. A route-only MeshNode's direct-target send/request
// must consult the real transport-level admission state
// (raw_mesh_node_owner_t::request_with_header's `_topology.peer(...)` gate),
// not fail with a blanket not_found merely because the target routing id is
// absent from this node's own Location Store page. Route-only
// (object_role=none) MeshNode peers are published for route/channel topology,
// but their descriptors expose no object placement capability. The absent
// entry below is therefore the unknown target, not this source node.
// classify_node_direct_target used to misclassify every direct-target
// send/request as not_found before the network call was attempted, regardless
// of whether the target was actually admitted. This node never admits any
// peer at all, so the correct (post-fix) outcome for an unknown target is
// not_connected -- the real topology-backed answer.
// Before the fix, classify_node_direct_target returned not_found here
// unconditionally without ever consulting the transport layer, so this
// assertion would have failed pre-fix and pins the fix now.
void verify_direct_target_falls_through_absent_location_store_entry ()
{
    zlink::framework::completion_owner_t completion_owner;
    auto registration = make_node ("tcp://127.0.0.1:0", "location-gate-node");
    registration->object_role = zlink::framework::object_role_t::none;

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::service_collection_t services;
    // v11: the MeshNode host resolves the Location store from the provider,
    // so this check registers a real (but empty) in-memory store like any
    // application would. The store never gets a "target" entry for the
    // routing id used below.
    auto owned_store =
      std::make_unique<zlink::framework::runtime::in_memory_location_repository_t> ();
    auto &location_store = *owned_store;
    services.add_singleton<zlink::framework::location_repository_t> (
      std::unique_ptr<zlink::framework::location_repository_t> (owned_store.release ()));
    services.add_singleton<zlink::framework::runtime::location_runtime_t> (
      std::make_unique<zlink::framework::runtime::location_runtime_t> (location_store));
    location_store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    provider.get_required<zlink::framework::runtime::location_runtime_t> ().start (
      *registration->routing_id);
    auto application_jobs = std::make_shared<zlink::framework::runtime::application_job_queue_t> (
      zlink::framework::runtime::application_job_queue_configuration_t{
        zlink::framework::application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    zlink::framework::runtime::mesh_node_host_service_t service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (), {registration},
      serializers, {}, {}, application_jobs);
    service.start (provider).result ().value ();
    const auto node = service.nodes ().front ();

    const auto descriptors = location_store.list_mesh_nodes ("vertical-mesh").result ().value ();
    const auto source_descriptor =
      std::find_if (descriptors.items.begin (), descriptors.items.end (), [&] (const auto &item) {
          return item.rid.to_hex () == registration->routing_id->to_hex ();
      });
    assert (source_descriptor != descriptors.items.end ());
    assert (source_descriptor->object_role == zlink::framework::object_role_t::none);
    assert (source_descriptor->object_capabilities.empty ());
    assert (!source_descriptor->entry_spot_id);
    assert (source_descriptor->channel_weights.contains ("work"));

    const std::vector<zlink::message_t> parts{zlink::message_t::from (std::string ("direct"))};
    const auto target = zlink::routing_id_t::from (std::string ("never-admitted-target"));
    const auto result = std::move (node->send_to_node (target, parts, std::vector<std::uint8_t>{}))
                          .result ()
                          .value ();
    assert (result == zlink::submit_result_t::not_connected);

    service.stop ();
}

// Residual-convergence pin (spec 32-framework-error-model.ko.md:76-77): a
// `Request` to a target this runtime has NEVER admitted (absent from
// service_topology_registry_t's live peer table, distinct from the `Send`
// case above whose target routing id merely never appears in the Location
// Store page) must complete NotFound, not Unavailable/not_connected.
// public_host_runtime_t::request_to_node used to fall back to
// `submitted(false)` unconditionally, which collapses every rejected
// request into `not_connected` regardless of whether the target was ever
// seen; that violates the spec's split between "target does not exist"
// (NotFound, line 76) and "target exists but is unreachable" (Unavailable,
// line 77). Matches Java's ZLinkJavaRawSpotNode.classifyNodeSendTarget
// (peerState absent -> TARGET_NOT_FOUND) and Node's
// raw-service-mesh-runtime.ts knownTarget gate.
void verify_request_to_never_admitted_target_reports_not_found ()
{
    zlink::framework::completion_owner_t completion_owner;
    auto registration = make_node ("tcp://127.0.0.1:0", "never-admitted-request-node");
    registration->object_role = zlink::framework::object_role_t::none;

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::service_collection_t services;
    auto owned_store =
      std::make_unique<zlink::framework::runtime::in_memory_location_repository_t> ();
    auto &location_store = *owned_store;
    services.add_singleton<zlink::framework::location_repository_t> (
      std::unique_ptr<zlink::framework::location_repository_t> (owned_store.release ()));
    services.add_singleton<zlink::framework::runtime::location_runtime_t> (
      std::make_unique<zlink::framework::runtime::location_runtime_t> (location_store));
    location_store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    provider.get_required<zlink::framework::runtime::location_runtime_t> ().start (
      *registration->routing_id);
    auto application_jobs = std::make_shared<zlink::framework::runtime::application_job_queue_t> (
      zlink::framework::runtime::application_job_queue_configuration_t{
        zlink::framework::application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    zlink::framework::runtime::mesh_node_host_service_t service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (), {registration},
      serializers, {}, {}, application_jobs);
    service.start (provider).result ().value ();
    const auto node = service.nodes ().front ();

    const std::vector<zlink::message_t> parts{zlink::message_t::from (std::string ("request"))};
    const auto target = zlink::routing_id_t::from (std::string ("never-admitted-request-target"));
    zlink::framework::runtime::host::pending_operation_t operation_id;
    const auto result =
      std::move (node->request_to_node (target, parts, operation_id, std::chrono::milliseconds (25),
                                        std::vector<std::uint8_t>{}))
        .result ()
        .value ();
    assert (result == zlink::submit_result_t::not_found);

    service.stop ();
}
} // namespace framework_mesh_node_vertical_tests
