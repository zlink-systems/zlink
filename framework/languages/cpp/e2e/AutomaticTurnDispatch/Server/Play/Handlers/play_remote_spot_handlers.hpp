/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "play_basic_spot_handlers.hpp"
#include "../Support/play_support.hpp"
#include "../../../Shared/automatic_turn_dispatch_contracts.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <string>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::play {

namespace yd = zlink::framework::e2e::automatic_turn_dispatch;

inline zlink::framework::task_t<yd::automatic_turn_dispatch_res_t>
handle_remote_spot_yield (zlink::framework::spot_context_t &context,
                          evidence_store_t &evidence,
                          const yd::remote_spot_await_req_t &request)
{
    const auto &spot_id = context.spot_id ();
    evidence.add ("remote-await-started|rid=" + evidence.node_rid + "|spot="
                  + spot_id + "|request=" + request.request_id + "|target="
                  + request.target_spot_id + "|handler=spot");
    auto call =
      context
        .request_to<yd::automatic_turn_dispatch_res_t> (
          zlink::routing_id_t::from ("play-b"),
          request.target_spot_id,
          yd::await_req_t{.request_id = request.request_id,
                          .delay_ms = request.delay_ms,
                          .correlation_id = "remote-spot"})
        .timeout (std::chrono::milliseconds (5000));
    evidence.add ("remote-await-released|rid=" + evidence.node_rid + "|spot="
                  + spot_id + "|request=" + request.request_id + "|target="
                  + request.target_spot_id + "|handler=spot");
    auto target_reply = co_await call.submit ();
    evidence.add ("remote-await-resumed|rid=" + evidence.node_rid + "|spot="
                  + spot_id + "|request=" + request.request_id + "|target="
                  + request.target_spot_id + "|targetNode=" + target_reply.node_rid
                  + "|handler=spot");
    evidence.add ("remote-await-completed|rid=" + evidence.node_rid + "|spot="
                  + spot_id + "|request=" + request.request_id + "|target="
                  + request.target_spot_id + "|targetNode=" + target_reply.node_rid
                  + "|handler=spot");
    co_return basic_spot_reply (context, evidence, "ATD-D2", request.request_id,
                                "remote-await-completed");
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
