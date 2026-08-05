/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/play_support.hpp"
#include "../../../Shared/automatic_turn_dispatch_contracts.hpp"

#include <zlink/framework.hpp>
#include <zlink/http_client.hpp>

#include <chrono>
#include <string>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
{

namespace yd = zlink::framework::e2e::automatic_turn_dispatch;

struct external_api_http_client_tag_t
{
};

using external_api_http_client_t =
  zlink::http_client::named_server_client_t<external_api_http_client_tag_t>;

inline zlink::framework::task_t<void>
handle_http_await (zlink::framework::spot_context_t &context,
                   evidence_store_t &evidence,
                   external_api_http_client_t &client,
                   const yd::http_await_msg_t &request)
{
    const auto spot_id = context.spot_id ();
    const auto prefix = "http-" + request.terminator;
    evidence.add (prefix + "-started|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request.request_id);
    auto call = client.get ("/delay")
                  .query ("requestId", request.request_id)
                  .query ("marker", request.terminator)
                  .query ("delayMs", std::to_string (request.delay_ms))
                  .timeout (std::chrono::seconds (5));
    const auto wait_marker = request.terminator == "yield" ? "http-yield-released"
                                                            : "http-async-held";
    evidence.add (std::string (wait_marker) + "|rid=" + evidence.node_rid + "|spot="
                  + spot_id + "|request=" + request.request_id);
    const auto response = request.terminator == "yield"
                            ? co_await call.yield<yd::external_delay_res_t> ()
                            : co_await call.submit<yd::external_delay_res_t> ();
    evidence.add (prefix + "-resumed|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request.request_id + "|marker=" + response.body.marker);
    evidence.add (prefix + "-completed|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request.request_id);
}

inline zlink::framework::task_t<void>
handle_io_worker_await (zlink::framework::spot_context_t &context,
                        evidence_store_t &evidence,
                        external_api_http_client_t &client,
                        const yd::io_worker_await_msg_t &request)
{
    const auto spot_id = context.spot_id ();
    evidence.add ("io-worker-started|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request.request_id + "|operation="
                  + request.operation_id);
    auto call = context.run_io_worker ([&client, request] {
        return client.get ("/delay")
          .query ("requestId", request.request_id)
          .query ("marker", request.operation_id)
          .query ("delayMs", std::to_string (request.delay_ms))
          .timeout (std::chrono::seconds (5))
          .submit<yd::external_delay_res_t> ();
    });
    evidence.add ("io-worker-released|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request.request_id + "|operation="
                  + request.operation_id);
    const auto response = co_await call.yield ();
    evidence.add ("io-worker-completed|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request.request_id + "|operation="
                  + request.operation_id + "|marker=" + response.body.marker);
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
