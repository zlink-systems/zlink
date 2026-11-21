/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/spots/spot_runtime.hpp"

#include "runtime/configuration/service_scope.hpp"
#include "runtime/locations/location_lifecycle.hpp"

namespace zlink::framework::detail
{

task_t<void> invoke_spot_closing (
  std::function<task_t<void> (void *, const spot_closing_context_t &, std::stop_token)> callback,
  void *instance,
  spot_closing_context_t context,
  std::stop_token cleanup_cancellation)
{
    if (cleanup_cancellation.stop_requested ())
        std::rethrow_exception (make_cancellation_exception (
          "The SPOT closing cleanup was cancelled before the lifecycle callback started."));
    auto closing_context = std::make_shared<const spot_closing_context_t> (std::move (context));
    auto closing = callback (instance, *closing_context, cleanup_cancellation);
    observe_task_terminal (closing, [closing_context] (const result_t<void> &) {});
    return closing;
}

void spot_context_state_t::settle_admitted_callbacks_core ()
{
    if (admitted_callback_terminal && callback_depth == 0) {
        auto terminal = std::exchange (admitted_callback_terminal, {});
        terminal->complete (result_t<void>::success ());
    }
}

task_t<void> spot_context_state_t::admitted_callbacks_terminal ()
{
    return state_sync ([this] {
        if (!admitted_callback_terminal)
            admitted_callback_terminal = std::make_shared<task_completion_source_t<void>> ();
        const auto terminal = admitted_callback_terminal;
        settle_admitted_callbacks_core ();
        return terminal->task ();
    });
}

void spot_context_state_t::detach_application_instance (
  bool notify_closing,
  spot_close_reason_t close_reason,
  std::chrono::system_clock::time_point deadline,
  std::stop_token cleanup_cancellation)
{
    auto lifetime_guard = shared_from_this ();
    state_sync ([this, close_reason] {
        admission_sealed = true;
        if (serial_queue)
            serial_queue->invalidate_execution_ready ();
        closed = close_reason;
    });

    auto owner = state_lane_owner ();
    if (!owner) {
        // Compatibility for an already-detached test/context. Production
        // contexts retain lane_owner from activation until destruction.
        auto instance = std::move (spot_instance);
        std::exception_ptr callback_error;
        if (notify_closing && lifecycle.on_closing && instance) {
            try {
                invoke_spot_closing (lifecycle.on_closing, instance.get (),
                                     spot_closing_context_t{close_reason, deadline},
                                     cleanup_cancellation)
                  .result ()
                  .value ();
            }
            catch (...) {
                callback_error = std::current_exception ();
            }
        }
        cancel_timers ();
        timer_handler_instances.clear ();
        instance.reset ();
        if (activation_scope) {
            activation_scope->close ();
            activation_scope.reset ();
        }
        node.reset ();
        channel_runtime.reset ();
        if (callback_error)
            std::rethrow_exception (callback_error);
        return;
    }

    const auto reservation =
      owner->lane
        .run ([this, &owner] {
            if (close_reservation != 0)
                return std::make_pair (close_reservation, false);
            return std::make_pair (reserve_close_core (*owner, close_reservation_kind_t::teardown),
                                   true);
        })
        .get ();
    std::exception_ptr detach_error;
    try {
        detach_application_instance_reserved (owner, reservation.first, notify_closing,
                                              close_reason, deadline, cleanup_cancellation);
    }
    catch (...) {
        detach_error = std::current_exception ();
    }
    if (reservation.second) {
        // A Close that merged into this teardown finds no incarnation left.
        auto merged = owner->lane
                        .run ([this, token = reservation.first] {
                            clear_close_reservation_core (token);
                            return std::exchange (merged_close_results, {});
                        })
                        .get ();
        for (auto &waiter : merged)
            waiter (result_t<bool>::success (false));
    }
    if (detach_error)
        std::rethrow_exception (detach_error);
}

bool spot_context_state_t::has_active_callback () const
{
    return state_sync ([this] { return callback_depth > 0; });
}

std::shared_ptr<spot_serial_executor_t> spot_context_state_t::ensure_spot_serial_executor ()
{
    if (spot_serial_executor)
        return spot_serial_executor;
    auto owner = lane_owner.lock ();
    if (!owner)
        owner = node;
    const auto worker_executor = serial_executor ? serial_executor
                                 : owner         ? owner->worker_executor
                                                 : nullptr;
    if (!owner || !worker_executor)
        return spot_serial_executor;
    if (!owner->lane.is_on_lane ())
        return owner->lane
          .run ([this, owner] { return ensure_spot_serial_executor_on_lane (owner->lane); })
          .get ();
    return ensure_spot_serial_executor_on_lane (owner->lane);
}

std::shared_ptr<spot_serial_executor_t>
spot_context_state_t::ensure_spot_serial_executor_on_lane (runtime::state_lane_t &state_lane)
{
    assert (state_lane.is_on_lane ());
    if (spot_serial_executor)
        return spot_serial_executor;
    const auto lane_owner_state = lane_owner.lock ();
    const auto owner = lane_owner_state ? lane_owner_state : node;
    const auto worker_executor = serial_executor ? serial_executor
                                 : owner         ? owner->worker_executor
                                                 : nullptr;
    if (!worker_executor)
        return {};
    const auto policy = is_entry_spot () ? runtime::serial_lane_policy_t::entry_spot ()
                        : execution_mode == user_spot_execution_mode_t::spot_wide
                          ? runtime::serial_lane_policy_t::spot_wide ()
                          : runtime::serial_lane_policy_t::per_actor_spot ();
    spot_serial_executor = std::make_shared<spot_serial_executor_t> (
      worker_executor, state_lane, policy, serial_queue,
      owner && owner->location_lifecycle
        ? owner->location_lifecycle->execution_runtime ().execution_policy ()
        : runtime::serial_execution_queue_t::execution_admission_t{});
    serial_queue = spot_serial_executor->spot_queue ();
    return spot_serial_executor;
}

bool spot_context_state_t::merge_into_running_close_core (const service::spot_close_done_t &done)
{
    if (close_reservation == 0 || close_reservation_kind == close_reservation_kind_t::idle)
        return false;
    merged_close_results.push_back (done);
    return true;
}

std::uint64_t spot_context_state_t::begin_idle_close_reservation ()
{
    auto owner = state_lane_owner ();
    if (!owner)
        return 0;
    return state_sync ([this, &owner] {
        if (node.get () != owner.get () || closed || actor_count != 0
            || !lifecycle_domain.allows_idle_eviction ()) {
            return std::uint64_t{0};
        }
        if (close_reservation != 0)
            return close_reservation_kind == close_reservation_kind_t::idle ? close_reservation
                                                                            : std::uint64_t{0};
        return reserve_close_core (*owner, close_reservation_kind_t::idle);
    });
}

void spot_context_state_t::cancel_idle_close_reservation (std::uint64_t token) noexcept
{
    try {
        if (auto owner = state_lane_owner ()) {
            state_sync ([this, token] {
                if (!closed && close_reservation == token
                    && close_reservation_kind == close_reservation_kind_t::idle) {
                    clear_close_reservation_core (token);
                    admission_sealed = false;
                }
            });
        }
    }
    catch (...) {
    }
}

bool spot_context_state_t::idle_close_ready (std::uint64_t reservation)
{
    auto owner = state_lane_owner ();
    if (!owner)
        return false;
    const auto sealed = owner->lane
                          .run ([this, &owner, reservation] {
                              return reservation != 0 && close_reservation == reservation
                                     && close_reservation_kind == close_reservation_kind_t::idle
                                     && node.get () == owner.get () && !closed && actor_count == 0
                                     && lifecycle_domain.allows_idle_eviction ()
                                     && callback_depth == 0 && admission_sealed
                                     && idle_age_allows_close_core (*owner);
                          })
                          .get ();
    return sealed && idle_quiescent ();
}

void spot_context_state_t::commit_idle_close (std::uint64_t reservation)
{
    auto owner = state_lane_owner ();
    if (!owner)
        return;
    const auto committed =
      owner->lane
        .run ([this, reservation] {
            if (close_reservation != reservation
                || close_reservation_kind != close_reservation_kind_t::idle || closed)
                return false;
            closed = spot_close_reason_t::idle_evicted;
            return true;
        })
        .get ();
    if (committed)
        close_application_then_release_location (owner, spot_close_reason_t::idle_evicted,
                                                 reservation);
}

std::uint64_t spot_context_state_t::reserve_close_core (spot_node_builder_state_t &owner,
                                                        close_reservation_kind_t kind)
{
    auto token = next_close_reservation++;
    if (token == 0) {
        token = next_close_reservation++;
    }
    if (next_close_reservation == 0)
        next_close_reservation = 1;
    if (serial_queue)
        serial_queue->invalidate_execution_ready ();
    close_reservation = token;
    close_reservation_kind = kind;
    const auto found = owner.spot_contexts_by_id.find (std::string (spot_id));
    close_registered_context =
      found == owner.spot_contexts_by_id.end () ? nullptr : std::addressof (found->second);
    return token;
}

void spot_context_state_t::clear_close_reservation_core (std::uint64_t token) noexcept
{
    if (token == 0 || close_reservation != token)
        return;
    close_reservation = 0;
    close_reservation_kind = close_reservation_kind_t::none;
    close_registered_context = nullptr;
}

bool spot_context_state_t::idle_age_allows_close_core (const spot_node_builder_state_t &owner) const
{
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds> (
                          std::chrono::steady_clock::now ().time_since_epoch ())
                          .count ();
    const auto timeout_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds> (owner.instance_spot_idle_timeout)
        .count ();
    const auto last_ns = last_application_work_completed_ns.load (std::memory_order_relaxed);
    return last_ns > 0 && now_ns >= last_ns && now_ns - last_ns >= timeout_ns;
}

void spot_context_state_t::detach_application_instance_reserved (
  const std::shared_ptr<spot_node_builder_state_t> &owner,
  std::uint64_t token,
  bool notify_closing,
  spot_close_reason_t close_reason,
  std::chrono::system_clock::time_point deadline,
  std::stop_token cleanup_cancellation)
{
    auto work =
      owner->lane
        .run ([this, &owner, token] {
            if (close_reservation != token || node.get () != owner.get ())
                return application_detach_work_t{};
            return application_detach_work_t{std::move (spot_instance), lifecycle.on_closing};
        })
        .get ();

    std::exception_ptr callback_error;
    if (notify_closing && work.on_closing && work.instance) {
        try {
            invoke_spot_closing (work.on_closing, work.instance.get (),
                                 spot_closing_context_t{close_reason, deadline},
                                 cleanup_cancellation)
              .result ()
              .value ();
        }
        catch (...) {
            callback_error = std::current_exception ();
        }
    }

    // The reservation transfers the closed activation's teardown resources
    // to this coordinator. Timer/native callbacks remain outside state turns.
    cancel_timers ();
    auto scope = owner->lane
                   .run ([this, token] {
                       if (close_reservation != token)
                           return std::shared_ptr<service_scope_t>{};
                       timer_handler_instances.clear ();
                       return activation_scope;
                   })
                   .get ();
    work.instance.reset ();
    if (scope)
        scope->close ();
    owner->lane
      .run ([this, &owner, token, &scope] {
          if (close_reservation != token)
              return;
          if (activation_scope == scope)
              activation_scope.reset ();
          if (node.get () == owner.get ())
              node.reset ();
          channel_runtime.reset ();
      })
      .get ();

    if (callback_error)
        std::rethrow_exception (callback_error);
}

void spot_context_state_t::close_application_then_release_location (
  const std::shared_ptr<spot_node_builder_state_t> &owner,
  spot_close_reason_t reason,
  std::uint64_t token,
  std::chrono::system_clock::time_point deadline,
  std::stop_token cleanup_cancellation)
{
    std::exception_ptr closing_error;
    try {
        // The location remains published until OnClosing has completed, so
        // another node cannot create a competing incarnation while the
        // application is still preserving its final state.
        detach_application_instance_reserved (owner, token, true, reason, deadline,
                                              cleanup_cancellation);
    }
    catch (...) {
        closing_error = std::current_exception ();
    }
    const auto rid = std::string (spot_id);
    // An OnClosing failure is a diagnostic; cleanup continues and the Close
    // result does not change (Spot address messaging §7 step 3).
    if (closing_error)
        report_spot_close_diagnostic (owner, rid, "on_closing_failed", closing_error);

    auto *location_lifecycle =
      owner->lane
        .run ([this, &owner, token] {
            return close_reservation == token ? owner->location_lifecycle : nullptr;
        })
        .get ();
    try {
        if (location_lifecycle) {
            (void) location_lifecycle->release_spot (spot_location_key_t{rid});
        }
    }
    catch (...) {
        if (reason != spot_close_reason_t::explicit_close)
            owner->lane.run ([this, token] { clear_close_reservation_core (token); }).get ();
        throw;
    }

    owner->lane
      .run ([this, &owner, &rid, token, reason] {
          if (close_reservation != token)
              return;
          const auto context = owner->spot_contexts_by_id.find (rid);
          const auto exact_context =
            context != owner->spot_contexts_by_id.end ()
            && std::addressof (context->second) == close_registered_context;
          if (exact_context) {
              if (reason != spot_close_reason_t::explicit_close)
                  owner->spot_contexts_by_id.erase (context);
              owner->spot_names_by_id.erase (rid);
              owner->native_spots_by_id.erase (rid);
              for (auto iterator = owner->spot_ids_by_name.begin ();
                   iterator != owner->spot_ids_by_name.end (); ++iterator) {
                  if (iterator->second == rid) {
                      owner->spot_ids_by_name.erase (iterator);
                      break;
                  }
              }
          }
          // Explicit Close keeps its reservation through the authority release.
          // Its settlement clears the reservation and takes merged completions.
          if (reason != spot_close_reason_t::explicit_close)
              clear_close_reservation_core (token);
      })
      .get ();
}

} // namespace zlink::framework::detail
