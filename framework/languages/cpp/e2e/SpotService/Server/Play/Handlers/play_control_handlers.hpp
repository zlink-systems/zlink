/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/spot_service_contracts.hpp"
#include "../../Shared/scenario_state.hpp"
#include "../../Shared/spot_actor_support.hpp"

#include <zlink/framework.hpp>

#include <string>

namespace
{

class spot_lifecycle_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t, zlink::framework::spot_node_manager_t>;
    using request_type = e2e::lifecycle_req_t;
    using reply_type = e2e::lifecycle_res_t;

    spot_lifecycle_handler_t (scenario_state_t &state,
                              zlink::framework::spot_node_manager_t &spots) :
        _state (state), _spots (spots)
    {
    }

    e2e::lifecycle_res_t handle (const e2e::lifecycle_req_t &request,
                                 const zlink::framework::route_handler_context_t &)
    {
        const auto rid = user_spot_id (request.key);
        const auto created = _spots.get_or_create_spot (e2e::user_spot, rid, request);
        const auto closed = _spots.close_spot (rid).result ();
        _state.record ("SpotLifecycleClosed", {}, rid,
                       closed && closed.value () ? "closed" : "not-closed");
        return {.spot_id = created.spot_id,
                .created = created.state == zlink::framework::spot_create_state_t::created,
                .closed = closed && closed.value ()};
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_node_manager_t &_spots;
};

class ensure_actor_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::spot_node_manager_t,
                                          zlink::framework::session_actor_manager_t>;
    using request_type = e2e::ensure_actor_req_t;
    using reply_type = e2e::ensure_actor_res_t;

    ensure_actor_handler_t (scenario_state_t &state,
                            zlink::framework::spot_node_manager_t &spots,
                            zlink::framework::session_actor_manager_t &actors) :
        _state (state), _spots (spots), _actors (actors)
    {
    }

    e2e::ensure_actor_res_t handle (
      const e2e::ensure_actor_req_t &request,
      const zlink::framework::route_handler_context_t &)
    {
        auto current = _spots.current_actor_ref (zlink::framework::actor_ref_t (
          zlink::framework::actor_id_t (request.actor_id), 1, e2e::spot_mesh,
          zlink::framework::node_rid_t::from_string (_state.node_rid)));
        if (current) {
            _state.record ("ActorEnsured", request.actor_id, {}, request.display_name);
            return {.actor = from_actor_ref (*current)};
        }
        auto actor = _actors.get_or_create (e2e::actor_type, request.actor_id);
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              actor.error_kind (),
              actor.error () ? actor.error ()->what () : "ensure actor create failed");
        }
        auto bound = _actors.bind_or_get (actor.value ().ref ()).submit ().result ();
        if (!bound) {
            throw zlink::framework::framework_exception_t (
              bound.error_kind (),
              bound.error () ? bound.error ()->what () : "ensure actor bind failed");
        }
        auto joined =
          bound.value ()
            .context ()
            .join_entry_spot (zlink::framework::node_rid_t::from_string (_state.node_rid),
                              zlink::framework::message_t {})
            .async ()
            .result ();
        const auto *joined_accepted =
          joined ? std::get_if<zlink::framework::actor_join_accepted_t<zlink::framework::message_t>> (&joined.value ()) : nullptr;
        if (joined_accepted == nullptr) {
            if (!joined) {
                throw zlink::framework::framework_exception_t (
                  joined.error_kind (),
                  joined.error () ? joined.error ()->what () : "ensure actor entry join failed");
            }
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::rejected,
              "ensure actor entry join was rejected");
        }
        _state.record ("ActorEnsured", request.actor_id, {}, request.display_name);
        return {.actor = from_actor_ref (joined_accepted->actor)};
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_node_manager_t &_spots;
    zlink::framework::session_actor_manager_t &_actors;
};

class create_spot_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::spot_node_manager_t>;

    create_spot_handler_t (scenario_state_t &state,
                           zlink::framework::spot_node_manager_t &spots) :
        _state (state), _spots (spots)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::create_spot_req_t> ();
        const auto rid = (request.spot_id);
        const auto created = _spots.get_or_create_spot (e2e::user_spot, rid, request);

        zlink::framework::http_response_t response;
        response.body = nlohmann::json (e2e::create_spot_res_t{
                          .spot_id = created.spot_id,
                          .owner_node_rid = _state.node_rid,
                          .created = created.state == zlink::framework::spot_create_state_t::created})
                          .dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_node_manager_t &_spots;
};

class create_alternate_spot_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::spot_node_manager_t>;

    create_alternate_spot_handler_t (scenario_state_t &state,
                                     zlink::framework::spot_node_manager_t &spots) :
        _state (state), _spots (spots)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::create_spot_req_t> ();
        const auto rid = (request.spot_id);
        const auto created = _spots.get_or_create_spot (e2e::alternate_spot, rid, request);

        zlink::framework::http_response_t response;
        response.body = nlohmann::json (e2e::create_spot_res_t{
                          .spot_id = created.spot_id,
                          .owner_node_rid = _state.node_rid,
                          .created = created.state == zlink::framework::spot_create_state_t::created})
                          .dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_node_manager_t &_spots;
};

class lifecycle_spot_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::spot_node_manager_t>;

    lifecycle_spot_handler_t (scenario_state_t &state,
                              zlink::framework::spot_node_manager_t &spots) :
        _state (state), _spots (spots)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::lifecycle_req_t> ();
        const auto rid =
          (e2e::user_spot_id_for_key (request.key));
        const auto created = _spots.get_or_create_spot (e2e::user_spot, rid, request);
        const auto closed = _spots.close_spot (rid).result ();
        _state.record ("SpotLifecycleClosed", {}, rid,
                       closed && closed.value () ? "closed" : "not-closed");

        zlink::framework::http_response_t response;
        response.body = nlohmann::json (e2e::lifecycle_res_t{
                          .spot_id = created.spot_id,
                          .created = created.state == zlink::framework::spot_create_state_t::created,
                          .closed = closed && closed.value ()})
                          .dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_node_manager_t &_spots;
};

class close_spot_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::spot_node_manager_t>;

    close_spot_handler_t (scenario_state_t &state, zlink::framework::spot_node_manager_t &spots) :
        _state (state), _spots (spots)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::close_spot_req_t> ();
        const auto rid =
          (e2e::user_spot_id_for_key (request.key));
        const auto closed = _spots.close_spot (rid).result ();
        _state.record ("SpotCloseRequested", {}, rid,
                       closed && closed.value () ? "closed" : "not-closed");

        zlink::framework::http_response_t response;
        response.body = nlohmann::json (
                          e2e::close_spot_res_t{.spot_id = rid,
                                                .closed = closed && closed.value ()})
                          .dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_node_manager_t &_spots;
};

class type_mismatch_spot_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::spot_node_manager_t,
                                          zlink::framework::session_actor_manager_t>;

    type_mismatch_spot_handler_t (scenario_state_t &state,
                                  zlink::framework::spot_node_manager_t &spots,
                                  zlink::framework::session_actor_manager_t &actors) :
        _state (state), _spots (spots), _actors (actors)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::type_mismatch_req_t> ();
        const auto rid =
          (e2e::user_spot_id_for_key (request.probe));
        const auto actor_id = "sm-a7-actor-" + request.probe;
        auto actor = _actors.get_or_create (e2e::actor_type, actor_id);
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              actor.error_kind (), actor.error () ? actor.error ()->what () : "actor create failed");
        }
        auto bound = _actors.bind_or_get (actor.value ().ref ()).submit ().result ();
        if (!bound) {
            throw zlink::framework::framework_exception_t (
              bound.error_kind (), bound.error () ? bound.error ()->what () : "actor bind failed");
        }
        auto joined = bound.value ()
                        .context ()
                        .join_entry_spot (
                          zlink::framework::node_rid_t::from_string (_state.node_rid),
                          zlink::framework::message_t {})
                        .async ()
                        .result ();
        if (!joined) {
            throw zlink::framework::framework_exception_t (
              joined.error_kind (),
              joined.error () ? joined.error ()->what () : "entry SPOT join failed");
        }
        auto current = _actors.find (actor_id);
        if (!current) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::not_found,
              "SM-A7 actor route was not found");
        }
        auto join_reply =
          current
            ->relay_request ("JoinReq",
                             zlink::message_t::from_json (e2e::join_req_t{
                               .key = request.probe,
                               .actor_id = actor_id,
                               .display_name = "SM-A7",
                               .level = 7,
                               .tags = {"type-mismatch"}}))
            .submit ()
            .result ();
        if (!join_reply) {
            throw zlink::framework::framework_exception_t (
              join_reply.error_kind (),
              join_reply.error () ? join_reply.error ()->what () : "SM-A7 join failed");
        }
        auto seeded =
          current
            ->relay_request ("StateReq",
                             zlink::message_t::from_json (
                               e2e::state_req_t{.op = "set", .amount = 17}))
            .submit ()
            .result ();
        if (!seeded) {
            throw zlink::framework::framework_exception_t (
              seeded.error_kind (),
              seeded.error () ? seeded.error ()->what () : "SM-A7 seed state failed");
        }
        try {
            (void) _spots.get_or_create_spot (e2e::alternate_spot, rid, request);
        }
        catch (const zlink::framework::framework_exception_t &error) {
            if (error.kind () == zlink::framework::framework_error_kind_t::type_mismatch) {
                const auto spot_name = _spots.spot_name_for (rid).value_or ("");
                auto observed =
                  current
                    ->relay_request ("StateReq",
                                     zlink::message_t::from_json (
                                       e2e::state_req_t{.op = "noop", .amount = 0}))
                    .submit ()
                    .result ();
                if (!observed) {
                    throw zlink::framework::framework_exception_t (
                      observed.error_kind (),
                      observed.error () ? observed.error ()->what ()
                                        : "SM-A7 observe state failed");
                }
                _state.record ("SpotTypeMismatch", {}, rid, spot_name);

                zlink::framework::http_response_t response;
                response.body = nlohmann::json (e2e::type_mismatch_res_t{
                                  .rejected = true,
                                  .error_kind = "spot_type_mismatch",
                                  .spot_name = spot_name,
                                  .value = observed.value ().parse_json<e2e::state_res_t> ().value})
                                  .dump ();
                return response;
            }
            throw;
        }

        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::internal_failure,
          "expected spot type mismatch");
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_node_manager_t &_spots;
    zlink::framework::session_actor_manager_t &_actors;
};

} // namespace
