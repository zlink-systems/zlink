/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "play_spot_types.hpp"
#include "../Handlers/play_basic_spot_handlers.hpp"
#include "../Handlers/play_execution_turn_handlers.hpp"
#include "../Handlers/play_failure_spot_handlers.hpp"
#include "../Handlers/play_remote_spot_handlers.hpp"
#include "../Handlers/play_timer_spot_handlers.hpp"
#include "../Support/play_support.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
{

namespace yd = zlink::framework::e2e::automatic_turn_dispatch;

class await_probe_spot_t
    : public zlink::framework::spot_t<await_actor_t>
{
  public:
    await_probe_spot_t (zlink::framework::spot_context_t context,
                        evidence_store_t &evidence,
                        external_api_http_client_t external_api) :
        _evidence (evidence), _context (std::move (context)),
        _external_api (std::move (external_api))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override
    {
        return _context;
    }

    const zlink::framework::spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ()
          .add_handler<&await_probe_spot_t::hold_req> (yd::hold_req_t::packet_name)
          .add_handler<&await_probe_spot_t::hold_command> (yd::hold_msg_t::packet_name)
          .add_handler<&await_probe_spot_t::await_req> (yd::await_req_t::packet_name)
          .add_handler<&await_probe_spot_t::await_command> (yd::await_msg_t::packet_name)
          .add_handler<&await_probe_spot_t::worker_await_req> (
            yd::worker_await_req_t::packet_name)
          .add_handler<&await_probe_spot_t::worker_await_command> (
            yd::worker_await_msg_t::packet_name)
          .add_handler<&await_probe_spot_t::http_await_command> (
            yd::http_await_msg_t::packet_name)
          .add_handler<&await_probe_spot_t::io_worker_await_command> (
            yd::io_worker_await_msg_t::packet_name)
          .add_handler<&await_probe_spot_t::await_timeout_req> (
            yd::await_timeout_req_t::packet_name)
          .add_handler<&await_probe_spot_t::await_timeout_command> (
            yd::await_timeout_msg_t::packet_name)
          .add_handler<&await_probe_spot_t::remote_spot_await_req> (
            yd::remote_spot_await_req_t::packet_name)
          .add_handler<&await_probe_spot_t::timer_start_command> (
            yd::timer_start_msg_t::packet_name)
          .add_handler<&await_probe_spot_t::timer_stop_command> (
            yd::timer_stop_msg_t::packet_name)
          .add_handler<&await_probe_spot_t::probe_req> (yd::probe_req_t::packet_name)
          .add_handler<&await_probe_spot_t::probe_command> (yd::probe_msg_t::packet_name)
          .add_actor_request<&await_probe_spot_t::actor_await_req> (
            yd::actor_await_req_t::packet_name)
          .add_actor_request<&await_probe_spot_t::actor_fast_req> (
            yd::actor_fast_req_t::packet_name)
          .add_actor_request<&await_probe_spot_t::actor_join_await_req> (
            yd::actor_join_await_req_t::packet_name)
          .add_actor_request<&await_probe_spot_t::actor_join_spot_req> (
            yd::actor_join_spot_req_t::packet_name)
          .add_actor_request<&await_probe_spot_t::actor_push_await_req> (
            yd::actor_push_await_req_t::packet_name);
    }

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view actor_id,
                   const zlink::framework::message_t &request_message) override
    {
        const auto request = request_message.decode<yd::delay_req_t> ();
        const auto spot_id = _context.spot_id ();
        _evidence.add ("actor-join-target-started|rid=" + _evidence.node_rid + "|spot="
                       + spot_id + "|actor=" + std::string (actor_id) + "|request="
                       + request.request_id + "|turn=" + current_turn_id () + "|handler=spot");
        std::this_thread::sleep_for (std::chrono::milliseconds (request.delay_ms));
        _evidence.add ("actor-join-target-completed|rid=" + _evidence.node_rid + "|spot="
                       + spot_id + "|actor=" + std::string (actor_id) + "|request="
                       + request.request_id + "|turn=" + current_turn_id () + "|handler=spot");
        co_return zlink::framework::spot_actor_join_result_t::accept (
          yd::delay_res_t{.request_id = request.request_id,
                          .marker = request.marker,
                          .node_rid = _evidence.node_rid});
    }

    zlink::framework::task_t<void> on_leave_actor (await_actor_t &actor) override
    {
        if (!actor.join_request_id.empty ()) {
            _evidence.add ("actor-join-source-left|rid=" + _evidence.node_rid + "|spot="
                           + _context.spot_id () + "|actor="
                           + actor.actor_id + "|request=" + actor.join_request_id + "|turn="
                           + current_turn_id ());
        }
        co_return;
    }

    zlink::framework::task_t<void> on_actor_joined (await_actor_t &actor) override
    {
        if (!actor.join_request_id.empty ()) {
            _evidence.add ("actor-join-target-joined|rid=" + _evidence.node_rid + "|spot="
                           + _context.spot_id () + "|actor="
                           + actor.actor_id + "|request=" + actor.join_request_id + "|turn="
                           + current_turn_id ());
        }
        co_return;
    }

    zlink::framework::task_t<yd::automatic_turn_dispatch_res_t> hold_req (const yd::hold_req_t &request)
    {
        co_await handle_basic_hold (_context, _evidence, request.request_id, request.delay_ms);
        co_return basic_spot_reply (_context, _evidence, "ATD-A1", request.request_id,
                                    "hold-completed");
    }

    zlink::framework::task_t<void> hold_command (const yd::hold_msg_t &request)
    {
        co_await handle_basic_hold (_context, _evidence, request.request_id, request.delay_ms);
    }

    zlink::framework::task_t<yd::automatic_turn_dispatch_res_t> await_req (const yd::await_req_t &request)
    {
        co_await handle_basic_yield (_context, _evidence, request.request_id, request.delay_ms,
                                     request.correlation_id);
        co_return basic_spot_reply (_context, _evidence, "ATD-A2", request.request_id,
                                    "await-completed");
    }

    zlink::framework::task_t<void> await_command (const yd::await_msg_t &request)
    {
        co_await handle_basic_yield (_context, _evidence, request.request_id, request.delay_ms,
                                     request.correlation_id);
    }

    zlink::framework::task_t<yd::automatic_turn_dispatch_res_t>
    worker_await_req (const yd::worker_await_req_t &request)
    {
        co_await handle_basic_worker_yield (_context, _evidence, request.request_id,
                                            request.delay_ms);
        co_return basic_spot_reply (_context, _evidence, "ATD-A4", request.request_id,
                                    "worker-await-completed");
    }

    zlink::framework::task_t<void> worker_await_command (
      const yd::worker_await_msg_t &request)
    {
        co_await handle_basic_worker_yield (_context, _evidence, request.request_id,
                                            request.delay_ms);
    }

    zlink::framework::task_t<void> http_await_command (const yd::http_await_msg_t &request)
    {
        co_await handle_http_await (_context, _evidence, _external_api, request);
    }

    zlink::framework::task_t<void>
    io_worker_await_command (const yd::io_worker_await_msg_t &request)
    {
        co_await handle_io_worker_await (_context, _evidence, _external_api, request);
    }

    zlink::framework::task_t<yd::await_timeout_res_t>
    await_timeout_req (const yd::await_timeout_req_t &request)
    {
        co_return co_await handle_await_timeout (_context, _evidence, request);
    }

    zlink::framework::task_t<void> await_timeout_command (
      const yd::await_timeout_msg_t &request)
    {
        co_await handle_await_timeout_command (_context, _evidence, request);
    }


    zlink::framework::task_t<yd::automatic_turn_dispatch_res_t>
    remote_spot_await_req (const yd::remote_spot_await_req_t &request)
    {
        co_return co_await handle_remote_spot_yield (_context, _evidence, request);
    }

    void timer_start_command (const yd::timer_start_msg_t &request)
    {
        handle_timer_start_command (_context, _evidence, _timers, _timer_mutex, request);
    }

    void timer_stop_command (const yd::timer_stop_msg_t &request)
    {
        handle_timer_stop_command (_timers, _timer_mutex, request);
    }

    yd::automatic_turn_dispatch_res_t probe_req (const yd::probe_req_t &request)
    {
        handle_basic_probe (_context, _evidence, request.request_id, request.marker);
        return basic_spot_reply (_context, _evidence, "ATD-PROBE", request.request_id,
                                 request.marker);
    }

    void probe_command (const yd::probe_msg_t &request)
    {
        handle_basic_probe (_context, _evidence, request.request_id, request.marker);
    }

    zlink::framework::task_t<yd::actor_await_res_t>
    actor_await_req (await_actor_t &actor,
                     zlink::framework::spot_actor_request_context_t &,
                     const yd::actor_await_req_t &request)
    {
        const auto spot_id = _context.spot_id ();
        const auto mailbox = "actor:" + actor.actor_id;
        _evidence.add ("actor-await-started|rid=" + _evidence.node_rid + "|spot=" + spot_id
                       + "|actor=" + actor.actor_id + "|mailbox=" + mailbox + "|request="
                       + request.request_id + "|handler=actor");
        auto call =
          _context.outbound ()
            .request (yd::delay_channel,
                      yd::delay_req_t{.request_id = request.request_id,
                                      .delay_ms = request.delay_ms,
                                      .marker = "actor-" + actor.actor_id})
            .timeout (std::chrono::milliseconds (3000));
        _evidence.add ("actor-await-released|rid=" + _evidence.node_rid + "|spot=" + spot_id
                       + "|actor=" + actor.actor_id + "|mailbox=" + mailbox + "|request="
                       + request.request_id + "|handler=actor");
        co_await call.submit<yd::delay_res_t> ();
        _evidence.add ("actor-await-resumed|rid=" + _evidence.node_rid + "|spot=" + spot_id
                       + "|actor=" + actor.actor_id + "|mailbox=" + mailbox + "|request="
                       + request.request_id + "|handler=actor");
        _evidence.add ("actor-await-completed|rid=" + _evidence.node_rid + "|spot=" + spot_id
                       + "|actor=" + actor.actor_id + "|mailbox=" + mailbox + "|request="
                       + request.request_id + "|handler=actor");
        co_return actor_reply ("ATD-B", request.request_id, actor.actor_id,
                               "actor-await-completed");
    }

    yd::actor_await_res_t actor_fast_req (await_actor_t &actor,
                                          zlink::framework::spot_actor_request_context_t &,
                                          const yd::actor_fast_req_t &request)
    {
        const auto spot_id = _context.spot_id ();
        const auto mailbox = "actor:" + actor.actor_id;
        _evidence.add ("actor-fast-started|rid=" + _evidence.node_rid + "|spot=" + spot_id
                       + "|actor=" + actor.actor_id + "|mailbox=" + mailbox + "|request="
                       + request.request_id + "|marker=" + request.marker + "|handler=actor");
        _evidence.add ("actor-fast-completed|rid=" + _evidence.node_rid + "|spot=" + spot_id
                       + "|actor=" + actor.actor_id + "|mailbox=" + mailbox + "|request="
                       + request.request_id + "|marker=" + request.marker + "|handler=actor");
        return actor_reply ("ATD-B", request.request_id, actor.actor_id, request.marker);
    }

    zlink::framework::task_t<yd::actor_await_res_t>
    actor_join_await_req (await_actor_t &actor,
                          zlink::framework::spot_actor_request_context_t &,
                          const yd::actor_join_await_req_t &request)
    {
        const auto spot_id = _context.spot_id ();
        const auto mailbox = "actor:" + actor.actor_id;
        _evidence.add ("actor-join-await-started|rid=" + _evidence.node_rid + "|spot="
                       + spot_id + "|actor=" + actor.actor_id + "|mailbox=" + mailbox
                       + "|request=" + request.request_id + "|target_node="
                       + request.target_node_rid + "|handler=actor");
        auto call = actor.context ()
                      .join_entry_spot (zlink::framework::node_rid_t::from_string (
                                          request.target_node_rid),
                                        yd::delay_req_t{.request_id = request.request_id,
                                                        .delay_ms = 350,
                                                        .marker = "join"})
                      .timeout (std::chrono::milliseconds (3000));
        _evidence.add ("actor-join-await-released|rid=" + _evidence.node_rid + "|spot="
                       + spot_id + "|actor=" + actor.actor_id + "|mailbox=" + mailbox
                       + "|request=" + request.request_id + "|target_node="
                       + request.target_node_rid + "|handler=actor");
        const auto joined = co_await call.async<yd::delay_res_t> ();
        const auto accepted =
          std::holds_alternative<zlink::framework::actor_join_accepted_t<yd::delay_res_t>> (
            joined)
            ? "true"
            : "false";
        _evidence.add ("actor-join-await-resumed|rid=" + _evidence.node_rid + "|spot="
                       + spot_id + "|actor=" + actor.actor_id + "|mailbox=" + mailbox
                       + "|request=" + request.request_id + "|target_node="
                       + request.target_node_rid + "|accepted=" + accepted
                       + "|handler=actor");
        _evidence.add ("actor-join-await-completed|rid=" + _evidence.node_rid + "|spot="
                       + spot_id + "|actor=" + actor.actor_id + "|mailbox=" + mailbox
                       + "|request=" + request.request_id + "|target_node="
                       + request.target_node_rid + "|accepted=" + accepted
                       + "|handler=actor");
        co_return actor_reply ("ATD-B3", request.request_id, actor.actor_id,
                               "actor-join-await-completed");
    }

    zlink::framework::task_t<yd::actor_await_res_t>
    actor_join_spot_req (await_actor_t &actor,
                         zlink::framework::spot_actor_request_context_t &,
                         const yd::actor_join_spot_req_t &request)
    {
        actor.join_request_id = request.request_id;
        _evidence.add ("actor-join-started|rid=" + _evidence.node_rid + "|spot="
                       + _context.spot_id () + "|actor="
                       + actor.actor_id + "|request=" + request.request_id + "|target="
                       + request.target_spot_id + "|turn=" + current_turn_id ());
        const auto joined =
          co_await actor.context ()
            .join_spot ((request.target_spot_id),
                        yd::delay_req_t{.request_id = request.request_id,
                                        .delay_ms = request.admission_delay_ms,
                                        .marker = "join"})
            .async<yd::delay_res_t> ();
        const auto accepted =
          std::holds_alternative<
            zlink::framework::actor_join_accepted_t<yd::delay_res_t>> (joined);
        _evidence.add ("actor-join-completed|rid=" + _evidence.node_rid + "|spot="
                       + request.target_spot_id + "|actor=" + actor.actor_id + "|request="
                       + request.request_id + "|accepted=" + (accepted ? "true" : "false")
                       + "|turn=" + current_turn_id ());
        actor.join_request_id.clear ();
        co_return actor_reply ("TD-E2", request.request_id, actor.actor_id,
                               "actor-join-completed");
    }

    zlink::framework::task_t<yd::actor_await_res_t>
    actor_push_await_req (await_actor_t &actor,
                          zlink::framework::spot_actor_request_context_t &,
                          const yd::actor_push_await_req_t &request)
    {
        const auto spot_id = _context.spot_id ();
        const auto mailbox = "actor:" + actor.actor_id;
        _evidence.add ("actor-push-await-started|rid=" + _evidence.node_rid + "|spot="
                       + spot_id + "|actor=" + actor.actor_id + "|mailbox=" + mailbox
                       + "|request=" + request.request_id + "|handler=actor");
        auto call =
          _context.outbound ()
            .request (yd::delay_channel,
                      yd::delay_req_t{.request_id = request.request_id,
                                      .delay_ms = request.delay_ms,
                                      .marker = "actor-push-" + actor.actor_id})
            .timeout (std::chrono::milliseconds (3000));
        _evidence.add ("actor-push-await-released|rid=" + _evidence.node_rid + "|spot="
                       + spot_id + "|actor=" + actor.actor_id + "|mailbox=" + mailbox
                       + "|request=" + request.request_id + "|handler=actor");
        co_await call.submit<yd::delay_res_t> ();
        _evidence.add ("actor-push-await-resumed|rid=" + _evidence.node_rid + "|spot="
                       + spot_id + "|actor=" + actor.actor_id + "|mailbox=" + mailbox
                       + "|request=" + request.request_id + "|handler=actor");
          actor.context ().bound_session ()
            .send (yd::actor_push_notify_t{.actor_id = actor.actor_id,
                                           .request_id = request.request_id,
                                           .value = request.value,
                                           .node_rid = _evidence.node_rid})
            .submit ();
        _evidence.add ("actor-push-await-completed|rid=" + _evidence.node_rid + "|spot="
                       + spot_id + "|actor=" + actor.actor_id + "|mailbox=" + mailbox
                       + "|request=" + request.request_id + "|handler=actor");
        co_return actor_reply ("ATD-D4", request.request_id, actor.actor_id,
                               "actor-push-await-completed");
    }

    zlink::framework::task_t<void> handle_timer_tick (
      const zlink::framework::timer_tick_t &tick)
    {
        co_await play::handle_timer_tick (_context, _evidence, _timers, _timer_mutex, tick);
        co_return;
    }

  private:
    static std::string current_turn_id ()
    {
        const auto turn = zlink::framework::detail::capture_current_serial_turn ();
        return std::to_string (reinterpret_cast<std::uintptr_t> (turn.get ()));
    }

    yd::actor_await_res_t
    actor_reply (std::string scenario_id,
                 std::string request_id,
                 std::string actor_id,
                 std::string marker) const
    {
        return {.scenario_id = std::move (scenario_id),
                .request_id = std::move (request_id),
                .actor_id = std::move (actor_id),
                .spot_id = _context.spot_id (),
                .node_rid = _evidence.node_rid,
                .marker = std::move (marker)};
    }

    evidence_store_t &_evidence;
    zlink::framework::spot_context_t _context;
    external_api_http_client_t _external_api;
    std::mutex _timer_mutex;
    std::map<std::string, await_timer_state_t> _timers;
};

inline zlink::framework::task_t<void>
await_timer_handler_t::handle (await_probe_spot_t &spot,
                               const zlink::framework::timer_tick_t &tick) const
{
    co_await spot.handle_timer_tick (tick);
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
