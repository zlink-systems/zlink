/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../../Shared/codecs.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <map>
#include <string>
#include <thread>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::session {

namespace yd = zlink::framework::e2e::automatic_turn_dispatch;

class await_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::route_client_t,
                                          zlink::framework::session_actor_manager_t,
                                          zlink::framework::spot_handle_resolver_t>;

    await_session_t (zlink::framework::route_client_t &routes,
                     zlink::framework::session_actor_manager_t &actors,
                     zlink::framework::spot_handle_resolver_t &spots) :
        _routes (routes), _actors (actors), _spots (spots)
    {
    }

    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        _bound_actors.clear ();
        co_return;
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_packet (
      zlink::framework::stream_t &stream,
      const zlink::framework::session_message_context_t &dispatch,
      const zlink::message_t &payload) override
    {
        const auto packet = std::string (dispatch.packet_name);
        if (packet == yd::bind_await_actors_req_t::packet_name) {
            auto request = payload.parse_json<yd::bind_await_actors_req_t> ();
            auto reply = co_await request_control<yd::bind_await_actors_res_t> (
              request, packet, target_or_default (dispatch));
            for (const auto &actor : reply.actors) {
                (void) co_await _actors.bind_or_get (to_actor_ref (actor)).submit ();
                _bound_actors[actor.actor_id] = actor.node_rid;
            }
            stream.reply_packet (zlink::message_t::from_json (reply)).submit ();
            co_return;
        }
        if (packet == yd::ensure_spot_req_t::packet_name) {
            auto request = payload.parse_json<yd::ensure_spot_req_t> ();
            if (request.spot_id.empty ()) {
                throw zlink::framework::framework_exception_t (
                  zlink::framework::framework_error_kind_t::protocol_error,
                  "EnsureSpotReq spot_id is missing at session relay");
            }
            auto reply = co_await request_control<yd::ensure_spot_res_t> (
              request, packet, target_or_default (dispatch));
            stream.reply_packet (zlink::message_t::from_json (reply)).submit ();
            co_return;
        }
        if (packet == yd::await_evidence_req_t::packet_name) {
            auto reply = co_await request_control<yd::await_evidence_res_t> (
              payload.parse_json<yd::await_evidence_req_t> (), packet, target_or_default (dispatch));
            stream.reply_packet (zlink::message_t::from_json (reply)).submit ();
            co_return;
        }
        if (packet == yd::await_evidence_wait_req_t::packet_name) {
            auto reply = co_await request_control<yd::await_evidence_res_t> (
              payload.parse_json<yd::await_evidence_wait_req_t> (), packet,
              target_or_default (dispatch));
            stream.reply_packet (zlink::message_t::from_json (reply)).submit ();
            co_return;
        }
        if (packet == yd::await_shutdown_scenario_req_t::packet_name) {
            auto reply =
              co_await run_shutdown_through_spot_route (
                payload.parse_json<yd::await_shutdown_scenario_req_t> ());
            stream.reply_packet (zlink::message_t::from_json (reply)).submit ();
            co_return;
        }
        if (packet == yd::await_shutdown_recovery_req_t::packet_name) {
            auto reply =
              co_await run_shutdown_recovery_through_spot_route (
                payload.parse_json<yd::await_shutdown_recovery_req_t> ());
            stream.reply_packet (zlink::message_t::from_json (reply)).submit ();
            co_return;
        }
        if (packet == yd::hold_req_t::packet_name) {
            auto reply = co_await request_spot<yd::automatic_turn_dispatch_res_t> (
              target_or_default (dispatch), spot_id (dispatch),
              payload.parse_json<yd::hold_req_t> (), packet);
            stream.reply_packet (zlink::message_t::from_json (reply)).submit ();
            co_return;
        }
        if (packet == yd::await_req_t::packet_name) {
            auto reply = co_await request_spot<yd::automatic_turn_dispatch_res_t> (
              target_or_default (dispatch), spot_id (dispatch),
              payload.parse_json<yd::await_req_t> (), packet);
            stream.reply_packet (zlink::message_t::from_json (reply)).submit ();
            co_return;
        }
        if (packet == yd::worker_await_req_t::packet_name) {
            auto reply = co_await request_spot<yd::automatic_turn_dispatch_res_t> (
              target_or_default (dispatch), spot_id (dispatch),
              payload.parse_json<yd::worker_await_req_t> (), packet);
            stream.reply_packet (zlink::message_t::from_json (reply)).submit ();
            co_return;
        }
        if (packet == yd::await_timeout_req_t::packet_name) {
            auto reply = co_await request_spot<yd::await_timeout_res_t> (
              target_or_default (dispatch), spot_id (dispatch),
              payload.parse_json<yd::await_timeout_req_t> (), packet);
            stream.reply_packet (zlink::message_t::from_json (reply)).submit ();
            co_return;
        }
        if (packet == yd::remote_spot_await_req_t::packet_name) {
            auto reply = co_await request_spot<yd::automatic_turn_dispatch_res_t> (
              target_or_default (dispatch), spot_id (dispatch),
              payload.parse_json<yd::remote_spot_await_req_t> (), packet);
            stream.reply_packet (zlink::message_t::from_json (reply)).submit ();
            co_return;
        }
        if (packet == yd::probe_req_t::packet_name) {
            auto reply = co_await request_spot<yd::automatic_turn_dispatch_res_t> (
              target_or_default (dispatch), spot_id (dispatch),
              payload.parse_json<yd::probe_req_t> (), packet);
            stream.reply_packet (zlink::message_t::from_json (reply)).submit ();
            co_return;
        }
        if (packet == yd::hold_msg_t::packet_name) {
            co_await send_spot (target_or_default (dispatch), spot_id (dispatch),
                                payload.parse_json<yd::hold_msg_t> (), packet);
            co_return;
        }
        if (packet == yd::await_msg_t::packet_name) {
            co_await send_spot (target_or_default (dispatch), spot_id (dispatch),
                                payload.parse_json<yd::await_msg_t> (), packet);
            co_return;
        }
        if (packet == yd::worker_await_msg_t::packet_name) {
            co_await send_spot (target_or_default (dispatch), spot_id (dispatch),
                                payload.parse_json<yd::worker_await_msg_t> (), packet);
            co_return;
        }
        if (packet == yd::http_await_msg_t::packet_name) {
            co_await send_spot (target_or_default (dispatch), spot_id (dispatch),
                                payload.parse_json<yd::http_await_msg_t> (), packet);
            co_return;
        }
        if (packet == yd::io_worker_await_msg_t::packet_name) {
            co_await send_spot (target_or_default (dispatch), spot_id (dispatch),
                                payload.parse_json<yd::io_worker_await_msg_t> (), packet);
            co_return;
        }
        if (packet == yd::await_timeout_msg_t::packet_name) {
            co_await send_spot (target_or_default (dispatch), spot_id (dispatch),
                                payload.parse_json<yd::await_timeout_msg_t> (), packet);
            co_return;
        }
        if (packet == yd::timer_start_msg_t::packet_name) {
            co_await send_spot (target_or_default (dispatch), spot_id (dispatch),
                                payload.parse_json<yd::timer_start_msg_t> (), packet);
            co_return;
        }
        if (packet == yd::timer_stop_msg_t::packet_name) {
            co_await send_spot (target_or_default (dispatch), spot_id (dispatch),
                                payload.parse_json<yd::timer_stop_msg_t> (), packet);
            co_return;
        }
        if (packet == yd::probe_msg_t::packet_name) {
            co_await send_spot (target_or_default (dispatch), spot_id (dispatch),
                                payload.parse_json<yd::probe_msg_t> (), packet);
            co_return;
        }
        auto actor = require_bound_actor (dispatch, packet);
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              actor.error_kind (), actor.error () ? actor.error ()->what ()
                                                  : "bound actor route is not found");
        }
        if (dispatch.can_reply) {
            auto actor_value = actor.value ();
            auto stream_copy = stream;
            auto payload_copy = payload;
            std::thread (
              [actor_value = std::move (actor_value), stream_copy = std::move (stream_copy),
               packet, payload_copy = std::move (payload_copy)] () mutable {
                  auto task = actor_value.relay_request (packet, payload_copy).submit ();
                  const auto &reply = task.result ();
                  if (reply) {
                      stream_copy.reply_packet (reply.value ()).submit ();
                  } else {
                      (void) stream_copy.close ().result ();
                  }
              })
              .detach ();
            co_return;
        }
        co_await actor.value ().relay (payload);
        co_return;
    }

  private:
    static zlink::framework::actor_ref_t to_actor_ref (const yd::await_actor_binding_t &actor)
    {
        return zlink::framework::actor_ref_t (
          zlink::framework::actor_id_t (actor.actor_id), actor.generation,
          yd::spot_channel,
          zlink::framework::node_rid_t::from_string (actor.node_rid));
    }

    zlink::framework::result_t<zlink::framework::session_actor_t>
    require_bound_actor (const zlink::framework::session_message_context_t &dispatch,
                         const std::string &packet) const
    {
        if (_bound_actors.empty ()) {
            return zlink::framework::result_t<zlink::framework::session_actor_t>::failure (
              zlink::framework::framework_error_kind_t::not_configured,
              "stream session is not bound before " + packet);
        }
        std::string actor_id;
        if (auto selected = dispatch.metadata.find (yd::actor_id_metadata)) {
            actor_id = std::string (*selected);
        } else if (_bound_actors.size () == 1) {
            actor_id = _bound_actors.begin ()->first;
        } else {
            return zlink::framework::result_t<zlink::framework::session_actor_t>::failure (
              zlink::framework::framework_error_kind_t::not_found,
              "actor-id metadata is required when multiple actors are bound for " + packet);
        }
        if (!_bound_actors.contains (actor_id)) {
            return zlink::framework::result_t<zlink::framework::session_actor_t>::failure (
              zlink::framework::framework_error_kind_t::not_found,
              "bound actor route is not found for " + actor_id + " / " + packet);
        }
        auto actor = _actors.find (actor_id);
        if (!actor) {
            return zlink::framework::result_t<zlink::framework::session_actor_t>::failure (
              zlink::framework::framework_error_kind_t::not_found,
              "bound actor route is not found for " + packet);
        }
        return zlink::framework::result_t<zlink::framework::session_actor_t>::success (
          std::move (*actor));
    }

    template <typename TReply, typename TRequest>
    zlink::framework::task_t<TReply>
    request_control (const TRequest &request, const std::string &packet, zlink::routing_id_t target)
    {
        const auto reply =
            /* control 요청 중에는 서버측에서 최대 3초까지 증거 marker를 기다리는 것(evidence
             * wait)이 있다. 전달 timeout을 그 대기와 같은 3초로 두면 정상적이지만 조금 느린
             * 응답에도 세션이 timeout 예외로 끝나 client stream이 끊긴다. 내부 대기보다
             * 넉넉한 값을 쓴다. */
            co_await _routes.request_to_node (yd::control_channel, target, request)
            .timeout (std::chrono::milliseconds (15000))
            .template submit<TReply> ();
        co_return reply;
    }

    zlink::framework::task_t<yd::await_scenario_res_t>
    run_shutdown_through_spot_route (const yd::await_shutdown_scenario_req_t &request)
    {
        const auto target = zlink::routing_id_t::from ("play-a");
        (void) co_await request_control<yd::ensure_spot_res_t> (
          yd::ensure_spot_req_t{.spot_id = request.spot_id},
          yd::ensure_spot_req_t::packet_name, target);
        (void) co_await request_spot<yd::automatic_turn_dispatch_res_t> (
          target, request.spot_id,
          yd::await_req_t{.request_id = request.request_id,
                          .delay_ms = request.delay_ms,
                          .correlation_id = "shutdown"},
          yd::await_req_t::packet_name, std::chrono::milliseconds (2000));
        auto evidence = co_await request_control<yd::await_evidence_res_t> (
          yd::await_evidence_req_t{.request_id = request.request_id},
          yd::await_evidence_req_t::packet_name, target);
        co_return yd::await_scenario_res_t{
          .operation = "await.e3-shutdown-unexpected-completion",
          .spot_id = request.spot_id,
          .evidence = std::move (evidence.evidence)};
    }

    zlink::framework::task_t<yd::await_scenario_res_t>
    run_shutdown_recovery_through_spot_route (
      const yd::await_shutdown_recovery_req_t &request)
    {
        const auto target = zlink::routing_id_t::from ("play-a");
        (void) co_await request_control<yd::ensure_spot_res_t> (
          yd::ensure_spot_req_t{.spot_id = request.spot_id},
          yd::ensure_spot_req_t::packet_name, target);
        (void) co_await request_spot<yd::automatic_turn_dispatch_res_t> (
          target, request.spot_id,
          yd::probe_req_t{.request_id = request.request_id,
                          .marker = "shutdown-recovery-probe"},
          yd::probe_req_t::packet_name);
        auto evidence = co_await request_control<yd::await_evidence_res_t> (
          yd::await_evidence_wait_req_t{.request_id = request.request_id,
                                        .marker = "probe-completed",
                                        .timeout_milliseconds = 3000},
          yd::await_evidence_wait_req_t::packet_name, target);
        co_return yd::await_scenario_res_t{
          .operation = "await.e3-shutdown-recovery",
          .spot_id = request.spot_id,
          .evidence = std::move (evidence.evidence)};
    }

    /* Resolves the opaque messaging handle for each bounded fixture attempt.
     * The handle itself owns the safe refresh rule of the spot-address
     * messaging contract; the retry here only covers startup races before
     * the location row exists. */
    zlink::framework::spot_handle_t resolve_spot (const std::string &spot_id)
    {
        auto handle =
          _spots.resolve_spot_handle ((spot_id))
            .result ()
            .value ();
        if (!handle) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::not_found,
              "Spot '" + spot_id + "' has no live location row.");
        }
        return *handle;
    }

    template <typename TRequest>
    zlink::framework::task_t<void>
    send_spot (zlink::routing_id_t target,
               const std::string &spot_id,
               const TRequest &request,
               const std::string &packet)
    {
        (void) target;
        (void) packet;
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (20);
        std::exception_ptr last;
        while (std::chrono::steady_clock::now () < deadline) {
            try {
                _routes.send_to_spot (resolve_spot (spot_id), request).submit ();
                co_return;
            }
            catch (const zlink::framework::framework_exception_t &) {
                last = std::current_exception ();
                std::this_thread::sleep_for (std::chrono::milliseconds (100));
            }
        }
        std::rethrow_exception (last);
    }

    template <typename TReply, typename TRequest>
    zlink::framework::task_t<TReply>
    request_spot (zlink::routing_id_t target,
                  const std::string &spot_id,
                  const TRequest &request,
                  const std::string &packet,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds (3000))
    {
        (void) target;
        (void) packet;
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (20);
        std::exception_ptr last;
        while (std::chrono::steady_clock::now () < deadline) {
            try {
                auto reply = co_await _routes.request_to_spot (resolve_spot (spot_id), request)
                               .timeout (timeout)
                               .template submit<TReply> ();
                co_return reply;
            }
            catch (const zlink::framework::framework_exception_t &error) {
                if (zlink::framework::detail::boundary_state (error)
                    == zlink::framework::detail::boundary_error_t::timed_out) {
                    throw;
                }
                last = std::current_exception ();
                std::this_thread::sleep_for (std::chrono::milliseconds (100));
            }
        }
        std::rethrow_exception (last);
    }

    static zlink::routing_id_t
    target_or_default (const zlink::framework::session_message_context_t &dispatch)
    {
        if (auto target = dispatch.metadata.find (yd::target_node_rid_metadata)) {
            return zlink::routing_id_t::from (std::string (*target));
        }
        return zlink::routing_id_t::from ("play-a");
    }

    static std::string spot_id (const zlink::framework::session_message_context_t &dispatch)
    {
        if (auto value = dispatch.metadata.find (yd::spot_id_metadata)) {
            return std::string (*value);
        }
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error,
          "spot-rid metadata is required");
    }

    zlink::framework::route_client_t &_routes;
    zlink::framework::session_actor_manager_t &_actors;
    zlink::framework::spot_handle_resolver_t &_spots;
    std::map<std::string, std::string> _bound_actors;
};

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::session
