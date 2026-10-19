/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <poll.h>
#include <thread>

namespace fw = zlink::framework;
namespace rt = fw::runtime;
using json = nlohmann::json;

int main (int argc, char **argv)
{
    if (argc != 3)
        return 2;
    rt::install_host_context_hooks ();
    rt::mesh::service_node_descriptor_t descriptor{};
    descriptor.mesh_name = argv[2];
    descriptor.node_routing_id = zlink::routing_id_t::from ("cross-bind-node").to_bytes ();
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.advertised_endpoint = argv[1];
    auto host = std::make_shared<rt::host::public_host_runtime_t> (rt::host::host_options_t{
      rt::mesh::raw_mesh_node_options_t{.descriptor = descriptor,
                                        .runtime_failures =
                                          std::make_shared<rt::runtime_failure_collector_t> ()},
      "entry",
      {"actor"}});
    fw::detail::actor_gateway_runtime_t gateway;
    json peer;
    bool visible = true;
    std::optional<fw::actor_ref_t> actor;
    host->configure_bound_session_operations (rt::host::bound_session_operations_t{
      .session_owner = [&] (const zlink::routing_id_t &rid, std::uint64_t generation)
        -> fw::task_t<std::optional<fw::location_owner_token_t>> {
          if (visible && !peer.is_null () && peer.at ("rid") == rid.to_string ()
              && std::stoull (peer.at ("generation").get<std::string> ()) == generation)
              co_return fw::location_owner_token_t{
                peer.at ("ownerId"), std::stoll (peer.at ("lease").get<std::string> ())};
          co_return std::nullopt;
      },
      .bind =
        [&] (const rt::protocol::bound_session_bind_t &bind, const zlink::routing_id_t &owner,
             std::uint64_t generation, const fw::location_owner_token_t &token,
             std::function<bool ()> reply) {
            const auto session = zlink::routing_id_t::from (bind.session_routing_id);
            if (bind.binding.state == rt::protocol::bound_session_binding_state_t::tombstone) {
                const auto retired = gateway.retire_bound_session_route (*actor, owner, session,
                                                                         bind.binding.generation);
                return rt::host::bound_session_bind_operation_result_t{
                  retired ? rt::stateful::stateful_error_t::none
                          : rt::stateful::stateful_error_t::conflict,
                  std::nullopt};
            }
            const auto transition = gateway.replace_session_route (
              *actor,
              [] (std::string, fw::stream_codec_t, const zlink::message_t &) -> fw::task_t<void> {
                  co_return;
              },
              fw::detail::actor_bound_session_route_t{
                owner, session, bind.actor.object_generation, generation,
                bind.actor.authority_owner_generation, bind.actor.owner_lease_generation,
                bind.binding.generation, 0, 0, false, token.owner_id,
                static_cast<std::uint64_t> (token.lease_generation)},
              fw::stream_codec_t::message_pack, std::move (reply));
            if (!transition)
                return rt::host::bound_session_bind_operation_result_t{
                  rt::stateful::stateful_error_t::conflict, std::nullopt};
            std::optional<rt::protocol::bound_session_replaced_t> replaced;
            const auto &change = transition.value ();
            if (change.changed && change.previous && change.previous->session_rid) {
                const auto &previous = *change.previous;
                replaced = rt::protocol::bound_session_replaced_t{
                  bind.actor,
                  {previous.node_rid.to_bytes (), previous.node_generation,
                   previous.session_owner_id, previous.session_owner_lease_generation,
                   previous.session_rid->to_bytes (), previous.binding_generation}};
            }
            return rt::host::bound_session_bind_operation_result_t{
              rt::stateful::stateful_error_t::none, replaced,
              change.current ? change.current->binding_generation : 0,
              change.current ? change.current->authority_owner_generation : 0};
        },
      .send = [] (const auto &, auto) { return rt::stateful::stateful_error_t::invalid; },
      .replaced =
        [] (const auto &) { throw std::logic_error ("Actor fixture received replacement"); },
      .commit_relocation_route = [] (const auto &, const auto &, const auto &) { return false; },
      .prepare_relocation_target_route = [] (const auto &, auto) { return false; }});
    host->start ();
    const auto output = [] (json value) { std::cout << value.dump () << std::endl; };
    output ({{"kind", "ready"},
             {"endpoint", host->status ().local_endpoint ()},
             {"generation", std::to_string (host->status ().lifecycle_generation ())},
             {"actor", {{"generation", "1"}}},
             {"authority", "1"},
             {"ownerId", "cross-bind-node-owner"}});
    for (;;) {
        (void) host->dispatch_ready ([] (const auto &, const auto &, auto) {}).result ().value ();
        pollfd input{0, POLLIN, 0};
        if (::poll (&input, 1, 1) <= 0)
            continue;
        std::string line;
        if (!std::getline (std::cin, line))
            break;
        const auto command = json::parse (line);
        const auto kind = command.at ("kind").get<std::string> ();
        if (kind == "actor") {
            const auto created = host->create_reserved_actor (
              "actor", {rt::stateful::object_kind_t::actor, "cross-bind-actor",
                        std::stoull (command.at ("generation").get<std::string> ()),
                        std::stoull (command.at ("authority").get<std::string> ()), argv[2],
                        "cross-bind-node"});
            actor = created.ref ();
        } else if (kind == "configure") {
            peer = command;
            if (!host->transport ().connect_peer (command.at ("endpoint")))
                throw std::runtime_error ("peer connect failed");
        } else if (kind == "descriptor") {
            visible = command.at ("visible");
        } else if (kind == "binding") {
            const auto bound = gateway.bound_session_route_async (*actor).result ().value ();
            if (!bound || !bound->session_rid)
                throw std::runtime_error ("Actor has no binding");
            output ({{"kind", kind},
                     {"binding",
                      {{"sessionRid", bound->session_rid->to_string ()},
                       {"bindingGeneration", std::to_string (bound->binding_generation)}}}});
            continue;
        } else {
            throw std::invalid_argument ("unsupported host command");
        }
        output ({{"kind", kind}});
    }
    host->close ();
}
