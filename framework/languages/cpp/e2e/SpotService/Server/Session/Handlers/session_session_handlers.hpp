/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/spot_service_contracts.hpp"
#include "../../Shared/scenario_state.hpp"
#include "../../Shared/spot_actor_support.hpp"

#include <zlink/framework.hpp>

#include <map>
#include <string>

namespace
{

class stream_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::route_client_t,
                                          zlink::framework::session_actor_manager_t>;

    stream_session_t (scenario_state_t &state,
                      zlink::framework::route_client_t &routes,
                      zlink::framework::session_actor_manager_t &actors) :
        _state (state), _routes (routes), _actors (actors)
    {
    }

    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &stream) override
    {
        _state.record ("StreamConnected", {}, {}, stream.session_id ());
        co_return;
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        // Framework가 disconnect 시점의 exact binding snapshot 전체에 통지한다.
        // Application callback은 Actor 목록을 순회하거나 통지를 다시 제출하지 않는다.
        _state.record ("StreamDisconnected");
        co_return;
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &error) override
    {
        _state.record ("StreamError", {}, {}, std::string (error.message ()));
        co_return;
    }

    zlink::framework::task_t<void> on_packet (
      zlink::framework::stream_t &stream,
      const zlink::framework::session_message_context_t &dispatch,
      const zlink::message_t &payload) override
    {
        if (dispatch.packet_name == "StreamAuthReq") {
            auto request = payload.parse_json<e2e::stream_auth_req_t> ();
            if (request.actor.actor_id.empty () || request.actor.actor_type.empty ()
                || (request.target_node_rid != "play-a" && request.target_node_rid != "play-b")) {
                _state.record ("StreamAuthFailed", request.actor_id, {}, request.target_node_rid);
                throw zlink::framework::framework_exception_t (
                  zlink::framework::framework_error_kind_t::protocol_error,
                  "stream auth target or actor ref is invalid");
            }
            _state.record ("StreamAuthEnsured", request.actor_id, {}, request.target_node_rid);
            auto bound_result = _actors.bind_or_get (to_actor_ref (request.actor)).submit ().result ();
            if (!bound_result) {
                throw zlink::framework::framework_exception_t (
                  bound_result.error_kind (),
                  bound_result.error () ? bound_result.error ()->what ()
                                       : "stream auth actor bind failed");
            }
            auto bound = bound_result.value ();
            const auto actor_id = std::string (bound.actor_id ());
            _state.record ("StreamAuthActorBound", actor_id, {}, request.target_node_rid);
            _bound_actors[actor_id] = request.target_node_rid;
            _bound_session_actors[actor_id] = bound;
            _state.record ("StreamAuthSessionBound", actor_id, {}, request.target_node_rid);
            _state.record ("StreamBound", actor_id, {},
                           request.target_node_rid + ":" + stream.session_id ());
            if (!dispatch.can_reply) {
                _state.record ("StreamAuthReplyUnavailable", actor_id, {},
                               request.target_node_rid);
                co_return;
            }
              stream
                .reply_packet (
                  zlink::message_t::from_json (
                    e2e::stream_auth_res_t{request.actor, _state.node_rid}))
                .submit ();
            _state.record ("StreamAuthReplied", actor_id, {}, request.target_node_rid);
            co_return;
        }
        if (dispatch.packet_name == "StreamEnsureAuthReq") {
            auto request = payload.parse_json<e2e::stream_ensure_auth_req_t> ();
            if (request.target_node_rid != "play-a" && request.target_node_rid != "play-b") {
                _state.record ("StreamAuthFailed", request.actor_id, {}, request.target_node_rid);
                throw zlink::framework::framework_exception_t (
                  zlink::framework::framework_error_kind_t::protocol_error,
                  "stream ensure auth target is invalid");
            }
            auto ensured_result =
              _routes
                .request_to_node (e2e::route_channel, zlink::routing_id_t::from (request.target_node_rid),
                          e2e::ensure_actor_req_t{.actor_id = request.actor_id,
                                                  .display_name = request.display_name})
                .submit<e2e::ensure_actor_res_t> ()
                .result ();
            if (!ensured_result) {
                throw zlink::framework::framework_exception_t (
                  ensured_result.error_kind (),
                  ensured_result.error () ? ensured_result.error ()->what ()
                                         : "stream ensure auth ensure actor failed");
            }
            const auto ensured = ensured_result.value ();
            auto bound_result = _actors.bind_or_get (to_actor_ref (ensured.actor)).submit ().result ();
            if (!bound_result) {
                throw zlink::framework::framework_exception_t (
                  bound_result.error_kind (),
                  bound_result.error () ? bound_result.error ()->what ()
                                       : "stream ensure auth actor bind failed");
            }
            auto bound = bound_result.value ();
            const auto actor_id = std::string (bound.actor_id ());
            _bound_actors[actor_id] = request.target_node_rid;
            _bound_session_actors[actor_id] = bound;
            _state.record ("StreamBound", actor_id, {},
                           request.target_node_rid + ":" + stream.session_id ());
            if (!dispatch.can_reply) {
                _state.record ("StreamAuthReplyUnavailable", actor_id, {},
                               request.target_node_rid);
                co_return;
            }
              stream
                .reply_packet (
                  zlink::message_t::from_json (
                    e2e::stream_auth_res_t{ensured.actor, _state.node_rid}))
                .submit ();
            co_return;
        }

        auto actor = require_bound_actor (dispatch, std::string (dispatch.packet_name));
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              actor.error_kind (),
              actor.error () ? actor.error ()->what () : "bound actor route is not found");
        }
        if (dispatch.can_reply) {
            auto reply = co_await actor.value ().relay_request (payload).submit ();
            if (dispatch.packet_name == "JoinReq") {
                const auto joined = reply.parse_json<e2e::join_res_t> ();
                auto rebound_result =
                  _actors.bind_or_get (to_actor_ref (joined.actor)).submit ().result ();
                if (!rebound_result) {
                    throw zlink::framework::framework_exception_t (
                      rebound_result.error_kind (),
                      rebound_result.error () ? rebound_result.error ()->what ()
                                             : "stream join actor bind failed");
                }
                auto rebound = rebound_result.value ();
                _bound_session_actors[std::string (rebound.actor_id ())] = rebound;
            }
            stream.reply_packet (reply).submit ();
            co_return;
        }
        co_await actor.value ().relay (payload);
        co_return;
    }

  private:
    zlink::framework::result_t<zlink::framework::session_actor_t>
    require_bound_actor (const zlink::framework::session_message_context_t &dispatch,
                         const std::string &packet_name) const
    {
        if (_bound_actors.empty ()) {
            return zlink::framework::result_t<zlink::framework::session_actor_t>::failure (
              zlink::framework::framework_error_kind_t::not_configured,
              "stream session is not bound before " + packet_name);
        }
        std::string actor_id;
        if (auto selected = dispatch.metadata.find ("actor-id")) {
            actor_id = std::string (*selected);
        } else if (_bound_actors.size () == 1) {
            actor_id = _bound_actors.begin ()->first;
        } else {
            return zlink::framework::result_t<zlink::framework::session_actor_t>::failure (
              zlink::framework::framework_error_kind_t::not_found,
              "actor-id metadata is required when multiple actors are bound for " + packet_name);
        }
        if (!_bound_actors.contains (actor_id)) {
            return zlink::framework::result_t<zlink::framework::session_actor_t>::failure (
              zlink::framework::framework_error_kind_t::not_found,
              "bound actor route is not found for " + actor_id + " / " + packet_name);
        }
        auto actor = _actors.find (actor_id);
        if (!actor) {
            return zlink::framework::result_t<zlink::framework::session_actor_t>::failure (
              zlink::framework::framework_error_kind_t::not_found,
              "bound actor route is not found for " + packet_name);
        }
        return zlink::framework::result_t<zlink::framework::session_actor_t>::success (
          std::move (*actor));
    }

    scenario_state_t &_state;
    zlink::framework::route_client_t &_routes;
    zlink::framework::session_actor_manager_t &_actors;
    std::map<std::string, std::string> _bound_actors;
    std::map<std::string, zlink::framework::session_actor_t> _bound_session_actors;
};

} // namespace
