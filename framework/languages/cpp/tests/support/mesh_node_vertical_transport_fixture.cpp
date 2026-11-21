/* SPDX-License-Identifier: Elastic-2.0 */
#include "mesh_node_vertical_transport_fixture.hpp"

#include "runtime_failure_fixture.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <utility>

namespace framework_mesh_node_vertical_fixture
{
std::shared_ptr<zlink::framework::detail::mesh_node_builder_state_t>
make_node (std::string endpoint, std::string routing_id)
{
    auto state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> ("vertical-mesh");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->listen_endpoint = std::move (endpoint);
    state->listen_port.reset ();
    state->routing_id = zlink::routing_id_t::from (routing_id);
    state->object_role = zlink::framework::object_role_t::server;
    state->channels.emplace (
      "work", zlink::framework::detail::mesh_channel_registration_t{100, {}, true, true});
    // The host admits object creation only for declared stable types.
    state->spot_state->snapshot.actor_types.emplace_back ("vertical.actor");
    return state;
}

std::shared_ptr<zlink::framework::detail::mesh_node_builder_state_t>
make_named_node (std::string mesh_name, std::string routing_id)
{
    auto state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> (std::move (mesh_name));
    state->core_context = std::make_shared<zlink::context_t> ();
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from (std::move (routing_id));
    state->object_role = zlink::framework::object_role_t::server;
    state->channels.emplace (
      "work", zlink::framework::detail::mesh_channel_registration_t{100, {}, true, true});
    // The host admits object creation only for declared stable types.
    state->spot_state->snapshot.actor_types.emplace_back ("vertical.actor");
    return state;
}
} // namespace framework_mesh_node_vertical_fixture
