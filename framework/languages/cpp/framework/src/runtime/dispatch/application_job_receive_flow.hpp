/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/application_job_queue.hpp"

#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Sockets/socket_options.hpp>

namespace zlink::framework::runtime
{

template <typename TSocket>
receive_flow_state_apply_result_t apply_application_job_receive_flow_state (
  TSocket &socket,
  application_job_queue_pressure_state_t state) noexcept
{
    try {
        socket.set_receive_flow_state (
          state == application_job_queue_pressure_state_t::paused
            ? zlink::receive_flow_state_t::paused
            : zlink::receive_flow_state_t::running);
        return receive_flow_state_apply_result_t::applied;
    }
    catch (const zlink::config_error_t &error) {
        if (error.result () == zlink::config_result_t::invalid_state)
            return receive_flow_state_apply_result_t::invalid_state;
        return receive_flow_state_apply_result_t::failed;
    }
    catch (...) {
        return receive_flow_state_apply_result_t::failed;
    }
}

} // namespace zlink::framework::runtime
