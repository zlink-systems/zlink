/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/timers/async_delay.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace zlink::framework::detail
{

task_t<runtime::host::bound_session_bind_operation_result_t> apply_bound_session_bind_async (
  std::weak_ptr<actor_gateway_state_t> weak_actor_gateway_runtime,
  std::weak_ptr<mesh_node_runtime_t> weak_application_mesh,
  std::shared_ptr<spot_node_builder_state_t> spot_state,
  stream_runtime_t stream_runtime,
  actor_ref_t actor,
  runtime::protocol::bound_session_bind_t bind,
  zlink::routing_id_t session_owner,
  std::uint64_t session_owner_generation,
  location_owner_token_t session_owner_token,
  std::function<
    bool (std::uint32_t, std::uint32_t, std::optional<runtime::protocol::stream_bind_reply_tail_t>)>
    submit_terminal_reply,
  delay_owner_t &delays,
  std::chrono::milliseconds callback_timeout);

} // namespace zlink::framework::detail
