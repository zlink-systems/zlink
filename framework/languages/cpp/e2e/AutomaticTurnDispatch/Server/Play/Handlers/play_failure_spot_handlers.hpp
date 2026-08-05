/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/play_support.hpp"
#include "../../../Shared/automatic_turn_dispatch_contracts.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <string>
#include <thread>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
{

namespace yd = zlink::framework::e2e::automatic_turn_dispatch;

inline std::string timeout_error_name (const zlink::framework::framework_exception_t &error)
{
    if (zlink::framework::detail::boundary_state (error) == zlink::framework::detail::boundary_error_t::timed_out) {
        return "Timeout";
    }
    return error.what ();
}

inline std::string cancellation_error_name (const zlink::framework::framework_exception_t &error)
{
    if (zlink::framework::detail::boundary_state (error) == zlink::framework::detail::boundary_error_t::cancelled) {
        return "Cancelled";
    }
    return error.what ();
}

inline yd::await_timeout_res_t
timeout_reply (const zlink::framework::spot_context_t &context,
               const evidence_store_t &evidence,
               const yd::await_timeout_req_t &request,
               bool timed_out,
               std::string error)
{
    return {.scenario_id = "ATD-E1",
            .request_id = request.request_id,
            .spot_id = context.spot_id (),
            .node_rid = evidence.node_rid,
            .timed_out = timed_out,
            .error = std::move (error)};
}

inline zlink::framework::task_t<yd::await_timeout_res_t>
handle_await_timeout (zlink::framework::spot_context_t &context,
                      evidence_store_t &evidence,
                      const yd::await_timeout_req_t &request)
{
    const auto spot_id = context.spot_id ();
    evidence.add ("timeout-await-started|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request.request_id + "|handler=spot");
    try {
        auto call =
          context.outbound ()
            .request (yd::delay_channel,
                      yd::delay_req_t{.request_id = request.request_id,
                                      .delay_ms = request.delay_ms,
                                      .marker = "timeout"})
            .timeout (std::chrono::milliseconds (request.timeout_ms));
        evidence.add ("timeout-await-released|rid=" + evidence.node_rid + "|spot=" + spot_id
                      + "|request=" + request.request_id + "|handler=spot");
        co_await call.submit<yd::delay_res_t> ();
        evidence.add ("timeout-await-unexpected-resumed|rid=" + evidence.node_rid
                      + "|spot=" + spot_id + "|request=" + request.request_id
                      + "|handler=spot");
        co_return timeout_reply (context, evidence, request, false, "");
    }
    catch (const zlink::framework::framework_exception_t &error) {
        evidence.add ("timeout-await-completed|rid=" + evidence.node_rid + "|spot=" + spot_id
                      + "|request=" + request.request_id + "|error="
                      + timeout_error_name (error) + "|handler=spot");
        co_return timeout_reply (context, evidence, request, true,
                                 timeout_error_name (error));
    }
}

inline zlink::framework::task_t<void>
handle_await_timeout_command (zlink::framework::spot_context_t &context,
                              evidence_store_t &evidence,
                              const yd::await_timeout_msg_t &request)
{
    const yd::await_timeout_req_t as_request{.request_id = request.request_id,
                                             .delay_ms = request.delay_ms,
                                             .timeout_ms = request.timeout_ms};
    (void) co_await handle_await_timeout (context, evidence, as_request);
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
