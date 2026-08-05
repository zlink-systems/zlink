/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Spots/play_spot_types.hpp"
#include "../Support/play_support.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <utility>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
{

namespace yd = zlink::framework::e2e::automatic_turn_dispatch;

class await_entry_spot_t
    : public zlink::framework::entry_spot_t<await_actor_t>
{
  public:
    await_entry_spot_t (zlink::framework::entry_spot_context_t context,
                        evidence_store_t &evidence) :
        _evidence (evidence), _context (std::move (context))
    {
    }

    zlink::framework::entry_spot_context_t &context () noexcept override
    {
        return _context;
    }

    const zlink::framework::entry_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_actor_request<&await_entry_spot_t::actor_fast_req> (
          yd::actor_fast_req_t::packet_name);
    }

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view actor_id,
                   const zlink::framework::message_t &request_message) override
    {
        const auto request = request_message.decode<yd::delay_req_t> ();
        const auto spot_id = _context.spot_id ();
        _evidence.add ("actor-join-target-started|rid=" + _evidence.node_rid + "|spot="
                       + spot_id + "|actor=" + std::string (actor_id) + "|request="
                       + request.request_id + "|handler=entry");
        std::this_thread::sleep_for (std::chrono::milliseconds (request.delay_ms));
        _evidence.add ("actor-join-target-completed|rid=" + _evidence.node_rid + "|spot="
                       + spot_id + "|actor=" + std::string (actor_id) + "|request="
                       + request.request_id + "|handler=entry");
        co_return zlink::framework::spot_actor_join_result_t::accept (
          yd::delay_res_t{.request_id = request.request_id,
                            .marker = request.marker,
                            .node_rid = _evidence.node_rid});
    }

    zlink::framework::task_t<void> on_actor_joined (await_actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_leave_actor (await_actor_t &) override
    {
        co_return;
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
            .timeout (std::chrono::milliseconds (5000));
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
                      .timeout (std::chrono::milliseconds (5000));
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
            .timeout (std::chrono::milliseconds (5000));
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

  private:
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
    zlink::framework::entry_spot_context_t _context;
};

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
