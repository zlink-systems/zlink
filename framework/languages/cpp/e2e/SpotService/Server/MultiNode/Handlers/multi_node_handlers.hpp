/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Spots/multi_node_spots.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace e2e = zlink::framework::e2e::spot_service;

namespace
{

inline bool evidence_entry_matches (const e2e::evidence_entry_t &entry,
                                    const std::string &marker,
                                    const std::string &spot_id,
                                    const std::string &value_part)
{
    return entry.marker == marker && entry.spot_id == spot_id
           && (value_part.empty () || entry.value.find (value_part) != std::string::npos);
}

inline const char *multi_node_spot_name_for (const std::string &node_rid)
{
    return node_rid == multi_node_a_name ? e2e::multi_spot_a : e2e::multi_spot_b;
}

inline const char *multi_node_route_channel_for (const std::string &node_rid)
{
    return node_rid == multi_node_a_name ? e2e::multi_route_channel_a
                                         : e2e::multi_route_channel_b;
}

inline e2e::state_res_t request_multi_node_state (zlink::framework::route_client_t &routes,
                                                  zlink::framework::spot_handle_resolver_t &handles,
                                                  const std::string &spot_id,
                                                  int delta)
{
    auto handle = handles.resolve_spot_handle ((spot_id))
                    .result ()
                    .value ();
    if (!handle) {
        throw std::runtime_error ("multi-node spot '" + spot_id + "' has no live location row");
    }
    auto reply = routes
                   .request_to_spot (*handle, e2e::state_req_t{.op = "add", .amount = delta})
                   .timeout (std::chrono::milliseconds (3000))
                   .submit<e2e::state_res_t> ()
                   .result ();
    if (reply) {
        return reply.value ();
    }
    throw std::runtime_error ("multi-node spot route failed for '" + spot_id
                              + "': "
                              + (reply.error () ? reply.error ()->what ()
                                                : "unknown route error"));
}

class multi_node_route_ping_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;
    using request_type = e2e::channel_control_ping_req_t;
    using reply_type = e2e::channel_control_ping_res_t;

    explicit multi_node_route_ping_handler_t (scenario_state_t &state) : _state (state) {}

    e2e::channel_control_ping_res_t handle (
      const e2e::channel_control_ping_req_t &request,
      const zlink::framework::route_handler_context_t &)
    {
        return {.node_rid = _state.node_rid, .value = request.value};
    }

  private:
    scenario_state_t &_state;
};

class multi_node_route_ping_proxy_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t,
                                                                zlink::framework::route_client_t>;

    multi_node_route_ping_proxy_handler_t (scenario_state_t &state,
                                           zlink::framework::route_client_t &routes) :
        _state (state), _routes (routes)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request =
          nlohmann::json::parse (http.body).get<e2e::channel_control_ping_req_t> ();
        auto reply =
          _routes
            .request_to_node (multi_node_route_channel_for (_state.node_rid),
                              zlink::routing_id_t::from (request.target_node_rid), request)
            .timeout (std::chrono::milliseconds (3000))
            .submit<e2e::channel_control_ping_res_t> ()
            .result ();
        if (!reply) {
            throw zlink::framework::framework_exception_t (
              reply.error_kind (),
              reply.error () ? reply.error ()->what () : "MultiNodeRoutePing failed");
        }

        zlink::framework::http_response_t response;
        response.body = nlohmann::json (reply.value ()).dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::route_client_t &_routes;
};

class multi_node_create_local_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<
      scenario_state_t, zlink::framework::spot_node_manager_t, zlink::framework::route_client_t>;
    using request_type = e2e::multi_node_create_spot_req_t;
    using reply_type = e2e::multi_node_create_spot_res_t;

    multi_node_create_local_handler_t (scenario_state_t &state,
                                       zlink::framework::spot_node_manager_t &spots,
                                       zlink::framework::route_client_t &routes) :
        _state (state), _spots (spots), _routes (routes)
    {
    }

    e2e::multi_node_create_spot_res_t handle (
      const e2e::multi_node_create_spot_req_t &request)
    {
        return create_spot (request);
    }

    e2e::multi_node_create_spot_res_t handle (
      const e2e::multi_node_create_spot_req_t &request,
      const zlink::framework::route_handler_context_t &)
    {
        return create_spot (request);
    }

  private:
    e2e::multi_node_create_spot_res_t create_spot (
      const e2e::multi_node_create_spot_req_t &request)
    {
        const auto rid = (request.spot_id);
        const auto created =
          _spots.get_or_create_spot (multi_node_spot_name_for (_state.node_rid), rid, request);
        const auto state =
          created.state == zlink::framework::spot_create_state_t::created ? "created"
                                                                          : "existing";
        _state.record ("MultiCreateSpot", {}, request.spot_id, state);
        return {.spot_id = request.spot_id,
                .node_rid = _state.node_rid,
                .state = state,
                .value = 0};
    }

    scenario_state_t &_state;
    zlink::framework::spot_node_manager_t &_spots;
    zlink::framework::route_client_t &_routes;
};

/* Spot-addressed messaging is a mesh member's surface: a spot handle carries
 * its mesh, so a node outside the SPOT mesh (the requester) cannot route to it
 * directly. The requester therefore forwards over its route channel and the
 * mesh member resolves the handle locally — the same split the .NET fixture
 * has, where /spot/state/request lives on the multi-node server itself. */
/* Mesh-member side of the forwarded state request: resolves the spot handle in
 * its own mesh and routes to the spot. */
class multi_node_state_member_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::route_client_t,
                                          zlink::framework::spot_handle_resolver_t>;
    using request_type = e2e::multi_node_state_route_req_t;
    using reply_type = e2e::state_res_t;

    multi_node_state_member_handler_t (zlink::framework::route_client_t &routes,
                                       zlink::framework::spot_handle_resolver_t &handles) :
        _routes (routes), _handles (handles)
    {
    }

    e2e::state_res_t handle (const e2e::multi_node_state_route_req_t &request,
                             const zlink::framework::route_handler_context_t &)
    {
        return request_multi_node_state (_routes, _handles, request.spot_id, request.delta);
    }

  private:
    zlink::framework::route_client_t &_routes;
    zlink::framework::spot_handle_resolver_t &_handles;
};

class multi_node_state_route_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::route_client_t,
                                          zlink::framework::spot_handle_resolver_t>;
    using request_type = e2e::multi_node_state_route_req_t;
    using reply_type = e2e::state_res_t;

    multi_node_state_route_handler_t (scenario_state_t &state,
                                      zlink::framework::route_client_t &routes,
                                      zlink::framework::spot_handle_resolver_t &handles) :
        _state (state), _routes (routes), _handles (handles)
    {
    }

    e2e::state_res_t handle (const e2e::multi_node_state_route_req_t &request)
    {
        auto reply = _routes
                       .request_to_node (multi_node_route_channel_for (_state.node_rid),
                                         zlink::routing_id_t::from (_state.node_rid), request)
                       .timeout (std::chrono::milliseconds (5000))
                       .submit<e2e::state_res_t> ()
                       .result ();
        if (reply) {
            return reply.value ();
        }
        throw std::runtime_error (
          "multi-node state route failed for '" + request.spot_id + "': "
          + (reply.error () ? reply.error ()->what () : "unknown route error"));
    }

  private:
    scenario_state_t &_state;
    zlink::framework::route_client_t &_routes;
    zlink::framework::spot_handle_resolver_t &_handles;
};

class multi_node_create_user_local_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::spot_node_manager_t>;

    multi_node_create_user_local_handler_t (scenario_state_t &state,
                                            zlink::framework::spot_node_manager_t &spots) :
        _state (state), _spots (spots)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::create_spot_req_t> ();
        const auto rid = (request.spot_id);
        const auto created =
          _spots.get_or_create_spot (multi_node_spot_name_for (_state.node_rid), rid, request);
        const auto state =
          created.state == zlink::framework::spot_create_state_t::created ? "created"
                                                                          : "existing";
        _state.record ("CreateUserSpot", {}, request.spot_id, state);

        zlink::framework::http_response_t response;
        response.body = nlohmann::json (e2e::create_spot_res_t{
                          .spot_id = request.spot_id,
                          .owner_node_rid = _state.node_rid,
                          .created = created.state == zlink::framework::spot_create_state_t::created})
                          .dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_node_manager_t &_spots;
};

class multi_node_spot_only_mesh_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::spot_node_manager_t>;

    multi_node_spot_only_mesh_handler_t (scenario_state_t &state,
                                         zlink::framework::spot_node_manager_t &spots) :
        _state (state), _spots (spots)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::spot_only_mesh_req_t> ();
        const auto rid = (request.source_spot_id);
        (void) _spots.get_or_create_spot (multi_node_spot_name_for (_state.node_rid), rid, request);
        const auto value_marker = "marker=" + request.marker;
        auto snapshot = _state.wait_until (
          [&] (const e2e::evidence_snapshot_t &current) {
              for (const auto &entry : current.entries) {
                  if (evidence_entry_matches (entry, "SpotOnlyRequest", request.source_spot_id,
                                              value_marker)) {
                      return true;
                  }
              }
              return false;
          },
          std::chrono::seconds (10));
        bool found = false;
        for (const auto &entry : snapshot.entries) {
            if (evidence_entry_matches (entry, "SpotOnlyRequest", request.source_spot_id,
                                        value_marker)) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::runtime_error ("timed out waiting for spot-only request evidence");
        }

        zlink::framework::http_response_t response;
        response.body =
          nlohmann::json (e2e::spot_only_mesh_res_t{
            .source_spot_id = request.source_spot_id,
            .target_spot_id = request.target_spot_id,
            .target_value = 7,
            .marker = request.marker})
            .dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_node_manager_t &_spots;
};

class multi_node_spot_only_join_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::session_actor_manager_t>;

    multi_node_spot_only_join_handler_t (scenario_state_t &state,
                                         zlink::framework::session_actor_manager_t &actors) :
        _state (state), _actors (actors)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::spot_only_join_req_t> ();
        auto actor = _actors.get_or_create (e2e::actor_type, request.actor_id);
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              actor.error_kind (),
              actor.error () ? actor.error ()->what () : "spot-only actor create failed");
        }
        auto bound = _actors.bind_or_get (actor.value ().ref ()).submit ().result ();
        if (!bound) {
            throw zlink::framework::framework_exception_t (
              bound.error_kind (),
              bound.error () ? bound.error ()->what () : "spot-only actor bind failed");
        }
        auto entry_joined =
          bound.value ()
            .context ()
            .join_entry_spot (zlink::framework::node_rid_t::from_string (_state.node_rid),
                              zlink::framework::message_t {})
            .async ()
            .result ();
        if (!entry_joined) {
            throw zlink::framework::framework_exception_t (
              entry_joined.error_kind (),
              entry_joined.error () ? entry_joined.error ()->what ()
                                    : "spot-only entry SPOT join failed");
        }
        auto current = _actors.find (request.actor_id);
        if (!current) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::not_found,
              "spot-only joined actor route was not found");
        }
        auto reply = current
                       ->relay_request ("SpotOnlyJoinReq",
                                        zlink::message_t::from_json (request))
                       .submit ()
                       .result ();
        if (!reply) {
            throw zlink::framework::framework_exception_t (
              reply.error_kind (),
              reply.error () ? reply.error ()->what () : "spot-only target join failed");
        }

        zlink::framework::http_response_t response;
        response.body =
          nlohmann::json (reply.value ().parse_json<e2e::spot_only_join_res_t> ()).dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::session_actor_manager_t &_actors;
};

} // namespace
