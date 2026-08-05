/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/delay_support.hpp"
#include "../../../Shared/automatic_turn_dispatch_contracts.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <thread>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::delay {

namespace yd = zlink::framework::e2e::automatic_turn_dispatch;

class delay_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<delay_state_t>;
    using request_type = yd::delay_req_t;
    using reply_type = yd::delay_res_t;

    explicit delay_handler_t (delay_state_t &state) : _state (state) {}

    yd::delay_res_t handle (const yd::delay_req_t &request)
    {
        _state.evidence.add ("delay-started|rid=" + _state.node_rid
                             + "|request=" + request.request_id
                             + "|marker=" + request.marker);
        std::this_thread::sleep_for (std::chrono::milliseconds (request.delay_ms));
        _state.evidence.add ("delay-completed|rid=" + _state.node_rid
                             + "|request=" + request.request_id
                             + "|marker=" + request.marker);
        return {.request_id = request.request_id,
                .marker = request.marker,
                .node_rid = _state.node_rid};
    }

  private:
    delay_state_t &_state;
};

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::delay
