/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/spot_service_contracts.hpp"
#include "../../Shared/scenario_state.hpp"
#include "../../Shared/spot_actor_support.hpp"

#include <zlink/framework.hpp>

#include <string>

namespace
{

template <typename TRequest>
zlink::framework::spot_create_result_t
create_spot (zlink::framework::spot_manager_t &spots,
             zlink::framework::spot_id_t spot_id,
             std::string stable_type,
             TRequest request)
{
    auto created = spots
                     .get_or_create (std::move (spot_id), std::move (stable_type))
                     .creation_request (std::move (request))
                     .async ()
                     .result ();
    if (!created) {
        throw zlink::framework::framework_exception_t (
          created.error_kind (),
          created.error () ? created.error ()->what () : "Spot creation failed");
    }
    return created.value ();
}

bool close_spot (zlink::framework::spot_manager_t &spots,
                 const zlink::framework::spot_id_t &spot_id)
{
    auto found = spots.find (spot_id).result ();
    if (!found) {
        throw zlink::framework::framework_exception_t (
          found.error_kind (),
          found.error () ? found.error ()->what () : "Spot lookup failed");
    }
    if (!found.value ()) {
        return false;
    }
    auto closed = spots.close (*found.value ()).result ();
    if (!closed) {
        throw zlink::framework::framework_exception_t (
          closed.error_kind (),
          closed.error () ? closed.error ()->what () : "Spot close failed");
    }
    return closed.value ();
}

class spot_lifecycle_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t, zlink::framework::spot_manager_t>;
    using request_type = e2e::lifecycle_req_t;
    using reply_type = e2e::lifecycle_res_t;

    spot_lifecycle_handler_t (scenario_state_t &state,
                              zlink::framework::spot_manager_t &spots) :
        _state (state), _spots (spots)
    {
    }

    e2e::lifecycle_res_t handle (
      const e2e::lifecycle_req_t &request,
      const zlink::framework::route_message_context_t &)
    {
        const auto rid = user_spot_id (request.key);
        const auto created = create_spot (_spots, rid, e2e::user_spot, request);
        const auto closed = close_spot (_spots, rid);
        _state.record ("SpotLifecycleClosed", {}, rid,
                       closed ? "closed" : "not-closed");
        return {.spot_id = std::string (created.spot.spot_id ()),
                .created = created.state == zlink::framework::spot_create_state_t::created,
                .closed = closed};
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_manager_t &_spots;
};

class ensure_actor_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::session_actor_manager_t>;
    using request_type = e2e::ensure_actor_req_t;
    using reply_type = e2e::ensure_actor_res_t;

    ensure_actor_handler_t (scenario_state_t &state,
                            zlink::framework::session_actor_manager_t &actors) :
        _state (state), _actors (actors)
    {
    }

    e2e::ensure_actor_res_t handle (
      const e2e::ensure_actor_req_t &request,
      const zlink::framework::route_message_context_t &)
    {
        auto current = _actors.find (request.actor_id);
        if (current) {
            _state.record ("ActorEnsured", request.actor_id, {}, request.display_name);
            return {.actor = from_actor_ref (current->ref ())};
        }
        auto actor = _actors.get_or_create (e2e::actor_type, request.actor_id);
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              actor.error_kind (),
              actor.error () ? actor.error ()->what () : "ensure actor create failed");
        }
        auto bound = _actors.bind_or_get (actor.value ().ref ()).async ().result ();
        if (!bound) {
            throw zlink::framework::framework_exception_t (
              bound.error_kind (),
              bound.error () ? bound.error ()->what () : "ensure actor bind failed");
        }
        _state.record ("ActorEnsured", request.actor_id, {}, request.display_name);
        return {.actor = from_actor_ref (bound.value ().ref ())};
    }

  private:
    scenario_state_t &_state;
    zlink::framework::session_actor_manager_t &_actors;
};

class create_spot_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::spot_manager_t>;

    create_spot_handler_t (scenario_state_t &state,
                           zlink::framework::spot_manager_t &spots) :
        _state (state), _spots (spots)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::create_spot_req_t> ();
        const auto rid = (request.spot_id);
        const auto created = create_spot (_spots, rid, e2e::user_spot, request);

        zlink::framework::http_response_t response;
        response.body = nlohmann::json (e2e::create_spot_res_t{
                          .spot_id = std::string (created.spot.spot_id ()),
                          .owner_node_rid = _state.node_rid,
                          .created = created.state == zlink::framework::spot_create_state_t::created})
                          .dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_manager_t &_spots;
};

class create_alternate_spot_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::spot_manager_t>;

    create_alternate_spot_handler_t (scenario_state_t &state,
                                     zlink::framework::spot_manager_t &spots) :
        _state (state), _spots (spots)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::create_spot_req_t> ();
        const auto rid = (request.spot_id);
        const auto created = create_spot (_spots, rid, e2e::alternate_spot, request);

        zlink::framework::http_response_t response;
        response.body = nlohmann::json (e2e::create_spot_res_t{
                          .spot_id = std::string (created.spot.spot_id ()),
                          .owner_node_rid = _state.node_rid,
                          .created = created.state == zlink::framework::spot_create_state_t::created})
                          .dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_manager_t &_spots;
};

class lifecycle_spot_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::spot_manager_t>;

    lifecycle_spot_handler_t (scenario_state_t &state,
                              zlink::framework::spot_manager_t &spots) :
        _state (state), _spots (spots)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::lifecycle_req_t> ();
        const auto rid =
          (e2e::user_spot_id_for_key (request.key));
        const auto created = create_spot (_spots, rid, e2e::user_spot, request);
        const auto closed = close_spot (_spots, rid);
        _state.record ("SpotLifecycleClosed", {}, rid,
                       closed ? "closed" : "not-closed");

        zlink::framework::http_response_t response;
        response.body = nlohmann::json (e2e::lifecycle_res_t{
                          .spot_id = std::string (created.spot.spot_id ()),
                          .created = created.state == zlink::framework::spot_create_state_t::created,
                          .closed = closed})
                          .dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_manager_t &_spots;
};

class close_spot_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::spot_manager_t>;

    close_spot_handler_t (scenario_state_t &state, zlink::framework::spot_manager_t &spots) :
        _state (state), _spots (spots)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::close_spot_req_t> ();
        const auto rid =
          (e2e::user_spot_id_for_key (request.key));
        const auto closed = close_spot (_spots, rid);
        _state.record ("SpotCloseRequested", {}, rid,
                       closed ? "closed" : "not-closed");

        zlink::framework::http_response_t response;
        response.body = nlohmann::json (
                          e2e::close_spot_res_t{.spot_id = rid,
                                                .closed = closed})
                          .dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_manager_t &_spots;
};

class type_mismatch_spot_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::spot_manager_t,
                                          zlink::framework::session_actor_manager_t>;

    type_mismatch_spot_handler_t (scenario_state_t &state,
                                  zlink::framework::spot_manager_t &spots,
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
        auto bound = _actors.bind_or_get (actor.value ().ref ()).async ().result ();
        if (!bound) {
            throw zlink::framework::framework_exception_t (
              bound.error_kind (), bound.error () ? bound.error ()->what () : "actor bind failed");
        }
        bound.value ().context ().join_entry_spot ().defer ();
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
            .async ()
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
            .async ()
            .result ();
        if (!seeded) {
            throw zlink::framework::framework_exception_t (
              seeded.error_kind (),
              seeded.error () ? seeded.error ()->what () : "SM-A7 seed state failed");
        }
        try {
            (void) create_spot (_spots, rid, e2e::alternate_spot, request);
        }
        catch (const zlink::framework::framework_exception_t &error) {
            if (error.kind () == zlink::framework::framework_error_kind_t::type_mismatch) {
                const auto spot_name = std::string (e2e::user_spot);
                auto observed =
                  current
                    ->relay_request ("StateReq",
                                     zlink::message_t::from_json (
                                       e2e::state_req_t{.op = "noop", .amount = 0}))
                    .async ()
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
    zlink::framework::spot_manager_t &_spots;
    zlink::framework::session_actor_manager_t &_actors;
};

} // namespace
