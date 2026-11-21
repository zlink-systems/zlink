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
class faulting_mesh_location_repository_t final
    : public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    void fail_next_retiring_write (std::size_t ordinal)
    {
        std::lock_guard lock (_fault_gate);
        _retiring_write = 0;
        _fail_retiring_write = ordinal;
    }

    std::vector<zlink::framework::framework_runtime_state_t> state_history () const
    {
        std::lock_guard lock (_fault_gate);
        return _state_history;
    }

    zlink::framework::task_t<zlink::framework::location_write_result_t>
    update_mesh_node (zlink::framework::mesh_node_descriptor_t descriptor,
                      zlink::framework::location_write_intent_t intent) override
    {
        {
            std::lock_guard lock (_fault_gate);
            _state_history.push_back (descriptor.state);
            if (descriptor.state == zlink::framework::framework_runtime_state_t::relocating
                && ++_retiring_write == _fail_retiring_write) {
                _fail_retiring_write = 0;
                return zlink::framework::task_t<zlink::framework::location_write_result_t> (
                  zlink::framework::result_t<zlink::framework::location_write_result_t>::success (
                    {zlink::framework::location_write_status_t::rejected_conflict, 0, {}}));
            }
        }
        return in_memory_location_repository_t::update_mesh_node (std::move (descriptor), intent);
    }

  private:
    mutable std::mutex _fault_gate;
    std::size_t _retiring_write = 0;
    std::size_t _fail_retiring_write = 0;
    std::vector<zlink::framework::framework_runtime_state_t> _state_history;
};

zlink::framework::framework_runtime_state_t
read_mesh_state (faulting_mesh_location_repository_t &store,
                 const std::string &mesh_name,
                 const zlink::routing_id_t &rid)
{
    const auto page = store.list_mesh_nodes (mesh_name).result ().value ();
    const auto found =
      std::find_if (page.items.begin (), page.items.end (),
                    [&rid] (const zlink::framework::mesh_node_descriptor_t &descriptor) {
                        return descriptor.rid.to_hex () == rid.to_hex ();
                    });
    assert (found != page.items.end ());
    return found->state;
}
} // namespace

namespace framework_mesh_node_vertical_tests
{
void verify_server_descriptor_publishes_framework_entry_spot_without_application_entry ()
{
    zlink::framework::completion_owner_t completion_owner;
    auto registration = make_named_node ("descriptor-entry", "descriptor-entry-node");
    registration->object_role = zlink::framework::object_role_t::server;
    assert (!registration->spot_state->snapshot.entry_spot_name);

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::service_collection_t services;
    auto owned_store =
      std::make_unique<zlink::framework::runtime::in_memory_location_repository_t> ();
    auto &store = *owned_store;
    services.add_singleton<zlink::framework::location_repository_t> (
      std::unique_ptr<zlink::framework::location_repository_t> (owned_store.release ()));
    services.add_singleton<zlink::framework::runtime::location_runtime_t> (
      std::make_unique<zlink::framework::runtime::location_runtime_t> (store));
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    provider.get_required<zlink::framework::runtime::location_runtime_t> ().start (
      *registration->routing_id);

    zlink::framework::runtime::mesh_node_host_service_t service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (), {registration},
      serializers);
    service.start (provider).result ().value ();
    const auto nodes = store.list_mesh_nodes ("descriptor-entry").result ().value ();
    assert (nodes.items.size () == 1);
    const auto &descriptor = nodes.items.front ();
    assert (descriptor.entry_spot_id);
    assert (!descriptor.entry_spot_id->empty ());
    assert (*descriptor.entry_spot_id
            == service.nodes ().front ()->native_node ().entry_spot ().spot_id ());
    service.stop ();
}

void verify_descriptor_retire_order_and_pre_seal_rollback ()
{
    zlink::framework::completion_owner_t completion_owner;
    using zlink::framework::framework_runtime_state_t;

    auto first = make_named_node ("vertical-order", "order-a");
    auto second = make_named_node ("vertical-order", "order-b");
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::service_collection_t services;
    auto owned_store = std::make_unique<faulting_mesh_location_repository_t> ();
    auto &store = *owned_store;
    services.add_singleton<zlink::framework::location_repository_t> (
      std::unique_ptr<zlink::framework::location_repository_t> (owned_store.release ()));
    services.add_singleton<zlink::framework::runtime::location_runtime_t> (
      std::make_unique<zlink::framework::runtime::location_runtime_t> (store));
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    provider.get_required<zlink::framework::runtime::location_runtime_t> ().start (
      *first->routing_id);
    zlink::framework::runtime::mesh_node_host_service_t service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (), {first, second},
      serializers);
    service.start (provider).result ().value ();

    assert (read_mesh_state (store, "vertical-order", *first->routing_id)
            == framework_runtime_state_t::serving);
    assert (read_mesh_state (store, "vertical-order", *second->routing_id)
            == framework_runtime_state_t::serving);

    // A partial Retiring publication is a pre-seal failure. The caller can
    // republish Serving before it closes admission, leaving both descriptors
    // selectable and the local runtime unchanged.
    store.fail_next_retiring_write (2);
    assert (!service.publish_descriptor_state (framework_runtime_state_t::relocating));
    assert (service.publish_descriptor_state (framework_runtime_state_t::serving));
    assert (read_mesh_state (store, "vertical-order", *first->routing_id)
            == framework_runtime_state_t::serving);
    assert (read_mesh_state (store, "vertical-order", *second->routing_id)
            == framework_runtime_state_t::serving);

    // Once preflight succeeds, the externally visible order is Retiring
    // before the dispatch seal and Draining only after relocation succeeds.
    assert (service.publish_descriptor_state (framework_runtime_state_t::relocating));
    service.seal_application_dispatch ();
    assert (read_mesh_state (store, "vertical-order", *first->routing_id)
            == framework_runtime_state_t::relocating);
    assert (service.publish_descriptor_state (framework_runtime_state_t::draining));
    assert (read_mesh_state (store, "vertical-order", *first->routing_id)
            == framework_runtime_state_t::draining);

    for (const auto &node : service.nodes ()) {
        assert (node->status ().state
                == zlink::framework::runtime::host::node_status_t::state_t::draining);
    }

    const auto history = store.state_history ();
    const auto first_retiring =
      std::find (history.begin (), history.end (), framework_runtime_state_t::relocating);
    const auto first_draining =
      std::find (first_retiring, history.end (), framework_runtime_state_t::draining);
    assert (first_retiring != history.end ());
    assert (first_draining != history.end ());
    assert (first_retiring < first_draining);
    service.stop ();
}
} // namespace framework_mesh_node_vertical_tests
