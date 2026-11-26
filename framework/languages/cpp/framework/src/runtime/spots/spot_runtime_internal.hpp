/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "spot_runtime.hpp"

#include "runtime/diagnostics/message_flow_tracer.hpp"

namespace zlink::framework::detail
{

class bound_session_delivery_fence_t;

struct handoff_terminal_route_t
{
    // Destination: the node that recorded the pending reply token.
    zlink::routing_id_t source_node;
    // Initiating owner identity, used only to select that pending entry.
    zlink::routing_id_t source_owner_node;
    runtime::protocol::wire_operation_id_t operation;
    std::uint64_t reply_route_id = 0;
    runtime::protocol::actor_route_fence_t source_fence;
    zlink::routing_id_t parking_node;
};

class transferred_owner_reservation_guard_t final
{
  public:
    explicit transferred_owner_reservation_guard_t (std::function<void ()> settle = {});

    ~transferred_owner_reservation_guard_t () noexcept;

    transferred_owner_reservation_guard_t (const transferred_owner_reservation_guard_t &) = delete;
    transferred_owner_reservation_guard_t &
    operator= (const transferred_owner_reservation_guard_t &) = delete;

    explicit operator bool () const noexcept;

    const std::function<void ()> &callback () const noexcept;

    std::function<void ()> release () noexcept;

    void dismiss () noexcept;

    void settle () noexcept;

  private:
    std::function<void ()> _settle;
};

class join_completion_delivery_fence_scope_t
{
  public:
    // Spot lifecycle callbacks can enqueue detached bound-Session sends before
    // the Actor completion callback exists. Keep that exact fence alive until
    // the completion path seals it or the owning transfer is abandoned.
    static std::shared_ptr<join_completion_delivery_fence_scope_t>
    begin (const detail::actor_gateway_runtime_t &gateway,
           const actor_ref_t &actor,
           std::function<void ()> abandoned = {});

    ~join_completion_delivery_fence_scope_t () noexcept;

    void settle (result_t<void> result, std::function<void (result_t<void>)> settled) noexcept;

  private:
    using settled_callback_t = std::function<void (result_t<void>)>;
    using abandoned_callback_t = std::function<void ()>;

    static void invoke_settled (const std::shared_ptr<settled_callback_t> &settled,
                                result_t<void> result) noexcept;

    static void invoke_abandoned (const std::shared_ptr<abandoned_callback_t> &abandoned) noexcept;

    void settle_owned (result_t<void> result,
                       settled_callback_t settled,
                       bool invoke_if_already_settled) noexcept;

    join_completion_delivery_fence_scope_t (
      std::shared_ptr<detail::actor_gateway_runtime_t> gateway,
      actor_ref_t actor,
      std::shared_ptr<detail::bound_session_delivery_fence_t> fence,
      std::shared_ptr<abandoned_callback_t> abandoned);

    std::shared_ptr<detail::actor_gateway_runtime_t> _gateway;
    actor_ref_t _actor;
    std::shared_ptr<detail::bound_session_delivery_fence_t> _fence;
    std::shared_ptr<abandoned_callback_t> _abandoned;
    std::mutex _mutex;
    bool _settled = false;
};

std::string actor_request_dedup_prefix (std::string_view actor_key);

std::string actor_request_dedup_key (std::string_view actor_key,
                                     const runtime::protocol::wire_operation_id_t &operation);

std::optional<std::uint64_t> handoff_u64 (const std::map<std::string, std::string> &metadata,
                                          std::string_view key);

std::optional<handoff_terminal_route_t>
handoff_terminal_route (const std::map<std::string, std::string> &metadata);

void settle_pending_handoff_request (
  detail::spot_node_builder_state_t::pending_handoff_request_t pending,
  const result_t<zlink::message_t> &completed);

task_t<bool> send_handoff_terminal (std::shared_ptr<detail::spot_node_builder_state_t> state,
                                    std::optional<handoff_terminal_route_t> route,
                                    result_t<zlink::message_t> completed);

task_t<bool> settle_handoff_packet (std::shared_ptr<detail::spot_node_builder_state_t> state,
                                    std::shared_ptr<runtime::serial_application_record_t> record,
                                    bool request,
                                    std::optional<handoff_terminal_route_t> route,
                                    result_t<zlink::message_t> completed);

void fail_handoff_backlog (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                           std::vector<detail::handoff_packet_t> discarded);

std::optional<zlink::routing_id_t>
handoff_routing_id (const std::map<std::string, std::string> &metadata, std::string_view key);

std::optional<runtime::protocol::actor_route_fence_t>
handoff_actor_route (const std::map<std::string, std::string> &metadata);


std::shared_ptr<runtime::offload_executor_t>
framework_worker_executor_core (const std::shared_ptr<detail::spot_node_builder_state_t> &node);

class callback_context_scope_t final
{
  public:
    explicit callback_context_scope_t (const spot_context_state_t *owner) noexcept;

    ~callback_context_scope_t ();

  private:
    const spot_context_state_t *_previous;
};

void report_spot_dispatch_error (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                 dispatch_error_surface_t surface,
                                 dispatch_message_kind_t message_kind,
                                 dispatch_error_reason_t reason,
                                 dispatch_error_action_t action,
                                 std::optional<std::string> packet_name = std::nullopt,
                                 std::optional<std::string> topic = std::nullopt,
                                 std::optional<std::string> spot_id = std::nullopt,
                                 std::optional<std::string> actor_id = std::nullopt,
                                 std::exception_ptr exception = nullptr,
                                 std::optional<std::string> correlation_id = std::nullopt);

void report_actor_control_terminal (const std::shared_ptr<detail::spot_node_builder_state_t> &node,
                                    const result_t<void> &terminal,
                                    std::optional<std::string> spot_id = {},
                                    std::optional<std::string> actor_id = {});

void report_spot_dispatch_trace (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                 message_flow_outcome_t outcome,
                                 dispatch_error_surface_t surface,
                                 dispatch_message_kind_t message_kind,
                                 std::string_view packet_name = {},
                                 std::string_view topic = {},
                                 std::string_view spot_id = {},
                                 std::string_view actor_id = {},
                                 std::string_view correlation_id = {},
                                 std::optional<message_flow_result_t> result = std::nullopt,
                                 std::optional<message_flow_reason_t> reason = std::nullopt,
                                 std::string_view detail_result = {},
                                 std::exception_ptr error = {});

void report_actor_leave_failure (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                 const result_t<void> &result,
                                 std::string_view spot_id,
                                 std::string_view actor_id = {},
                                 std::string_view transfer_id = {});

template <typename BuildResult>
void report_actor_dispatch_stage_trace_lazy (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  message_flow_outcome_t outcome,
  dispatch_message_kind_t message_kind,
  std::string_view packet_name,
  std::string_view spot_id,
  std::string_view actor_id,
  std::string_view stage,
  BuildResult &&build_result)
{
    if (!state) {
        return;
    }
    detail::message_flow_tracer_t tracer (state->dispatch);
    if (tracer.enabled (message_flow_log_mode_t::detailed)) {
        tracer.trace (message_flow_log_mode_t::detailed, outcome, [&] {
            return message_flow_event_t{
              .outcome = outcome,
              .surface = dispatch_error_surface_t::spot_actor,
              .message_kind = message_kind,
              .packet_name = packet_name.empty () ? std::nullopt
                                                  : std::make_optional (std::string (packet_name)),
              .spot_id =
                spot_id.empty () ? std::nullopt : std::make_optional (std::string (spot_id)),
              .actor_id =
                actor_id.empty () ? std::nullopt : std::make_optional (std::string (actor_id)),
              .detail_stage = std::string (stage),
              .detail_result = std::forward<BuildResult> (build_result) ()};
        });
    }
}

void report_actor_dispatch_stage_trace (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  message_flow_outcome_t outcome,
  dispatch_message_kind_t message_kind,
  std::string_view packet_name,
  std::string_view spot_id,
  std::string_view actor_id,
  std::string_view stage,
  std::string_view result);

void decrement_actor_count_unlocked (detail::spot_context_state_t &state);

void erase_actor_route_unlocked (detail::spot_node_builder_state_t &state, const std::string &key);

std::uint64_t spot_generation (const detail::spot_context_state_t &context);

void deactivate_actor_location (std::weak_ptr<detail::spot_node_builder_state_t> weak_state,
                                const actor_location_t &location);

result_t<void> claim_pending_actor_location_before_activation (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  const actor_ref_t &source_actor,
  const spot_id_t &source_spot_id,
  const actor_ref_t &committed,
  const detail::spot_context_state_t &target,
  bool &claimed);

task_t<void> release_actor_location_async (std::shared_ptr<detail::spot_node_builder_state_t> state,
                                           actor_ref_t actor);

void release_actor_location (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                             const actor_ref_t &actor);

result_t<void>
update_actor_location_after_move (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                  const actor_ref_t &actor,
                                  const detail::spot_context_state_t &context,
                                  bool entry);

task_t<std::optional<std::uint64_t>>
resolve_target_spot_generation (std::shared_ptr<detail::spot_node_builder_state_t> state,
                                zlink::routing_id_t target_node_rid,
                                spot_id_t target_spot_id);

} // namespace zlink::framework::detail
