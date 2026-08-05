/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/play_support.hpp"
#include "../../../Shared/automatic_turn_dispatch_contracts.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <utility>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::play {

namespace yd = zlink::framework::e2e::automatic_turn_dispatch;

inline yd::automatic_turn_dispatch_res_t
basic_spot_reply (const zlink::framework::spot_context_t &context,
                  const evidence_store_t &evidence,
                  std::string scenario_id,
                  std::string request_id,
                  std::string marker)
{
    return {.scenario_id = std::move (scenario_id),
            .request_id = std::move (request_id),
            .spot_id = context.spot_id (),
            .node_rid = evidence.node_rid,
            .marker = std::move (marker)};
}

inline zlink::framework::task_t<void>
handle_basic_hold (zlink::framework::spot_context_t &context,
                   evidence_store_t &evidence,
                   const std::string &request_id,
                   int delay_ms)
{
    const auto spot_id = context.spot_id ();
    evidence.add ("hold-started|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request_id + "|handler=spot");
    co_await context.outbound ()
      .request (yd::delay_channel,
                yd::delay_req_t{.request_id = request_id,
                                .delay_ms = delay_ms,
                                .marker = "hold"})
      .timeout (std::chrono::milliseconds (5000))
      .submit<yd::delay_res_t> ();
    evidence.add ("hold-resumed|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request_id + "|handler=spot");
    evidence.add ("hold-completed|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request_id + "|handler=spot");
    co_return;
}

inline zlink::framework::task_t<void>
handle_basic_yield (zlink::framework::spot_context_t &context,
                    evidence_store_t &evidence,
                    const std::string &request_id,
                    int delay_ms,
                    const std::string &correlation_id)
{
    const auto spot_id = context.spot_id ();
    evidence.add ("await-started|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request_id + "|correlation="
                  + correlation_id + "|handler=spot");
    auto call =
      context.outbound ()
        .request (yd::delay_channel,
                  yd::delay_req_t{.request_id = request_id,
                                  .delay_ms = delay_ms,
                                  .marker = "await"})
        .timeout (std::chrono::milliseconds (5000));
    evidence.add ("await-released|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request_id + "|correlation="
                  + correlation_id + "|handler=spot");
    co_await call.submit<yd::delay_res_t> ();
    evidence.add ("await-resumed|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request_id + "|correlation="
                  + correlation_id + "|handler=spot");
    evidence.add ("await-completed|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request_id + "|correlation="
                  + correlation_id + "|handler=spot");
    co_return;
}

inline zlink::framework::task_t<void>
handle_basic_worker_yield (zlink::framework::spot_context_t &context,
                           evidence_store_t &evidence,
                           const std::string &request_id,
                           int delay_ms)
{
    const auto spot_id = context.spot_id ();
    evidence.add ("worker-await-started|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request_id + "|handler=spot");
    auto call = context.run_cpu_worker ([request_id, delay_ms] {
        std::this_thread::sleep_for (std::chrono::milliseconds (delay_ms));
        return request_id;
    });
    evidence.add ("worker-await-released|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request_id + "|handler=spot");
    co_await call.yield ();
    evidence.add ("worker-await-resumed|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request_id + "|handler=spot");
    evidence.add ("worker-await-completed|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request_id + "|handler=spot");
    co_return;
}

inline void
handle_basic_probe (zlink::framework::spot_context_t &context,
                    evidence_store_t &evidence,
                    const std::string &request_id,
                    const std::string &marker)
{
    const auto spot_id = context.spot_id ();
    evidence.add ("probe-started|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request_id + "|marker=" + marker
                  + "|handler=spot");
    evidence.add ("probe-completed|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request_id + "|marker=" + marker
                  + "|handler=spot");
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
