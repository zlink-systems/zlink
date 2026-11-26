/* SPDX-License-Identifier: Elastic-2.0 */

#include "spot_runtime_internal.hpp"

#include <zlink/framework/contracts/configuration/zlink_builder.hpp>

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/route_channel_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/actor_execution_context.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/configuration/request_defaults.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/request_deadline.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/spots/spot_route_internal_dispatcher.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include "runtime/timers/core_timer_drain_loop.hpp"
#include "runtime/utils/uuid.hpp"

#include <zlink/framework/contracts/channels/call.hpp>

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Eventing/timers.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Messaging/topic_message.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <algorithm>
#include <charconv>
#include <cassert>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace zlink::framework
{

namespace service = runtime::host;

namespace
{
using detail::handoff_u64;

constexpr std::uint32_t actor_recv_info_no_bind_flag = 1u;

class remote_actor_commit_turn_state_t
{
  public:
    enum class phase_t
    {
        queued,
        active,
        committing,
        committed,
        terminal
    };

    struct outcome_t
    {
        bool success = false;
        framework_error_kind_t error_kind = framework_error_kind_t::internal_failure;
        std::string error;
    };

    bool begin_active ()
    {
        std::lock_guard lock (_mutex);
        if (_phase == phase_t::terminal)
            return false;
        _phase = phase_t::active;
        return true;
    }

    bool try_begin_commit ()
    {
        std::lock_guard lock (_mutex);
        if (_phase == phase_t::terminal || _stop_requested)
            return false;
        _phase = phase_t::committing;
        return true;
    }

    void mark_committed ()
    {
        std::lock_guard lock (_mutex);
        if (_phase != phase_t::terminal)
            _phase = phase_t::committed;
    }

    void select_cancel_error (framework_error_kind_t kind, std::string error)
    {
        std::lock_guard lock (_mutex);
        _cancel_kind = kind;
        _cancel_error = std::move (error);
    }

    void request_stop ()
    {
        bool notify = false;
        std::function<void (outcome_t)> terminal;
        outcome_t outcome;
        {
            std::lock_guard lock (_mutex);
            _stop_requested = true;
            if (_phase == phase_t::queued) {
                _phase = phase_t::terminal;
                _outcome = outcome_t{false, _cancel_kind, _cancel_error};
                outcome = _outcome;
                terminal = std::move (_terminal);
                notify = true;
            }
        }
        if (notify)
            _condition.notify_all ();
        if (terminal)
            terminal (std::move (outcome));
    }

    void settle (outcome_t value)
    {
        std::function<void (outcome_t)> terminal;
        outcome_t outcome;
        {
            std::lock_guard lock (_mutex);
            if (_phase == phase_t::terminal)
                return;
            _phase = phase_t::terminal;
            _outcome = std::move (value);
            outcome = _outcome;
            terminal = std::move (_terminal);
        }
        _condition.notify_all ();
        if (terminal)
            terminal (std::move (outcome));
    }

    void on_terminal (std::function<void (outcome_t)> terminal)
    {
        outcome_t outcome;
        bool invoke = false;
        {
            std::lock_guard lock (_mutex);
            if (_phase == phase_t::terminal) {
                outcome = _outcome;
                invoke = true;
            } else {
                _terminal = std::move (terminal);
            }
        }
        if (invoke && terminal)
            terminal (std::move (outcome));
    }

    bool wait_until (std::chrono::steady_clock::time_point until)
    {
        std::unique_lock lock (_mutex);
        return _condition.wait_until (lock, until, [&] { return _phase == phase_t::terminal; });
    }

    void wait ()
    {
        std::unique_lock lock (_mutex);
        _condition.wait (lock, [&] { return _phase == phase_t::terminal; });
    }

    outcome_t result () const
    {
        std::lock_guard lock (_mutex);
        return _outcome;
    }

    outcome_t cancel_outcome () const
    {
        std::lock_guard lock (_mutex);
        return outcome_t{false, _cancel_kind, _cancel_error};
    }

    bool terminal () const
    {
        std::lock_guard lock (_mutex);
        return _phase == phase_t::terminal;
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _condition;
    phase_t _phase = phase_t::queued;
    bool _stop_requested = false;
    framework_error_kind_t _cancel_kind = framework_error_kind_t::shutting_down;
    std::string _cancel_error = "remote Actor handoff replay was cancelled";
    outcome_t _outcome;
    std::function<void (outcome_t)> _terminal;
};

class remote_actor_commit_deadline_t final
{
  public:
    static std::shared_ptr<remote_actor_commit_deadline_t>
    start (std::chrono::steady_clock::time_point deadline,
           std::shared_ptr<runtime::offload_executor_t> executor,
           std::shared_ptr<runtime::runtime_failure_collector_t> failures,
           std::function<void ()> expired)
    {
        auto owner = std::shared_ptr<remote_actor_commit_deadline_t> (
          new remote_actor_commit_deadline_t (std::move (failures), std::move (expired)));
        std::weak_ptr<remote_actor_commit_deadline_t> weak_owner = owner;
        std::weak_ptr<runtime::offload_executor_t> weak_executor = executor;
        const auto now = std::chrono::steady_clock::now ();
        owner->_timer.start (deadline > now ? deadline - now : std::chrono::nanoseconds (1), 1,
                             [weak_owner, weak_executor] (std::uint64_t) {
                                 auto fire = [weak_owner] {
                                     if (auto active = weak_owner.lock ())
                                         active->fire ();
                                 };
                                 if (auto active_executor = weak_executor.lock ();
                                     active_executor && active_executor->try_submit_internal (fire))
                                     return;
                                 std::thread (std::move (fire)).detach ();
                             });
        return owner;
    }

    ~remote_actor_commit_deadline_t () noexcept { cancel (); }

    void cancel () noexcept
    {
        bool stop = false;
        {
            std::lock_guard lock (_mutex);
            if (_cancelled)
                return;
            _cancelled = true;
            _expired = {};
            stop = !_firing;
        }
        if (!stop)
            return;
        try {
            _timer.stop ();
        }
        catch (...) {
            _failures->report (std::current_exception ());
        }
    }

  private:
    explicit remote_actor_commit_deadline_t (
      std::shared_ptr<runtime::runtime_failure_collector_t> failures,
      std::function<void ()> expired) :
        _failures (std::move (failures)), _timer (_failures), _expired (std::move (expired))
    {
    }

    void fire () noexcept
    {
        std::function<void ()> expired;
        {
            std::lock_guard lock (_mutex);
            if (_cancelled || _firing)
                return;
            _firing = true;
            expired = std::move (_expired);
        }
        if (expired) {
            try {
                expired ();
            }
            catch (...) {
                _failures->report (std::current_exception ());
            }
        }
        {
            std::lock_guard lock (_mutex);
            _firing = false;
            _cancelled = true;
        }
    }

    std::mutex _mutex;
    std::shared_ptr<runtime::runtime_failure_collector_t> _failures;
    detail::core_timer_drain_loop_t _timer;
    std::function<void ()> _expired;
    bool _cancelled = false;
    bool _firing = false;
};

void order_bound_session_handoff (std::vector<detail::handoff_packet_t> &backlog)
{
    std::map<std::uint64_t, std::vector<std::size_t>> positions_by_binding;
    for (std::size_t index = 0; index < backlog.size (); ++index) {
        const auto binding =
          handoff_u64 (backlog[index].metadata, detail::bound_session_relay_binding_key);
        const auto sequence =
          handoff_u64 (backlog[index].metadata, detail::bound_session_relay_sequence_key);
        if (sequence && *sequence != 0)
            positions_by_binding[binding.value_or (0)].push_back (index);
    }
    for (const auto &[binding, positions] : positions_by_binding) {
        (void) binding;
        if (positions.size () < 2)
            continue;
        std::vector<std::pair<std::uint64_t, detail::handoff_packet_t>> ordered;
        ordered.reserve (positions.size ());
        for (const auto position : positions) {
            const auto sequence =
              handoff_u64 (backlog[position].metadata, detail::bound_session_relay_sequence_key);
            ordered.emplace_back (*sequence, std::move (backlog[position]));
        }
        std::stable_sort (
          ordered.begin (), ordered.end (),
          [] (const auto &left, const auto &right) { return left.first < right.first; });
        for (std::size_t index = 0; index < positions.size (); ++index)
            backlog[positions[index]] = std::move (ordered[index].second);
    }
}

struct actor_join_authority_fence_t
{
    std::uint64_t object_generation = 0;
    std::uint64_t node_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    std::uint64_t owner_lease_generation = 0;
};

result_t<std::string> actor_type_from_authority (runtime::live_location_reader_t &store,
                                                 const actor_ref_t &wire_actor,
                                                 const actor_join_authority_fence_t &fence,
                                                 bool actor_type_from_authority_only)
{
    const auto actor_id = wire_actor.actor_id ().value ();
    // Object/authority-owner/owner-lease generations are bounded counters;
    // the MeshNode lifecycle generation is an opaque full-range token.
    if (actor_id.empty () || fence.object_generation == 0 || fence.node_generation == 0
        || fence.authority_owner_generation == 0 || fence.owner_lease_generation == 0) {
        return result_t<std::string>::failure (framework_error_kind_t::protocol_error,
                                               "remote Actor Join authority fence is incomplete");
    }
    try {
        const auto read = runtime::infrastructure_result (
          [&] { return store.read_authority (runtime::actor_authority_key (actor_id)); });
        if (!read) {
            return result_t<std::string>::failure (
              framework_error_kind_t::unavailable,
              "remote Actor Join could not read its Authority row");
        }
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read.value ());
        if (snapshot == nullptr) {
            return result_t<std::string>::failure (
              framework_error_kind_t::not_found,
              "remote Actor Join Actor Authority row is missing");
        }
        if (snapshot->allocation.state != placement_allocation_state_t::active
            || snapshot->allocation.object_kind != placement_object_kind_t::actor
            || snapshot->allocation.stable_type.empty ()
            || snapshot->object_generation != fence.object_generation
            || snapshot->allocation.target.node_rid.value () != wire_actor.node_rid ().value ()
            || snapshot->allocation.target.node_lifecycle_generation != fence.node_generation
            || snapshot->authority_owner_generation != fence.authority_owner_generation
            || snapshot->owner.lease_generation <= 0
            || static_cast<std::uint64_t> (snapshot->owner.lease_generation)
                 != fence.owner_lease_generation) {
            return result_t<std::string>::failure (
              framework_error_kind_t::protocol_error,
              "remote Actor Join Authority row does not exactly match its route fence");
        }
        const auto projection =
          runtime::decode_actor_authority_payload (snapshot->payload, snapshot->object_generation);
        if (!projection || projection->actor.actor_id ().value () != actor_id) {
            return result_t<std::string>::failure (
              framework_error_kind_t::protocol_error,
              "remote Actor Join Actor Authority row is incomplete");
        }
        const auto stable_type = snapshot->allocation.stable_type;
        // Canonical actorJoin(28) intentionally has no wire stable type.
        // The legacy/JSON path still supplies one and remains protected by
        // this forgery cross-check; the canonical path obtains its type only
        // from the Authority row above.
        const auto wire_type =
          ::zlink::framework::detail::actor_ref_access_t::actor_type (wire_actor);
        if ((!actor_type_from_authority_only || !wire_type.empty ()) && wire_type != stable_type) {
            return result_t<std::string>::failure (
              framework_error_kind_t::type_mismatch,
              "remote Actor Join wire type does not match its Authority row");
        }
        return result_t<std::string>::success (stable_type);
    }
    catch (const std::exception &) {
        return result_t<std::string>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor Join could not read its Authority row");
    }
    catch (...) {
        return result_t<std::string>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor Join could not read its Authority row");
    }
}

std::shared_ptr<runtime::offload_executor_t>
framework_deadline_executor_core (const std::shared_ptr<detail::spot_node_builder_state_t> &node)
{
    if (node && node->deadline_executor)
        return node->deadline_executor;
    auto executor = std::make_shared<runtime::offload_executor_t> (1, "zlink-spot-deadline");
    if (node)
        node->deadline_executor = executor;
    return executor;
}

std::shared_ptr<runtime::offload_executor_t>
framework_deadline_executor (const std::shared_ptr<detail::spot_node_builder_state_t> &node)
{
    return node->lane.run ([&] { return framework_deadline_executor_core (node); }).get ();
}

zlink::message_t framework_reply_or_empty (const std::optional<message_t> &reply,
                                           serializer_registry_t &serializers)
{
    return reply ? detail::message_to_raw (*reply, serializers) : zlink::message_t{};
}

void erase_retired_actor_route_on_lane (detail::spot_node_builder_state_t &state,
                                        const detail::removed_actor_message_follow_t &entry,
                                        bool retained_follow_route)
{
    assert (state.lane.is_on_lane ());
    const auto &key = entry.actor_key;
    const auto current_route = state.actor_routes.find (key);
    const bool same_route =
      current_route == state.actor_routes.end ()
      || (current_route->second.node_rid.value () == entry.target_route.node_rid.value ()
          && current_route->second.spot_id == entry.target_route.spot_id
          && current_route->second.spot_name == entry.target_route.spot_name);
    const auto current_generation = state.actor_generations.find (key);
    if (!state.actor_authority_fences.contains (key) && !retained_follow_route && same_route
        && (current_generation == state.actor_generations.end ()
            || current_generation->second == entry.target_generation)) {
        state.actor_routes.erase (key);
        state.native_actors.erase (key);
    }
}
} // namespace

namespace detail
{

std::shared_ptr<join_completion_delivery_fence_scope_t>
join_completion_delivery_fence_scope_t::begin (const detail::actor_gateway_runtime_t &gateway,
                                               const actor_ref_t &actor,
                                               std::function<void ()> abandoned)
{
    auto gateway_owner = std::make_shared<detail::actor_gateway_runtime_t> (gateway);
    auto fence = gateway_owner->begin_join_completion_delivery_fence (actor);
    auto abandon_owner = std::make_shared<std::function<void ()>> (std::move (abandoned));
    return std::shared_ptr<join_completion_delivery_fence_scope_t> (
      new join_completion_delivery_fence_scope_t (std::move (gateway_owner), actor,
                                                  std::move (fence), std::move (abandon_owner)));
}

join_completion_delivery_fence_scope_t::~join_completion_delivery_fence_scope_t () noexcept
{
    const auto abandoned = _abandoned;
    try {
        settle_owned (
          result_t<void>::failure (framework_error_kind_t::unavailable,
                                   "Actor Join ended before bound Session delivery settlement"),
          [abandoned] (result_t<void>) noexcept { invoke_abandoned (abandoned); }, false);
    }
    catch (...) {
        invoke_abandoned (abandoned);
    }
}

void join_completion_delivery_fence_scope_t::settle (
  result_t<void> result, std::function<void (result_t<void>)> settled) noexcept
{
    settle_owned (std::move (result), std::move (settled), true);
}

void join_completion_delivery_fence_scope_t::invoke_settled (
  const std::shared_ptr<settled_callback_t> &settled, result_t<void> result) noexcept
{
    try {
        if (settled && *settled)
            (*settled) (std::move (result));
    }
    catch (...) {
    }
}

void join_completion_delivery_fence_scope_t::invoke_abandoned (
  const std::shared_ptr<abandoned_callback_t> &abandoned) noexcept
{
    try {
        if (abandoned && *abandoned)
            (*abandoned) ();
    }
    catch (...) {
    }
}

void join_completion_delivery_fence_scope_t::settle_owned (result_t<void> result,
                                                           settled_callback_t settled,
                                                           bool invoke_if_already_settled) noexcept
{
    std::shared_ptr<settled_callback_t> settled_owner;
    try {
        settled_owner = std::make_shared<settled_callback_t> (std::move (settled));
    }
    catch (...) {
        invoke_abandoned (_abandoned);
        return;
    }

    std::shared_ptr<detail::actor_gateway_runtime_t> gateway;
    std::shared_ptr<detail::bound_session_delivery_fence_t> fence;
    {
        std::lock_guard lock (_mutex);
        if (!_settled && _gateway && _fence) {
            _settled = true;
            gateway = _gateway;
            fence = _fence;
        }
    }
    if (!gateway || !fence) {
        if (invoke_if_already_settled)
            invoke_settled (settled_owner, std::move (result));
        return;
    }
    try {
        gateway->settle_join_completion_delivery_fence (
          _actor, fence, std::move (result), [settled_owner] (result_t<void> value) noexcept {
              invoke_settled (settled_owner, std::move (value));
          });
    }
    catch (...) {
        try {
            invoke_settled (settled_owner, result_t<void>::failure (
                                             framework_error_kind_t::internal_failure,
                                             "Actor Join delivery fence settlement failed"));
        }
        catch (...) {
            invoke_abandoned (_abandoned);
        }
    }
}

join_completion_delivery_fence_scope_t::join_completion_delivery_fence_scope_t (
  std::shared_ptr<detail::actor_gateway_runtime_t> gateway,
  actor_ref_t actor,
  std::shared_ptr<detail::bound_session_delivery_fence_t> fence,
  std::shared_ptr<abandoned_callback_t> abandoned) :
    _gateway (std::move (gateway)),
    _actor (std::move (actor)),
    _fence (std::move (fence)),
    _abandoned (std::move (abandoned))
{
}

task_t<std::shared_ptr<spot_node_runtime_t::actor_join_state_snapshot_t>>
spot_node_runtime_t::actor_join_state_snapshot (const actor_ref_t &actor_ref,
                                                spot_id_t spot_id,
                                                const zlink::message_t &request)
{
    std::optional<std::string> dynamic_spot_name;
    const auto key = actor_key (actor_ref);
    auto select = [&] {
        return _state->lane.run_task ([&] {
            auto snapshot_state = std::make_shared<actor_join_state_snapshot_t> ();
            auto &snapshot = *snapshot_state;
            dynamic_spot_name.reset ();
            auto context = find_context_core (spot_id);
            if (context && context->_state->accepts_membership_core (*_state)) {
                snapshot.context.emplace (std::move (*context));
                const auto &context_state = snapshot.context->_state;
                snapshot.spot_instance = context_state->spot_instance;
                snapshot.serializers = context_state->channel_runtime
                                         ? context_state->channel_runtime->serializers
                                         : nullptr;
                snapshot.mesh_name = context_state->mesh_name;
                snapshot.node_rid = detail::effective_spot_node_rid (_state->snapshot);
                if (const auto source = _state->actor_spot_ids.find (key);
                    source != _state->actor_spot_ids.end ()) {
                    snapshot.source_spot_id = source->second;
                }
                snapshot.message_follow_duration = _state->message_follow_duration;

                const auto factory = _state->actor_factories.find (std::string (
                  ::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
                if (factory != _state->actor_factories.end ()) {
                    snapshot.registration.emplace (factory->second);
                    const auto admission =
                      context_state->actor_admissions.find (factory->second.actor_type);
                    if (admission != context_state->actor_admissions.end ()
                        && (context_state->is_entry_spot () || admission->second.join)) {
                        snapshot.admission.emplace (admission->second);
                    }
                }

                const auto instance = _state->actor_instances.find (key);
                if (instance != _state->actor_instances.end () && instance->second) {
                    snapshot.actor_instance = instance->second;
                    detail::record_actor_instance_index_unlocked (*_state, actor_ref,
                                                                  snapshot.actor_instance.get ());
                }
                return snapshot_state;
            }
            for (const auto &[spot_name, _] : _state->spot_factories) {
                if (_state->snapshot.entry_spot_name
                    && spot_name == *_state->snapshot.entry_spot_name) {
                    continue;
                }
                if (dynamic_spot_name) {
                    dynamic_spot_name.reset ();
                    break;
                }
                dynamic_spot_name = spot_name;
            }
            return snapshot_state;
        });
    };
    auto selected = co_await select ();
    if (!selected->context && dynamic_spot_name) {
        (void) co_await get_or_create_spot_async (*dynamic_spot_name, spot_id, request);
        co_return co_await select ();
    }
    co_return selected;
}

result_t<spot_actor_admission_callbacks_t>
spot_node_runtime_t::actor_admission (spot_context_t &context,
                                      std::type_index actor_type,
                                      spot_id_t spot_id,
                                      const actor_ref_t &actor_ref)
{
    std::optional<spot_actor_admission_callbacks_t> selected;
    _state->lane
      .run ([&] {
          const auto current = find_context_core (context.spot_id ());
          if (!current || current->_state.get () != context._state.get ()
              || !context._state->accepts_membership_core (*_state)) {
              return;
          }
          const auto admission = context._state->actor_admissions.find (actor_type);
          if (admission != context._state->actor_admissions.end ()
              && (context._state->is_entry_spot () || admission->second.join))
              selected.emplace (admission->second);
      })
      .get ();
    if (!selected) {
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
          dispatch_error_reason_t::handler_missing, dispatch_error_action_t::reply_error,
          "actor.join", std::nullopt, std::string (spot_id),
          std::string (actor_ref.actor_id ().value ()));
        return result_t<spot_actor_admission_callbacks_t>::failure (
          framework_error_kind_t::not_found, "spot actor join callback is not registered");
    }
    return result_t<spot_actor_admission_callbacks_t>::success (std::move (*selected));
}

task_t<std::function<void (std::function<void (result_t<void>)>)>>
spot_node_runtime_t::commit_accepted_actor_join (
  const std::string &key,
  spot_context_t &context,
  const actor_ref_t &committed,
  std::type_index actor_type,
  void *actor,
  const spot_actor_admission_callbacks_t &admission,
  bool create_entry_actor,
  const zlink::message_t &create_request,
  std::string operation_id,
  bool &authority_committed,
  std::optional<std::chrono::steady_clock::time_point> deadline)
{
    authority_committed = false;
    struct commit_plan_t
    {
        std::optional<spot_context_t> previous_context;
        std::shared_ptr<service::mesh_node_t> native_node;
        std::shared_ptr<void> target_spot_instance;
        serializer_registry_t *serializers = nullptr;
        std::string node_rid;
        spot_id_t target_spot_id;
        bool create_entry_actor = false;
        bool create_native_actor = false;
        bool valid = false;
    };
    auto &target_state = *context._state;
    const auto plan =
      _state->lane
        .run ([&] {
            commit_plan_t result;
            const auto target = find_context_core (context.spot_id ());
            if (!target || target->_state.get () != context._state.get ()
                || !target_state.accepts_membership_core (*_state)) {
                return result;
            }
            if (const auto previous = _state->actor_spot_ids.find (key);
                previous != _state->actor_spot_ids.end ()) {
                auto previous_context = find_context_core (previous->second);
                if (previous_context)
                    result.previous_context.emplace (std::move (*previous_context));
            }
            result.native_node = _state->native_node.lock ();
            result.target_spot_instance = target_state.spot_instance;
            result.serializers =
              target_state.channel_runtime ? target_state.channel_runtime->serializers : nullptr;
            result.node_rid = detail::effective_spot_node_rid (_state->snapshot);
            result.target_spot_id = target_state.spot_id;
            result.create_entry_actor = create_entry_actor && target_state.is_entry_spot ()
                                        && !_state->actor_created_keys.contains (key)
                                        && admission.on_create_actor;
            result.create_native_actor = result.native_node && !_state->native_actors.contains (key)
                                         && !_state->mesh_runtime_owned_native_actor_ids.contains (
                                           std::string (committed.actor_id ().value ()));
            result.valid = true;
            return result;
        })
        .get ();
    if (!plan.valid) {
        throw framework_exception_t (framework_error_kind_t::not_found,
                                     "target spot is not registered");
    }
    const auto caller_owns_source_turn =
      plan.previous_context && plan.previous_context->_state->owns_current_serial_turn ();
    bool created_entry_actor = false;
    if (plan.create_entry_actor) {
        if (!plan.serializers) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "spot create actor requires a serializer registry");
        }
        result_t<actor_create_response_t> create_result =
          result_t<actor_create_response_t>::failure (
            framework_error_kind_t::internal_failure,
            "Entry Spot actor creation callback did not complete");
        auto create_actor = [&] () -> task_t<void> {
            create_result = co_await runtime::await_result (admission.on_create_actor (
              plan.target_spot_instance.get (), actor, create_request, *plan.serializers));
        };
        const bool caller_owns_target_turn =
          caller_owns_source_turn && plan.previous_context->_state.get () == context._state.get ();
        if (caller_owns_target_turn)
            co_await create_actor ();
        else
            co_await target_state.run_serial_task ("spot-actor-create", create_actor);
        if (!create_result.value ().accepted)
            throw framework_exception_t (framework_error_kind_t::rejected,
                                         "Entry Spot rejected Actor creation");
        created_entry_actor =
          _state->lane
            .run ([&] {
                const auto target = find_context_core (plan.target_spot_id);
                if (!target || target->_state.get () != context._state.get ()
                    || !target_state.accepts_membership_core (*_state)
                    || target_state.spot_instance.get () != plan.target_spot_instance.get ()) {
                    return false;
                }
                _state->actor_created_keys.insert (key);
                return true;
            })
            .get ();
        if (!created_entry_actor) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "target spot is not registered");
        }
    }
    /*
     * Publish membership before lifecycle callbacks. Application callbacks
     * observe the committed target Context, and a callback failure must not
     * roll authority back to the previous owner.
     */
    auto services = _state->lane.run ([&] { return _state->root_services; }).get ();
    if (services && _state->location_lifecycle) {
        auto &repository = services->get_required<location_repository_t> ();
        const auto authority_key = runtime::actor_authority_key (committed.actor_id ().value ());
        auto current = co_await repository.read_authority (authority_key);
        std::optional<framework_error_kind_t> commit_failure;
        for (;;) {
            const auto *snapshot = std::get_if<authority_snapshot_t> (&current);
            auto member = snapshot
                            ? runtime::decode_direct_actor_authority_payload (snapshot->payload)
                            : std::nullopt;
            if (snapshot && snapshot->object_generation != committed.object_generation ())
                throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                             "Actor generation does not match current authority");
            if (!member)
                throw framework_exception_t (framework_error_kind_t::unavailable,
                                             "Actor membership authority is unavailable");
            if (member->has_relocation_state)
                throw framework_exception_t (framework_error_kind_t::unavailable,
                                             "Actor membership is relocating");
            if (commit_failure)
                throw framework_exception_t (*commit_failure, "Actor membership commit conflicted");
            const auto kind = target_state.is_entry_spot ()
                                ? runtime::actor_authority_spot_kind_t::entry
                                : runtime::actor_authority_spot_kind_t::user;
            const auto generation =
              target_state.is_entry_spot ()
                ? (plan.native_node ? plan.native_node->status ().lifecycle_generation ()
                                    : target_state.object_generation)
                : target_state.object_generation;
            if (member->current_spot_id != std::string (target_state.spot_id)
                || member->current_spot_generation != generation
                || member->current_spot_kind != kind) {
                member->current_spot_id = std::string (target_state.spot_id);
                member->current_spot_generation = generation;
                member->current_spot_kind = kind;
                std::function<task_t<void> (std::optional<authority_snapshot_t>)> confirm;
                if (plan.native_node)
                    confirm = co_await plan.native_node->prepare_store_ready (
                      {runtime::stateful::object_kind_t::actor,
                       std::string (committed.actor_id ().value ()), snapshot->object_generation,
                       snapshot->authority_owner_generation, snapshot->allocation.target.mesh_name,
                       std::string (snapshot->allocation.target.node_rid.value ())});
                const auto stored = co_await repository.compare_exchange_authority (
                  authority_key, snapshot->store_version,
                  authority_put_t{runtime::encode_actor_authority_payload (*member), deadline});
                if (std::holds_alternative<authority_generation_exhausted_t> (stored))
                    throw framework_exception_t (framework_error_kind_t::internal_failure,
                                                 "Actor authority generation is exhausted");
                if (const auto *conflict = std::get_if<authority_conflict_t> (&stored)) {
                    current = conflict->current;
                    commit_failure =
                      conflict->membership_failure.value_or (framework_error_kind_t::unavailable);
                    continue;
                }
                if (const auto *ready = std::get_if<authority_stored_t> (&stored); ready && confirm)
                    co_await confirm (ready->snapshot);
            }
            break;
        }
    }
    authority_committed = true;
    const auto location_updated =
      update_actor_location_after_move (_state, committed, target_state, false);
    if (!location_updated) {
        throw framework_exception_t (location_updated.error_kind (),
                                     location_updated.error ()
                                       ? location_updated.error ()->what ()
                                       : "actor committed location update failed");
    }
    std::unique_ptr<service::actor_t> native_actor;
    if (plan.create_native_actor) {
        native_actor = std::make_unique<service::actor_t> (plan.native_node->create_actor (
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (committed)),
          std::string (committed.actor_id ().value ())));
    }
    std::function<result_t<void> (const actor_ref_t &)> update_actor_registry_ref;
    struct leave_projection_t
    {
        std::shared_ptr<spot_context_state_t> context;
        std::shared_ptr<void> spot_instance;
        std::function<task_t<void> (void *, void *)> callback;
        std::shared_ptr<void> actor_instance;
    } leave;
    const auto route_committed =
      _state->lane
        .run ([&] {
            const auto target = find_context_core (plan.target_spot_id);
            if (!target || target->_state.get () != context._state.get ()
                || !target_state.accepts_membership_core (*_state)
                || target_state.spot_instance.get () != plan.target_spot_instance.get ()) {
                return false;
            }
            std::optional<spot_context_t> previous_context;
            if (const auto previous = _state->actor_spot_ids.find (key);
                previous != _state->actor_spot_ids.end ()) {
                auto selected = find_context_core (previous->second);
                if (selected)
                    previous_context.emplace (std::move (*selected));
            }
            if (previous_context)
                decrement_actor_count_unlocked (*previous_context->_state);
            // Replace the native projection without discarding the committed owner fence.
            _state->native_actors.erase (key);
            _state->destroyed_actor_keys.erase (key);
            if (native_actor && !_state->native_actors.contains (key)
                && !_state->mesh_runtime_owned_native_actor_ids.contains (
                  std::string (committed.actor_id ().value ()))) {
                _state->native_actors.emplace (key, std::move (native_actor));
            }
            detail::record_actor_context_route_unlocked (*_state, key, plan.node_rid, target_state,
                                                         committed.object_generation ());
            update_actor_registry_ref = _state->update_actor_registry_ref;
            if (previous_context) {
                const auto previous_admission =
                  previous_context->_state->actor_admissions.find (actor_type);
                if (previous_admission != previous_context->_state->actor_admissions.end ()
                    && previous_admission->second.on_leave_actor
                    && previous_context->_state->spot_instance) {
                    leave.context = previous_context->_state;
                    leave.spot_instance = previous_context->_state->spot_instance;
                    leave.callback = previous_admission->second.on_leave_actor;
                    if (const auto instance = _state->actor_instances.find (key);
                        instance != _state->actor_instances.end ())
                        leave.actor_instance = instance->second;
                }
            }
            return true;
        })
        .get ();
    if (!route_committed) {
        throw framework_exception_t (framework_error_kind_t::not_found,
                                     "target spot is not registered");
    }
    if (update_actor_registry_ref) {
        const auto updated = update_actor_registry_ref (committed);
        if (!updated) {
            throw framework_exception_t (updated.error_kind (), updated.error ()
                                                                  ? updated.error ()->what ()
                                                                  : "actor ref update failed");
        }
    }
    if (actor_transfer_marker_enabled ()) {
        emit_actor_transfer_marker ("location_committed", committed,
                                    operation_id.empty () ? key : std::move (operation_id),
                                    context.spot_id (), _state->node_rid);
    }

    if (!created_entry_actor && admission.on_actor_joined) {
        co_await target_state.run_serial_task ("spot-actor-joined", [&] {
            return admission.on_actor_joined (plan.target_spot_instance.get (), actor);
        });
    }
    if (!leave.callback)
        co_return std::function<void (std::function<void (result_t<void>)>)>{};
    co_return [leave = std::move (leave), actor,
               state = _state] (std::function<void (result_t<void>)> completed) mutable {
        leave.context->run_serial_task_async (
          "spot-actor-leave",
          [leave, actor] {
              return leave.callback (leave.spot_instance.get (),
                                     leave.actor_instance ? leave.actor_instance.get () : actor);
          },
          [state, spot_id = leave.context->spot_id,
           completed = std::move (completed)] (result_t<void> left) mutable {
              report_actor_leave_failure (state, left, spot_id);
              if (completed)
                  completed (std::move (left));
          });
    };
}

task_t<void> spot_node_runtime_t::replay_actor_handoff_batch (actor_ref_t actor_ref,
                                                              std::vector<handoff_packet_t> backlog,
                                                              service_provider_t services,
                                                              std::string transfer_id,
                                                              bool reuse_active_actor_queue,
                                                              std::function<bool ()> stop_requested)
{
    if (backlog.empty ())
        co_return;
    order_bound_session_handoff (backlog);
    const auto key = actor_key (actor_ref);
    std::optional<spot_id_t> location;
    std::shared_ptr<spot_context_state_t> context_state;
    std::shared_ptr<void> actor_instance;
    std::type_index actor_type{typeid (void)};
    co_await _state->lane.run_task ([&] {
        if (const auto local = _state->actor_spot_ids.find (key);
            local != _state->actor_spot_ids.end ()) {
            location = local->second;
        } else if (const auto routed = _state->actor_routes.find (key);
                   routed != _state->actor_routes.end ()) {
            location = routed->second.spot_id;
        }
        const auto factory = _state->actor_factories.find (
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
        const auto actor = _state->actor_instances.find (key);
        if (location && factory != _state->actor_factories.end ()
            && actor != _state->actor_instances.end () && actor->second) {
            const auto context = find_context_core (*location);
            if (context && context->_state->spot_instance) {
                context_state = context->_state;
                actor_instance = actor->second;
                actor_type = factory->second.actor_type;
            }
        }
    });

    struct fail_handoff_packet_t
    {
        static task_t<void> run (std::decay_t<decltype (_state)> node,
                                 std::decay_t<decltype (actor_ref)> actor_ref,
                                 std::decay_t<decltype (transfer_id)> transfer_id,
                                 handoff_packet_t packet,
                                 framework_error_kind_t kind,
                                 std::string message)
        {
            const auto terminal_route =
              packet.is_request ? handoff_terminal_route (packet.metadata) : std::nullopt;
            const auto failure = result_t<zlink::message_t>::failure (kind, std::move (message));
            if (spot_node_runtime_t (node).actor_transfer_marker_enabled (
                  message_flow_result_t::failed)) {
                spot_node_runtime_t (node).emit_actor_transfer_marker (
                  "handoff_replay_failed", actor_ref, transfer_id, std::nullopt, std::nullopt,
                  failure.exception ());
            }
            const auto sent = co_await runtime::await_result (settle_handoff_packet (
              node, packet.application_record, packet.is_request, terminal_route, failure));
            if ((!sent || !sent.value ())
                && spot_node_runtime_t (node).actor_transfer_marker_enabled (
                  message_flow_result_t::failed)) {
                spot_node_runtime_t (node).emit_actor_transfer_marker (
                  "handoff_terminal_send_failed", actor_ref, transfer_id, std::nullopt,
                  std::nullopt,
                  !sent ? sent.exception ()
                        : std::make_exception_ptr (framework_exception_t (
                            framework_error_kind_t::unavailable,
                            "Actor handoff terminal send was not accepted")));
            }
        }
    };
    auto terminal_failure = [node = _state, actor_ref,
                             transfer_id] (handoff_packet_t &packet, framework_error_kind_t kind,
                                           std::string message) -> task_t<void> {
        return fail_handoff_packet_t::run (node, actor_ref, transfer_id, std::move (packet), kind,
                                           std::move (message));
    };
    if (!context_state || !location || !context_state->channel_runtime
        || !context_state->channel_runtime->serializers) {
        for (auto &packet : backlog) {
            if (!packet.is_request) {
                detail::dispatch_error_reporter_t (_state->dispatch).report_lazy ([&] {
                    auto event = message_dispatch_error_event_t{
                      .surface = dispatch_error_surface_t::spot_actor,
                      .message_kind = dispatch_message_kind_t::actor_send,
                      .reason = dispatch_error_reason_t::stale_target,
                      .action = dispatch_error_action_t::drop,
                      .packet_name = packet.packet_name,
                      .actor_id = std::string (actor_ref.actor_id ().value ()),
                      .mesh_name = _state->snapshot.name};
                    if (runtime::flow_context_t::tracing (
                          detail::message_flow_tracer_t (_state->dispatch).mode ())
                        && packet.application_record) {
                        const auto decoded = detail::decode_actor_packet_header (
                          packet.application_record->application_header (),
                          stream_message_kind_t::send, true);
                        const auto &header = decoded.value ();
                        event.flow_id = header.flow_id;
                        event.flow_origin = header.flow_origin;
                    }
                    return event;
                });
                runtime::runtime_metrics_t metrics (_state->monitoring);
                if (metrics.enabled ())
                    metrics.counter ("zlink.mesh_node.messages.dropped", "{message}", 1,
                                     {{"mesh_name", _state->snapshot.name},
                                      {"surface", "actor"},
                                      {"message_kind", "send"},
                                      {"reason", "stale_target"}});
            }
            co_await terminal_failure (packet, framework_error_kind_t::unavailable,
                                       "target Actor handoff replay owner is unavailable");
        }
        co_return;
    }
    for (auto &packet : backlog) {
        if (stop_requested && stop_requested ()) {
            co_await terminal_failure (packet, framework_error_kind_t::shutting_down,
                                       "target Actor handoff replay was cancelled during shutdown");
            continue;
        }
        if (spot_node_runtime_t (_state).actor_transfer_marker_enabled ()) {
            emit_actor_transfer_marker ("handoff_replay_enqueued", actor_ref, transfer_id,
                                        *location);
        }
        auto *invocation =
          packet.application_record ? packet.application_record->invocation () : nullptr;
        spot_inbound_message_t metadata =
          invocation ? std::move (invocation->second)
                     : spot_inbound_message_t{.content_type = std::move (packet.content_type),
                                              .values = std::move (packet.metadata)};
        const auto terminal_route =
          packet.is_request ? handoff_terminal_route (metadata.values) : std::nullopt;
        const auto replay_operation =
          terminal_route ? std::make_optional (terminal_route->operation) : std::nullopt;
        if (replay_operation) {
            const auto claim = _state->dispatched_request_replies.claim (
              actor_request_dedup_key (key, *replay_operation));
            if (claim.state != runtime::exactly_once_claim_state::claimed)
                continue;
        }
        const auto kind =
          packet.is_request ? spot_handler_kind_t::actor_request : spot_handler_kind_t::actor_send;
        result_t<zlink::message_t> result = result_t<zlink::message_t>::failure (
          framework_error_kind_t::internal_failure, "Actor handoff replay did not complete");
        std::exception_ptr replay_exception;
        std::optional<runtime::flow_context_t::scope_t> flow_scope;
        try {
            const auto mode = detail::message_flow_tracer_t (_state->dispatch).mode ();
            if (runtime::flow_context_t::tracing (mode) && packet.application_record) {
                const auto decoded = detail::decode_actor_packet_header (
                  packet.application_record->application_header (),
                  packet.is_request ? stream_message_kind_t::request : stream_message_kind_t::send,
                  true);
                const auto &header = decoded.value ();
                flow_scope.emplace (runtime::flow_context_t::enter (
                  header.flow_id, header.flow_origin, mode, flow_origin_t::inbound, std::nullopt));
            }
            if (packet.application_record && !packet.application_record->invocation ()) {
                runtime::stateful::with_accepted_application (
                  *packet.application_record->payload (), [&] (auto, const auto &application) {
                      std::array<std::span<const std::byte>, 2> descriptors;
                      auto parts =
                        application.parts ()
                          ? runtime::messaging::message_parts_t::borrow (*application.parts ())
                          : runtime::messaging::message_parts_t::borrow (
                              std::span<const std::span<const std::byte>> (descriptors)
                                .first (runtime::protocol::decode_application_part_views (
                                  application, descriptors)));
                      auto body = runtime::messaging::envelope_codec_t{}.decode_body (parts);
                      if (!body)
                          std::rethrow_exception (body.exception ());
                      packet.application_record->set_invocation (std::move (body.value ()),
                                                                 std::move (metadata));
                  });
                metadata = std::move (packet.application_record->invocation ()->second);
            }
            result = result_t<zlink::message_t>::success (
              co_await spot_handler_registry_t (context_state)
                .invoke_erased (
                  {}, kind, packet.packet_name, {}, actor_type, context_state->spot_instance.get (),
                  actor_instance.get (), services, *context_state->channel_runtime->serializers,
                  packet.application_record && packet.application_record->invocation ()
                    ? packet.application_record->invocation ()->first
                    : zlink::message_t::from (std::move (packet.payload)),
                  std::move (metadata), true, key, std::string (*location), {},
                  reuse_active_actor_queue
                    ? spot_handler_registry_t::actor_queue_dispatch_t::current_turn
                    : spot_handler_registry_t::actor_queue_dispatch_t::acquire,
                  {}, {}, 0, {}, {}, {}, packet.application_record, {},
                  actor_ref.actor_id ().value ()));
        }
        catch (const framework_exception_t &error) {
            replay_exception = std::current_exception ();
            result = result_t<zlink::message_t>::failure (error.kind (), error.what ());
        }
        catch (const std::exception &error) {
            replay_exception = std::current_exception ();
            result = result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                          error.what ());
        }
        if (replay_operation) {
            const auto dedup_key = actor_request_dedup_key (key, *replay_operation);
            if (result)
                (void) _state->dispatched_request_replies.complete (dedup_key, result.value ());
            else
                (void) _state->dispatched_request_replies.erase (dedup_key);
        }
        if (!result
            && spot_node_runtime_t (_state).actor_transfer_marker_enabled (
              message_flow_result_t::failed)) {
            emit_actor_transfer_marker ("handoff_replay_failed", actor_ref, transfer_id,
                                        std::nullopt, std::nullopt,
                                        replay_exception ? replay_exception : result.exception ());
        }
        const auto sent = co_await runtime::await_result (settle_handoff_packet (
          _state, packet.application_record, packet.is_request, terminal_route, result));
        if ((!sent || !sent.value ())
            && spot_node_runtime_t (_state).actor_transfer_marker_enabled (
              message_flow_result_t::failed)) {
            emit_actor_transfer_marker ("handoff_terminal_send_failed", actor_ref, transfer_id,
                                        std::nullopt, std::nullopt,
                                        !sent ? sent.exception ()
                                              : std::make_exception_ptr (framework_exception_t (
                                                  framework_error_kind_t::unavailable,
                                                  "Actor handoff terminal send was not accepted")));
        }
    }
}

void spot_node_runtime_t::enqueue_actor_handoff_replay (const actor_ref_t &actor_ref,
                                                        std::vector<handoff_packet_t> backlog,
                                                        service_provider_t &services,
                                                        std::string transfer_id)
{
    auto replay_owner = std::make_shared<spot_node_runtime_t> (_state);
    auto replay = replay_owner->replay_actor_handoff_batch (
      actor_ref, std::move (backlog), services, std::move (transfer_id), false, {});
    // Register the terminal observer before this fire-and-forget entry returns:
    // the replay coroutine owns its serial handler and terminal-send continuations.
    detail::observe_task_completion (replay, [replay_owner] (const result_t<void> &) {});
}

void spot_node_runtime_t::replay_actor_handoff_until_move_closed (const actor_ref_t &actor_ref)
{
    replay_actor_handoff_core (actor_ref).result ().value ();
}

task_t<void>
spot_node_runtime_t::replay_actor_handoff_until_move_closed_async (actor_ref_t actor_ref)
{
    co_await replay_actor_handoff_core (actor_ref);
}

task_t<std::size_t> spot_node_runtime_t::settle_abandoned_actor_leave_reservations_async (
  std::function<void ()> after_coordinator_snapshot)
{
    struct pending_leave_identity_t
    {
        std::string key;
        std::string transfer_id;
        std::uint64_t source_generation;
        std::string source_spot_id;
        std::string target_spot_id;
        runtime::protocol::actor_route_fence_t target_fence;
    };
    const auto state = _state;
    auto identities = co_await state->lane.run_task ([state] {
        std::vector<pending_leave_identity_t> result;
        result.reserve (state->pending_remote_actor_leaves.size ());
        for (const auto &pending : state->pending_remote_actor_leaves)
            result.push_back ({actor_key (pending.source_actor), pending.transfer_id,
                               pending.source_actor.object_generation (), pending.source_spot_id,
                               pending.target_spot_id, pending.target_fence});
        return result;
    });
    std::erase_if (identities, [state] (const auto &identity) {
        return state->actor_transfer_coordinator->matches_source_remote_transfer (
          identity.key, identity.transfer_id);
    });
    const auto identity_key = [] (const auto &identity) {
        return std::pair<std::string_view, std::string_view>{identity.key, identity.transfer_id};
    };
    std::ranges::sort (identities, {}, identity_key);
    if (after_coordinator_snapshot)
        after_coordinator_snapshot ();
    auto sweep = co_await state->lane.run_task ([state, &identities, identity_key] {
        std::pair<std::size_t, std::vector<std::function<void ()>>> result;
        result.second.reserve (identities.size ());
        for (auto found = state->pending_remote_actor_leaves.begin ();
             found != state->pending_remote_actor_leaves.end ();) {
            const auto key = actor_key (found->source_actor);
            const auto candidates = std::ranges::equal_range (
              identities, std::pair<std::string_view, std::string_view>{key, found->transfer_id},
              {}, identity_key);
            const bool in_snapshot = std::ranges::any_of (candidates, [&] (const auto &identity) {
                return identity.key == key && identity.transfer_id == found->transfer_id
                       && identity.source_generation == found->source_actor.object_generation ()
                       && identity.source_spot_id == found->source_spot_id
                       && identity.target_spot_id == found->target_spot_id
                       && identity.target_fence == found->target_fence;
            });
            if (!in_snapshot) {
                ++found;
                continue;
            }
            if (found->transfer_owner_reservation) {
                result.second.push_back (std::move (found->transfer_owner_reservation));
                found->transferred_owner_byte_cost = 0;
            }
            found = state->pending_remote_actor_leaves.erase (found);
            ++result.first;
        }
        return result;
    });
    for (auto &reservation : sweep.second) {
        transferred_owner_reservation_guard_t abandoned (std::move (reservation));
        abandoned.settle ();
    }
    co_return sweep.first;
}

task_t<void> spot_node_runtime_t::replay_actor_handoff_core (const actor_ref_t &actor_ref)
{
    auto owner = std::make_shared<spot_node_runtime_t> (_state);
    auto opened = owner->open_local_actor_handoff (actor_ref);
    detail::observe_task_completion (opened, [owner] (const result_t<result_t<void>> &terminal) {
        if (!terminal || !terminal.value ())
            owner->_state->runtime_failures ()->report (terminal ? terminal.value ().exception ()
                                                                 : terminal.exception ());
    });
    return task_t<void> (result_t<void>::success ());
}

task_t<actor_join_reply_t> spot_node_runtime_t::join_actor_to_spot_erased (
  const actor_ref_t &actor_ref,
  spot_id_t spot_id,
  const zlink::message_t &request,
  const std::optional<zlink::message_t> &actor_snapshot,
  actor_context_t actor_context,
  std::uint64_t operation_id_high,
  std::uint64_t operation_id_low,
  std::function<void (std::function<void (result_t<void>)>)> *source_leave,
  std::optional<std::chrono::steady_clock::time_point> deadline)
{
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        co_return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                         "actor ref is empty");
    }
    const auto key = actor_key (actor_ref);
    auto join_state = co_await actor_join_state_snapshot (actor_ref, spot_id, request);
    auto &join_snapshot = *join_state;
    if (!join_snapshot.context) {
        co_return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                         "target spot is not registered");
    }
    if (!join_snapshot.registration) {
        co_return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                         "actor factory is not registered");
    }
    auto &context = *join_snapshot.context;
    auto &registration = *join_snapshot.registration;
    const auto committed = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (join_snapshot.node_rid),
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
      std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation ());
    /* Registration is double-checked: an already-registered actor is taken
     * in one node state turn without touching the factory, and only a first
     * registration constructs outside the lane because the factory is user
     * code. The map entry and its identity index entry are then installed
     * together in one turn, so a
     * concurrent destroy never sees one without the other. */
    std::shared_ptr<void> actor_instance = join_snapshot.actor_instance;
    bool application_callback_crossed = false;
    if (!actor_instance) {
        std::shared_ptr<void> created_instance;
        if (registration.create_context_instance) {
            if (!actor_context._state) {
                co_return result_t<actor_join_reply_t>::failure (
                  framework_error_kind_t::not_configured,
                  "Actor factory requires a Framework Actor context");
            }
            if (actor_context.serializer_registry () == nullptr) {
                co_return result_t<actor_join_reply_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "Actor materialization Context has no serializer registry");
            }
            auto committed_context =
              actor_context_t (actor_context._state, committed, 0, join_snapshot.mesh_name);
            created_instance = registration.create_context_instance (std::move (committed_context));
        } else {
            created_instance =
              registration.create_instance (std::string (actor_ref.actor_id ().value ()));
        }
        application_callback_crossed = true;
        if (!created_instance) {
            co_return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                             "actor factory returned null");
        }
        actor_instance = install_actor_instance (actor_ref, key, std::move (created_instance));
        if (!actor_instance) {
            co_return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                             "actor has been destroyed");
        }
    }
    if (actor_snapshot) {
        registration.deserialize_instance (actor_instance.get (), *actor_snapshot,
                                           *join_snapshot.serializers);
        application_callback_crossed = true;
    }
    if (application_callback_crossed) {
        auto refreshed_admission =
          actor_admission (context, registration.actor_type, spot_id, actor_ref);
        if (!refreshed_admission) {
            co_return detail::propagate_failure<actor_join_reply_t> (refreshed_admission,
                                                                     "actor admission failed");
        }
        join_snapshot.admission.emplace (std::move (refreshed_admission.value ()));
    }
    if (!join_snapshot.admission) {
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
          dispatch_error_reason_t::handler_missing, dispatch_error_action_t::reply_error,
          "actor.join", std::nullopt, std::string (spot_id),
          std::string (actor_ref.actor_id ().value ()));
        co_return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found, "spot actor join callback is not registered");
    }

    auto &admission_callbacks = *join_snapshot.admission;
    auto &serializers = *join_snapshot.serializers;
    const auto response =
      context._state->is_entry_spot ()
        ? spot_actor_join_result_t::accept ()
        : admission_callbacks.join (join_snapshot.spot_instance.get (),
                                    actor_ref.actor_id ().value (), request, serializers);
    if (!response.accepted) {
        co_return result_t<actor_join_reply_t>::success (
          actor_join_reply_t{1, actor_ref, framework_reply_or_empty (response.reply, serializers)});
    }

    try {
        co_await seal_actor_application_suffix (actor_ref, [state = _state, key] {
            state->actor_transfer_coordinator->try_begin_local (key).value ();
        });
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<actor_join_reply_t> (error);
    }
    bool claimed_location = false;
    const auto location_claim = claim_pending_actor_location_before_activation (
      _state, actor_ref, join_snapshot.source_spot_id, committed, *context._state,
      claimed_location);
    if (!location_claim) {
        replay_actor_handoff_until_move_closed (actor_ref);
        co_return result_t<actor_join_reply_t>::failure (
          location_claim.error_kind (), location_claim.error () ? location_claim.error ()->what ()
                                                                : "actor location claim failed");
    }
    bool authority_committed = false;
    auto fail_local_commit = [&] {
        if (!authority_committed && (claimed_location || join_snapshot.source_spot_id.empty ())) {
            release_actor_location (_state, committed);
        }
        if (authority_committed) {
            _state->actor_transfer_coordinator->mark_reconcile (
              key, join_snapshot.message_follow_duration);
            replay_actor_handoff_until_move_closed (committed);
        } else {
            replay_actor_handoff_until_move_closed (actor_ref);
        }
    };
    try {
        if (!registration.create_context_instance) {
            registration.configure_instance (actor_instance.get (), committed, nullptr);
        }
        auto leave = co_await commit_accepted_actor_join (
          key, context, committed, registration.actor_type, actor_instance.get (),
          admission_callbacks, true, request,
          /* The operation id string only feeds the actor-transfer marker —
           * do not assemble it when the marker gate is off (spec 26 §4). */
          !actor_transfer_marker_enabled () || (operation_id_high == 0 && operation_id_low == 0)
            ? std::string{}
            : std::to_string (operation_id_high) + ":" + std::to_string (operation_id_low),
          authority_committed, deadline);
        if (source_leave)
            *source_leave = std::move (leave);
        else if (leave)
            leave ({});
    }
    catch (...) {
        fail_local_commit ();
        co_return detail::current_exception_result<actor_join_reply_t> ();
    }
    co_return result_t<actor_join_reply_t>::success (
      actor_join_reply_t{0, committed, framework_reply_or_empty (response.reply, serializers)});
}

task_t<actor_join_reply_t> spot_node_runtime_t::join_remote_actor_to_spot_erased (
  const actor_ref_t &actor_ref,
  spot_id_t spot_id,
  const zlink::message_t &request,
  actor_context_t actor_context,
  std::optional<std::chrono::steady_clock::time_point> deadline)
{
    /* graceful-drain-handoff §4-2/§5.2: a draining node rejects new actor
    * admission and joins; already-admitted transfer commits stay accepted. */
    if (_state->lane.run ([&] { return _state->host_draining (); }).get ()) {
        co_return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::shutting_down,
          "spot node is draining and rejects new actor joins");
    }
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        co_return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                         "actor ref is empty");
    }
    const auto key = actor_key (actor_ref);
    auto join_state = co_await actor_join_state_snapshot (actor_ref, spot_id, request);
    auto &join_snapshot = *join_state;
    if (!join_snapshot.context) {
        co_return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                         "target spot is not registered");
    }
    if (!join_snapshot.registration) {
        co_return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                         "actor factory is not registered");
    }
    auto &context = *join_snapshot.context;
    auto &registration = *join_snapshot.registration;
    const auto committed = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (join_snapshot.node_rid),
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
      std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation ());
    /* Registration is double-checked: an already-registered actor is taken
     * in one node state turn without touching the factory, and only a first
     * registration constructs outside the lane because the factory is user
     * code. The map entry and its identity index entry are then installed
     * together in one turn, so a
     * concurrent destroy never sees one without the other. */
    std::shared_ptr<void> actor_instance = join_snapshot.actor_instance;
    bool application_callback_crossed = false;
    if (!actor_instance) {
        std::shared_ptr<void> created_instance;
        if (registration.create_context_instance) {
            if (!actor_context._state) {
                co_return result_t<actor_join_reply_t>::failure (
                  framework_error_kind_t::not_configured,
                  "Actor factory requires a Framework Actor context");
            }
            auto committed_context =
              actor_context_t (actor_context._state, committed, 0, join_snapshot.mesh_name);
            created_instance = registration.create_context_instance (std::move (committed_context));
        } else {
            created_instance =
              registration.create_instance (std::string (actor_ref.actor_id ().value ()));
        }
        application_callback_crossed = true;
        if (!created_instance) {
            co_return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                             "actor factory returned null");
        }
        actor_instance = install_actor_instance (actor_ref, key, std::move (created_instance));
        if (!actor_instance) {
            co_return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                             "actor has been destroyed");
        }
    }
    if (!registration.create_context_instance) {
        auto committed_context =
          actor_context_t (actor_context._state, committed, 0, join_snapshot.mesh_name);
        registration.configure_instance (actor_instance.get (), committed, &committed_context);
        application_callback_crossed = true;
    }

    if (application_callback_crossed) {
        auto refreshed_admission =
          actor_admission (context, registration.actor_type, spot_id, actor_ref);
        if (!refreshed_admission) {
            co_return detail::propagate_failure<actor_join_reply_t> (refreshed_admission,
                                                                     "actor admission failed");
        }
        join_snapshot.admission.emplace (std::move (refreshed_admission.value ()));
    }
    if (!join_snapshot.admission) {
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
          dispatch_error_reason_t::handler_missing, dispatch_error_action_t::reply_error,
          "actor.join", std::nullopt, std::string (spot_id),
          std::string (actor_ref.actor_id ().value ()));
        co_return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found, "spot actor join callback is not registered");
    }

    auto &admission_callbacks = *join_snapshot.admission;
    auto &serializers = *join_snapshot.serializers;
    const auto response =
      context._state->is_entry_spot ()
        ? spot_actor_join_result_t::accept ()
        : admission_callbacks.join (join_snapshot.spot_instance.get (),
                                    actor_ref.actor_id ().value (), request, serializers);
    if (!response.accepted) {
        if (!registration.create_context_instance) {
            registration.configure_instance (actor_instance.get (), actor_ref, &actor_context);
        }
        co_return result_t<actor_join_reply_t>::success (
          actor_join_reply_t{1, actor_ref, framework_reply_or_empty (response.reply, serializers)});
    }

    bool claimed_location = false;
    const auto location_claim = claim_pending_actor_location_before_activation (
      _state, actor_ref, spot_id_t{}, committed, *context._state, claimed_location);
    if (!location_claim) {
        co_return result_t<actor_join_reply_t>::failure (
          location_claim.error_kind (), location_claim.error () ? location_claim.error ()->what ()
                                                                : "actor location claim failed");
    }
    bool authority_committed = false;
    try {
        auto leave = co_await commit_accepted_actor_join (
          key, context, committed, registration.actor_type, actor_instance.get (),
          admission_callbacks, false, request, key, authority_committed, deadline);
        if (leave)
            leave ({});
    }
    catch (...) {
        if (claimed_location && !authority_committed) {
            release_actor_location (_state, committed);
        }
        throw;
    }
    co_return result_t<actor_join_reply_t>::success (
      actor_join_reply_t{0, committed, framework_reply_or_empty (response.reply, serializers)});
}

task_t<std::optional<std::chrono::steady_clock::time_point>>
spot_node_runtime_t::next_management_activity_on_lane_async (
  std::shared_ptr<spot_node_builder_state_t> state,
  std::optional<std::chrono::steady_clock::time_point> next)
{
    const auto pending = co_await state->lane.run_task ([state, next] () mutable {
        std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> cleanups;
        for (const auto &entry : state->pending_handoff_requests) {
            if (!next || entry.second.deadline < *next)
                next = entry.second.deadline;
        }
        for (const auto &cleanup : state->pending_remote_source_cleanups) {
            const auto deadline = cleanup.leave_completed
                                    ? cleanup.not_before
                                    : std::max (cleanup.not_before, cleanup.leave_deadline);
            cleanups.emplace_back (actor_key (cleanup.source_actor), deadline);
        }
        return std::pair{next, std::move (cleanups)};
    });
    next = pending.first;
    for (const auto &[key, deadline] : pending.second) {
        if (co_await state->actor_transfer_coordinator->blocks_dispatch_async (key))
            continue;
        if (!next || deadline < *next)
            next = deadline;
    }
    co_return next;
}

task_t<std::optional<std::chrono::steady_clock::time_point>>
spot_node_runtime_t::next_management_activity_async () const
{
    std::optional<std::chrono::steady_clock::time_point> next;
    try {
        next = _state->actor_transfer_coordinator->next_activity ();
        // The owner already published an exact bound unless pending cleanup
        // requires its lane. Keep that decision before creating an awaitable frame.
        if (const auto own = _state->management_next_due.load (std::memory_order_acquire);
            own != spot_node_builder_state_t::management_due_now) {
            if (own != spot_node_builder_state_t::management_never_due) {
                const auto deadline =
                  std::chrono::steady_clock::time_point (std::chrono::steady_clock::duration (own));
                if (!next || deadline < *next)
                    next = deadline;
            }
            return task_t<std::optional<std::chrono::steady_clock::time_point>> (
              result_t<std::optional<std::chrono::steady_clock::time_point>>::success (next));
        }
    }
    catch (...) {
        return task_t<std::optional<std::chrono::steady_clock::time_point>> (
          result_access_t::failure<std::optional<std::chrono::steady_clock::time_point>> (
            std::current_exception ()));
    }
    return next_management_activity_on_lane_async (_state, next);
}

// Runs on the spot state lane after the management sweep.
static void publish_management_next_due_on_lane (spot_node_builder_state_t &state)
{
    auto next = spot_node_builder_state_t::management_never_due;
    if (!state.pending_remote_actor_leaves.empty ()
        || !state.pending_remote_source_cleanups.empty ())
        next = spot_node_builder_state_t::management_due_now;
    else
        for (const auto &entry : state.pending_handoff_requests)
            next = std::min (next, entry.second.deadline.time_since_epoch ().count ());
    state.management_next_due.store (next, std::memory_order_release);
}

static bool spot_management_due (const spot_node_builder_state_t &state,
                                 std::chrono::steady_clock::time_point now) noexcept
{
    return now.time_since_epoch ().count ()
           >= state.management_next_due.load (std::memory_order_acquire);
}

std::size_t spot_node_runtime_t::cleanup_expired_actor_admissions ()
{
    return cleanup_expired_actor_admissions_at (std::chrono::steady_clock::now ());
}

static task_t<std::optional<std::chrono::steady_clock::time_point>>
advance_due_spot_management_async (std::shared_ptr<spot_node_builder_state_t> state,
                                   std::chrono::steady_clock::time_point now,
                                   bool include_next_activity)
{
    spot_node_runtime_t owner (std::move (state));
    co_await owner.cleanup_expired_actor_admissions_at_async (now);
    if (!include_next_activity)
        co_return std::nullopt;
    co_return co_await owner.next_management_activity_async ();
}

static task_t<std::optional<std::chrono::steady_clock::time_point>>
advance_spot_management_async (std::shared_ptr<spot_node_builder_state_t> state,
                               bool include_next_activity)
{
    const auto now = std::chrono::steady_clock::now ();
    // Spec 08 section 4.2: the existing published bounds decide whether a
    // sweep is due. A known result does not need another coroutine owner.
    if (!spot_management_due (*state, now) && !state->actor_transfer_coordinator->due (now)) {
        try {
            if (!include_next_activity)
                return task_t<std::optional<std::chrono::steady_clock::time_point>> (
                  result_t<std::optional<std::chrono::steady_clock::time_point>>::success (
                    std::nullopt));
            return spot_node_runtime_t (std::move (state)).next_management_activity_async ();
        }
        catch (...) {
            return task_t<std::optional<std::chrono::steady_clock::time_point>> (
              result_access_t::failure<std::optional<std::chrono::steady_clock::time_point>> (
                std::current_exception ()));
        }
    }
    return advance_due_spot_management_async (std::move (state), now, include_next_activity);
}

task_t<std::optional<std::chrono::steady_clock::time_point>>
spot_node_runtime_t::advance_management_async (bool include_next_activity)
{
    return advance_spot_management_async (_state, include_next_activity);
}

std::size_t spot_node_runtime_t::cleanup_expired_actor_admissions_at (
  std::chrono::steady_clock::time_point now, std::function<void ()> after_coordinator_snapshot)
{
    return cleanup_expired_actor_admissions_at_async (now, std::move (after_coordinator_snapshot))
      .result ()
      .value ();
}

task_t<std::size_t> spot_node_runtime_t::cleanup_expired_actor_admissions_at_async (
  std::chrono::steady_clock::time_point now, std::function<void ()> after_coordinator_snapshot)
{
    struct source_cleanup_identity_t
    {
        std::string key;
        std::string transfer_id;
        runtime::protocol::actor_route_fence_t source_fence;
    };
    struct node_snapshot_t
    {
        std::vector<source_cleanup_identity_t> source_cleanups;
        std::vector<std::string> blocked_candidates;
    };
    struct sweep_result_t
    {
        std::size_t removed = 0;
        std::vector<spot_node_builder_state_t::pending_handoff_request_t> expired_requests;
        std::vector<spot_node_builder_state_t::pending_remote_source_cleanup_t> cleaned_sources;
        std::vector<actor_ref_t> released_sources;
    };
    const auto state = _state;
    auto identities = co_await state->lane.run_task ([state] {
        node_snapshot_t result;
        result.source_cleanups.reserve (state->pending_remote_source_cleanups.size ());
        result.blocked_candidates.reserve (state->pending_remote_source_cleanups.size ());
        for (const auto &cleanup : state->pending_remote_source_cleanups) {
            const auto key = actor_key (cleanup.source_actor);
            result.source_cleanups.push_back ({key, cleanup.transfer_id, cleanup.source_fence});
            result.blocked_candidates.push_back (key);
        }
        return result;
    });
    auto snapshot = co_await state->actor_transfer_coordinator->cleanup_expired_async (
      now, std::move (identities.blocked_candidates));
    const auto abandoned_leaves = co_await settle_abandoned_actor_leave_reservations_async (
      std::move (after_coordinator_snapshot));
    if (actor_transfer_marker_enabled ()) {
        for (const auto &entry : snapshot.expired_admissions)
            emit_actor_transfer_marker ("pending_admission_expired", entry.admission.source_actor,
                                        entry.transfer_id, entry.admission.target_spot_id);
    }
    std::size_t removed = snapshot.expired_admissions.size () + abandoned_leaves;
    for (auto &entry : snapshot.expired_admissions)
        fail_handoff_backlog (state, std::move (entry.discarded_backlog));
    auto sweep = co_await state->lane.run_task ([state, now, &identities, &snapshot] {
        sweep_result_t result;
        for (auto pending = state->pending_handoff_requests.begin ();
             pending != state->pending_handoff_requests.end ();) {
            if (pending->second.deadline <= now) {
                result.expired_requests.push_back (std::move (pending->second));
                pending = state->pending_handoff_requests.erase (pending);
            } else {
                ++pending;
            }
        }
        result.removed += result.expired_requests.size ();
        for (auto found = state->pending_remote_source_cleanups.begin ();
             found != state->pending_remote_source_cleanups.end ();) {
            const auto key = actor_key (found->source_actor);
            const bool in_snapshot =
              std::ranges::any_of (identities.source_cleanups, [&] (const auto &identity) {
                  return identity.key == key && identity.transfer_id == found->transfer_id
                         && identity.source_fence == found->source_fence;
              });
            if (!in_snapshot || found->not_before > now
                || (!found->leave_completed && found->leave_deadline > now)
                || std::binary_search (snapshot.blocked_dispatch_keys.begin (),
                                       snapshot.blocked_dispatch_keys.end (), key)) {
                ++found;
                continue;
            }
            const auto current_fence = state->actor_authority_fences.find (key);
            const bool actor_has_newer_local_authority =
              current_fence != state->actor_authority_fences.end ()
              && current_fence->second != found->source_fence;
            if (!actor_has_newer_local_authority) {
                state->actor_instances.erase (key);
                detail::erase_actor_instance_index_unlocked (
                  *state,
                  ::zlink::framework::detail::actor_ref_access_t::actor_type (found->source_actor),
                  found->source_actor.actor_id ().value ());
                result.released_sources.push_back (found->source_actor);
                if (current_fence != state->actor_authority_fences.end ()
                    && current_fence->second == found->source_fence)
                    state->actor_authority_fences.erase (current_fence);
            }
            result.cleaned_sources.push_back (std::move (*found));
            found = state->pending_remote_source_cleanups.erase (found);
            ++result.removed;
        }
        publish_management_next_due_on_lane (*state);
        return result;
    });
    removed += sweep.removed;
    for (auto &pending : sweep.expired_requests)
        settle_pending_handoff_request (std::move (pending), result_t<zlink::message_t>::failure (
                                                               framework_error_kind_t::unavailable,
                                                               "Actor handoff request expired"));
    for (const auto &actor : sweep.released_sources)
        co_await release_actor_location_async (state, actor);
    if (actor_transfer_marker_enabled ()) {
        for (const auto &cleanup : sweep.cleaned_sources)
            emit_actor_transfer_marker ("source_cleanup", cleanup.source_actor, cleanup.transfer_id,
                                        cleanup.target_spot_id);
    }
    const auto removed_message_follow_routes =
      _state->actor_transfer_coordinator->remove_expired_message_follow (now);
    std::vector<std::pair<actor_ref_t, std::string>> removed_route_markers;
    if (!removed_message_follow_routes.empty ()) {
        std::vector<bool> retained_follow_routes;
        retained_follow_routes.reserve (removed_message_follow_routes.size ());
        for (const auto &entry : removed_message_follow_routes)
            retained_follow_routes.push_back (
              _state->actor_transfer_coordinator->has_message_follow_route (entry.actor_key));
        co_await _state->lane.run_task ([&] {
            const auto local_node_rid =
              node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot));
            for (std::size_t index = 0; index < removed_message_follow_routes.size (); ++index) {
                const auto &entry = removed_message_follow_routes[index];
                const auto &key = entry.actor_key;
                // Message Follow ended (§10.4-3): remove the retained route. The
                // current local authority and any other retained source fences
                // remain independent from this expired source route.
                erase_retired_actor_route_on_lane (*_state, entry, retained_follow_routes[index]);
                const auto separator = key.find (':');
                if (separator != std::string::npos) {
                    removed_route_markers.emplace_back (
                      ::zlink::framework::detail::actor_ref_access_t::make (
                        local_node_rid, key.substr (0, separator), key.substr (separator + 1),
                        entry.source_fence.object_generation),
                      entry.transfer_id.empty () ? key : entry.transfer_id);
                }
                ++removed;
            }
            return true;
        });
    }
    if (actor_transfer_marker_enabled ()) {
        for (const auto &[actor_ref, transfer_id] : removed_route_markers)
            emit_actor_transfer_marker ("message_follow_route_removed", actor_ref, transfer_id);
    }
    // After relay-ready is accepted, an unknown cutover outcome cannot reopen
    // source dispatch (04-relocation-flow §9). At the reconciliation deadline,
    // adopt a visible target commit or fail the parked requests Unavailable.
    // A source-owner snapshot cannot authorize replay: the target may still
    // commit within its Restore validity.
    const auto expired_reconciles = _state->actor_transfer_coordinator->take_due_reconciles (now);
    for (const auto &expired : expired_reconciles) {
        const auto &key = expired.actor_key;
        const auto separator = key.find (':');
        if (separator == std::string::npos)
            continue;
        const auto [generation, local_node_rid] = co_await _state->lane.run_task ([&] {
            std::uint64_t value = 0;
            if (const auto found = _state->actor_generations.find (key);
                found != _state->actor_generations.end ()) {
                value = found->second;
            }
            return std::make_pair (
              value, node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot)));
        });
        const auto actor_ref = ::zlink::framework::detail::actor_ref_access_t::make (
          local_node_rid, key.substr (0, separator), key.substr (separator + 1), generation);

        if (!expired.context) {
            // No target identity was captured for this reconcile (a call
            // site outside spec 28's relay-ready-ambiguous FINALIZE case,
            // e.g. a target-local post-authority-commit failure): unchanged
            // prior behavior, not the Location-Store-reconciled outcomes
            // below, which require the captured identity to compare against.
            co_await replay_actor_handoff_until_move_closed_async (actor_ref);
            ++removed;
            continue;
        }

        bool target_committed = false;
        {
            const auto relocation_authority =
              co_await _state->lane.run_task ([&] { return _state->relocation_authority; });
            if (relocation_authority) {
                try {
                    const auto current = relocation_authority->read (
                      runtime::stateful::object_kind_t::actor, key.substr (separator + 1));
                    target_committed =
                      current && current->target.key == expired.context->target_fence.actor_id
                      && current->target.object_generation
                           == expired.context->target_fence.object_generation
                      && current->target.node_id
                           == expired.context->target_actor.node_rid ().value ()
                      && current->target.authority_owner_generation
                           == expired.context->target_fence.authority_owner_generation;
                }
                catch (...) {
                    target_committed = false;
                }
            }
        }
        if (actor_transfer_marker_enabled ()) {
            emit_actor_transfer_marker (target_committed ? "reconcile_deadline_adopted_target"
                                                         : "reconcile_deadline_fast_failed",
                                        actor_ref, key);
        }
        if (target_committed) {
            const auto state = _state;
            const auto ctx = *expired.context;
            const auto transfer_id = ctx.transfer_id.empty () ? key : ctx.transfer_id;
            bool submitted = false;
            const auto worker = co_await _state->lane.run_task ([&] {
                return _state->root_services ? framework_worker_executor_core (_state)
                                             : std::shared_ptr<runtime::offload_executor_t>{};
            });
            if (worker) {
                submitted = worker->try_submit_internal ([state, actor_ref, ctx, transfer_id] {
                    auto owner = std::make_shared<spot_node_runtime_t> (state);
                    auto task = owner->complete_remote_actor_transfer (
                      actor_ref, ctx.target_actor, ctx.target_route, ctx.source_fence,
                      {ctx.target_fence.target_node_routing_id,
                       ctx.target_fence.target_node_generation,
                       {},
                       ctx.target_fence.owner_lease_generation},
                      ctx.target_fence.authority_owner_generation, transfer_id);
                    detail::observe_task_completion (task, [owner] (const result_t<void> &) {});
                });
            }
            if (!submitted) {
                // Executor unavailable: cannot safely adopt the target route
                // without a place to drain the backlog through, so fall back
                // to the same fast-fail as the indeterminate case.
                auto task = fast_fail_reconcile_backlog (actor_ref, key);
                detail::observe_task_completion (task, [] (const result_t<void> &) {});
            }
        } else {
            auto task = fast_fail_reconcile_backlog (actor_ref, key);
            detail::observe_task_completion (task, [] (const result_t<void> &) {});
        }
        ++removed;
    }
    co_return removed;
}

std::vector<handoff_packet_t>
spot_node_runtime_t::take_actor_handoff_backlog (const actor_ref_t &actor_ref)
{
    return _state->actor_transfer_coordinator->take_backlog (actor_key (actor_ref));
}

bool spot_node_runtime_t::actor_transfer_in_progress (const actor_ref_t &actor_ref) const
{
    return !::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)
           && _state->actor_transfer_coordinator->blocks_dispatch (actor_key (actor_ref));
}


task_t<bool> spot_node_runtime_t::actor_transfer_in_progress_async (actor_ref_t actor) const
{
    const auto state = _state;
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor))
        return task_t<bool> (result_t<bool>::success (false));
    return state->actor_transfer_coordinator->blocks_dispatch_async (actor_key (actor));
}

std::optional<spot_id_t> spot_node_runtime_t::resolve_entry_spot_id () const
{
    return _state->lane
      .run ([&] () -> std::optional<spot_id_t> {
          const auto node = _state->native_node.lock ();
          if (!node)
              return std::nullopt;
          return spot_id_t (node->entry_spot ().spot_id ());
      })
      .get ();
}

task_t<bool> spot_node_runtime_t::entry_accepts_actor (std::string stable_type) const
{
    return _state->lane.run_task ([state = _state, stable_type = std::move (stable_type)] {
        const auto factory = state->actor_factories.find (stable_type);
        if (factory == state->actor_factories.end () || !state->snapshot.entry_spot_name)
            return false;
        const auto entry_id = state->spot_ids_by_name.find (*state->snapshot.entry_spot_name);
        if (entry_id == state->spot_ids_by_name.end ())
            return false;
        const auto entry = state->spot_contexts_by_id.find (std::string (entry_id->second));
        return entry != state->spot_contexts_by_id.end () && entry->second._state
               && entry->second._state->actor_admissions.contains (factory->second.actor_type);
    });
}

void spot_node_runtime_t::dispatch_wire_actor_join_admission (
  const spot_id_t &target_spot_id,
  std::string_view actor_id,
  std::function<task_t<void> ()> admission,
  std::function<void (result_t<void>)> rejected,
  std::function<void ()> completed)
{
    const auto selected = find_active_remote_actor_join_target (target_spot_id);
    if (!selected) {
        rejected (detail::propagate_failure<void> (selected, "Actor Join target admission failed"));
        return;
    }
    const auto target = selected.value ();
    auto finished = [rejected = std::move (rejected),
                     completed = std::move (completed)] (result_t<void> outcome) {
        if (!outcome)
            rejected (outcome);
        if (completed)
            completed ();
    };
    // The local type projection names a retained source move; it does not
    // replace the Store authority/type validation inside admission. Local
    // membership controls supply no Actor ID because their source reservation
    // belongs to the very Join this lifecycle item will complete.
    const auto type = resolve_actor_type (actor_id);
    auto source = type ? _state->actor_transfer_coordinator->wait_for_source_completion (
                           *type + ":" + std::string (actor_id))
                       : task_t<void> (result_t<void>::success ());
    // Observe before submitting the target lifecycle item: its gate must stay
    // available while the coordinator completes the previous source move.
    detail::observe_task_completion (
      source, [target, admission = std::move (admission),
               finished = std::move (finished)] (const result_t<void> &outcome) mutable {
          if (!outcome) {
              finished (outcome);
              return;
          }
          target->run_serial_task_async ("spot-actor-admission", std::move (admission),
                                         std::move (finished));
      });
}

result_t<std::shared_ptr<spot_context_state_t>>
spot_node_runtime_t::find_active_remote_actor_join_target (const spot_id_t &target_spot_id) const
{
    return _state->lane
      .run ([&] () -> result_t<std::shared_ptr<spot_context_state_t>> {
          const auto found = _state->spot_contexts_by_id.find (std::string (target_spot_id));
          if (found == _state->spot_contexts_by_id.end ())
              return result_t<std::shared_ptr<spot_context_state_t>>::failure (
                framework_error_kind_t::unavailable, "target spot is not registered");
          if (found->second._state) {
              const auto context = found->second._state;
              if (context->accepts_membership_core (*_state))
                  return result_t<std::shared_ptr<spot_context_state_t>>::success (context);
              if (context->closed)
                  return detail::result_access_t::failure<std::shared_ptr<spot_context_state_t>> (
                    context->admission_error ());
          }
          return result_t<std::shared_ptr<spot_context_state_t>>::failure (
            framework_error_kind_t::unavailable, "target spot is not active locally");
      })
      .get ();
}

task_t<spot_actor_join_result_t> spot_node_runtime_t::admit_remote_actor_to_spot (
  std::string transfer_id,
  const actor_ref_t &actor_ref,
  spot_id_t source_spot_id,
  spot_id_t target_spot_id,
  const zlink::message_t &request,
  std::uint64_t completion_operation_id_high,
  std::uint64_t completion_operation_id_low,
  std::uint64_t actor_authority_owner_generation,
  std::uint64_t actor_node_generation,
  std::uint64_t expected_owner_lease_generation,
  bool actor_type_from_authority_only,
  std::uint64_t target_spot_generation,
  std::uint64_t target_spot_authority_owner_generation)
{
    /* graceful-drain-handoff §4-2/§5.2: a draining node rejects new actor
    * admission and joins; already-admitted transfer commits stay accepted. */
    if (_state->lane.run ([&] { return _state->host_draining (); }).get ()) {
        return task_t<spot_actor_join_result_t> (result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::shutting_down,
          "spot node is draining and rejects new actor admission"));
    }
    if (transfer_id.empty () || ::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return task_t<spot_actor_join_result_t> (result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote actor admission requires transfer and actor identity"));
    }

    auto services = _state->lane.run ([&] { return _state->root_services; }).get ();
    if (!services) {
        return task_t<spot_actor_join_result_t> (result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::unavailable, "remote Actor Join Location Store is unavailable"));
    }
    runtime::live_location_reader_t *store = nullptr;
    try {
        store = &services->get_required<runtime::live_location_reader_t> ();
    }
    catch (const std::exception &) {
        return task_t<spot_actor_join_result_t> (result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::unavailable, "remote Actor Join Location Store is unavailable"));
    }
    const auto stable_type = actor_type_from_authority (
      *store, actor_ref,
      actor_join_authority_fence_t{actor_ref.object_generation (), actor_node_generation,
                                   actor_authority_owner_generation,
                                   expected_owner_lease_generation},
      actor_type_from_authority_only);
    if (!stable_type) {
        return task_t<spot_actor_join_result_t> (
          detail::propagate_failure<spot_actor_join_result_t> (
            stable_type, "remote Actor Join authority resolution failed"));
    }
    const auto store_actor = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (std::string (actor_ref.node_rid ().value ())), stable_type.value (),
      std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation ());

    cleanup_expired_actor_admissions ();
    auto selected = find_active_remote_actor_join_target (target_spot_id);
    if (!selected)
        return task_t<spot_actor_join_result_t> (
          detail::propagate_failure<spot_actor_join_result_t> (
            selected, "Actor Join target admission failed"));
    auto target_state = selected.value ();

    std::optional<spot_node_builder_state_t::actor_factory_registration_t> actor_factory;
    std::optional<spot_actor_admission_callbacks_t> admission;
    std::chrono::milliseconds admission_timeout;
    _state->lane
      .run ([&] {
          const auto factory = _state->actor_factories.find (
            std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (store_actor)));
          if (factory == _state->actor_factories.end ())
              return;
          actor_factory = factory->second;
          const auto found_admission =
            target_state->actor_admissions.find (factory->second.actor_type);
          if (found_admission != target_state->actor_admissions.end ()
              && (target_state->is_entry_spot () || found_admission->second.join)) {
              admission = found_admission->second;
          }
          admission_timeout =
            _state->channel_runtime
              ? _state->channel_runtime->default_request_timeout
              : std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::seconds (30));
      })
      .get ();
    if (!actor_factory) {
        return task_t<spot_actor_join_result_t> (result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::rejected,
          "remote Actor Join Authority stable type is not registered locally"));
    }
    if (!admission) {
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
          dispatch_error_reason_t::handler_missing, dispatch_error_action_t::reply_error,
          "actor.join", std::nullopt, std::string (target_spot_id),
          std::string (store_actor.actor_id ().value ()));
        return task_t<spot_actor_join_result_t> (result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::not_found, "spot actor join callback is not registered"));
    }

    auto &target = *target_state;
    // A resend of an admitted attempt belongs to its existing preparation and
    // does not invoke the admission callback a second time.
    if (const auto existing = _state->actor_transfer_coordinator->admission (transfer_id)) {
        if (!existing->matches_prepare (store_actor, source_spot_id, target_spot_id,
                                        completion_operation_id_high,
                                        completion_operation_id_low)) {
            return task_t<spot_actor_join_result_t> (result_t<spot_actor_join_result_t>::failure (
              framework_error_kind_t::protocol_error,
              "remote actor admission conflicts with the pending prepare"));
        }
        return task_t<spot_actor_join_result_t> (result_t<spot_actor_join_result_t>::success (
          spot_actor_join_result_t{true, existing->admission_reply}));
    }
    // Displace an older attempt before submitting the newer admission.
    std::vector<handoff_packet_t> displaced_backlog;
    (void) _state->actor_transfer_coordinator->admit_attempt (actor_key (store_actor), transfer_id,
                                                              displaced_backlog);
    fail_handoff_backlog (_state, std::move (displaced_backlog));
    // Admission dispatch has observed the previous source terminal before
    // entering the target lifecycle. Normal callback rejection does not install
    // an admission; accepted attempts arbitrate in try_add_admission.
    /* The admission callback is user code on the target Spot's lifecycle
     * lane; its item completes this task. A caller that holds that turn runs
     * it inline. The state lane is not held across it. */
    struct admission_outcome_t
    {
        spot_actor_join_result_t response;
        bool admission_conflict = false;
    };
    auto outcome = std::make_shared<admission_outcome_t> ();
    auto admitted = std::make_shared<task_completion_source_t<spot_actor_join_result_t>> ();
    auto result = admitted->task ();
    auto state = _state;
    target.run_serial_task_async (
      "spot-actor-admission",
      [state, target_state, outcome, admission = *admission, actor_factory = *actor_factory,
       actor_id = std::string (actor_ref.actor_id ().value ()), request,
       serializers = target.channel_runtime->serializers, actor_type_from_authority_only,
       transfer_id, store_actor, source_spot_id, target_spot_id, admission_timeout,
       target_spot_generation, target_spot_authority_owner_generation,
       actor_authority_owner_generation, actor_node_generation, expected_owner_lease_generation,
       completion_operation_id_high, completion_operation_id_low] {
          auto &target = *target_state;
          auto &response = outcome->response;
          response = target.is_entry_spot () ? spot_actor_join_result_t{true, {}}
                                             : admission.join (target.spot_instance.get (),
                                                               actor_id, request, *serializers);
          if (response.accepted
              && !state->actor_transfer_coordinator->try_add_admission (
                transfer_id,
                pending_actor_admission_t{
                  .actor_key = actor_key (store_actor),
                  .source_actor = store_actor,
                  .source_spot_id = source_spot_id,
                  .target_spot_id = target_spot_id,
                  .canonical_application_join =
                    actor_type_from_authority_only && !target.is_instance_spot (),
                  .deadline = std::chrono::steady_clock::now () + admission_timeout,
                  .completion_operation_id_high = completion_operation_id_high,
                  .completion_operation_id_low = completion_operation_id_low,
                  .reservation_token = transfer_id,
                  .reserved_payload_bytes = runtime::protocol::actor_join_reserved_payload_bytes (
                    request.size (), actor_factory.relocation.kind
                                         == detail::factory_relocation_kind_t::preserve_state
                                       ? runtime::protocol::actor_join_snapshot_content_type
                                       : runtime::protocol::actor_join_recreate_content_type),
                  .target_spot_generation = target_spot_generation,
                  .target_spot_authority_owner_generation = target_spot_authority_owner_generation,
                  .source_actor_authority_owner_generation = actor_authority_owner_generation,
                  .source_node_generation = actor_node_generation,
                  .source_owner_lease_generation = expected_owner_lease_generation,
                  .admission_reply = response.reply})) {
              outcome->admission_conflict = true;
          }
          return task_t<void> (result_t<void>::success ());
      },
      [state, outcome, admitted, transfer_id, completion_operation_id_high,
       completion_operation_id_low, actor_authority_owner_generation] (result_t<void> ran) mutable {
          if (!ran) {
              admitted->complete (detail::propagate_failure<spot_actor_join_result_t> (
                ran, "remote actor admission failed"));
              return;
          }
          if (outcome->admission_conflict) {
              admitted->complete (result_t<spot_actor_join_result_t>::failure (
                framework_error_kind_t::unavailable,
                "remote actor admission conflicts with the current membership"));
              return;
          }
          if (outcome->response.accepted
              && ((completion_operation_id_high == 0 && completion_operation_id_low == 0)
                  || actor_authority_owner_generation == 0
                  || actor_authority_owner_generation
                       == std::numeric_limits<std::uint64_t>::max ())) {
              fail_handoff_backlog (
                state, state->actor_transfer_coordinator->fail_commit (transfer_id, false));
              admitted->complete (result_t<spot_actor_join_result_t>::failure (
                framework_error_kind_t::protocol_error,
                "remote Actor Join completion identity is invalid"));
              return;
          }
          admitted->complete (
            result_t<spot_actor_join_result_t>::success (std::move (outcome->response)));
      });
    return result;
}

task_t<result_t<std::string>> spot_node_runtime_t::transfer_actor_out (const actor_ref_t &actor_ref,
                                                                       std::string transfer_id)
{
    const auto key = actor_key (actor_ref);
    auto content_type =
      _state->lane
        .run ([&] () -> result_t<std::string> {
            const auto actor = _state->actor_instances.find (key);
            if (actor == _state->actor_instances.end () || !actor->second
                || !_state->actor_spot_ids.contains (key)) {
                return result_t<std::string>::failure (
                  framework_error_kind_t::not_found, "source actor is not joined to a local spot");
            }
            const auto factory = _state->actor_factories.find (
              std::string (detail::actor_ref_access_t::actor_type (actor_ref)));
            if (factory == _state->actor_factories.end ()) {
                return result_t<std::string>::failure (framework_error_kind_t::not_found,
                                                       "source actor factory is not registered");
            }
            const auto type =
              factory->second.relocation.kind == detail::factory_relocation_kind_t::preserve_state
                ? runtime::protocol::actor_join_snapshot_content_type
                : runtime::protocol::actor_join_recreate_content_type;
            return result_t<std::string>::success (std::string (type));
        })
        .get ();
    if (!content_type)
        co_return content_type;

    try {
        co_await seal_actor_application_suffix (actor_ref, [state = _state, key, transfer_id] {
            state->actor_transfer_coordinator->try_begin_source_remote (key, transfer_id).value ();
        });
    }
    catch (const framework_exception_t &error) {
        replay_actor_handoff_until_move_closed (actor_ref);
        co_return detail::result_access_t::failure<std::string> (error);
    }
    co_return content_type;
}

std::string spot_node_runtime_t::next_actor_transfer_id ()
{
    const auto local_node_rid =
      _state->lane.run ([&] { return detail::effective_spot_node_rid (_state->snapshot); }).get ();
    return _state->actor_transfer_coordinator->next_transfer_id (local_node_rid);
}

std::optional<std::string>
spot_node_runtime_t::reserved_actor_transfer_id (const actor_ref_t &actor_ref) const
{
    const auto key = actor_key (actor_ref);
    if (_state->actor_transfer_coordinator->phase (key) != actor_move_phase_t::source_reserved) {
        return std::nullopt;
    }
    return _state->actor_transfer_coordinator->transfer_id (key);
}

std::pair<std::uint64_t, std::uint64_t>
spot_node_runtime_t::actor_join_operation_id (std::string_view transfer_id) const
{
    /*
     * Actor Join completion replay needs an ID that is independent from the
     * relocation ID. Derive two 64-bit words with independent FNV-1a domains
     * from the source-generated transfer ID. The transfer ID already includes
     * the source node identity and a monotonically increasing sequence.
     */
    constexpr std::uint64_t fnv_prime = 1099511628211ULL;
    auto hash = [] (std::string_view value, std::uint64_t seed) {
        constexpr std::uint64_t prime = 1099511628211ULL;
        auto result = seed;
        for (const auto byte : value) {
            result ^= static_cast<unsigned char> (byte);
            result *= prime;
        }
        return result;
    };
    auto high = hash (transfer_id, 14695981039346656037ULL);
    auto low = hash (transfer_id, 1099511628211ULL);
    low ^= static_cast<std::uint64_t> (transfer_id.size ());
    low *= fnv_prime;
    if (high == 0 && low == 0)
        low = 1;
    return {high, low};
}

result_t<std::shared_ptr<deferred_barrier_t>>
spot_node_runtime_t::reserve_actor_join_barrier (const actor_ref_t &actor_ref)
{
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<std::shared_ptr<deferred_barrier_t>>::failure (
          framework_error_kind_t::not_found, "Actor join barrier source is empty");
    }

    const auto key = actor_key (actor_ref);
    const auto reservation_id = next_actor_transfer_id ();
    const auto claimed =
      _state->actor_transfer_coordinator->try_reserve_source (key, reservation_id);
    if (!claimed)
        return detail::propagate_failure<std::shared_ptr<deferred_barrier_t>> (
          claimed, "Actor join reservation failed");

    auto coordinator =
      _state->lane
        .run ([&] {
            const auto location = _state->actor_spot_ids.find (key);
            if (location == _state->actor_spot_ids.end ())
                return std::shared_ptr<detail::spot_serial_executor_t>{};
            const auto context = _state->spot_contexts_by_id.find (std::string (location->second));
            return context == _state->spot_contexts_by_id.end () || !context->second._state
                     ? std::shared_ptr<detail::spot_serial_executor_t>{}
                     : context->second._state->ensure_spot_serial_executor ();
        })
        .get ();
    if (!coordinator) {
        fail_handoff_backlog (_state, _state->actor_transfer_coordinator->cancel_move (key));
        return result_t<std::shared_ptr<deferred_barrier_t>>::failure (
          framework_error_kind_t::not_found, "Actor join barrier target is unavailable");
    }
    auto cancel_reservation = [state = _state, key, reservation_id] {
        fail_handoff_backlog (state,
                              state->actor_transfer_coordinator->cancel_move (key, reservation_id));
    };
    auto settle_reservation = [state = _state, actor_ref,
                               reservation_id] (deferred_barrier_t::async_completion_t release) {
        auto owner = std::make_shared<spot_node_runtime_t> (state);
        auto opened = owner->open_local_actor_handoff (actor_ref, reservation_id);
        detail::observe_task_completion (opened, [owner, release = std::move (release)] (
                                                   const result_t<result_t<void>> &terminal) {
            release (terminal ? terminal.value ()
                              : detail::result_access_t::failure<void> (*terminal.error ()));
        });
    };
    const auto executor = coordinator->actor_executor (key);
    if (!executor) {
        cancel_reservation ();
        return result_t<std::shared_ptr<deferred_barrier_t>>::failure (
          framework_error_kind_t::shutting_down, "Deferred Actor join mailbox is unavailable");
    }
    return executor->queue ()->reserve_deferred_barrier_next (
      "actor-join", std::move (cancel_reservation), std::move (settle_reservation));
}

void spot_node_runtime_t::cancel_local_actor_handoff (const actor_ref_t &actor_ref)
{
    fail_handoff_backlog (_state,
                          _state->actor_transfer_coordinator->cancel_move (actor_key (actor_ref)));
}

task_t<void> spot_node_runtime_t::seal_actor_application_suffix (const actor_ref_t &actor_ref,
                                                                 std::function<void ()> seal)
{
    auto suffix = co_await take_actor_application_suffix (actor_ref, std::move (seal));
    std::vector<handoff_packet_t> preserved;
    preserved.reserve (suffix.size ());
    for (auto &record : suffix) {
        handoff_packet_t packet;
        std::optional<spot_inbound_message_t> invocation_metadata;
        if (auto *invocation = record->invocation ())
            invocation_metadata = std::move (invocation->second);
        runtime::stateful::with_accepted_application (
          *record->payload (), [&] (auto kind, const auto &application) {
              const runtime::messaging::message_parts_t parts (
                runtime::protocol::decode_application_parts (application));
              const auto header = detail::decode_actor_packet_header (
                parts,
                kind == runtime::protocol::frozen_record_kind_t::actor_request
                  ? detail::stream_message_kind_t::request
                  : detail::stream_message_kind_t::send,
                false);
              if (!header)
                  std::rethrow_exception (header.exception ());
              packet.packet_name = header.value ().message_name;
              packet.content_type = invocation_metadata
                                      ? std::move (invocation_metadata->content_type)
                                      : std::string (header.value ().content_type);
              packet.metadata = invocation_metadata ? std::move (invocation_metadata->values)
                                                    : header.value ().metadata.values ();
              packet.is_request = kind == runtime::protocol::frozen_record_kind_t::actor_request;
          });
        packet.application_record = std::move (record);
        preserved.push_back (std::move (packet));
    }
    _state->actor_transfer_coordinator->prepend_source_suffix (actor_key (actor_ref),
                                                               std::move (preserved));
}

task_t<std::shared_ptr<runtime::serial_execution_queue_t>>
spot_node_runtime_t::actor_application_queue (std::string_view actor_id) const
{
    return _state->lane.run_task ([state = _state, actor_id = std::string (actor_id)] {
        const auto type = state->actor_types_by_id.find (actor_id);
        if (type == state->actor_types_by_id.end ())
            return std::shared_ptr<runtime::serial_execution_queue_t>{};
        const auto key = type->second + ":" + actor_id;
        const auto location = state->actor_spot_ids.find (key);
        if (location == state->actor_spot_ids.end ())
            return std::shared_ptr<runtime::serial_execution_queue_t>{};
        const auto context = state->spot_contexts_by_id.find (std::string (location->second));
        if (context == state->spot_contexts_by_id.end () || !context->second._state)
            return std::shared_ptr<runtime::serial_execution_queue_t>{};
        const auto executor =
          context->second._state->ensure_spot_serial_executor ()->actor_executor (key);
        return executor ? executor->queue () : std::shared_ptr<runtime::serial_execution_queue_t>{};
    });
}

task_t<bool> spot_node_runtime_t::notify_actor_application_ready (
  std::string actor_id, std::function<std::optional<runtime::serial_prefix_work_t> ()> take)
{
    auto owner = std::make_shared<spot_node_runtime_t> (_state);
    auto queue = co_await owner->actor_application_queue (actor_id);
    if (!queue)
        co_return false;
    queue->notify_control (
      "actor-ready", [owner, actor_id = std::move (actor_id), queue, take = std::move (take)] {
          auto accepted = owner->accept_actor_application_ready (actor_id, queue, take);
          detail::observe_task_completion (
            accepted,
            [owner, failures = owner->_state->runtime_failures ()] (const result_t<void> &result) {
                if (!result)
                    failures->report (result.exception ());
            });
      });
    co_return true;
}

task_t<void> spot_node_runtime_t::accept_actor_application_ready (
  std::string actor_id,
  std::shared_ptr<runtime::serial_execution_queue_t> queue,
  std::function<std::optional<runtime::serial_prefix_work_t> ()> take)
{
    // Resolve membership again before obtaining the native claim: a move may
    // have replaced the private mailbox after notification publication.
    if (co_await actor_application_queue (actor_id) != queue) {
        (void) co_await notify_actor_application_ready (std::move (actor_id), std::move (take));
        co_return;
    }
    (void) co_await queue->accept_application_ready (std::move (take));
}

task_t<std::vector<std::shared_ptr<runtime::serial_application_record_t>>>
spot_node_runtime_t::take_actor_application_suffix (const actor_ref_t &actor_ref,
                                                    std::function<void ()> seal) const
{
    auto queue = co_await actor_application_queue (actor_ref.actor_id ().value ());
    if (!queue) {
        if (seal)
            seal ();
        co_return std::vector<std::shared_ptr<runtime::serial_application_record_t>>{};
    }
    co_return co_await queue->take_application_suffix (std::move (seal));
}

task_t<result_t<void>>
spot_node_runtime_t::open_local_actor_handoff (actor_ref_t actor_ref,
                                               std::string expected_source_transfer)
{
    const auto state = _state;
    const auto key = actor_key (actor_ref);
    // Retain lane inputs across cancellation and settle before releasing the Join turn.
    auto [context, executor, services] = co_await state->lane.run_task ([state, key] {
        std::shared_ptr<spot_context_state_t> context;
        spot_serial_executor_t::actor_executor_ptr_t executor;
        const auto location = state->actor_spot_ids.find (key);
        if (location != state->actor_spot_ids.end ()) {
            const auto target = spot_node_runtime_t (state).find_context_core (location->second);
            if (target && target->_state) {
                context = target->_state;
                executor = context->ensure_spot_serial_executor ()->actor_executor (key);
            }
        }
        return std::tuple{std::move (context), std::move (executor), state->root_services};
    });
    if (!state->actor_transfer_coordinator->phase (key))
        co_return result_t<void>::success ();
    const auto fail_before_prefix = [&] (framework_error_kind_t kind, const char *message) {
        fail_handoff_backlog (
          state, state->actor_transfer_coordinator->cancel_move (key, expected_source_transfer));
        return result_t<void>::failure (kind, message);
    };
    if (!context)
        co_return fail_before_prefix (framework_error_kind_t::unavailable,
                                      "local Actor handoff target is unavailable");
    if (!executor)
        co_return fail_before_prefix (framework_error_kind_t::shutting_down,
                                      "local Actor handoff target queue is unavailable");
    if (!services)
        co_return fail_before_prefix (framework_error_kind_t::unavailable,
                                      "local Actor handoff services are unavailable");
    auto owner = std::make_shared<spot_node_runtime_t> (state);
    const auto accepted = co_await executor->queue ()->adopt_application_prefix (
      [owner, actor_ref, key, context, services = std::move (*services),
       expected_source_transfer] () mutable {
          // The target FIFO owns this turn. Removing the previous move and
          // installing all record positions therefore precedes handler entry.
          const auto transfer_id =
            owner->_state->actor_transfer_coordinator->transfer_id (key).value_or (key);
          std::vector<runtime::serial_prefix_work_t> prefix;
          owner->_state->actor_transfer_coordinator->finish_move_replay (
            key,
            [&] (std::vector<handoff_packet_t> backlog) {
                order_bound_session_handoff (backlog);
                prefix.reserve (backlog.size ());
                for (auto &packet : backlog) {
                    runtime::serial_prefix_work_t item;
                    item.name = "actor-handoff-prefix";
                    item.options.application_record = packet.application_record;
                    item.cancel = [owner, packet] () mutable {
                        if (packet.application_record && packet.application_record->invocation ()) {
                            packet.content_type = std::move (
                              packet.application_record->invocation ()->second.content_type);
                            packet.metadata =
                              std::move (packet.application_record->invocation ()->second.values);
                        }
                        std::vector<handoff_packet_t> rejected;
                        rejected.push_back (std::move (packet));
                        fail_handoff_backlog (owner->_state, std::move (rejected));
                    };
                    item.work = [owner, actor_ref, services, transfer_id, context,
                                 packet = std::move (packet)] (auto complete) mutable {
                        std::vector<handoff_packet_t> single;
                        single.push_back (std::move (packet));
                        auto replay = owner->replay_actor_handoff_batch (
                          actor_ref, std::move (single), services, transfer_id, true, {});
                        detail::serial_turn_scope_t mailbox_scope ({});
                        detail::observe_task_completion (
                          replay, [owner, complete = std::move (complete)] (
                                    const result_t<void> &result) mutable {
                              complete ([result] {
                                  if (!result)
                                      std::rethrow_exception (result.exception ());
                              });
                          });
                    };
                    prefix.push_back (std::move (item));
                }
            },
            expected_source_transfer);
          return prefix;
      });
    co_return accepted ? result_t<void>::success ()
                       : fail_before_prefix (framework_error_kind_t::shutting_down,
                                             "local Actor handoff prefix was refused");
}

task_t<result_t<void>>
spot_node_runtime_t::deliver_actor_join_completion (const actor_ref_t &actor_ref,
                                                    const actor_join_completion_t &completion,
                                                    std::optional<spot_id_t> source_spot_id)
{
    auto settled = std::make_shared<task_completion_source_t<result_t<void>>> ();
    auto task = settled->task ();
    deliver_actor_join_completion_async (
      actor_ref, completion, std::move (source_spot_id), [settled] (result_t<void> value) mutable {
          settled->complete (result_t<result_t<void>>::success (std::move (value)));
      });
    co_return co_await task;
}

void spot_node_runtime_t::deliver_actor_join_completion_async (
  actor_ref_t actor_ref,
  actor_join_completion_t completion,
  std::optional<spot_id_t> source_spot_id,
  std::function<void (result_t<void>)> completed,
  std::function<void (result_t<void>, std::function<void (result_t<void>)>)> settle_delivery)
{
    /* A commit-preceding failure can arrive after source membership removal.
     * The Actor instance and generation are the completion fence; the source
     * Spot is only a routing hint. */
    (void) source_spot_id;
    std::shared_ptr<join_completion_delivery_fence_scope_t> fallback_delivery_scope;
    if (!settle_delivery) {
        auto root_services = _state->lane.run ([&] { return _state->root_services; }).get ();
        if (root_services) {
            try {
                fallback_delivery_scope = join_completion_delivery_fence_scope_t::begin (
                  root_services->get_required<actor_gateway_runtime_t> (), actor_ref);
            }
            catch (const framework_exception_t &error) {
                completed (detail::result_access_t::failure<void> (error));
                return;
            }
            catch (const std::exception &error) {
                completed (result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                    error.what ()));
                return;
            }
            catch (...) {
                completed (
                  result_t<void>::failure (framework_error_kind_t::internal_failure,
                                           "Actor Join delivery fence initialization failed"));
                return;
            }
            settle_delivery =
              [fallback_delivery_scope] (result_t<void> result,
                                         std::function<void (result_t<void>)> settled) mutable {
                  fallback_delivery_scope->settle (std::move (result), std::move (settled));
              };
        }
    }
    auto settle_result = [settle_delivery = std::move (settle_delivery)] (
                           result_t<void> result,
                           std::function<void (result_t<void>)> settled) mutable {
        if (settle_delivery) {
            settle_delivery (std::move (result), std::move (settled));
        } else if (settled) {
            settled (std::move (result));
        }
    };
    auto completion_owner = std::make_shared<actor_join_completion_t> (std::move (completion));
    const auto operation = std::visit (
      [] (const auto &value) {
          return std::pair<std::uint64_t, std::uint64_t>{value.operation_id_high,
                                                         value.operation_id_low};
      },
      *completion_owner);
    if (operation.first == 0 && operation.second == 0) {
        auto failure =
          result_t<void>::failure (framework_error_kind_t::protocol_error,
                                   "Actor Join completion operation ID must be non-zero");
        settle_result (std::move (failure), std::move (completed));
        return;
    }

    actor_join_completion_callback_t callback;
    std::shared_ptr<spot_context_state_t> callback_context;
    std::shared_ptr<void> actor;
    std::optional<result_t<void>> immediate;
    _state->lane
      .run ([&] {
          if (_state->delivered_join_completions.contains (operation)
              || _state->delivering_join_completions.contains (operation)) {
              immediate.emplace (result_t<void>::success ());
          } else {
              const auto key = actor_key (actor_ref);
              const auto factory = _state->actor_factories.find (std::string (
                ::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
              const auto instance = _state->actor_instances.find (key);
              if (factory == _state->actor_factories.end ()
                  || instance == _state->actor_instances.end () || !instance->second) {
                  immediate.emplace (
                    result_t<void>::failure (framework_error_kind_t::not_found,
                                             "Actor Join completion Actor is not registered"));
              } else {
                  callback = factory->second.on_join_completed;
                  actor = instance->second;
                  const auto membership = _state->actor_spot_ids.find (key);
                  if (membership != _state->actor_spot_ids.end ()) {
                      const auto context =
                        _state->spot_contexts_by_id.find (std::string (membership->second));
                      if (context != _state->spot_contexts_by_id.end ())
                          callback_context = context->second._state;
                  }
                  _state->delivering_join_completions.insert (operation);
              }
          }
      })
      .get ();
    if (immediate) {
        settle_result (std::move (*immediate), std::move (completed));
        return;
    }

    const auto settle_state = [] (std::shared_ptr<spot_node_builder_state_t> node,
                                  actor_ref_t actor,
                                  std::pair<std::uint64_t, std::uint64_t> operation,
                                  result_t<void> result) -> task_t<result_t<void>> {
        const auto native = co_await node->lane.run_task ([node, operation, result] {
            node->delivering_join_completions.erase (operation);
            if (result)
                node->delivered_join_completions.insert (operation);
            return node->native_node.lock ();
        });
        if (native) {
            const auto owner = co_await native->resolve_actor_async (actor);
            if (owner) {
                auto notified = native->notify_stateful_owner (*owner);
                detail::observe_task_completion (
                  notified,
                  [native, failures = node->runtime_failures ()] (const result_t<bool> &terminal) {
                      if (!terminal)
                          failures->report (terminal.exception ());
                  });
            }
        }
        co_return result;
    };
    auto settle = [node = _state, actor_ref, operation, settle_state,
                   completed = std::move (completed)] (result_t<void> result) mutable {
        auto settled = settle_state (node, actor_ref, operation, std::move (result));
        detail::observe_task_completion (
          settled, [node, completed = std::move (completed)] (
                     const result_t<result_t<void>> &settled) mutable {
              auto result = settled ? settled.value ()
                                    : detail::propagate_failure<void> (
                                        settled, "Actor Join completion state settlement failed");
              node->runtime_failures ()->capture ([&] { completed (std::move (result)); });
          });
    };
    auto settle_after_delivery = [settle_result = std::move (settle_result),
                                  settle = std::move (settle)] (result_t<void> result) mutable {
        settle_result (std::move (result), std::move (settle));
    };

    if (std::holds_alternative<actor_join_failed_t> (*completion_owner)) {
        const auto failed_kind = std::get<actor_join_failed_t> (*completion_owner).error_kind;
        detail::message_flow_tracer_t (_state->dispatch)
          .trace (message_flow_outcome_t::replied, message_flow_result_t::failed, [&] {
              auto event = message_flow_event_t{message_flow_outcome_t::replied,
                                                dispatch_error_surface_t::spot_actor,
                                                dispatch_message_kind_t::actor_request,
                                                std::string ("JoinSpot"),
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt,
                                                std::string (actor_ref.actor_id ().value ()),
                                                std::nullopt,
                                                dispatch_error_reason_t::handler_exception,
                                                dispatch_error_action_t::reply_error,
                                                std::make_exception_ptr (framework_exception_t (
                                                  failed_kind, "deferred Actor Join failed"))};
              event.result = message_flow_result_t::failed;
              event.reason = message_flow_reason_t::activation_rejected;
              return event;
          });
    }

    if (!callback) {
        settle_after_delivery (result_t<void>::success ());
        return;
    }
    try {
        auto invoke_callback = [callback, actor, completion_owner, operation, actor_ref,
                                callback_context] {
            runtime::actor_execution_scope_t actor_scope (
              actor_key (actor_ref),
              callback_context ? std::string (callback_context->spot_id) : std::string{});
            return callback (actor.get (),
                             std::holds_alternative<actor_join_accepted_t> (*completion_owner)
                               ? actor_join_completion_outcome_t::accepted
                               : (std::holds_alternative<actor_join_rejected_t> (*completion_owner)
                                    ? actor_join_completion_outcome_t::rejected
                                    : actor_join_completion_outcome_t::failed),
                             operation.first, operation.second,
                             std::get_if<actor_join_accepted_t> (completion_owner.get ())
                               ? &std::get<actor_join_accepted_t> (*completion_owner).actor
                               : nullptr,
                             std::holds_alternative<actor_join_accepted_t> (*completion_owner)
                               ? std::get<actor_join_accepted_t> (*completion_owner).reply
                               : (std::holds_alternative<actor_join_rejected_t> (*completion_owner)
                                    ? std::get<actor_join_rejected_t> (*completion_owner).reply
                                    : std::optional<message_t>{}),
                             std::holds_alternative<actor_join_failed_t> (*completion_owner)
                               ? std::get<actor_join_failed_t> (*completion_owner).error_kind
                               : framework_error_kind_t::internal_failure,
                             std::holds_alternative<actor_join_failed_t> (*completion_owner)
                               && detail::is_transient_error (
                                 std::get<actor_join_failed_t> (*completion_owner).error_kind));
        };
        auto finish_callback = [settle =
                                  settle_after_delivery] (const result_t<void> &result) mutable {
            if (result) {
                settle (result_t<void>::success ());
                return;
            }
            settle (
              detail::propagate_failure<void> (result, "Actor Join completion callback failed"));
        };
        const auto callback_executor =
          callback_context ? callback_context->ensure_spot_serial_executor () : nullptr;
        if (callback_executor && callback_executor->uses_spot_execution_gate ()) {
            const auto turn = detail::capture_current_serial_turn ();
            callback_context->run_serial_task_async (
              "actor-join-completion", std::move (invoke_callback), std::move (finish_callback), {},
              {}, {}, {}, 0, turn);
            return;
        }
        auto observed = std::make_shared<task_t<void>> (invoke_callback ());
        detail::observe_task_completion (
          *observed,
          [completion_owner, actor = std::move (actor), observed,
           finish_callback = std::move (finish_callback)] (const result_t<void> &result) mutable {
              (void) completion_owner;
              (void) actor;
              finish_callback (result);
          });
    }
    catch (const framework_exception_t &error) {
        settle_after_delivery (detail::result_access_t::failure<void> (error));
    }
    catch (const std::exception &error) {
        settle_after_delivery (
          result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ()));
    }
    catch (...) {
        settle_after_delivery (result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                        "Actor Join completion callback failed"));
    }
}

task_t<result_t<void>> spot_node_runtime_t::submit_remote_actor_leave_async (
  std::string transfer_id,
  actor_ref_t source_actor,
  spot_id_t source_spot_id,
  std::uint64_t source_spot_generation,
  spot_id_t target_spot_id,
  runtime::protocol::actor_route_fence_t target_fence,
  std::function<void ()> transfer_owner_reservation,
  std::size_t transferred_owner_byte_cost)
{
    return [] (spot_node_runtime_t self, std::string transfer_id, actor_ref_t source_actor,
               spot_id_t source_spot_id, std::uint64_t source_spot_generation,
               spot_id_t target_spot_id, runtime::protocol::actor_route_fence_t target_fence,
               std::function<void ()> transfer_owner_reservation,
               std::size_t transferred_owner_byte_cost) -> task_t<result_t<void>> {
        transferred_owner_reservation_guard_t inbound_owner_reservation (
          std::move (transfer_owner_reservation));
        if (transfer_id.empty () || source_spot_id.empty () || target_spot_id.empty ()
            || target_fence.actor_id != source_actor.actor_id ().value ()
            || target_fence.object_generation != source_actor.object_generation ()
            || target_fence.target_node_routing_id.empty ()
            || target_fence.target_node_generation == 0
            || target_fence.authority_owner_generation == 0
            || target_fence.owner_lease_generation == 0) {
            co_return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                               "remote Actor leave command fence is invalid");
        }
        std::shared_ptr<spot_context_state_t> source_state;
        std::shared_ptr<void> actor_instance;
        std::function<task_t<void> (void *, void *)> leave_callback;
        const auto key = actor_key (source_actor);
        const auto requested_source_spot_generation = source_spot_generation;
        const auto matches_target = [&] (const auto &cleanup) {
            const auto &target = cleanup.target_identity;
            return target.target_node_routing_id == target_fence.target_node_routing_id
                   && target.target_node_generation == target_fence.target_node_generation
                   && target.target_owner_lease_generation == target_fence.owner_lease_generation
                   && target_fence.authority_owner_generation
                        > cleanup.source_fence.authority_owner_generation;
        };
        const auto matches_cleanup = [&] (const auto &candidate) {
            return candidate.transfer_id == transfer_id
                   && candidate.source_actor.actor_id () == source_actor.actor_id ()
                   && ::zlink::framework::detail::actor_ref_access_t::actor_type (
                        candidate.source_actor)
                        == ::zlink::framework::detail::actor_ref_access_t::actor_type (source_actor)
                   && candidate.source_actor.object_generation ()
                        == source_actor.object_generation ()
                   && candidate.source_actor.node_rid ().value ()
                        == source_actor.node_rid ().value ()
                   && candidate.target_spot_id == target_spot_id;
        };
        const auto matches_pending_leave = [&] (const auto &candidate) {
            return candidate.transfer_id == transfer_id
                   && candidate.source_actor.actor_id () == source_actor.actor_id ()
                   && ::zlink::framework::detail::actor_ref_access_t::actor_type (
                        candidate.source_actor)
                        == ::zlink::framework::detail::actor_ref_access_t::actor_type (source_actor)
                   && candidate.source_actor.object_generation ()
                        == source_actor.object_generation ()
                   && candidate.source_actor.node_rid ().value ()
                        == source_actor.node_rid ().value ()
                   && candidate.source_spot_id == source_spot_id
                   && candidate.target_spot_id == target_spot_id
                   && candidate.target_fence == target_fence;
        };
        const auto matches_pending_owner = [&] (const auto &candidate) {
            return candidate.transfer_id == transfer_id
                   && actor_key (candidate.source_actor) == key;
        };
        struct source_leave_projection_t
        {
            std::shared_ptr<spot_context_state_t> state;
            std::shared_ptr<service::spot_t> native_spot;
            std::uint64_t source_spot_generation = 0;
        };
        struct source_leave_projection_result_t
        {
            std::optional<source_leave_projection_t> projection;
            bool deferred = false;
        };

        const auto project = [&] (std::optional<bool> matches_transfer)
          -> std::optional<source_leave_projection_result_t> {
            const auto cleanup =
              std::find_if (self._state->pending_remote_source_cleanups.begin (),
                            self._state->pending_remote_source_cleanups.end (), matches_cleanup);
            auto resolved_generation = requested_source_spot_generation;
            if (cleanup != self._state->pending_remote_source_cleanups.end ()) {
                if (!matches_target (*cleanup) || cleanup->source_spot_id != source_spot_id
                    || cleanup->source_spot_generation == 0
                    || (requested_source_spot_generation != 0
                        && requested_source_spot_generation != cleanup->source_spot_generation)) {
                    return source_leave_projection_result_t{};
                }
                resolved_generation = cleanup->source_spot_generation;
            } else {
                if (!matches_transfer)
                    return std::nullopt;
                if (!*matches_transfer) {
                    return source_leave_projection_result_t{};
                }
                const auto pending = std::find_if (
                  self._state->pending_remote_actor_leaves.begin (),
                  self._state->pending_remote_actor_leaves.end (), matches_pending_owner);
                if (pending == self._state->pending_remote_actor_leaves.end ()) {
                    self._state->management_next_due.store (
                      spot_node_builder_state_t::management_due_now, std::memory_order_release);
                    self._state->pending_remote_actor_leaves.push_back (
                      spot_node_builder_state_t::pending_remote_actor_leave_t{
                        .transfer_id = transfer_id,
                        .source_actor = source_actor,
                        .source_spot_id = source_spot_id,
                        .target_spot_id = target_spot_id,
                        .target_fence = target_fence});
                    auto &inserted = self._state->pending_remote_actor_leaves.back ();
                    inserted.transfer_owner_reservation = inbound_owner_reservation.release ();
                    inserted.transferred_owner_byte_cost = transferred_owner_byte_cost;
                } else if (!matches_pending_leave (*pending)) {
                    // The exact source_remote move owns one deferred lifecycle
                    // command. A conflicting command cannot allocate another slot
                    // or replace the already authority-validated identity.
                    return source_leave_projection_result_t{};
                }
                return source_leave_projection_result_t{.deferred = true};
            }
            if (const auto current = self._state->actor_authority_fences.find (key);
                current != self._state->actor_authority_fences.end ()
                && (cleanup == self._state->pending_remote_source_cleanups.end ()
                    || current->second != cleanup->source_fence))
                return source_leave_projection_result_t{};
            const auto context =
              self._state->spot_contexts_by_id.find (std::string (source_spot_id));
            const auto actor = self._state->actor_instances.find (key);
            const auto factory = self._state->actor_factories.find (std::string (
              ::zlink::framework::detail::actor_ref_access_t::actor_type (source_actor)));
            if (context == self._state->spot_contexts_by_id.end () || !context->second._state
                || !context->second._state->spot_instance
                || actor == self._state->actor_instances.end () || !actor->second
                || factory == self._state->actor_factories.end ()) {
                return source_leave_projection_result_t{};
            }
            auto state = context->second._state;
            auto native_spot = state->native_spot.lock ();
            if (!native_spot)
                return source_leave_projection_result_t{};
            return source_leave_projection_result_t{
              .projection = source_leave_projection_t{std::move (state), std::move (native_spot),
                                                      resolved_generation}};
        };
        auto projection_result =
          co_await self._state->lane.run_task ([&] { return project (std::nullopt); });
        if (!projection_result) {
            const auto matches_transfer =
              co_await self._state->actor_transfer_coordinator
                ->matches_source_remote_transfer_async (key, transfer_id);
            projection_result =
              co_await self._state->lane.run_task ([&] { return project (matches_transfer); });
        }
        if (projection_result->deferred) {
            co_return result_t<void>::success ();
        }
        const auto &projection = projection_result->projection;
        if (!projection
            || projection->native_spot->status ().lifecycle_generation ()
                 != projection->source_spot_generation) {
            co_return result_t<void>::success ();
        }

        std::function<void ()> pending_owner_reservation;
        std::size_t pending_owner_byte_cost = 0;
        std::vector<std::function<void ()>> superseded_owner_reservations;
        const auto committed_leave = co_await self._state->lane.run_task ([&] {
            superseded_owner_reservations.reserve (
              self._state->pending_remote_actor_leaves.size ());
            auto cleanup =
              std::find_if (self._state->pending_remote_source_cleanups.begin (),
                            self._state->pending_remote_source_cleanups.end (), matches_cleanup);
            auto resolved_generation = requested_source_spot_generation;
            if (cleanup != self._state->pending_remote_source_cleanups.end ()) {
                if (!matches_target (*cleanup) || cleanup->source_spot_id != source_spot_id
                    || cleanup->source_spot_generation == 0
                    || (requested_source_spot_generation != 0
                        && requested_source_spot_generation != cleanup->source_spot_generation)) {
                    return false;
                }
                resolved_generation = cleanup->source_spot_generation;
            } else {
                return false;
            }
            if (const auto current = self._state->actor_authority_fences.find (key);
                current != self._state->actor_authority_fences.end ()
                && (cleanup == self._state->pending_remote_source_cleanups.end ()
                    || current->second != cleanup->source_fence))
                return false;
            const auto context =
              self._state->spot_contexts_by_id.find (std::string (source_spot_id));
            const auto actor = self._state->actor_instances.find (key);
            const auto factory = self._state->actor_factories.find (std::string (
              ::zlink::framework::detail::actor_ref_access_t::actor_type (source_actor)));
            if (resolved_generation != projection->source_spot_generation
                || context == self._state->spot_contexts_by_id.end ()
                || context->second._state != projection->state
                || !context->second._state->spot_instance
                || context->second._state->native_spot.lock () != projection->native_spot
                || actor == self._state->actor_instances.end () || !actor->second
                || factory == self._state->actor_factories.end ()) {
                return false;
            }
            source_state = context->second._state;
            const auto admission = source_state->actor_admissions.find (factory->second.actor_type);
            if (admission != source_state->actor_admissions.end ())
                leave_callback = admission->second.on_leave_actor;

            if (cleanup != self._state->pending_remote_source_cleanups.end ()) {
                // Membership lifecycle is independent from Message Follow, whose
                // duration may be zero. Validate against the exact committed
                // cleanup fence instead of requiring a live follow route.
                if (cleanup->leave_submitted || !matches_target (*cleanup)
                    || cleanup->target_spot_id != target_spot_id) {
                    return false;
                }
                cleanup->leave_submitted = true;
                // No OnLeave handler registered: there is nothing to await, so
                // this transfer's source cleanup is already unblocked.
                if (!leave_callback)
                    cleanup->leave_completed = true;
                if (const auto host = self._state->native_node.lock ())
                    host->signal_dispatch_activity ();
            } else
                return false;

            const auto exact_pending =
              std::find_if (self._state->pending_remote_actor_leaves.begin (),
                            self._state->pending_remote_actor_leaves.end (), matches_pending_leave);
            if (exact_pending != self._state->pending_remote_actor_leaves.end ()) {
                pending_owner_reservation = std::move (exact_pending->transfer_owner_reservation);
                pending_owner_byte_cost = exact_pending->transferred_owner_byte_cost;
                exact_pending->transferred_owner_byte_cost = 0;
            }
            for (auto &pending : self._state->pending_remote_actor_leaves) {
                if (matches_pending_owner (pending) && pending.transfer_owner_reservation) {
                    superseded_owner_reservations.push_back (
                      std::move (pending.transfer_owner_reservation));
                    pending.transferred_owner_byte_cost = 0;
                }
            }

            const auto local = self._state->actor_spot_ids.find (key);
            if (local != self._state->actor_spot_ids.end () && local->second == source_spot_id) {
                erase_actor_route_unlocked (*self._state, key);
            }
            decrement_actor_count_unlocked (*source_state);
            actor_instance = actor->second;
            std::erase_if (self._state->pending_remote_actor_leaves, matches_pending_owner);
            return true;
        });
        for (auto &reservation : superseded_owner_reservations) {
            transferred_owner_reservation_guard_t abandoned (std::move (reservation));
            abandoned.settle ();
        }
        if (!committed_leave) {
            co_return result_t<void>::success ();
        }

        const bool use_pending_owner_reservation = static_cast<bool> (pending_owner_reservation);
        transferred_owner_reservation_guard_t selected_owner_reservation (
          use_pending_owner_reservation ? std::move (pending_owner_reservation)
                                        : inbound_owner_reservation.release ());
        const auto selected_owner_byte_cost =
          use_pending_owner_reservation ? pending_owner_byte_cost : transferred_owner_byte_cost;
        if (leave_callback) {
            auto state = self._state;
            source_state->run_serial_task_async (
              "spot-actor-remote-leave",
              [source_state, actor_instance,
               leave_callback = std::move (leave_callback)] () mutable {
                  return leave_callback (source_state->spot_instance.get (), actor_instance.get ());
              },
              [state = std::move (state), transfer_id] (result_t<void> left) {
                  // 실행 전 거부·취소 또는 실행한 callback의 실제 종료가 이 단계의 terminal이다.
                  // Source 정리는 이 완료를 따르며 이미 commit된 target Join 결과를 바꾸지 않는다.
                  state->lane
                    .run ([&] {
                        const auto found =
                          std::find_if (state->pending_remote_source_cleanups.begin (),
                                        state->pending_remote_source_cleanups.end (),
                                        [&] (const auto &candidate) {
                                            return candidate.transfer_id == transfer_id;
                                        });
                        if (found != state->pending_remote_source_cleanups.end ()) {
                            found->leave_completed = true;
                            if (const auto host = state->native_node.lock ())
                                host->signal_dispatch_activity ();
                            report_actor_leave_failure (state, left, found->source_spot_id,
                                                        found->source_actor.actor_id ().value (),
                                                        transfer_id);
                        } else {
                            report_actor_leave_failure (state, left, {}, {}, transfer_id);
                        }
                    })
                    .get ();
              },
              {}, {}, {}, selected_owner_reservation.release (), selected_owner_byte_cost);
        }
        co_return result_t<void>::success ();
    }(*this, std::move (transfer_id), std::move (source_actor), std::move (source_spot_id),
                                                        source_spot_generation,
                                                        std::move (target_spot_id),
                                                        std::move (target_fence),
                                                        std::move (transfer_owner_reservation),
                                                        transferred_owner_byte_cost);
}

void spot_node_runtime_t::fail_remote_actor_transfer (
  const actor_ref_t &actor_ref,
  bool reconcile,
  std::optional<reconcile_target_context_t> reconcile_context)
{
    const auto key = actor_key (actor_ref);
    if (reconcile) {
        // Genuinely ambiguous outcome (e.g. the cutover submission itself
        // failed, so whether the target received it is unknown): retain the
        // reconcile phase, but only up to message_follow_duration -- see
        // move_state_t::reconcile_deadline. The captured target identity
        // lets the deadline handler reconcile against Location Store
        // authority truth instead of blindly replaying locally (spec 28
        // relay-ready irreversibility).
        const auto message_follow_duration =
          _state->lane.run ([&] { return _state->message_follow_duration; }).get ();
        _state->actor_transfer_coordinator->mark_reconcile (key, message_follow_duration,
                                                            std::move (reconcile_context));
    } else {
        replay_actor_handoff_until_move_closed (actor_ref);
    }
}

task_t<void> spot_node_runtime_t::fast_fail_reconcile_backlog (
  actor_ref_t actor_ref,
  std::string key,
  std::optional<runtime::protocol::actor_route_fence_t> source_fence)
{
    return fast_fail_reconcile_backlog_core (_state, std::move (actor_ref), std::move (key),
                                             std::move (source_fence));
}

task_t<void> spot_node_runtime_t::fast_fail_reconcile_backlog_core (
  std::shared_ptr<spot_node_builder_state_t> state,
  actor_ref_t actor_ref,
  std::string key,
  std::optional<runtime::protocol::actor_route_fence_t> source_fence)
{
    // The management coroutine can finish before this detached terminal task.
    // Keep its runtime wrapper in the coroutine frame across lane suspension.
    spot_node_runtime_t owner (state);
    if (source_fence) {
        co_await state->lane.run_task ([&] {
            const auto current = state->actor_authority_fences.find (key);
            if (current == state->actor_authority_fences.end () || current->second == *source_fence)
                erase_actor_route_unlocked (*state, key);
        });
    }
    for (;;) {
        auto completion =
          source_fence
            ? state->actor_transfer_coordinator->finish_move_replay (key)
            : actor_move_completion_t{std::nullopt,
                                      state->actor_transfer_coordinator->take_backlog (key), true};
        auto backlog = std::move (completion.backlog);
        for (auto &packet : backlog) {
            if (!packet.is_request)
                continue;
            spot_inbound_message_t metadata;
            metadata.content_type = std::move (packet.content_type);
            metadata.values = std::move (packet.metadata);
            const auto terminal_route = handoff_terminal_route (metadata.values);
            if (!terminal_route)
                continue;
            const auto failure = result_t<zlink::message_t>::failure (
              framework_error_kind_t::unavailable,
              "actor relocation outcome could not be reconciled against the Location "
              "Store; actor remains unavailable");
            const auto sent = co_await runtime::await_result (
              send_handoff_terminal (state, terminal_route, failure));
            const bool delivered = sent && sent.value ();
            if (delivered ? owner.actor_transfer_marker_enabled ()
                          : owner.actor_transfer_marker_enabled (message_flow_result_t::failed)) {
                owner.emit_actor_transfer_marker (
                  "reconcile_deadline_fast_failed", actor_ref, key, std::nullopt, std::nullopt,
                  delivered ? nullptr
                  : !sent   ? sent.exception ()
                            : std::make_exception_ptr (framework_exception_t (
                              framework_error_kind_t::unavailable,
                              "Actor handoff terminal send was not accepted")));
            }
        }
        if (completion.completed)
            break;
    }
    (void) co_await owner.settle_abandoned_actor_leave_reservations_async ();
}

task_t<void> spot_node_runtime_t::complete_remote_actor_transfer (
  const actor_ref_t &source_actor,
  const actor_ref_t &target_actor,
  spot_route_t target_route,
  runtime::protocol::actor_route_fence_t source_fence,
  runtime::protocol::relocation_target_fence_t target_identity,
  std::optional<std::uint64_t> follow_authority_generation,
  std::string transfer_id)
{
    std::function<task_t<std::optional<zlink::message_t>> (
      const actor_ref_t &, const runtime::messaging::envelope_header_t &, const zlink::message_t &,
      std::chrono::milliseconds, const zlink::routing_id_t &,
      const runtime::protocol::actor_route_fence_t &, std::uint8_t,
      const runtime::protocol::wire_operation_id_t &, std::uint64_t)>
      late_handoff_relay;
    const auto key = actor_key (source_actor);
    if (transfer_id.empty ()) {
        transfer_id = key;
    }
    const auto late_transfer_id = transfer_id;
    const auto target_spot_id = target_route.spot_id;
    struct source_spot_projection_t
    {
        spot_id_t spot_id;
        std::shared_ptr<service::spot_t> native;
    };
    auto source =
      _state->lane
        .run ([&] {
            source_spot_projection_t result;
            if (const auto spot = _state->actor_spot_ids.find (key);
                spot != _state->actor_spot_ids.end ()) {
                result.spot_id = spot->second;
                const auto context =
                  _state->spot_contexts_by_id.find (std::string (result.spot_id));
                if (context != _state->spot_contexts_by_id.end () && context->second._state)
                    result.native = context->second._state->native_spot.lock ();
            }
            return result;
        })
        .get ();
    const auto source_spot_generation =
      source.native ? source.native->status ().lifecycle_generation () : 0;
    const auto source_spot_id = source.spot_id;
    const auto now = std::chrono::steady_clock::now ();
    // Keep the old-generation Message Follow route independent from the actor's
    // newer local target. The coordinator owns this route; source cleanup owns the leave claim;
    // complete its turns before entering the node lane.
    if (follow_authority_generation) {
        const runtime::protocol::actor_route_fence_t target_fence{
          std::string (source_actor.actor_id ().value ()),
          source_actor.object_generation (),
          target_identity.target_node_routing_id,
          target_identity.target_node_generation,
          *follow_authority_generation,
          target_identity.target_owner_lease_generation};
        _state->actor_transfer_coordinator->activate_message_follow (
          key, source_fence, target_actor, target_route, target_fence,
          now + _state->message_follow_duration, transfer_id);
    }
    std::optional<runtime::protocol::actor_route_fence_t> pending_leave_fence;
    bool replay_pending_leave = false;
    _state
      ->lane

      .run_checked ([&] {
          if (const auto current = _state->actor_authority_fences.find (key);
              current != _state->actor_authority_fences.end () && current->second == source_fence) {
              _state->actor_authority_fences.erase (current);
          }
          if (follow_authority_generation)
              detail::record_actor_route_unlocked (*_state, key, std::move (target_route),
                                                   target_actor.object_generation ());
          else if (!_state->actor_authority_fences.contains (key))
              erase_actor_route_unlocked (*_state, key);
          // Commit acknowledgement fixes the new owner and Message Follow route first.
          // Releasing stale source ownership is post-commit housekeeping: losing the
          // source process now cannot roll back the accepted target generation.
          _state->management_next_due.store (spot_node_builder_state_t::management_due_now,
                                             std::memory_order_release);
          _state->pending_remote_source_cleanups.push_back (
            spot_node_builder_state_t::pending_remote_source_cleanup_t{
              .source_actor = source_actor,
              .source_fence = source_fence,
              .transfer_id = std::move (transfer_id),
              .source_spot_id = std::move (source.spot_id),
              .source_spot_generation = source_spot_generation,
              .target_spot_id = target_spot_id,
              .target_identity = target_identity,
              .not_before = now + std::chrono::seconds (1),
              .leave_submitted = false,
              .leave_completed = false,
              .leave_deadline = now + _state->message_follow_duration});
          if (const auto host = _state->native_node.lock ())
              host->signal_dispatch_activity ();
          const auto pending_leave = std::find_if (
            _state->pending_remote_actor_leaves.begin (),
            _state->pending_remote_actor_leaves.end (), [&] (const auto &candidate) {
                return candidate.transfer_id == late_transfer_id
                       && candidate.source_actor.actor_id () == source_actor.actor_id ()
                       && ::zlink::framework::detail::actor_ref_access_t::actor_type (
                            candidate.source_actor)
                            == ::zlink::framework::detail::actor_ref_access_t::actor_type (
                              source_actor)
                       && candidate.source_actor.object_generation ()
                            == source_actor.object_generation ()
                       && candidate.source_actor.node_rid ().value ()
                            == source_actor.node_rid ().value ()
                       && candidate.source_spot_id == source_spot_id
                       && candidate.target_spot_id == target_spot_id
                       && candidate.target_fence.target_node_routing_id
                            == target_identity.target_node_routing_id
                       && candidate.target_fence.target_node_generation
                            == target_identity.target_node_generation
                       && candidate.target_fence.owner_lease_generation
                            == target_identity.target_owner_lease_generation
                       && candidate.target_fence.authority_owner_generation
                            > source_fence.authority_owner_generation;
            });
          if (pending_leave != _state->pending_remote_actor_leaves.end ()) {
              replay_pending_leave = true;
              pending_leave_fence = pending_leave->target_fence;
          }
          // Publish the Message Follow route before removing the moving state. A
          // packet that already selected the source route can still reach the
          // coordinator while this transition runs; finish_move_replay() removes the
          // moving state only after an atomic empty-backlog observation.
          if (follow_authority_generation)
              late_handoff_relay = _state->actor_message_follow_relay;
      })
      .get ();
    if (replay_pending_leave) {
        try {
            (void) co_await submit_remote_actor_leave_async (late_transfer_id, source_actor,
                                                             source_spot_id, source_spot_generation,
                                                             target_spot_id, *pending_leave_fence);
        }
        catch (...) {
            // The target notification remains one-way post-commit housekeeping.
        }
    }

    for (;;) {
        auto replay = _state->actor_transfer_coordinator->finish_move_replay (key);
        order_bound_session_handoff (replay.backlog);
        for (auto &packet : replay.backlog) {
            if (!late_handoff_relay) {
                if (actor_transfer_marker_enabled (message_flow_result_t::failed)) {
                    emit_actor_transfer_marker ("handoff_late_relay_unavailable", source_actor,
                                                late_transfer_id, std::nullopt, std::nullopt,
                                                std::make_exception_ptr (framework_exception_t (
                                                  framework_error_kind_t::unavailable,
                                                  "Actor handoff replay owner is unavailable")));
                }
                std::vector<handoff_packet_t> discarded;
                discarded.push_back (std::move (packet));
                fail_handoff_backlog (_state, std::move (discarded));
                continue;
            }
            const auto handoff_source =
              handoff_routing_id (packet.metadata, actor_handoff_source_node_key);
            const auto handoff_route = handoff_actor_route (packet.metadata);
            const auto handoff_hop = handoff_u64 (packet.metadata, actor_handoff_hop_count_key);
            const auto handoff_operation_high =
              handoff_u64 (packet.metadata, actor_handoff_operation_high_key);
            const auto handoff_operation_low =
              handoff_u64 (packet.metadata, actor_handoff_operation_low_key);
            const auto handoff_reply_route =
              handoff_u64 (packet.metadata, actor_handoff_reply_route_key);
            const bool preserves_terminal_route = handoff_source && handoff_operation_high
                                                  && handoff_operation_low && handoff_reply_route;
            const bool preserves_stale_route = handoff_route && handoff_hop && *handoff_hop <= 8;
            runtime::messaging::envelope_header_t header;
            header.kind = packet.is_request ? runtime::messaging::message_kind_t::request
                                            : runtime::messaging::message_kind_t::command;
            header.channel_name = "actor";
            header.message_name = std::move (packet.packet_name);
            header.content_type = std::move (packet.content_type);
            header.metadata = std::move (packet.metadata);
            if (const auto deadline =
                  header.metadata.find (std::string (actor_handoff_deadline_key));
                deadline != header.metadata.end ())
                header.deadline = deadline->second;
            header.metadata.insert_or_assign ("__zlink.actorHandoffLateReplay", "1");
            // Route through the just-published Message Follow target. The
            // target Actor owner does not admit application records until its
            // completion opens, while that completion waits for this source
            // backlog to drain. The exact target Spot route can stage the
            // packet and acknowledge admission without creating that cycle.
            bool relayed = false;
            std::optional<result_t<zlink::message_t>> terminal;
            try {
                const auto reply = co_await late_handoff_relay (
                  source_actor, header, zlink::message_t::from (std::move (packet.payload)),
                  std::chrono::seconds (30),
                  preserves_terminal_route ? *handoff_source
                                           : zlink::routing_id_t::from (std::uint32_t{0}),
                  preserves_stale_route ? *handoff_route : runtime::protocol::actor_route_fence_t{},
                  preserves_stale_route ? static_cast<std::uint8_t> (*handoff_hop) : 0,
                  preserves_terminal_route
                    ? runtime::protocol::wire_operation_id_t{*handoff_operation_high,
                                                             *handoff_operation_low}
                    : runtime::protocol::wire_operation_id_t{},
                  preserves_terminal_route ? *handoff_reply_route : 0);
                relayed = true;
                if (packet.is_request && reply)
                    terminal = result_t<zlink::message_t>::success (*reply);
            }
            catch (const framework_exception_t &error) {
                terminal = result_t<zlink::message_t>::failure (error.kind (), error.what ());
            }
            catch (const std::exception &error) {
                terminal = result_t<zlink::message_t>::failure (
                  framework_error_kind_t::internal_failure, error.what ());
            }
            if (!relayed && actor_transfer_marker_enabled (message_flow_result_t::failed)) {
                emit_actor_transfer_marker ("handoff_late_relay_failed", source_actor, key,
                                            std::nullopt, std::nullopt, terminal->exception ());
            }
            if (packet.is_request && terminal) {
                (void) co_await send_handoff_terminal (
                  _state, handoff_terminal_route (header.metadata.values ()), *terminal);
            }
        }
        if (replay.completed) {
            break;
        }
    }
    co_return;
}

bool spot_node_runtime_t::actor_transfer_marker_enabled (
  message_flow_result_t result) const noexcept
{
    return message_flow_tracer_t (_state->dispatch)
      .enabled_for (message_flow_outcome_t::dispatched, result);
}

void spot_node_runtime_t::emit_actor_transfer_marker (std::string marker,
                                                      const actor_ref_t &actor_ref,
                                                      std::string transfer_id,
                                                      std::optional<spot_id_t> spot_id,
                                                      std::optional<node_rid_t> target_node_rid,
                                                      std::exception_ptr exception) const
{
    auto build_event = [&] {
        return message_flow_event_t{
          .outcome = message_flow_outcome_t::dispatched,
          .surface = dispatch_error_surface_t::spot_actor,
          .message_kind = dispatch_message_kind_t::actor_request,
          .packet_name = marker,
          .channel_name = target_node_rid
                            ? std::make_optional (std::string (target_node_rid->value ()))
                            : std::nullopt,
          .correlation_id = transfer_id,
          .source_rid = std::string (_state->node_rid.value ()),
          .spot_id = spot_id,
          .actor_id = std::string (actor_ref.actor_id ().value ()),
          .exception = exception,
          .flow_id = transfer_id,
          .flow_origin = flow_origin_t::lifecycle};
    };
    const auto tracer = message_flow_tracer_t (_state->dispatch);
    if (exception)
        tracer.trace (message_flow_outcome_t::dispatched, message_flow_result_t::failed,
                      build_event);
    else
        tracer.trace (message_flow_outcome_t::dispatched, transfer_id, build_event);
}

result_t<actor_join_reply_t>
spot_node_runtime_t::prepare_remote_actor_to_spot (std::string transfer_id,
                                                   const actor_ref_t &actor_ref,
                                                   spot_id_t target_spot_id,
                                                   zlink::message_t transfer_state,
                                                   actor_context_t actor_context,
                                                   bool defer_joined_callback)
{
    cleanup_expired_actor_admissions ();
    std::vector<handoff_packet_t> discarded_backlog;
    const auto pending = _state->actor_transfer_coordinator->begin_commit (
      transfer_id, actor_ref, target_spot_id, discarded_backlog);
    fail_handoff_backlog (_state, std::move (discarded_backlog));
    if (!pending) {
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::protocol_error, "remote actor commit has no matching admission");
    }
    const auto key = actor_key (actor_ref);
    struct prepare_plan_t
    {
        spot_node_builder_state_t::actor_factory_registration_t factory;
        spot_actor_admission_callbacks_t admission;
        std::shared_ptr<spot_context_state_t> context;
        std::string node_rid;
        bool available = false;
        bool lifecycle_available = false;
        bool context_reserved = false;
    };
    const auto plan =
      _state->lane
        .run ([&] {
            prepare_plan_t result;
            const auto factory = _state->actor_factories.find (
              std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
            const auto context = _state->spot_contexts_by_id.find (std::string (target_spot_id));
            if (factory == _state->actor_factories.end ()
                || context == _state->spot_contexts_by_id.end () || !context->second._state
                || !context->second._state->spot_instance) {
                return result;
            }
            result.factory = factory->second;
            result.context = context->second._state;
            result.node_rid = detail::effective_spot_node_rid (_state->snapshot);
            result.available = true;
            const auto admission =
              result.context->actor_admissions.find (result.factory.actor_type);
            if (admission != result.context->actor_admissions.end ()) {
                result.admission = admission->second;
                result.lifecycle_available = true;
            }
            if (!result.lifecycle_available)
                return result;
            if (!result.context->accepts_membership_core (*_state)
                || _state->actor_instances.contains (key) || _state->actor_spot_ids.contains (key)
                || _state->pending_actor_contexts.contains (key)) {
                return result;
            }
            result.context->actor_count++;
            _state->pending_actor_contexts.emplace (key, target_spot_id);
            result.context_reserved = true;
            return result;
        })
        .get ();
    if (!plan.available) {
        fail_handoff_backlog (_state,
                              _state->actor_transfer_coordinator->fail_commit (transfer_id, false));
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found, "remote actor commit dependencies are not registered");
    }
    if (!plan.lifecycle_available) {
        fail_handoff_backlog (_state,
                              _state->actor_transfer_coordinator->fail_commit (transfer_id, false));
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found, "target spot actor lifecycle is not registered");
    }

    const auto committed = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (plan.node_rid),
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
      std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation ());
    if (!plan.context_reserved) {
        fail_handoff_backlog (_state,
                              _state->actor_transfer_coordinator->fail_commit (transfer_id, false));
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::unavailable,
          "target Spot closed before the remote Actor could be prepared");
    }

    bool claimed_location = false;
    auto location_claim = claim_pending_actor_location_before_activation (
      _state, pending->source_actor, pending->source_spot_id, committed, *plan.context,
      claimed_location);
    auto release_context_reservation = [&] {
        _state->lane
          .run ([&] {
              const auto reservation = _state->pending_actor_contexts.find (key);
              if (reservation == _state->pending_actor_contexts.end ()
                  || reservation->second != target_spot_id) {
                  return;
              }
              decrement_actor_count_unlocked (*plan.context);
              _state->pending_actor_contexts.erase (reservation);
          })
          .get ();
    };
    if (!location_claim) {
        release_context_reservation ();
        fail_handoff_backlog (_state,
                              _state->actor_transfer_coordinator->fail_commit (transfer_id, false));
        return result_t<actor_join_reply_t>::failure (
          location_claim.error_kind (), location_claim.error () ? location_claim.error ()->what ()
                                                                : "actor location claim failed");
    }

    auto fail_target_commit = [&] (bool reconcile) {
        release_context_reservation ();
        if (claimed_location) {
            release_actor_location (_state, committed);
        }
        fail_handoff_backlog (
          _state, _state->actor_transfer_coordinator->fail_commit (transfer_id, reconcile));
    };

    auto committed_context =
      actor_context_t (actor_context._state, committed, 0, plan.context->mesh_name);
    std::shared_ptr<void> actor;
    try {
        const auto &registration = plan.factory;
        actor = registration.create_context_instance
                  ? registration.create_context_instance (std::move (committed_context))
                  : registration.create_instance (std::string (actor_ref.actor_id ().value ()));
        if (actor && !registration.create_context_instance) {
            registration.configure_instance (actor.get (), committed, &committed_context);
        }
        if (actor
            && registration.relocation.kind == detail::factory_relocation_kind_t::preserve_state) {
            const auto bytes = transfer_state.to_bytes ();
            std::vector<std::byte> payload;
            payload.reserve (bytes.size ());
            std::transform (bytes.begin (), bytes.end (), std::back_inserter (payload),
                            [] (std::uint8_t value) { return static_cast<std::byte> (value); });
            auto restored = registration.restore (actor.get (), std::move (payload), {}).result ();
            if (!restored) {
                fail_target_commit (false);
                return result_t<actor_join_reply_t>::failure (
                  restored.error_kind (),
                  restored.error () ? restored.error ()->what () : "actor restore failed");
            }
        }
    }
    catch (const framework_exception_t &error) {
        fail_target_commit (false);
        return detail::result_access_t::failure<actor_join_reply_t> (error);
    }
    catch (const std::exception &error) {
        fail_target_commit (false);
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure,
                                                      error.what ());
    }
    catch (...) {
        fail_target_commit (false);
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure,
                                                      "actor transfer-in failed");
    }
    if (!actor) {
        fail_target_commit (false);
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "actor materialization returned no actor");
    }

    auto &target = *plan.context;
    try {
        if (!defer_joined_callback && plan.admission.on_actor_joined) {
            const auto completed =
              target
                .run_serial_task (
                  "spot-actor-transfer-joined",
                  [&] () -> task_t<void> {
                      const auto updated =
                        actor_gateway_runtime_t (actor_context._state).update_actor_ref (committed);
                      if (!updated) {
                          throw framework_exception_t (
                            updated.error_kind (), updated.error () != nullptr
                                                     ? updated.error ()->what ()
                                                     : "target actor gateway ref update failed");
                      }
                      co_await plan.admission.on_actor_joined (target.spot_instance.get (),
                                                               actor.get ());
                  })
                .result ();
            if (!completed) {
                fail_target_commit (false);
                return result_t<actor_join_reply_t>::failure (
                  completed.error_kind (), completed.error () != nullptr
                                             ? completed.error ()->what ()
                                             : "spot actor joined callback failed");
            }
        }
    }
    catch (const framework_exception_t &error) {
        _state->lane
          .run ([&] {
              detail::record_actor_instance_index_unlocked (*_state, committed, actor.get ());
              _state->actor_instances[actor_key (committed)] = actor;
          })
          .get ();
        fail_target_commit (true);
        return detail::result_access_t::failure<actor_join_reply_t> (error);
    }
    catch (const std::exception &error) {
        _state->lane
          .run ([&] {
              detail::record_actor_instance_index_unlocked (*_state, committed, actor.get ());
              _state->actor_instances[actor_key (committed)] = actor;
          })
          .get ();
        fail_target_commit (true);
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure,
                                                      error.what ());
    }
    catch (...) {
        _state->lane
          .run ([&] {
              detail::record_actor_instance_index_unlocked (*_state, committed, actor.get ());
              _state->actor_instances[actor_key (committed)] = actor;
          })
          .get ();
        fail_target_commit (true);
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure,
                                                      "target joined callback failed");
    }

    // The node lane was released around the joined callback above — the
    // only window in this synchronous PREPARE where a newer exact identity
    // for the same object can run try_add_admission's target_committing
    // eviction (spec 15 §4.2 newest-attempt-wins) concurrently. Re-check
    // before publishing the staged Actor: installing it for an already
    // evicted transfer would let a dead identity's late PREPARE win the
    // actor_key slot instead of discarding it (spec 15 §4.2 "이전 identity의
    // 늦은 chunk와 Restore는 조립에 연결하지 않고 폐기한다").
    if (!_state->actor_transfer_coordinator->is_current (key, transfer_id)) {
        fail_target_commit (false);
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::rejected,
          "remote Actor Join admission was superseded by a newer attempt");
    }
    const auto installed =
      _state->lane
        .run ([&] {
            const auto context = _state->spot_contexts_by_id.find (std::string (target_spot_id));
            if (context == _state->spot_contexts_by_id.end ()
                || context->second._state != plan.context
                || !target.accepts_membership_core (*_state)) {
                return false;
            }
            const auto reservation = _state->pending_actor_contexts.find (key);
            if (reservation == _state->pending_actor_contexts.end ()
                || reservation->second != target_spot_id) {
                return false;
            }
            const auto coordinator = target.ensure_spot_serial_executor ();
            if (!coordinator || !coordinator->ensure_actor_queue (key))
                return false;
            detail::record_actor_instance_index_unlocked (*_state, committed, actor.get ());
            _state->actor_instances[key] = std::move (actor);
            _state->destroyed_actor_keys.erase (key);
            // Core installs the target Actor during the finalize transfer fence.
            decrement_actor_count_unlocked (target);
            _state->pending_actor_contexts.erase (reservation);
            detail::record_actor_context_route_unlocked (*_state, key, plan.node_rid, target,
                                                         committed.object_generation ());
            return true;
        })
        .get ();
    if (!installed) {
        fail_target_commit (false);
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::unavailable,
          "target Spot closed before the remote Actor could be staged");
    }
    return result_t<actor_join_reply_t>::success (
      actor_join_reply_t{0, committed, zlink::message_t{}});
}

result_t<void> spot_node_runtime_t::stage_remote_actor_commit_backlog (
  const std::string &transfer_id, std::vector<handoff_packet_t> handoff_backlog)
{
    if (!_state->actor_transfer_coordinator->stage_commit_backlog (transfer_id,
                                                                   std::move (handoff_backlog))) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "target Actor handoff backlog could not be staged atomically");
    }
    return result_t<void>::success ();
}

task_t<result_t<void>> spot_node_runtime_t::commit_remote_actor_authority (
  std::string transfer_id,
  actor_ref_t actor_ref,
  spot_id_t target_spot_id,
  std::uint64_t target_spot_generation,
  std::uint64_t source_authority_owner_generation,
  std::string source_mesh_name,
  std::string target_mesh_name,
  std::uint64_t target_node_lifecycle_generation,
  location_owner_token_t target_owner,
  std::string expected_source_store_version,
  std::uint64_t *committed_previous_authority_owner_generation,
  std::uint64_t *committed_target_authority_owner_generation)
{
    const auto pending =
      _state->actor_transfer_coordinator->pending_commit (transfer_id, actor_ref, target_spot_id);
    if (!pending) {
        co_return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "remote Actor authority commit has no pending admission");
    }
    struct authority_commit_plan_t
    {
        std::shared_ptr<runtime::stateful::authority_relocation_port_t> relocation_authority;
        std::shared_ptr<service::mesh_node_t> native;
        runtime::location_lifecycle_t *location_lifecycle = nullptr;
        std::string target_node_id;
        std::string local_mesh_name;
        std::uint64_t membership_epoch = 0;
    };
    const auto plan = _state->lane
                        .run ([&] {
                            authority_commit_plan_t result;
                            result.relocation_authority = _state->relocation_authority;
                            result.native = _state->native_node.lock ();
                            result.location_lifecycle = _state->location_lifecycle;
                            result.target_node_id =
                              detail::effective_spot_node_rid (_state->snapshot);
                            result.local_mesh_name = _state->snapshot.name;
                            const auto epoch = _state->core_actor_membership_epochs.find (
                              std::string (actor_ref.actor_id ().value ()));
                            if (epoch != _state->core_actor_membership_epochs.end ())
                                result.membership_epoch = epoch->second;
                            return result;
                        })
                        .get ();
    if (!plan.relocation_authority) {
        co_return result_t<void>::failure (
          framework_error_kind_t::not_configured,
          "remote Actor authority commit requires a Location Store");
    }
    if (expected_source_store_version.empty () || source_authority_owner_generation == 0
        || source_authority_owner_generation == std::numeric_limits<std::uint64_t>::max ()
        || target_mesh_name.empty () || target_node_lifecycle_generation == 0
        || target_owner.owner_id.empty () || target_owner.lease_generation <= 0) {
        co_return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                           "remote Actor authority commit fence is invalid");
    }

    const auto &native = plan.native;
    const auto &target_node_id = plan.target_node_id;
    if (!native || native->status ().routing_id ().to_string () != target_node_id
        || native->status ().lifecycle_generation () != target_node_lifecycle_generation) {
        co_return result_t<void>::failure (framework_error_kind_t::unavailable,
                                           "remote Actor authority target lifecycle is stale");
    }
    if (target_spot_generation == 0 || source_mesh_name.empty () || target_mesh_name.empty ()) {
        co_return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                           "remote Actor authority route identity is incomplete");
    }
    const runtime::stateful::object_ref_t source{runtime::stateful::object_kind_t::actor,
                                                 std::string (actor_ref.actor_id ().value ()),
                                                 actor_ref.object_generation (),
                                                 source_authority_owner_generation,
                                                 std::move (source_mesh_name),
                                                 std::string (actor_ref.node_rid ().value ())};
    const runtime::stateful::object_ref_t target{
      runtime::stateful::object_kind_t::actor, source.key,       source.object_generation,
      source.authority_owner_generation,       target_mesh_name, target_node_id};
    const auto target_actor = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (target_node_id),
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
      std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation ());

    std::vector<std::byte> inventory_bytes;
    inventory_bytes.reserve (source.key.size () + sizeof (source.object_generation)
                             + sizeof (source.authority_owner_generation));
    for (const auto value : source.key)
        inventory_bytes.push_back (static_cast<std::byte> (static_cast<unsigned char> (value)));
    for (int shift = 56; shift >= 0; shift -= 8) {
        inventory_bytes.push_back (
          static_cast<std::byte> ((source.object_generation >> shift) & 0xffu));
        inventory_bytes.push_back (
          static_cast<std::byte> ((source.authority_owner_generation >> shift) & 0xffu));
    }
    const auto digest = runtime::sha256 (inventory_bytes);
    runtime::stateful::inventory_digest_t inventory_digest{};
    for (std::size_t index = 0; index != inventory_digest.size (); ++index)
        inventory_digest[index] = std::to_integer<std::uint8_t> (digest[index]);

    runtime::stateful::authority_publish_result_t published;
    try {
        const object_creation_target_t target_placement{
          target_mesh_name, node_rid_t::from_string (target_node_id),
          target_node_lifecycle_generation, target_owner};
        published = plan.relocation_authority->publish (
          source, target, target_owner, target_placement, {}, 0, inventory_digest,
          runtime::encode_actor_authority_payload (runtime::actor_authority_payload_t{
            .state = runtime::actor_authority_state_t::ready,
            .stable_type = std::string (
              ::zlink::framework::detail::actor_ref_access_t::actor_type (target_actor)),
            .actor_id = std::string (target_actor.actor_id ().value ()),
            .current_spot_id = target_spot_id,
            .current_spot_generation = target_spot_generation,
            .current_spot_kind = runtime::actor_authority_spot_kind_t::user,
            .owner_id = target_owner.owner_id,
            .owner_lease_generation = static_cast<std::uint64_t> (target_owner.lease_generation),
            .mesh_name = target_placement.mesh_name,
            .node_rid = target_placement.node_rid,
            .node_generation = target_placement.node_lifecycle_generation}),
          expected_source_store_version);
        if (published.status != runtime::stateful::authority_publish_status_t::published
            || !published.current) {
            published.current = plan.relocation_authority->read (source.kind, source.key);
        }
    }
    catch (const std::exception &error) {
        co_return result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ());
    }
    const auto &current = published.current;
    if (!current) {
        co_return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor authority commit conflicted with current authority");
    }
    if (current->source != source) {
        co_return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor authority commit conflicted with current authority");
    }
    const bool target_identity_matches =
      current->target.kind == target.kind && current->target.key == target.key
      && current->target.object_generation == target.object_generation
      && current->target.mesh_name == target.mesh_name && current->target.node_id == target.node_id
      && current->target.authority_owner_generation > current->source.authority_owner_generation;
    if (!target_identity_matches) {
        co_return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor authority commit conflicted with current authority");
    }
    if (current->target_owner.owner_id != target_owner.owner_id
        || current->target_owner.lease_generation != target_owner.lease_generation) {
        co_return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor authority commit conflicted with current authority");
    }
    if (!current->relocation_reference.empty () || current->checksum_crc32c != 0) {
        co_return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor authority commit conflicted with current authority");
    }
    if (current->inventory_digest != inventory_digest) {
        co_return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor authority commit conflicted with current authority");
    }
    //  Spec 28-relocation-flow §8: the committed target authority row is the
    //  node's own local Actor authority from this point on. Adopt it into the
    //  local object record so a LATER Join originating from this node fences
    //  with the generation the Store actually holds. Adoption is advisory —
    //  a record that is not in a locally advanceable state keeps the
    //  committed authority in the fence map below and reconciles on its own
    //  lifecycle path, so it must never fail the committed transfer.
    const runtime::stateful::object_ref_t committed_target{
      runtime::stateful::object_kind_t::actor,
      current->target.key,
      current->target.object_generation,
      current->target.authority_owner_generation,
      current->target.mesh_name,
      current->target.node_id};
    (void) co_await native->advance_local_actor_authority (committed_target, current->ready);
    const auto local_node_routing_id = native->status ().routing_id ().to_bytes ();
    _state->lane
      .run ([&] {
          _state->actor_authority_fences.insert_or_assign (
            actor_key (target_actor),
            runtime::protocol::actor_route_fence_t{
              std::string (target_actor.actor_id ().value ()), target_actor.object_generation (),
              local_node_routing_id, target_node_lifecycle_generation,
              current->target.authority_owner_generation,
              static_cast<std::uint64_t> (target_owner.lease_generation)});
      })
      .get ();
    //  Spec 28-relocation-flow §8: the cross-node Join moved the Actor owner
    //  to this target in the same CAS, and the staged transfer skips the
    //  ordinary local claim. Adopt the committed authority into this node's
    //  location lifecycle now — otherwise update_actor_location_after_move
    //  and a later Spot leave renew against an untracked entry.
    if (plan.location_lifecycle
        && !plan.location_lifecycle->owns_actor (actor_location_key_t{
          plan.local_mesh_name, std::string (target_actor.actor_id ().value ())})) {
        const auto adopted = plan.location_lifecycle->claim_actor (
          actor_location_t{
            .mesh_name = plan.local_mesh_name,
            .actor_id = std::string (target_actor.actor_id ().value ()),
            .actor_type = std::string (
              ::zlink::framework::detail::actor_ref_access_t::actor_type (target_actor)),
            .actor_ref = target_actor,
            .owner_node_rid = zlink::routing_id_t::from (std::string (target_node_id)),
            .owner_node_generation = target_node_lifecycle_generation,
            .spot_id = target_spot_id,
            .spot_generation = target_spot_generation,
            .spot_kind = zlink::spot_kind::user,
            .membership_epoch = plan.membership_epoch},
          [weak_state = std::weak_ptr<detail::spot_node_builder_state_t> (_state)] (
            const actor_location_t &lost) { deactivate_actor_location (weak_state, lost); },
          true);
        if (adopted.status != location_write_status_t::stored) {
            co_return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                               "committed remote Actor authority adoption failed");
        }
    }
    if (committed_previous_authority_owner_generation != nullptr)
        *committed_previous_authority_owner_generation = current->source.authority_owner_generation;
    if (committed_target_authority_owner_generation != nullptr)
        *committed_target_authority_owner_generation = current->target.authority_owner_generation;
    if (actor_transfer_marker_enabled ()) {
        emit_actor_transfer_marker ("location_committed", target_actor, transfer_id,
                                    target_spot_id);
    }
    co_return result_t<void>::success ();
}

std::optional<actor_join_reply_t>
spot_node_runtime_t::completed_remote_actor_commit (const std::string &transfer_id,
                                                    const actor_ref_t &source_actor,
                                                    const spot_id_t &target_spot_id) const
{
    const auto completed = _state->actor_transfer_coordinator->completed_commit (
      transfer_id, source_actor, target_spot_id);
    if (!completed)
        return std::nullopt;
    const auto target_actor = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid (),
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (source_actor)),
      std::string (source_actor.actor_id ().value ()), source_actor.object_generation ());
    return actor_join_reply_t{0, target_actor, zlink::message_t{}};
}

task_t<actor_join_reply_t>
spot_node_runtime_t::commit_remote_actor_to_spot (std::string transfer_id,
                                                  const actor_ref_t &actor_ref,
                                                  spot_id_t target_spot_id,
                                                  zlink::message_t transfer_state,
                                                  actor_context_t actor_context,
                                                  std::vector<handoff_packet_t> handoff_backlog,
                                                  service_provider_t *services)
{
    std::optional<actor_gateway_runtime_t> actor_gateway;
    if (actor_context._state)
        actor_gateway.emplace (actor_context._state);
    auto prepared = prepare_remote_actor_to_spot (
      transfer_id, actor_ref, target_spot_id, std::move (transfer_state), std::move (actor_context),
      actor_gateway.has_value ());
    if (!prepared) {
        co_return prepared;
    }
    if (!handoff_backlog.empty () && services == nullptr) {
        fail_handoff_backlog (_state,
                              _state->actor_transfer_coordinator->fail_commit (transfer_id, true));
        co_return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote actor handoff backlog requires a service provider");
    }
    const auto staged =
      stage_remote_actor_commit_backlog (transfer_id, std::move (handoff_backlog));
    if (!staged) {
        fail_handoff_backlog (_state,
                              _state->actor_transfer_coordinator->fail_commit (transfer_id, true));
        co_return detail::propagate_failure<actor_join_reply_t> (
          staged, "remote actor handoff backlog staging failed");
    }
    if (services != nullptr) {
        co_return co_await finalize_remote_actor_to_spot (
          std::move (transfer_id), actor_ref, std::move (target_spot_id), *services,
          actor_gateway ? &*actor_gateway : nullptr);
    }
    service_collection_t empty_services;
    auto empty_provider = empty_services.build_provider ();
    co_return co_await finalize_remote_actor_to_spot (std::move (transfer_id), actor_ref,
                                                      std::move (target_spot_id), empty_provider,
                                                      actor_gateway ? &*actor_gateway : nullptr);
}

void spot_node_runtime_t::finalize_remote_actor_to_spot_async (
  std::string transfer_id,
  const actor_ref_t &actor_ref,
  spot_id_t target_spot_id,
  service_provider_t &services,
  actor_gateway_runtime_t *actor_gateway,
  std::optional<std::chrono::steady_clock::time_point> deadline,
  std::function<void (result_t<actor_join_reply_t>)> completion,
  std::function<task_t<void> ()> submit_source_leave)
{
    if (!completion)
        return;
    struct completion_state_t
    {
        std::mutex mutex;
        bool settled = false;
        std::function<void (result_t<actor_join_reply_t>)> callback;
    };
    auto completion_state = std::make_shared<completion_state_t> ();
    completion_state->callback = std::move (completion);
    auto complete_result = [completion_state] (result_t<actor_join_reply_t> result) mutable {
        std::function<void (result_t<actor_join_reply_t>)> callback;
        {
            std::lock_guard lock (completion_state->mutex);
            if (completion_state->settled)
                return;
            completion_state->settled = true;
            callback = std::move (completion_state->callback);
        }
        if (callback)
            callback (std::move (result));
    };
    const auto pending =
      _state->actor_transfer_coordinator->pending_commit (transfer_id, actor_ref, target_spot_id);
    if (!pending) {
        complete_result (result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote actor finalize has no matching prepared commit"));
        return;
    }
    if (pending->completion_operation_id_high == 0 && pending->completion_operation_id_low == 0) {
        fail_handoff_backlog (_state,
                              _state->actor_transfer_coordinator->fail_commit (transfer_id, true));
        complete_result (result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote Actor Join completion OperationId is invalid"));
        return;
    }
    const auto key = actor_key (actor_ref);
    struct finalize_plan_t
    {
        std::shared_ptr<spot_context_state_t> target_state;
        std::shared_ptr<void> actor_instance;
        std::shared_ptr<spot_serial_executor_t> executor;
        const void *actor_queue_identity = nullptr;
        std::shared_ptr<runtime::offload_executor_t> deadline_executor;
        std::function<task_t<void> (void *, void *)> joined_callback;
        std::string node_rid;
        std::chrono::milliseconds request_timeout{
          std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::seconds (30))};
        bool dependencies_available = false;
        bool lifecycle_available = true;
    };
    const auto plan =
      _state->lane
        .run ([&] {
            finalize_plan_t result;
            result.node_rid = detail::effective_spot_node_rid (_state->snapshot);
            if (_state->channel_runtime)
                result.request_timeout = _state->channel_runtime->default_request_timeout;
            const auto context = _state->spot_contexts_by_id.find (std::string (target_spot_id));
            const auto factory = _state->actor_factories.find (
              std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
            const auto actor = _state->actor_instances.find (key);
            if (context == _state->spot_contexts_by_id.end () || !context->second._state
                || !context->second._state->spot_instance
                || factory == _state->actor_factories.end ()
                || actor == _state->actor_instances.end () || !actor->second) {
                return result;
            }
            result.target_state = context->second._state;
            result.actor_instance = actor->second;
            if (actor_gateway != nullptr) {
                const auto admission =
                  result.target_state->actor_admissions.find (factory->second.actor_type);
                if (admission == result.target_state->actor_admissions.end ()) {
                    result.lifecycle_available = false;
                    return result;
                }
                result.joined_callback = admission->second.on_actor_joined;
            }
            result.executor = result.target_state->ensure_spot_serial_executor ();
            if (!result.executor)
                return result;
            result.actor_queue_identity = result.executor->actor_queue_identity (key);
            if (!result.actor_queue_identity)
                return result;
            result.deadline_executor = framework_deadline_executor_core (_state);
            result.dependencies_available = true;
            return result;
        })
        .get ();
    if (!plan.dependencies_available) {
        fail_handoff_backlog (_state,
                              _state->actor_transfer_coordinator->fail_commit (transfer_id, true));
        complete_result (result_t<actor_join_reply_t>::failure (
          plan.lifecycle_available ? framework_error_kind_t::not_found
                                   : framework_error_kind_t::not_found,
          plan.lifecycle_available ? "prepared remote actor commit dependencies are unavailable"
                                   : "target spot actor lifecycle is not registered"));
        return;
    }
    if (!deadline)
        deadline = std::chrono::steady_clock::now () + plan.request_timeout;
    const auto committed = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (plan.node_rid),
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
      std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation ());
    const auto actor_instance = plan.actor_instance;
    const auto target_state = plan.target_state;
    const auto executor = plan.executor;
    std::optional<actor_gateway_runtime_t> actor_gateway_owner;
    if (actor_gateway != nullptr)
        actor_gateway_owner.emplace (*actor_gateway);
    auto joined_callback = plan.joined_callback;
    std::shared_ptr<join_completion_delivery_fence_scope_t> delivery_scope;
    auto fail_delivery_setup = [node = _state, transfer_id, complete_result] (
                                 framework_error_kind_t kind, const char *error) mutable noexcept {
        try {
            fail_handoff_backlog (
              node, node->actor_transfer_coordinator->fail_commit (transfer_id, true));
        }
        catch (...) {
        }
        try {
            complete_result (result_t<actor_join_reply_t>::failure (kind, error));
        }
        catch (...) {
        }
    };
    if (actor_gateway_owner) {
        try {
            const auto updated = actor_gateway_owner->update_actor_ref (committed);
            if (!updated) {
                fail_delivery_setup (updated.error_kind (),
                                     updated.error () != nullptr
                                       ? updated.error ()->what ()
                                       : "target actor gateway ref update failed");
                return;
            }
            if (joined_callback) {
                delivery_scope = join_completion_delivery_fence_scope_t::begin (
                  *actor_gateway_owner, committed, [fail_delivery_setup] () mutable noexcept {
                      fail_delivery_setup (framework_error_kind_t::unavailable,
                                           "remote Actor Join delivery scope was abandoned");
                  });
            }
        }
        catch (const framework_exception_t &error) {
            fail_delivery_setup (error.kind (), error.what ());
            return;
        }
        catch (const std::exception &error) {
            fail_delivery_setup (framework_error_kind_t::internal_failure, error.what ());
            return;
        }
        catch (...) {
            fail_delivery_setup (framework_error_kind_t::internal_failure,
                                 "Actor Join delivery fence initialization failed");
            return;
        }
    }
    const auto actor_queue_identity = plan.actor_queue_identity;
    std::weak_ptr<spot_node_builder_state_t> node_owner = _state;
    std::weak_ptr<spot_context_state_t> target_state_owner = target_state;
    std::weak_ptr<void> actor_instance_owner = actor_instance;
    auto services_owner = services;

    auto commit_state = std::make_shared<remote_actor_commit_turn_state_t> ();
    struct submission_state_t
    {
        enum class phase_t
        {
            lifecycle,
            actor
        };

        std::mutex mutex;
        std::weak_ptr<runtime::serial_execution_queue_t> queue;
        std::optional<runtime::serial_submission_id_t> id;
        bool cancellation_requested = false;
        bool lifecycle_was_active = false;
        bool lifecycle_started = false;
        phase_t phase = phase_t::lifecycle;
    };
    struct deadline_state_t
    {
        std::mutex mutex;
        std::shared_ptr<remote_actor_commit_deadline_t> timer;
    };
    auto submission_state = std::make_shared<submission_state_t> ();
    auto track_submission =
      [submission_state] (const std::shared_ptr<runtime::serial_execution_queue_t> &queue,
                          runtime::serial_submission_id_t id) {
          bool cancel = false;
          {
              std::lock_guard lock (submission_state->mutex);
              submission_state->queue = queue;
              submission_state->id = id;
              cancel = submission_state->cancellation_requested;
          }
          if (cancel)
              (void) queue->cancel_submission (id);
      };
    auto deadline_state = std::make_shared<deadline_state_t> ();
    auto deadline_executor = plan.deadline_executor;
    std::weak_ptr<spot_node_builder_state_t> terminal_node = _state;
    auto finish_terminal =
      [terminal_node, transfer_id, committed, deadline_state,
       complete_result] (remote_actor_commit_turn_state_t::outcome_t outcome) mutable noexcept {
          try {
              std::shared_ptr<remote_actor_commit_deadline_t> timer;
              {
                  std::lock_guard lock (deadline_state->mutex);
                  timer = std::move (deadline_state->timer);
              }
              if (timer)
                  timer->cancel ();
              auto node = terminal_node.lock ();
              if (!outcome.success && node)
                  fail_handoff_backlog (
                    node, node->actor_transfer_coordinator->fail_commit (transfer_id, true));
              if (outcome.success) {
                  complete_result (result_t<actor_join_reply_t>::success (
                    actor_join_reply_t{0, committed, zlink::message_t{}}));
              } else {
                  complete_result (result_t<actor_join_reply_t>::failure (
                    outcome.error_kind,
                    outcome.error.empty () ? "remote Actor handoff replay failed" : outcome.error));
              }
          }
          catch (...) {
              try {
                  complete_result (result_t<actor_join_reply_t>::failure (
                    framework_error_kind_t::internal_failure,
                    "remote Actor handoff terminal settlement failed"));
              }
              catch (...) {
              }
          }
      };
    commit_state->on_terminal (
      [delivery_scope,
       finish_terminal] (remote_actor_commit_turn_state_t::outcome_t outcome) mutable noexcept {
          try {
              if (!delivery_scope) {
                  finish_terminal (std::move (outcome));
                  return;
              }
              auto fence_result =
                outcome.success ? result_t<void>::success ()
                                : result_t<void>::failure (outcome.error_kind,
                                                           outcome.error.empty ()
                                                             ? "remote Actor handoff replay failed"
                                                             : outcome.error);
              delivery_scope->settle (
                std::move (fence_result), [finish_terminal, outcome = std::move (outcome)] (
                                            result_t<void> settled) mutable noexcept {
                    try {
                        if (!settled) {
                            outcome = remote_actor_commit_turn_state_t::outcome_t{
                              false, settled.error_kind (),
                              settled.error () != nullptr ? settled.error ()->what ()
                                                          : "bound Session delivery failed"};
                        }
                        finish_terminal (std::move (outcome));
                    }
                    catch (...) {
                        finish_terminal (remote_actor_commit_turn_state_t::outcome_t{
                          false, framework_error_kind_t::internal_failure,
                          "bound Session delivery settlement failed"});
                    }
                });
          }
          catch (...) {
              finish_terminal (remote_actor_commit_turn_state_t::outcome_t{
                false, framework_error_kind_t::internal_failure,
                "remote Actor handoff delivery settlement failed"});
          }
      });
    if (deadline) {
        std::weak_ptr<remote_actor_commit_turn_state_t> weak_commit = commit_state;
        auto timer = remote_actor_commit_deadline_t::start (
          *deadline, deadline_executor, _state->runtime_failures (),
          [weak_commit, submission_state] {
              auto commit = weak_commit.lock ();
              if (!commit)
                  return;
              commit->select_cancel_error (
                framework_error_kind_t::deadline_exceeded,
                "remote Actor handoff replay exceeded the Join deadline");
              std::shared_ptr<runtime::serial_execution_queue_t> queue;
              std::optional<runtime::serial_submission_id_t> id;
              submission_state_t::phase_t phase;
              bool lifecycle_started = false;
              {
                  std::lock_guard lock (submission_state->mutex);
                  submission_state->cancellation_requested = true;
                  queue = submission_state->queue.lock ();
                  id = submission_state->id;
                  phase = submission_state->phase;
                  lifecycle_started = submission_state->lifecycle_started;
                  if (phase == submission_state_t::phase_t::lifecycle && (!queue || !id)) {
                      submission_state->lifecycle_was_active = lifecycle_started;
                  }
              }
              if (queue && id)
                  (void) queue->cancel_submission (*id);
              if (phase == submission_state_t::phase_t::lifecycle)
                  return;
              commit->request_stop ();
          });
        bool terminal = false;
        {
            std::lock_guard lock (deadline_state->mutex);
            deadline_state->timer = timer;
            terminal = commit_state->terminal ();
        }
        if (terminal) {
            timer->cancel ();
            std::lock_guard lock (deadline_state->mutex);
            deadline_state->timer.reset ();
        }
    }
    const auto completion_operation_high = pending->completion_operation_id_high;
    const auto completion_operation_low = pending->completion_operation_id_low;
    const auto admission_reply = pending->admission_reply;
    auto submit_actor_turn = [node_owner, actor_ref, committed, key, target_spot_id,
                              target_state_owner, actor_instance_owner, executor,
                              actor_queue_identity, services_owner = std::move (services_owner),
                              transfer_id, deadline, completion_operation_high,
                              completion_operation_low, admission_reply, commit_state,
                              delivery_scope, submission_state,
                              submit_source_leave = std::move (submit_source_leave),
                              track_submission] (result_t<void> joined) mutable {
        bool cancellation_requested = false;
        bool lifecycle_was_active = false;
        {
            std::lock_guard lock (submission_state->mutex);
            submission_state->queue.reset ();
            submission_state->id.reset ();
            cancellation_requested = submission_state->cancellation_requested;
            lifecycle_was_active =
              submission_state->lifecycle_was_active
              || (cancellation_requested && submission_state->lifecycle_started);
            submission_state->phase = submission_state_t::phase_t::actor;
            if (lifecycle_was_active)
                submission_state->cancellation_requested = false;
        }
        if (cancellation_requested && !lifecycle_was_active) {
            commit_state->request_stop ();
            return;
        }
        if (commit_state->terminal ())
            return;
        if (cancellation_requested && lifecycle_was_active) {
            joined =
              result_t<void>::failure (framework_error_kind_t::deadline_exceeded,
                                       "spot actor joined callback exceeded the Join deadline");
        } else if (joined && deadline && std::chrono::steady_clock::now () >= *deadline) {
            joined =
              result_t<void>::failure (framework_error_kind_t::deadline_exceeded,
                                       "spot actor joined callback exceeded the Join deadline");
        }
        auto submitted = executor->execute_actor_cancellable (
          key, "remote-actor-commit-replay",
          [node_owner, actor_ref, committed, key, target_spot_id, target_state_owner,
           actor_instance_owner, actor_queue_identity, executor,
           services_owner = std::move (services_owner), joined = std::move (joined), transfer_id,
           deadline, completion_operation_high, completion_operation_low, admission_reply,
           commit_state, delivery_scope,
           submit_source_leave = std::move (submit_source_leave)] (auto actor_complete) mutable {
              auto settle_turn =
                [commit_state, actor_complete] (
                  remote_actor_commit_turn_state_t::outcome_t outcome) mutable noexcept {
                    try {
                        actor_complete (
                          [commit_state, outcome = std::move (outcome)] () mutable noexcept {
                              try {
                                  commit_state->settle (std::move (outcome));
                              }
                              catch (...) {
                              }
                          });
                    }
                    catch (...) {
                        try {
                            commit_state->settle (remote_actor_commit_turn_state_t::outcome_t{
                              false, framework_error_kind_t::internal_failure,
                              "remote Actor commit turn settlement failed"});
                        }
                        catch (...) {
                        }
                    }
                };
              if (!commit_state->begin_active ()) {
                  actor_complete ([] {});
                  return;
              }

              auto node = node_owner.lock ();
              auto target_state = target_state_owner.lock ();
              auto actor_instance = actor_instance_owner.lock ();
              if (!node || !target_state || !actor_instance) {
                  settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                    false, framework_error_kind_t::shutting_down,
                    "remote Actor handoff owner was released before activation"});
                  return;
              }
              auto replay_services = std::move (services_owner);

              runtime::actor_execution_scope_t actor_scope (key, std::string (target_spot_id));
              auto fail_precommit = [node, transfer_id, settle_turn] (framework_error_kind_t kind,
                                                                      std::string error) mutable {
                  fail_handoff_backlog (
                    node, node->actor_transfer_coordinator->fail_commit (transfer_id, true));
                  settle_turn (
                    remote_actor_commit_turn_state_t::outcome_t{false, kind, std::move (error)});
              };
              try {
                  const auto exact_pending = node->actor_transfer_coordinator->pending_commit (
                    transfer_id, actor_ref, target_spot_id);
                  const auto owner_fence_changed =
                    !exact_pending
                    || !node->lane
                          .run ([&] {
                              const auto context =
                                node->spot_contexts_by_id.find (std::string (target_spot_id));
                              const auto actor = node->actor_instances.find (key);
                              const auto generation = node->actor_generations.find (key);
                              if (!exact_pending || context == node->spot_contexts_by_id.end ()
                                  || context->second._state != target_state
                                  || !target_state->spot_instance
                                  || actor == node->actor_instances.end ()
                                  || actor->second != actor_instance
                                  || generation == node->actor_generations.end ()
                                  || generation->second != committed.object_generation ()
                                  || !executor->actor_queue_matches (key, actor_queue_identity)) {
                                  return false;
                              }
                              return true;
                          })
                          .get ();
                  if (owner_fence_changed) {
                      fail_handoff_backlog (
                        node, node->actor_transfer_coordinator->fail_commit (transfer_id, true));
                      settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                        false, framework_error_kind_t::invalid_operation,
                        "remote Actor handoff owner fence changed before activation"});
                      return;
                  }
                  // This transition decides the deadline race before any Join
                  // completion or location publication. Once it succeeds, the
                  // accepted transfer owns the Actor turn until every retained
                  // packet settles; later cancellation cannot truncate replay.
                  if (!commit_state->try_begin_commit ()) {
                      const auto cancelled = commit_state->cancel_outcome ();
                      fail_precommit (cancelled.error_kind, cancelled.error);
                      return;
                  }

                  const bool target_joined = static_cast<bool> (joined);
                  auto continue_after_joined = [node, actor_ref, committed, key, target_spot_id,
                                                target_state, actor_instance, actor_queue_identity,
                                                executor, services = std::move (replay_services),
                                                transfer_id, deadline, completion_operation_high,
                                                completion_operation_low, admission_reply,
                                                commit_state, delivery_scope,
                                                joined = std::move (joined),
                                                settle_turn] () mutable noexcept {
                      try {
                          runtime::actor_execution_scope_t actor_scope (
                            key, std::string (target_spot_id));
                          std::optional<std::pair<framework_error_kind_t, std::string>>
                            completion_failure;
                          if (!joined) {
                              completion_failure = std::make_pair (
                                joined.error_kind (),
                                joined.error () != nullptr
                                  ? std::string (joined.error ()->what ())
                                  : std::string ("spot actor joined callback failed"));
                          }
                          actor_join_completion_t completion =
                            completion_failure
                              ? actor_join_completion_t (actor_join_failed_t{
                                  completion_operation_high, completion_operation_low,
                                  completion_failure->first})
                              : actor_join_completion_t (actor_join_accepted_t{
                                  completion_operation_high, completion_operation_low, committed,
                                  admission_reply});
                          std::function<void (result_t<void>, std::function<void (result_t<void>)>)>
                            settle_delivery;
                          if (delivery_scope) {
                              settle_delivery =
                                [delivery_scope] (
                                  result_t<void> result,
                                  std::function<void (result_t<void>)> settled) mutable {
                                    delivery_scope->settle (std::move (result),
                                                            std::move (settled));
                                };
                          }
                          spot_node_runtime_t (node).deliver_actor_join_completion_async (
                            committed, std::move (completion), target_spot_id,
                            [node, actor_ref, committed, key, target_spot_id, target_state,
                             actor_instance, actor_queue_identity, executor,
                             services = std::move (services), transfer_id, completion_failure,
                             commit_state,
                             settle_turn] (result_t<void> delivered) mutable noexcept {
                                try {
                                    runtime::actor_execution_scope_t actor_scope (
                                      key, std::string (target_spot_id));
                                    if (!delivered) {
                                        fail_handoff_backlog (
                                          node, node->actor_transfer_coordinator->fail_commit (
                                                  transfer_id, true));
                                        settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                                          false, delivered.error_kind (),
                                          delivered.error () != nullptr
                                            ? delivered.error ()->what ()
                                            : "remote Actor Join completion callback failed"});
                                        return;
                                    }
                                    if (completion_failure) {
                                        fail_handoff_backlog (
                                          node, node->actor_transfer_coordinator->fail_commit (
                                                  transfer_id, true));
                                        settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                                          false, completion_failure->first,
                                          completion_failure->second});
                                        return;
                                    }

                                    std::optional<std::vector<handoff_packet_t>> replay;
                                    std::optional<remote_actor_commit_turn_state_t::outcome_t>
                                      commit_failure;
                                    const auto exact_pending =
                                      node->actor_transfer_coordinator->pending_commit (
                                        transfer_id, actor_ref, target_spot_id);
                                    struct completion_plan_t
                                    {
                                        bool valid = false;
                                        std::function<result_t<void> (const actor_ref_t &)>
                                          update_actor_ref;
                                    };
                                    const auto plan =
                                      node->lane
                                        .run ([&] {
                                            completion_plan_t result;
                                            const auto context = node->spot_contexts_by_id.find (
                                              std::string (target_spot_id));
                                            const auto actor = node->actor_instances.find (key);
                                            const auto generation =
                                              node->actor_generations.find (key);
                                            if (!exact_pending
                                                || context == node->spot_contexts_by_id.end ()
                                                || context->second._state != target_state
                                                || !target_state->spot_instance
                                                || actor == node->actor_instances.end ()
                                                || actor->second != actor_instance
                                                || generation == node->actor_generations.end ()
                                                || generation->second
                                                     != committed.object_generation ()
                                                || !executor->actor_queue_matches (
                                                  key, actor_queue_identity)) {
                                                return result;
                                            }
                                            result.valid = true;
                                            result.update_actor_ref =
                                              node->update_actor_registry_ref;
                                            return result;
                                        })
                                        .get ();
                                    if (!exact_pending || !plan.valid) {
                                        commit_failure.emplace (
                                          false, framework_error_kind_t::invalid_operation,
                                          "remote Actor handoff owner fence changed during "
                                          "completion");
                                    } else {
                                        const auto location_updated =
                                          update_actor_location_after_move (node, committed,
                                                                            *target_state, false);
                                        if (!location_updated) {
                                            commit_failure.emplace (
                                              false, location_updated.error_kind (),
                                              location_updated.error () != nullptr
                                                ? location_updated.error ()->what ()
                                                : "actor committed location update failed");
                                        } else if (plan.update_actor_ref) {
                                            const auto updated = plan.update_actor_ref (committed);
                                            if (!updated) {
                                                commit_failure.emplace (
                                                  false, updated.error_kind (),
                                                  updated.error () != nullptr
                                                    ? updated.error ()->what ()
                                                    : "actor ref update failed");
                                            }
                                        }
                                    }
                                    if (!commit_failure) {
                                        const auto still_pending =
                                          node->actor_transfer_coordinator->pending_commit (
                                            transfer_id, actor_ref, target_spot_id);
                                        const auto still_owned =
                                          still_pending
                                          && node->lane
                                               .run ([&] {
                                                   const auto context =
                                                     node->spot_contexts_by_id.find (
                                                       std::string (target_spot_id));
                                                   const auto actor =
                                                     node->actor_instances.find (key);
                                                   const auto generation =
                                                     node->actor_generations.find (key);
                                                   return context
                                                            != node->spot_contexts_by_id.end ()
                                                          && context->second._state == target_state
                                                          && target_state->spot_instance
                                                          && actor != node->actor_instances.end ()
                                                          && actor->second == actor_instance
                                                          && generation
                                                               != node->actor_generations.end ()
                                                          && generation->second
                                                               == committed.object_generation ()
                                                          && executor->actor_queue_matches (
                                                            key, actor_queue_identity);
                                               })
                                               .get ();
                                        if (still_owned) {
                                            replay = node->actor_transfer_coordinator
                                                       ->complete_commit_and_take_backlog (
                                                         transfer_id, actor_ref, target_spot_id);
                                        } else {
                                            commit_failure.emplace (
                                              false, framework_error_kind_t::invalid_operation,
                                              "remote Actor handoff owner fence changed during "
                                              "completion");
                                        }
                                    }
                                    if (commit_failure) {
                                        fail_handoff_backlog (
                                          node, node->actor_transfer_coordinator->fail_commit (
                                                  transfer_id, true));
                                    }
                                    if (commit_failure) {
                                        settle_turn (std::move (*commit_failure));
                                        return;
                                    }
                                    if (!replay) {
                                        fail_handoff_backlog (
                                          node, node->actor_transfer_coordinator->fail_commit (
                                                  transfer_id, true));
                                        settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                                          false, framework_error_kind_t::invalid_operation,
                                          "remote Actor commit fence changed before replay "
                                          "publication"});
                                        return;
                                    }
                                    if (node->location_lifecycle) {
                                        const auto actors = executor->actor_executor_snapshot ();
                                        if (actors) {
                                            const auto actor = actors->find (key);
                                            if (actor != actors->end ())
                                                actor->second->queue ()->confirm_execution_ready (
                                                  node->location_lifecycle->execution_runtime ()
                                                    .execution_clock () ());
                                        }
                                    }
                                    commit_state->mark_committed ();
                                    auto replay_owner =
                                      std::make_shared<spot_node_runtime_t> (node);
                                    auto replay_task = replay_owner->replay_actor_handoff_batch (
                                      committed, std::move (*replay), services, transfer_id, true,
                                      {});
                                    detail::observe_task_completion (
                                      replay_task, [settle_turn, replay_owner] (
                                                     const result_t<void> &result) mutable {
                                          settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                                            static_cast<bool> (result),
                                            result ? framework_error_kind_t::internal_failure
                                                   : result.error_kind (),
                                            result.error () ? result.error ()->what ()
                                                            : std::string{}});
                                      });
                                }
                                catch (const framework_exception_t &error) {
                                    try {
                                        fail_handoff_backlog (
                                          node, node->actor_transfer_coordinator->fail_commit (
                                                  transfer_id, true));
                                    }
                                    catch (...) {
                                    }
                                    settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                                      false, error.kind (), error.what ()});
                                }
                                catch (const std::exception &error) {
                                    try {
                                        fail_handoff_backlog (
                                          node, node->actor_transfer_coordinator->fail_commit (
                                                  transfer_id, true));
                                    }
                                    catch (...) {
                                    }
                                    settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                                      false, framework_error_kind_t::internal_failure,
                                      error.what ()});
                                }
                                catch (...) {
                                    try {
                                        fail_handoff_backlog (
                                          node, node->actor_transfer_coordinator->fail_commit (
                                                  transfer_id, true));
                                    }
                                    catch (...) {
                                    }
                                    settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                                      false, framework_error_kind_t::internal_failure,
                                      "remote Actor Join completion settlement failed"});
                                }
                            },
                            std::move (settle_delivery));
                      }
                      catch (const framework_exception_t &error) {
                          try {
                              fail_handoff_backlog (
                                node,
                                node->actor_transfer_coordinator->fail_commit (transfer_id, true));
                          }
                          catch (...) {
                          }
                          settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                            false, error.kind (), error.what ()});
                      }
                      catch (const std::exception &error) {
                          try {
                              fail_handoff_backlog (
                                node,
                                node->actor_transfer_coordinator->fail_commit (transfer_id, true));
                          }
                          catch (...) {
                          }
                          settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                            false, framework_error_kind_t::internal_failure, error.what ()});
                      }
                      catch (...) {
                          try {
                              fail_handoff_backlog (
                                node,
                                node->actor_transfer_coordinator->fail_commit (transfer_id, true));
                          }
                          catch (...) {
                          }
                          settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                            false, framework_error_kind_t::internal_failure,
                            "remote Actor Join completion delivery failed"});
                      }
                  };

                  if (!target_joined || !submit_source_leave) {
                      continue_after_joined ();
                      return;
                  }
                  auto report_source_leave_failure =
                    [node, actor_ref, transfer_id] (const result_t<void> &submitted) {
                        if (submitted)
                            return;
                        report_spot_dispatch_trace (
                          node, message_flow_outcome_t::completed,
                          dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::control,
                          "spot_actor_leave_submit", {}, {}, actor_ref.actor_id ().value (),
                          transfer_id, message_flow_result_t::failed, std::nullopt,
                          "leave_submit_failed",
                          submitted.error () ? std::make_exception_ptr (*submitted.error ())
                                             : nullptr);
                    };
                  try {
                      auto submitted_leave =
                        std::make_shared<task_t<void>> (submit_source_leave ());
                      detail::observe_task_terminal (
                        *submitted_leave, [submitted_leave, report_source_leave_failure] (
                                            const result_t<void> &submitted) {
                            report_source_leave_failure (submitted);
                        });
                  }
                  catch (const framework_exception_t &error) {
                      report_source_leave_failure (detail::result_access_t::failure<void> (error));
                  }
                  catch (const std::exception &error) {
                      report_source_leave_failure (result_t<void>::failure (
                        framework_error_kind_t::internal_failure, error.what ()));
                  }
                  catch (...) {
                      report_source_leave_failure (
                        result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                 "source Actor leave submission failed"));
                  }
                  continue_after_joined ();
              }
              catch (const framework_exception_t &error) {
                  fail_precommit (error.kind (), error.what ());
              }
              catch (const std::exception &error) {
                  fail_precommit (framework_error_kind_t::internal_failure, error.what ());
              }
              catch (...) {
                  fail_precommit (framework_error_kind_t::internal_failure,
                                  "remote Actor handoff owner failed during activation");
              }
          },
          [commit_state] { commit_state->request_stop (); },
          runtime::serial_work_options_t{runtime::serial_work_lane_t::application});
        if (!submitted) {
            commit_state->settle (remote_actor_commit_turn_state_t::outcome_t{
              false, submitted.error_kind (),
              submitted.error () != nullptr
                ? submitted.error ()->what ()
                : "remote Actor handoff replay owner could not be reserved"});
            return;
        }
        track_submission (submitted.value ().queue, submitted.value ().id);
    };

    if (joined_callback) {
        target_state->run_serial_task_async (
          "spot-actor-transfer-joined",
          [target_state, actor_instance, joined_callback = std::move (joined_callback)] () mutable {
              return joined_callback (target_state->spot_instance.get (), actor_instance.get ());
          },
          std::move (submit_actor_turn), track_submission,
          [submission_state] {
              std::lock_guard lock (submission_state->mutex);
              submission_state->lifecycle_started = true;
              if (submission_state->cancellation_requested)
                  submission_state->lifecycle_was_active = true;
          },
          [submission_state] (bool started) {
              std::lock_guard lock (submission_state->mutex);
              if (submission_state->phase == submission_state_t::phase_t::lifecycle) {
                  submission_state->lifecycle_was_active = started;
              }
          });
    } else {
        submit_actor_turn (result_t<void>::success ());
    }
}

task_t<actor_join_reply_t> spot_node_runtime_t::finalize_remote_actor_to_spot (
  std::string transfer_id,
  const actor_ref_t &actor_ref,
  spot_id_t target_spot_id,
  service_provider_t &services,
  actor_gateway_runtime_t *actor_gateway,
  std::optional<std::chrono::steady_clock::time_point> deadline)
{
    auto completion = std::make_shared<task_completion_source_t<actor_join_reply_t>> ();
    auto result = completion->task ();
    finalize_remote_actor_to_spot_async (
      std::move (transfer_id), actor_ref, std::move (target_spot_id), services, actor_gateway,
      deadline, [completion] (result_t<actor_join_reply_t> value) mutable {
          completion->complete (std::move (value));
      });
    return result;
}

task_t<actor_join_reply_t> spot_node_runtime_t::join_actor_to_entry_spot_erased (
  const actor_ref_t &actor_ref,
  node_rid_t spot_node_rid,
  const zlink::message_t &request,
  const std::optional<zlink::message_t> &actor_snapshot,
  actor_context_t actor_context,
  std::function<void (std::function<void (result_t<void>)>)> *source_leave,
  std::optional<std::chrono::steady_clock::time_point> deadline)
{
    /* graceful-drain-handoff §4-2/§5.2: a draining node rejects new actor
    * admission and joins; already-admitted transfer commits stay accepted. */
    if (_state->host_draining ()) {
        co_return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::shutting_down,
          "spot node is draining and rejects new actor joins");
    }
    enum class entry_selection_t
    {
        selected,
        node_mismatch,
        not_registered,
        not_created
    };
    struct entry_selection_result_t
    {
        entry_selection_t selection = entry_selection_t::not_created;
        std::optional<spot_id_t> entry_id;
    };
    const auto entry =
      _state->lane
        .run ([&] {
            entry_selection_result_t result;
            if (spot_node_rid.empty ()
                || spot_node_rid.value () != detail::effective_spot_node_rid (_state->snapshot)) {
                result.selection = entry_selection_t::node_mismatch;
                return result;
            }
            if (!_state->snapshot.entry_spot_name) {
                result.selection = entry_selection_t::not_registered;
                return result;
            }
            const auto entry_id = _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
            if (entry_id == _state->spot_ids_by_name.end ()) {
                result.selection = entry_selection_t::not_created;
                return result;
            }
            result.selection = entry_selection_t::selected;
            result.entry_id = entry_id->second;
            return result;
        })
        .get ();
    if (entry.selection == entry_selection_t::node_mismatch) {
        co_return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                         "spot node rid does not match this node");
    }
    if (entry.selection == entry_selection_t::not_registered) {
        co_return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                         "entry spot is not registered");
    }
    if (!entry.entry_id) {
        co_return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                         "entry spot is not created");
    }
    co_return co_await join_actor_to_spot_erased (actor_ref, *entry.entry_id, request,
                                                  actor_snapshot, std::move (actor_context), 0, 0,
                                                  source_leave, deadline);
}

void spot_node_runtime_t::record_core_actor_transfer_activation (std::string actor_id,
                                                                 std::uint64_t membership_epoch)
{
    _state->lane
      .run ([&] {
          _state->mesh_runtime_owned_native_actor_ids.insert (actor_id);
          _state->core_actor_membership_epochs[std::move (actor_id)] = membership_epoch;
      })
      .get ();
}

task_t<zlink::submit_result_t>
spot_node_runtime_t::send_actor_leave_notification (const zlink::routing_id_t &target_node_rid,
                                                    runtime::messaging::message_parts_t parts) const
{
    auto sender = _state->lane.run ([&] { return _state->actor_leave_notification_sender; }).get ();
    if (!sender) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Actor OnLeave node notification transport is not configured");
    }
    auto native_parts = parts.items ();
    if (native_parts.empty ()) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "Actor OnLeave notification requires at least one message part");
    }
    co_return co_await sender (target_node_rid, std::move (native_parts));
}

result_t<std::uint64_t> spot_node_runtime_t::resolve_wire_actor_join_target (
  const runtime::protocol::spot_route_fence_t &fence, bool entry) const
{
    if (fence.spot_id.empty () || fence.object_generation == 0
        || fence.target_node_routing_id.empty () || fence.target_node_generation == 0
        || fence.authority_owner_generation == 0 || fence.owner_lease_generation == 0) {
        return result_t<std::uint64_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote Actor Join target Spot fence is incomplete");
    }

    auto services = _state->lane.run ([&] { return _state->root_services; }).get ();
    if (!services) {
        return result_t<std::uint64_t>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor Join target Spot Authority Store is unavailable");
    }
    if (entry) {
        const auto entry_id = resolve_entry_spot_id ();
        const auto node = native_node ();
        const auto owner =
          services->get_required<runtime::location_runtime_t> ().current_owner_token ();
        if (!entry_id || *entry_id != fence.spot_id || !node || !owner
            || node->status ().routing_id ().to_bytes () != fence.target_node_routing_id
            || node->status ().lifecycle_generation () != fence.target_node_generation
            || fence.object_generation != fence.target_node_generation
            || fence.authority_owner_generation != fence.target_node_generation
            || owner->lease_generation <= 0
            || static_cast<std::uint64_t> (owner->lease_generation) != fence.owner_lease_generation)
            return result_t<std::uint64_t>::failure (framework_error_kind_t::unavailable,
                                                     "Entry Spot descriptor fence is unavailable");
        return result_t<std::uint64_t>::success (fence.object_generation);
    }
    runtime::live_location_reader_t *store = nullptr;
    try {
        store = &services->get_required<runtime::live_location_reader_t> ();
    }
    catch (const std::exception &) {
        return result_t<std::uint64_t>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor Join target Spot Authority Store is unavailable");
    }

    try {
        // Admission fences must read the Authority row at this boundary.
        // store_location_resolvers_t may return a valid-but-stale cache
        // projection, which is suitable for routing but cannot authorize a
        // canonical actorJoin target.
        const auto read = runtime::infrastructure_result (
          [&] { return store->read_authority (runtime::spot_authority_key (fence.spot_id)); });
        if (!read) {
            return result_t<std::uint64_t>::failure (
              framework_error_kind_t::unavailable,
              "remote Actor Join target Spot Authority row could not be read");
        }
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read.value ());
        if (snapshot == nullptr) {
            return result_t<std::uint64_t>::failure (
              framework_error_kind_t::not_found,
              "remote Actor Join target Spot Authority row is missing");
        }
        if (snapshot->allocation.state != placement_allocation_state_t::active
            || snapshot->allocation.object_kind != placement_object_kind_t::user_spot
            || snapshot->object_generation != fence.object_generation
            || snapshot->allocation.target.node_rid.value ()
                 != zlink::routing_id_t::from (fence.target_node_routing_id).to_string ()
            || snapshot->allocation.target.node_lifecycle_generation != fence.target_node_generation
            || snapshot->authority_owner_generation != fence.authority_owner_generation
            || snapshot->owner.lease_generation <= 0
            || static_cast<std::uint64_t> (snapshot->owner.lease_generation)
                 != fence.owner_lease_generation) {
            return result_t<std::uint64_t>::failure (
              framework_error_kind_t::protocol_error,
              "remote Actor Join target Spot Authority row does not exactly match its route fence");
        }
        return result_t<std::uint64_t>::success (snapshot->object_generation);
    }
    catch (const std::exception &) {
        return result_t<std::uint64_t>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor Join target Spot Authority row could not be read");
    }
    catch (...) {
        return result_t<std::uint64_t>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor Join target Spot Authority row could not be read");
    }
}

/* One local Actor Join control (spot_control JOINED with an actor_join
 * operation). It runs in the target Spot's lifecycle item, so the target's
 * own lifecycle steps run inline (Spot-Actor membership: Join on the
 * lifecycle lane; handler turn and execution gate §7). */
task_t<actor_join_reply_t> spot_node_runtime_t::run_actor_join_control (
  const actor_ref_t &actor,
  const service::actor_control_t &control,
  const std::string &join_spot_id,
  const std::vector<zlink::message_t> &parts,
  const zlink::message_t &request,
  bool remote_transfer,
  bool targets_entry_spot,
  const std::string &local_node_rid,
  service_provider_t &services,
  serializer_registry_t &serializers,
  std::function<void (std::function<void (result_t<void>)>)> *source_leave,
  std::optional<std::chrono::steady_clock::time_point> deadline)
{
    spot_node_runtime_t owner (_state);
    if (remote_transfer) {
        const auto target_spot = spot_id_t (join_spot_id);
        const auto transfer_id = parts[2].to_string ();
        auto admitted = co_await runtime::await_result (owner.admit_remote_actor_to_spot (
          transfer_id, actor, spot_id_t (parts[3].to_string ()), target_spot, request));
        if (!admitted) {
            co_return detail::propagate_failure<actor_join_reply_t> (
              admitted, "remote actor admission failed");
        }
        const auto admission_reply =
          framework_reply_or_empty (admitted.value ().reply, serializers);
        if (!admitted.value ().accepted) {
            co_return result_t<actor_join_reply_t>::success (
              actor_join_reply_t{1, actor, admission_reply});
        }
        auto native = co_await owner.native_node_async ();
        if (!native) {
            co_return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::internal_failure, "target Core MeshNode is unavailable");
        }
        service::actor_transfer_prepare_t transfer_prepare{
          .role = service::actor_transfer_role_t::target,
          .transfer_id = transfer_id,
          .actor = control.current_actor,
          .source_spot_id = parts[3].to_string (),
          .target_spot_id = join_spot_id,
          .target_node_rid = native->status ().routing_id ()};
        service::actor_transfer_token_t transfer_token;
        service::actor_transfer_prepare_result_t transfer_result{actor, 0};
        if (!co_await native->prepare_actor_transfer_async (transfer_prepare, transfer_token,
                                                            transfer_result)) {
            co_return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::internal_failure,
              "target Framework Actor relocation prepare failed");
        }
        _state->lane
          .run ([&] {
              // Framework target prepare installed the transferred Actor with the
              // source generation. Application materialization must reuse it.
              _state->mesh_runtime_owned_native_actor_ids.insert (
                std::string (actor.actor_id ().value ()));
          })
          .get ();
        auto committed = co_await runtime::await_result (owner.commit_remote_actor_to_spot (
          transfer_id, actor, target_spot, parts[1],
          services.get_required<actor_gateway_runtime_t> ().actor_context (actor), {}, &services));
        if (!committed) {
            _state->lane
              .run ([&] {
                  _state->mesh_runtime_owned_native_actor_ids.erase (
                    std::string (actor.actor_id ().value ()));
              })
              .get ();
            co_return committed;
        }
        const auto new_membership_epoch = transfer_result.membership_epoch + 1;
        const auto core_committed = co_await transfer_token.commit (new_membership_epoch);
        bool activated = false;
        if (core_committed)
            activated = co_await transfer_token.activate ();
        if (!activated) {
            co_return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::internal_failure,
              "target Framework Actor relocation activation failed");
        }
        if (actor_transfer_marker_enabled ()) {
            emit_actor_transfer_marker ("location_committed", committed.value ().actor, transfer_id,
                                        target_spot);
        }
        co_return result_t<actor_join_reply_t>::success (actor_join_reply_t{
          committed.value ().result_code, committed.value ().actor, admission_reply});
    }
    if (targets_entry_spot) {
        auto &actor_gateway = services.get_required<actor_gateway_runtime_t> ();
        auto actor_context = actor_gateway.actor_context (actor);
        if (actor_context.serializer_registry () == nullptr)
            co_return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::protocol_error,
              "Actor gateway Context has no serializer registry");
        co_return co_await owner.join_actor_to_entry_spot_erased (
          actor, node_rid_t::from_string (local_node_rid), request, std::nullopt,
          std::move (actor_context), source_leave, deadline);
    }
    co_return co_await owner.join_actor_to_spot_erased (
      actor, spot_id_t (join_spot_id), request, std::nullopt, {}, 0, 0, source_leave, deadline);
}

} // namespace detail

} // namespace zlink::framework
