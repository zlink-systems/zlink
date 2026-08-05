/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/scenario_state.hpp"

#include <zlink/framework.hpp>
#include <zlink/http_client.hpp>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace e2e = zlink::framework::e2e::spot_service;

struct play_a_owner_http_client_tag_t;
struct play_b_owner_http_client_tag_t;
using play_a_owner_http_client_t =
  zlink::http_client::named_server_client_t<play_a_owner_http_client_tag_t>;
using play_b_owner_http_client_t =
  zlink::http_client::named_server_client_t<play_b_owner_http_client_tag_t>;

inline std::string public_error_kind_name (zlink::framework::framework_error_kind_t kind)
{
    switch (kind) {
        case zlink::framework::framework_error_kind_t::not_found:
            return "actor_dispatch_handler_not_found";
        case zlink::framework::framework_error_kind_t::not_found:
            return "handler_not_found";
        case zlink::framework::framework_error_kind_t::not_found:
            return "route_handler_not_found";
        default:
            return "request_failed";
    }
}

class join_spot_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::session_actor_manager_t>;
    join_spot_handler_t (scenario_state_t &state,
                         zlink::framework::session_actor_manager_t &actors) :
        _state (state), _actors (actors)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::join_req_t> ();
        auto actor = bind_actor (request);
        auto joined = actor.context ()
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
        auto current = _actors.find (request.actor_id);
        if (!current) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::not_found,
              "joined actor route was not found");
        }
        auto reply =
          current->relay_request ("JoinReq", zlink::message_t::from_json (request))
            .submit ()
            .result ();
        if (!reply) {
            throw zlink::framework::framework_exception_t (
              reply.error_kind (),
              reply.error () ? reply.error ()->what () : "JoinReq failed");
        }
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (reply.value ().parse_json<e2e::join_res_t> ()).dump ();
        return response;
    }

  private:
    zlink::framework::session_actor_t bind_actor (const e2e::join_req_t &request)
    {
        auto actor = _actors.get_or_create (e2e::actor_type, request.actor_id);
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              actor.error_kind (),
              actor.error () ? actor.error ()->what () : "actor create failed");
        }
        auto bound = _actors.bind_or_get (actor.value ().ref ()).submit ().result ();
        if (!bound) {
            throw zlink::framework::framework_exception_t (
              bound.error_kind (),
              bound.error () ? bound.error ()->what () : "actor bind failed");
        }
        return bound.value ();
    }

    scenario_state_t &_state;
    zlink::framework::session_actor_manager_t &_actors;
};

class channel_echo_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;
    using request_type = e2e::channel_echo_req_t;
    using reply_type = e2e::channel_echo_res_t;

    explicit channel_echo_handler_t (scenario_state_t &state) : _state (state) {}

    e2e::channel_echo_res_t handle (const e2e::channel_echo_req_t &request)
    {
        _state.record ("ChannelEcho", {}, {}, request.value);
        return {.value = "echo-" + request.value, .handled_by = _state.node_rid};
    }

    e2e::channel_echo_res_t handle (
      const e2e::channel_echo_req_t &request,
      const zlink::framework::route_handler_context_t &)
    {
        return handle (request);
    }

  private:
    scenario_state_t &_state;
};

class channel_command_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;
    using message_type = e2e::channel_msg_t;

    explicit channel_command_handler_t (scenario_state_t &state) : _state (state) {}

    void handle (const e2e::channel_msg_t &command)
    {
        _state.record ("ChannelMsg", {}, {}, command.command_id);
    }

  private:
    scenario_state_t &_state;
};

class channel_slow_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;
    using request_type = e2e::channel_slow_req_t;
    using reply_type = e2e::channel_slow_res_t;

    explicit channel_slow_handler_t (scenario_state_t &state) : _state (state) {}

    e2e::channel_slow_res_t handle (const e2e::channel_slow_req_t &request)
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (request.delay_ms));
        _state.record ("ChannelSlow", {}, {}, request.value);
        return {.value = "slow-" + request.value};
    }

  private:
    scenario_state_t &_state;
};

class mutate_spot_state_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::session_actor_manager_t>;

    explicit mutate_spot_state_handler_t (zlink::framework::session_actor_manager_t &actors) :
        _actors (actors)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::spot_state_req_t> ();
        auto actor = _actors.find (request.actor_id);
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::not_found,
              "state actor route was not found");
        }
        auto reply =
          actor->relay_request ("StateReq", zlink::message_t::from_json (request.state))
            .submit ()
            .result ();
        if (!reply) {
            throw zlink::framework::framework_exception_t (
              reply.error_kind (),
              reply.error () ? reply.error ()->what () : "StateReq failed");
        }

        zlink::framework::http_response_t response;
        response.body = nlohmann::json (reply.value ().parse_json<e2e::state_res_t> ()).dump ();
        return response;
    }

  private:
    zlink::framework::session_actor_manager_t &_actors;
};

class complex_actor_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::session_actor_manager_t>;

    complex_actor_handler_t (scenario_state_t &state,
                             zlink::framework::session_actor_manager_t &actors) :
        _state (state), _actors (actors)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request =
          nlohmann::json::parse (http.body).get<e2e::spot_complex_actor_req_t> ();
        auto actor = _actors.get_or_create (e2e::actor_type, request.join.actor_id);
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              actor.error_kind (),
              actor.error () ? actor.error ()->what () : "complex actor create failed");
        }
        auto bound = _actors.bind_or_get (actor.value ().ref ()).submit ().result ();
        if (!bound) {
            throw zlink::framework::framework_exception_t (
              bound.error_kind (),
              bound.error () ? bound.error ()->what () : "complex actor bind failed");
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
                                    : "complex entry SPOT join failed");
        }
        auto current = _actors.find (request.join.actor_id);
        if (!current) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::not_found,
              "complex actor route was not found");
        }
        auto join_reply =
          current->relay_request ("JoinReq", zlink::message_t::from_json (request.join))
            .submit ()
            .result ();
        if (!join_reply) {
            throw zlink::framework::framework_exception_t (
              join_reply.error_kind (),
              join_reply.error () ? join_reply.error ()->what () : "complex JoinReq failed");
        }
        auto complex_reply =
          current->relay_request ("ComplexActorReq",
                                  zlink::message_t::from_json (request.complex))
            .submit ()
            .result ();
        if (!complex_reply) {
            throw zlink::framework::framework_exception_t (
              complex_reply.error_kind (),
              complex_reply.error () ? complex_reply.error ()->what ()
                                     : "ComplexActorReq failed");
        }

        zlink::framework::http_response_t response;
        response.body =
          nlohmann::json (e2e::spot_complex_actor_res_t{
            .join = join_reply.value ().parse_json<e2e::join_res_t> (),
            .complex = complex_reply.value ().parse_json<e2e::complex_actor_res_t> ()})
            .dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::session_actor_manager_t &_actors;
};

class missing_actor_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::session_actor_manager_t>;

    missing_actor_handler_t (scenario_state_t &state,
                             zlink::framework::session_actor_manager_t &actors) :
        _state (state), _actors (actors)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::missing_actor_req_t> ();
        auto actor = _actors.get_or_create (e2e::actor_type, request.join.actor_id);
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              actor.error_kind (),
              actor.error () ? actor.error ()->what () : "missing actor create failed");
        }
        auto bound = _actors.bind_or_get (actor.value ().ref ()).submit ().result ();
        if (!bound) {
            throw zlink::framework::framework_exception_t (
              bound.error_kind (),
              bound.error () ? bound.error ()->what () : "missing actor bind failed");
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
                                    : "missing actor entry SPOT join failed");
        }
        auto current = _actors.find (request.join.actor_id);
        if (!current) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::not_found,
              "missing actor route was not found");
        }
        auto join_reply =
          current->relay_request ("JoinReq", zlink::message_t::from_json (request.join))
            .submit ()
            .result ();
        if (!join_reply) {
            throw zlink::framework::framework_exception_t (
              join_reply.error_kind (),
              join_reply.error () ? join_reply.error ()->what () : "missing actor JoinReq failed");
        }
        auto missing =
          current
            ->relay_request (request.packet_name,
                             zlink::message_t::from_json (e2e::state_req_t{"add", 1}))
            .submit ()
            .result ();
        const auto failed = !missing.has_value ();
        zlink::framework::http_response_t response;
        response.body =
          nlohmann::json (e2e::missing_actor_res_t{
            .failed = failed,
            .error_kind = failed ? public_error_kind_name (missing.error_kind ()) : "none"})
            .dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::session_actor_manager_t &_actors;
};

class remote_actor_flow_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::session_actor_manager_t,
                                          play_a_owner_http_client_t,
                                          play_b_owner_http_client_t>;

    remote_actor_flow_handler_t (scenario_state_t &state,
                                 zlink::framework::session_actor_manager_t &actors,
                                 play_a_owner_http_client_t &play_a,
                                 play_b_owner_http_client_t &play_b) :
        _state (state), _actors (actors), _play_a (play_a), _play_b (play_b)
    {
    }

    zlink::framework::task_t<zlink::framework::http_response_t>
    handle (const zlink::framework::http_request_t &http)
    {
        const auto request =
          nlohmann::json::parse (http.body).get<e2e::remote_actor_flow_req_t> ();
        const auto target_node = e2e::owner_node_rid_for_key (request.join.key);
        if (target_node != _state.node_rid) {
            co_return co_await forward_to_owner (request, target_node);
        }

        auto actor = _actors.get_or_create (e2e::actor_type, request.join.actor_id);
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              actor.error_kind (),
              actor.error () ? actor.error ()->what () : "remote actor create failed");
        }
        auto bound = _actors.bind_or_get (actor.value ().ref ()).submit ().result ();
        if (!bound) {
            throw zlink::framework::framework_exception_t (
              bound.error_kind (),
              bound.error () ? bound.error ()->what () : "remote actor bind failed");
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
                                    : "remote entry SPOT join failed");
        }
        auto current = _actors.find (request.join.actor_id);
        if (!current) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::not_found,
              "remote actor route was not found");
        }
        auto join_reply =
          current->relay_request ("JoinReq", zlink::message_t::from_json (request.join))
            .submit ()
            .result ();
        if (!join_reply) {
            throw zlink::framework::framework_exception_t (
              join_reply.error_kind (),
              join_reply.error () ? join_reply.error ()->what () : "remote JoinReq failed");
        }
        auto state_reply =
          current->relay_request ("StateReq", zlink::message_t::from_json (request.state))
            .submit ()
            .result ();
        if (!state_reply) {
            throw zlink::framework::framework_exception_t (
              state_reply.error_kind (),
              state_reply.error () ? state_reply.error ()->what () : "remote StateReq failed");
        }

        zlink::framework::http_response_t response;
        response.body =
          nlohmann::json (e2e::remote_actor_flow_res_t{
            .join = join_reply.value ().parse_json<e2e::join_res_t> (),
            .state = state_reply.value ().parse_json<e2e::state_res_t> ()})
            .dump ();
        co_return response;
    }

  private:
    zlink::framework::task_t<zlink::framework::http_response_t>
    forward_to_owner (const e2e::remote_actor_flow_req_t &request,
                      const std::string &target_node)
    {
        auto forwarded = co_await owner_client (target_node)
                           .post ("/spot/remote-actor")
                           .body (request)
                           .yield_raw ();
        if (forwarded.status >= 400) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::internal_failure,
              "remote actor HTTP forward status " + std::to_string (forwarded.status) + ": "
                + forwarded.body);
        }
        zlink::framework::http_response_t response;
        response.body = std::move (forwarded.body);
        co_return response;
    }

    zlink::http_client::server_client_t &owner_client (const std::string &target_node)
    {
        return target_node == "play-b" ? static_cast<zlink::http_client::server_client_t &> (_play_b)
                                        : static_cast<zlink::http_client::server_client_t &> (_play_a);
    }

    scenario_state_t &_state;
    zlink::framework::session_actor_manager_t &_actors;
    play_a_owner_http_client_t &_play_a;
    play_b_owner_http_client_t &_play_b;
};

class remote_actor_request_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::route_client_t,
                                          zlink::framework::session_actor_manager_t,
                                          play_a_owner_http_client_t,
                                          play_b_owner_http_client_t>;

    remote_actor_request_handler_t (scenario_state_t &state,
                                    zlink::framework::route_client_t &routes,
                                    zlink::framework::session_actor_manager_t &actors,
                                    play_a_owner_http_client_t &play_a,
                                    play_b_owner_http_client_t &play_b) :
        _state (state), _routes (routes), _actors (actors), _play_a (play_a), _play_b (play_b)
    {
    }

    zlink::framework::task_t<zlink::framework::http_response_t>
    handle (const zlink::framework::http_request_t &http)
    {
        const auto request =
          nlohmann::json::parse (http.body).get<e2e::remote_actor_request_req_t> ();
        const auto target_node = e2e::owner_node_rid_for_key (request.join.key);
        if (target_node != _state.node_rid) {
            co_return co_await forward_to_owner (request, target_node);
        }
        auto actor = _actors.get_or_create (e2e::actor_type, request.join.actor_id);
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              actor.error_kind (),
              actor.error () ? actor.error ()->what () : "remote actor create failed");
        }
        _state.record ("ActorEnsured", request.join.actor_id, {}, request.join.display_name);
        auto bound = _actors.bind_or_get (actor.value ().ref ()).submit ().result ();
        if (!bound) {
            throw zlink::framework::framework_exception_t (
              bound.error_kind (),
              bound.error () ? bound.error ()->what () : "remote request actor bind failed");
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
                                    : "remote actor entry SPOT join failed");
        }
        auto current = _actors.find (request.join.actor_id);
        if (!current) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::not_found,
              "remote actor route was not found");
        }
        auto join_reply =
          current->relay_request ("JoinReq", zlink::message_t::from_json (request.join))
            .submit ()
            .result ();
        if (!join_reply) {
            throw zlink::framework::framework_exception_t (
              join_reply.error_kind (),
              join_reply.error () ? join_reply.error ()->what () : "remote actor JoinReq failed");
        }
        _state.record ("RemoteActorRequestSent", request.join.actor_id, {},
                       target_node + ":" + std::to_string (request.state.amount));
        auto reply =
          current->relay_request ("StateReq", zlink::message_t::from_json (request.state))
            .submit ()
            .result ();
        if (!reply) {
            throw zlink::framework::framework_exception_t (
              reply.error_kind (),
              reply.error () ? reply.error ()->what () : "remote actor request failed");
        }
        auto state = reply.value ().parse_json<e2e::state_res_t> ();
        _state.record ("RemoteActorRequestReply", request.join.actor_id, {},
                       state.owner_node_rid + ":" + std::to_string (state.value));

        zlink::framework::http_response_t response;
        response.body = nlohmann::json (state).dump ();
        co_return response;
    }

  private:
    zlink::framework::task_t<zlink::framework::http_response_t>
    forward_to_owner (const e2e::remote_actor_request_req_t &request,
                      const std::string &target_node)
    {
        auto forwarded = co_await owner_client (target_node)
                           .post ("/spot/remote-actor-request")
                           .body (request)
                           .yield_raw ();
        if (forwarded.status >= 400) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::internal_failure,
              "remote actor request HTTP forward status " + std::to_string (forwarded.status)
                + ": " + forwarded.body);
        }
        zlink::framework::http_response_t response;
        response.body = std::move (forwarded.body);
        co_return response;
    }

    zlink::http_client::server_client_t &owner_client (const std::string &target_node)
    {
        return target_node == "play-b" ? static_cast<zlink::http_client::server_client_t &> (_play_b)
                                        : static_cast<zlink::http_client::server_client_t &> (_play_a);
    }

    scenario_state_t &_state;
    zlink::framework::route_client_t &_routes;
    zlink::framework::session_actor_manager_t &_actors;
    play_a_owner_http_client_t &_play_a;
    play_b_owner_http_client_t &_play_b;
};

class worker_spot_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::session_actor_manager_t>;

    explicit worker_spot_handler_t (zlink::framework::session_actor_manager_t &actors) :
        _actors (actors)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::spot_worker_req_t> ();
        auto actor = _actors.find (request.actor_id);
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::not_found,
              "worker actor route was not found");
        }
        auto reply =
          actor->relay_request ("WorkerReq", zlink::message_t::from_json (request.worker))
            .submit ()
            .result ();
        if (!reply) {
            throw zlink::framework::framework_exception_t (
              reply.error_kind (),
              reply.error () ? reply.error ()->what () : "WorkerReq failed");
        }

        zlink::framework::http_response_t response;
        response.body = nlohmann::json (reply.value ().parse_json<e2e::worker_res_t> ()).dump ();
        return response;
    }

  private:
    zlink::framework::session_actor_manager_t &_actors;
};
