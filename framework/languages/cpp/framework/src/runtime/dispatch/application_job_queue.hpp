/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/monitoring/framework_runtime.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{

struct application_job_queue_configuration_t
{
    application_job_queue_profile_t configured_profile = application_job_queue_profile_t::balanced;
    std::optional<std::uint32_t> configured_manual_max;
    std::uint32_t effective_processor_count = 1;
    std::uint32_t effective_max_queued_application_jobs = 1;
    std::uint32_t pause_threshold_percent =
      detail::application_job_queue_default_pause_threshold_percent;
    std::uint32_t resume_threshold_percent =
      detail::application_job_queue_default_resume_threshold_percent;
};

enum class receive_flow_state_apply_result_t
{
    applied,
    invalid_state,
    closing_invalid_state,
    failed
};

struct application_job_queue_pressure_metrics_t
{
    std::uint64_t running_transition_count = 0;
    std::uint64_t paused_transition_count = 0;
    std::chrono::nanoseconds cumulative_pause_duration{};
    std::uint64_t flow_state_config_failure_count = 0;
};

struct application_job_queue_observation_t
{
    application_job_queue_status_t status;
    application_job_queue_pressure_metrics_t pressure;
};

/* Host-instance aggregate for the shared Application Job Queue.  A permit
 * counts only while receive supply is reserved or a handler job is queued.
 * The permit is returned immediately before the first application handler
 * instruction; the handler's asynchronous lifetime is deliberately outside
 * this capacity boundary. */
class application_job_queue_t
{
  private:
    enum class permit_phase_t
    {
        reserved,
        queued,
        released
    };

    struct state_t;

    struct receive_flow_socket_entry_t
    {
        explicit receive_flow_socket_entry_t (
          std::function<receive_flow_state_apply_result_t (application_job_queue_pressure_state_t)>
            configured_setter) :
            setter (std::move (configured_setter))
        {
        }

        receive_flow_state_apply_result_t apply (application_job_queue_pressure_state_t state,
                                                 std::uint64_t sequence) noexcept
        {
            std::lock_guard lock (mutex);
            if (closing || close_requested.load (std::memory_order_acquire))
                return receive_flow_state_apply_result_t::closing_invalid_state;
            if (has_applied) {
                if (sequence <= last_applied_sequence)
                    return receive_flow_state_apply_result_t::applied;
                if (last_applied_state == state) {
                    last_applied_sequence = sequence;
                    return receive_flow_state_apply_result_t::applied;
                }
            }
            receive_flow_state_apply_result_t result;
            try {
                result = setter (state);
            }
            catch (...) {
                result = receive_flow_state_apply_result_t::failed;
            }
            if (result == receive_flow_state_apply_result_t::invalid_state) {
                if (close_requested.load (std::memory_order_acquire))
                    result = receive_flow_state_apply_result_t::closing_invalid_state;
            }
            if (result == receive_flow_state_apply_result_t::applied) {
                has_applied = true;
                last_applied_sequence = sequence;
                last_applied_state = state;
            }
            return result;
        }

        void close () noexcept
        {
            request_close ();
            std::lock_guard lock (mutex);
            closing = true;
            setter = {};
        }

        void request_close () noexcept { close_requested.store (true, std::memory_order_release); }

        std::mutex mutex;
        std::function<receive_flow_state_apply_result_t (application_job_queue_pressure_state_t)>
          setter;
        std::uint64_t last_applied_sequence = 0;
        application_job_queue_pressure_state_t last_applied_state =
          application_job_queue_pressure_state_t::running;
        bool has_applied = false;
        bool closing = false;
        std::atomic_bool close_requested{false};
    };

    struct pressure_transition_t
    {
        application_job_queue_pressure_state_t state =
          application_job_queue_pressure_state_t::running;
        std::uint64_t sequence = 0;
        std::vector<std::shared_ptr<receive_flow_socket_entry_t>> sockets;
    };

    struct waiter_state_t;

  public:
    class permit_t
    {
      public:
        permit_t () = default;
        ~permit_t () { release_without_handler (); }
        permit_t (permit_t &&other) noexcept :
            _owner (std::move (other._owner)),
            _phase (std::exchange (other._phase, permit_phase_t::released))
        {
        }
        permit_t &operator= (permit_t &&other) noexcept
        {
            if (this != &other) {
                release_without_handler ();
                _owner = std::move (other._owner);
                _phase = std::exchange (other._phase, permit_phase_t::released);
            }
            return *this;
        }
        permit_t (const permit_t &) = delete;
        permit_t &operator= (const permit_t &) = delete;
        explicit operator bool () const noexcept { return _phase != permit_phase_t::released; }
        void mark_queued ()
        {
            if (_phase != permit_phase_t::reserved)
                return;
            if (auto owner = _owner.lock ()) {
                owner->credits.fetch_add (queued_credit - 1, std::memory_order_acq_rel);
                _phase = permit_phase_t::queued;
            }
        }
        void release_for_handler_entry () noexcept { release (); }
        void release_without_handler () noexcept { release (); }

      private:
        friend class application_job_queue_t;
        explicit permit_t (std::weak_ptr<state_t> owner) :
            _owner (std::move (owner)), _phase (permit_phase_t::reserved)
        {
        }
        void release () noexcept
        {
            const auto phase = std::exchange (_phase, permit_phase_t::released);
            if (phase == permit_phase_t::released)
                return;
            if (auto owner = _owner.lock ())
                application_job_queue_t::release_credits (std::move (owner), phase, 1);
            _owner.reset ();
        }
        std::weak_ptr<state_t> _owner;
        permit_phase_t _phase = permit_phase_t::released;
    };

    class permit_batch_t
    {
      public:
        permit_batch_t () = default;
        ~permit_batch_t () { release_without_handler (); }
        permit_batch_t (permit_batch_t &&other) noexcept :
            _owner (std::move (other._owner)), _remaining (std::exchange (other._remaining, 0))
        {
        }
        permit_batch_t &operator= (permit_batch_t &&other) noexcept
        {
            if (this != &other) {
                release_without_handler ();
                _owner = std::move (other._owner);
                _remaining = std::exchange (other._remaining, 0);
            }
            return *this;
        }
        permit_batch_t (const permit_batch_t &) = delete;
        permit_batch_t &operator= (const permit_batch_t &) = delete;
        explicit operator bool () const noexcept { return _remaining != 0; }
        std::uint32_t size () const noexcept { return _remaining; }
        permit_batch_t take_batch (std::uint32_t requested) noexcept
        {
            const auto count = std::min (requested, _remaining);
            _remaining -= count;
            return permit_batch_t (_owner, count);
        }
        permit_t take () noexcept
        {
            auto part = take_batch (1);
            if (!part)
                return {};
            part._remaining = 0;
            return permit_t (std::move (part._owner));
        }
        void release_without_handler () noexcept
        {
            const auto count = std::exchange (_remaining, 0);
            if (count != 0) {
                if (auto owner = _owner.lock ())
                    application_job_queue_t::release_credits (std::move (owner),
                                                              permit_phase_t::reserved, count);
            }
            _owner.reset ();
        }

      private:
        friend class application_job_queue_t;
        permit_batch_t (std::weak_ptr<state_t> owner, std::uint32_t count) :
            _owner (std::move (owner)), _remaining (count)
        {
        }
        std::weak_ptr<state_t> _owner;
        std::uint32_t _remaining = 0;
    };

    enum class origin_t
    {
        remote,
        local
    };
    using local_backlog_sink_t = std::function<void (std::uint64_t, std::uint32_t)>;

    using supply_callback_t = std::function<void (std::optional<permit_t>)>;
    using batch_supply_callback_t = std::function<void (std::optional<permit_batch_t>)>;

    using receive_flow_state_setter_t =
      std::function<receive_flow_state_apply_result_t (application_job_queue_pressure_state_t)>;

    // The host wires this to its existing diagnostics sink. The queue keeps
    // the metric and diagnostic callback at the same failure boundary; the
    // callback is optional for standalone queue users and tests.
    using receive_flow_config_failure_sink_t = std::function<void ()>;

    class receive_flow_registration_t
    {
      public:
        receive_flow_registration_t () = default;
        ~receive_flow_registration_t () { close (); }

        receive_flow_registration_t (receive_flow_registration_t &&other) noexcept :
            _owner (std::move (other._owner)),
            _entry (std::move (other._entry)),
            _registration_id (std::exchange (other._registration_id, 0))
        {
        }

        receive_flow_registration_t &operator= (receive_flow_registration_t &&other) noexcept
        {
            if (this != &other) {
                close ();
                _owner = std::move (other._owner);
                _entry = std::move (other._entry);
                _registration_id = std::exchange (other._registration_id, 0);
            }
            return *this;
        }

        receive_flow_registration_t (const receive_flow_registration_t &) = delete;
        receive_flow_registration_t &operator= (const receive_flow_registration_t &) = delete;

        explicit operator bool () const noexcept { return _registration_id != 0; }

        void close () noexcept
        {
            auto owner = _owner.lock ();
            auto entry = std::exchange (_entry, {});
            const auto registration_id = std::exchange (_registration_id, 0);
            if (!owner || !entry || registration_id == 0)
                return;
            application_job_queue_t::deregister_receive_flow_socket (
              std::move (owner), registration_id, std::move (entry));
        }

      private:
        friend class application_job_queue_t;

        receive_flow_registration_t (std::weak_ptr<state_t> owner,
                                     std::shared_ptr<receive_flow_socket_entry_t> entry,
                                     std::uint64_t registration_id) :
            _owner (std::move (owner)),
            _entry (std::move (entry)),
            _registration_id (registration_id)
        {
        }

        std::weak_ptr<state_t> _owner;
        std::shared_ptr<receive_flow_socket_entry_t> _entry;
        std::uint64_t _registration_id = 0;
    };

    class waiter_t
    {
      public:
        waiter_t () = default;
        ~waiter_t () { (void) cancel (); }

        waiter_t (waiter_t &&other) noexcept :
            _owner (std::move (other._owner)), _waiter (std::move (other._waiter))
        {
        }

        waiter_t &operator= (waiter_t &&other) noexcept
        {
            if (this != &other) {
                (void) cancel ();
                _owner = std::move (other._owner);
                _waiter = std::move (other._waiter);
            }
            return *this;
        }

        waiter_t (const waiter_t &) = delete;
        waiter_t &operator= (const waiter_t &) = delete;

        bool cancel () noexcept
        {
            auto owner = _owner.lock ();
            auto waiter = std::exchange (_waiter, {});
            if (!owner || !waiter)
                return false;
            return application_job_queue_t::cancel_waiter (std::move (owner), std::move (waiter));
        }

      private:
        friend class application_job_queue_t;

        waiter_t (std::weak_ptr<state_t> owner, std::shared_ptr<waiter_state_t> waiter) :
            _owner (std::move (owner)), _waiter (std::move (waiter))
        {
        }

        std::weak_ptr<state_t> _owner;
        std::shared_ptr<waiter_state_t> _waiter;
    };

    /* One receiving owner's pending supply (Application job queue §3: permit
     * before receive). The queue's waiter callback fills it, so the owner
     * keeps its own management work going while it waits; `notify`, when
     * given, is how that callback wakes the owner. */
    class supply_request_t
    {
      public:
        /* The supply once it is ready within `wait`: a permit, or an empty
         * permit after the queue stopped. Empty while the supply is pending. */
        std::optional<std::optional<permit_t>> take (application_job_queue_t &queue,
                                                     std::chrono::milliseconds wait,
                                                     std::function<void ()> notify,
                                                     origin_t origin)
        {
            if (!_supply.valid ()) {
                auto filled = std::make_shared<std::promise<std::optional<permit_t>>> ();
                _supply = filled->get_future ();
                _waiter = queue.wait_for_supply (
                  [filled, notify = std::move (notify)] (std::optional<permit_t> permit) {
                      filled->set_value (std::move (permit));
                      if (notify)
                          notify ();
                  },
                  origin);
            }
            if (_supply.wait_for (wait) != std::future_status::ready)
                return std::nullopt;
            _waiter = {};
            return _supply.get ();
        }

      private:
        waiter_t _waiter;
        std::future<std::optional<permit_t>> _supply;
    };

    explicit application_job_queue_t (application_job_queue_configuration_t configuration,
                                      receive_flow_config_failure_sink_t failure_sink = {},
                                      local_backlog_sink_t local_backlog_sink = {}) :
        _state (std::make_shared<state_t> (
          std::move (configuration), std::move (failure_sink), std::move (local_backlog_sink)))
    {
    }

    application_job_queue_t (const application_job_queue_t &) = delete;
    application_job_queue_t &operator= (const application_job_queue_t &) = delete;

    std::optional<permit_t> try_reserve_supply ()
    {
        auto batch = try_reserve_supply_batch (1);
        if (!batch)
            return std::nullopt;
        return batch.take ();
    }

    permit_batch_t try_reserve_supply_batch (std::uint32_t requested)
    {
        if (requested == 0)
            return {};
        std::optional<pressure_transition_t> transition;
        permit_batch_t batch;
        {
            std::lock_guard lock (_state->mutex);
            if (_state->stopped || !_state->waiters.empty ())
                return {};
            const auto available = _state->configuration.effective_max_queued_application_jobs
                                   - permits_in_use (_state->credits.load ());
            const auto count = std::min (requested, available);
            if (count == 0)
                return {};
            batch = reserve_locked (_state, count);
            transition = evaluate_pressure_locked (*_state);
        }
        dispatch_pressure_transition (_state, std::move (transition));
        return batch;
    }

    waiter_t wait_for_supply (supply_callback_t callback, origin_t origin)
    {
        if (!callback)
            throw std::invalid_argument ("Application Job Queue waiter callback is required");
        return wait_for_supply_batch (
          [callback = std::move (callback)] (std::optional<permit_batch_t> budget) mutable {
              std::optional<permit_t> permit;
              if (budget)
                  permit.emplace (budget->take ());
              callback (std::move (permit));
          },
          origin, 1);
    }

    waiter_t wait_for_supply_batch (batch_supply_callback_t callback,
                                    origin_t origin,
                                    std::uint32_t requested)
    {
        if (!callback || requested == 0)
            throw std::invalid_argument ("Application Job Queue waiter callback is required");

        auto waiter = std::make_shared<waiter_state_t> (origin, requested);
        waiter->callback = std::move (callback);
        std::optional<permit_batch_t> immediate;
        std::optional<pressure_transition_t> transition;
        bool stopped = false;
        std::optional<std::uint64_t> warning;
        {
            std::lock_guard lock (_state->mutex);
            if (_state->stopped) {
                waiter->terminal = true;
                stopped = true;
            } else if (_state->waiters.empty ()
                       && permits_in_use (_state->credits.load ())
                            < _state->configuration.effective_max_queued_application_jobs) {
                waiter->terminal = true;
                const auto available = _state->configuration.effective_max_queued_application_jobs
                                       - permits_in_use (_state->credits.load ());
                immediate = reserve_locked (_state, std::min (requested, available));
                transition = evaluate_pressure_locked (*_state);
            } else {
                waiter->started_at = std::chrono::steady_clock::now ();
                waiter->measurement_epoch = _state->measurement_epoch;
                _state->waiters.push_back (waiter);
                publish_waiters_locked (*_state);
                ++_state->capacity_wait_count;
                warning = update_local_waiters_locked (*_state, origin, 1);
            }
        }
        dispatch_pressure_transition (_state, std::move (transition));
        if (warning && _state->local_backlog_sink)
            _state->local_backlog_sink (
              *warning, _state->configuration.effective_max_queued_application_jobs);
        if (immediate)
            waiter->callback (std::move (immediate));
        else if (stopped)
            waiter->callback (std::nullopt);
        if (!immediate && !stopped)
            settle_released_capacity (_state);
        return waiter_t (_state, std::move (waiter));
    }

    // For a caller whose own call is the wait (a synchronous public submit).
    std::optional<permit_t> wait_for_supply_blocking (origin_t origin)
    {
        supply_request_t request;
        for (;;) {
            if (auto supply = request.take (*this, std::chrono::hours (1), {}, origin))
                return std::move (*supply);
        }
    }

    application_job_queue_status_t snapshot () const noexcept
    {
        return observation_snapshot ().status;
    }

    application_job_queue_pressure_metrics_t pressure_metrics_snapshot () const noexcept
    {
        return observation_snapshot ().pressure;
    }

    application_job_queue_observation_t observation_snapshot () const noexcept
    {
        std::lock_guard lock (_state->mutex);
        const auto now = std::chrono::steady_clock::now ();
        const auto credits = _state->credits.load ();
        return {
          {_state->configuration.configured_profile, _state->configuration.configured_manual_max,
           _state->configuration.effective_processor_count,
           _state->configuration.effective_max_queued_application_jobs, reserved_count (credits),
           queued_count (credits), permits_in_use (credits), _state->peak_permits_in_use,
           static_cast<std::uint32_t> (_state->waiters.size ()), _state->capacity_wait_count,
           std::chrono::nanoseconds (
             static_cast<std::chrono::nanoseconds::rep> (std::min<std::uint64_t> (
               _state->capacity_wait_duration_ns,
               static_cast<std::uint64_t> (
                 std::numeric_limits<std::chrono::nanoseconds::rep>::max ())))),
           _state->configuration.pause_threshold_percent,
           _state->configuration.resume_threshold_percent, _state->pause_threshold_permit_count,
           _state->resume_threshold_permit_count, pressure_state (_state->credits.load ()),
           saturated_duration (current_pause_duration_ns_locked (*_state, now))},
          {_state->running_transition_count, _state->paused_transition_count,
           saturated_duration (cumulative_pause_duration_ns_locked (*_state, now)),
           _state->flow_state_config_failure_count}};
    }

    receive_flow_registration_t register_receive_flow_socket (receive_flow_state_setter_t setter)
    {
        if (!setter)
            throw std::invalid_argument ("Receive-flow socket state setter is required");
        auto entry = std::make_shared<receive_flow_socket_entry_t> (std::move (setter));
        for (;;) {
            application_job_queue_pressure_state_t desired_state;
            std::uint64_t desired_sequence = 0;
            {
                std::lock_guard lock (_state->mutex);
                if (_state->stopped)
                    throw std::logic_error (
                      "Cannot register a receive-flow socket after queue stop");
                desired_state = pressure_state (_state->credits.load ());
                desired_sequence = _state->pressure_sequence;
            }

            const auto result = entry->apply (desired_state, desired_sequence);
            if (result != receive_flow_state_apply_result_t::applied) {
                bool stopped = false;
                {
                    std::lock_guard lock (_state->mutex);
                    stopped = _state->stopped;
                }
                if (result == receive_flow_state_apply_result_t::failed
                    || (result == receive_flow_state_apply_result_t::invalid_state && !stopped)) {
                    record_flow_state_config_failure (_state);
                }
                if (stopped) {
                    entry->close ();
                    throw std::logic_error (
                      "Cannot register a receive-flow socket after queue stop");
                }
                throw std::runtime_error (
                  "Failed to apply the current Application Job Queue pressure state");
            }

            bool stopped = false;
            {
                std::lock_guard lock (_state->mutex);
                stopped = _state->stopped;
                if (!stopped) {
                    if (_state->pressure_sequence != desired_sequence
                        || pressure_state (_state->credits.load ()) != desired_state) {
                        continue;
                    }
                    const auto registration_id = _state->next_receive_flow_registration_id++;
                    _state->receive_flow_sockets.emplace (registration_id, entry);
                    return receive_flow_registration_t (_state, std::move (entry), registration_id);
                }
            }
            entry->close ();
            throw std::logic_error ("Cannot register a receive-flow socket after queue stop");
        }
    }

    void reset_metrics () noexcept
    {
        std::lock_guard lock (_state->mutex);
        ++_state->measurement_epoch;
        _state->peak_permits_in_use = permits_in_use (_state->credits.load ());
        _state->capacity_wait_count = 0;
        _state->capacity_wait_duration_ns = 0;
        _state->running_transition_count = 0;
        _state->paused_transition_count = 0;
        _state->cumulative_pause_duration_ns = 0;
        _state->flow_state_config_failure_count = 0;
        if (pressure_state (_state->credits.load ())
            == application_job_queue_pressure_state_t::paused) {
            _state->pause_accounted_at = std::chrono::steady_clock::now ();
        }
    }

    void stop () noexcept
    {
        std::vector<batch_supply_callback_t> callbacks;
        {
            std::lock_guard lock (_state->mutex);
            if (_state->stopped)
                return;
            _state->stopped = true;
            for (auto &waiter : _state->waiters) {
                if (!waiter || waiter->terminal)
                    continue;
                waiter->terminal = true;
                update_local_waiters_locked (*_state, waiter->origin, -1);
                callbacks.push_back (std::move (waiter->callback));
            }
            _state->waiters.clear ();
            publish_waiters_locked (*_state);
            for (const auto &[_, socket] : _state->receive_flow_sockets) {
                if (socket)
                    socket->request_close ();
            }
            _state->receive_flow_sockets.clear ();
        }
        for (auto &callback : callbacks) {
            try {
                callback (std::nullopt);
            }
            catch (...) {
            }
        }
    }

  private:
    struct waiter_state_t
    {
        explicit waiter_state_t (origin_t value, std::uint32_t count) :
            origin (value), requested (count)
        {
        }
        const origin_t origin;
        const std::uint32_t requested;
        std::uint32_t granted = 0;
        batch_supply_callback_t callback;
        std::chrono::steady_clock::time_point started_at{};
        std::uint64_t measurement_epoch = 0;
        std::atomic_bool terminal{false};
    };

    struct state_t
    {
        explicit state_t (application_job_queue_configuration_t configured,
                          receive_flow_config_failure_sink_t configured_failure_sink,
                          local_backlog_sink_t configured_local_backlog_sink) :
            configuration (std::move (configured)),
            flow_state_config_failure_sink (std::move (configured_failure_sink)),
            local_backlog_sink (std::move (configured_local_backlog_sink))
        {
            if (configuration.effective_processor_count == 0
                || configuration.effective_max_queued_application_jobs == 0
                || configuration.effective_max_queued_application_jobs
                     > static_cast<std::uint32_t> (std::numeric_limits<std::int32_t>::max ())) {
                throw std::invalid_argument ("Application Job Queue effective capacity is invalid");
            }
            if (configuration.pause_threshold_percent < 1
                || configuration.pause_threshold_percent > 100
                || configuration.resume_threshold_percent > 99
                || configuration.resume_threshold_percent
                     >= configuration.pause_threshold_percent) {
                throw std::invalid_argument (
                  "Application Job Queue pressure thresholds are invalid");
            }
            const auto maximum =
              static_cast<std::uint64_t> (configuration.effective_max_queued_application_jobs);
            pause_threshold_permit_count = static_cast<std::uint32_t> (
              (maximum * configuration.pause_threshold_percent + 99u) / 100u);
            resume_threshold_permit_count = static_cast<std::uint32_t> (
              (maximum * configuration.resume_threshold_percent) / 100u);
        }

        mutable std::mutex mutex;
        application_job_queue_configuration_t configuration;
        std::list<std::shared_ptr<waiter_state_t>> waiters;
        std::atomic<std::uint64_t> credits{0};
        std::uint32_t peak_permits_in_use = 0;
        std::int64_t local_waiters = 0;
        bool local_backlog_logged = false;
        std::uint64_t capacity_wait_count = 0;
        std::uint64_t capacity_wait_duration_ns = 0;
        std::uint64_t measurement_epoch = 0;
        std::uint32_t pause_threshold_permit_count = 1;
        std::uint32_t resume_threshold_permit_count = 0;
        std::chrono::steady_clock::time_point pause_started_at{};
        std::chrono::steady_clock::time_point pause_accounted_at{};
        std::uint64_t pressure_sequence = 0;
        std::uint64_t running_transition_count = 0;
        std::uint64_t paused_transition_count = 0;
        std::uint64_t cumulative_pause_duration_ns = 0;
        std::uint64_t flow_state_config_failure_count = 0;
        receive_flow_config_failure_sink_t flow_state_config_failure_sink;
        local_backlog_sink_t local_backlog_sink;
        std::map<std::uint64_t, std::shared_ptr<receive_flow_socket_entry_t>> receive_flow_sockets;
        std::uint64_t next_receive_flow_registration_id = 1;
        bool stopped = false;
    };

    static std::uint64_t duration_ns (std::chrono::steady_clock::time_point from,
                                      std::chrono::steady_clock::time_point to) noexcept
    {
        if (from == std::chrono::steady_clock::time_point{} || to <= from)
            return 0;
        const auto elapsed =
          std::chrono::duration_cast<std::chrono::nanoseconds> (to - from).count ();
        return elapsed <= 0 ? 0 : static_cast<std::uint64_t> (elapsed);
    }

    static std::uint64_t saturated_add (std::uint64_t left, std::uint64_t right) noexcept
    {
        return right > std::numeric_limits<std::uint64_t>::max () - left
                 ? std::numeric_limits<std::uint64_t>::max ()
                 : left + right;
    }

    static std::chrono::nanoseconds saturated_duration (std::uint64_t nanoseconds) noexcept
    {
        return std::chrono::nanoseconds (
          static_cast<std::chrono::nanoseconds::rep> (std::min<std::uint64_t> (
            nanoseconds, static_cast<std::uint64_t> (
                           std::numeric_limits<std::chrono::nanoseconds::rep>::max ()))));
    }

    static std::uint64_t
    current_pause_duration_ns_locked (const state_t &owner,
                                      std::chrono::steady_clock::time_point now) noexcept
    {
        return pressure_state (owner.credits.load ())
                   == application_job_queue_pressure_state_t::paused
                 ? duration_ns (owner.pause_started_at, now)
                 : 0;
    }

    static std::uint64_t
    cumulative_pause_duration_ns_locked (const state_t &owner,
                                         std::chrono::steady_clock::time_point now) noexcept
    {
        const auto active =
          pressure_state (owner.credits.load ()) == application_job_queue_pressure_state_t::paused
            ? duration_ns (owner.pause_accounted_at, now)
            : 0;
        return saturated_add (owner.cumulative_pause_duration_ns, active);
    }

    static constexpr std::uint64_t queued_credit = std::uint64_t{1} << 31;
    static constexpr std::uint64_t credit_mask = queued_credit - 1;
    static constexpr std::uint64_t paused_credit = std::uint64_t{1} << 62;
    static constexpr std::uint64_t waiting_credit = std::uint64_t{1} << 63;
    static application_job_queue_pressure_state_t pressure_state (std::uint64_t credits) noexcept
    {
        return (credits & paused_credit) != 0 ? application_job_queue_pressure_state_t::paused
                                              : application_job_queue_pressure_state_t::running;
    }
    static void publish_waiters_locked (state_t &owner) noexcept
    {
        if (!owner.waiters.empty ())
            owner.credits.fetch_or (waiting_credit, std::memory_order_acq_rel);
        else
            owner.credits.fetch_and (~waiting_credit, std::memory_order_acq_rel);
    }
    static std::uint32_t reserved_count (std::uint64_t credits) noexcept
    {
        return static_cast<std::uint32_t> (credits & credit_mask);
    }
    static std::uint32_t queued_count (std::uint64_t credits) noexcept
    {
        return static_cast<std::uint32_t> ((credits >> 31) & credit_mask);
    }
    static std::uint32_t permits_in_use (std::uint64_t credits) noexcept
    {
        return reserved_count (credits) + queued_count (credits);
    }

    static std::optional<pressure_transition_t> evaluate_pressure_locked (state_t &owner)
    {
        const auto now = std::chrono::steady_clock::now ();
        bool changed = false;
        if (pressure_state (owner.credits.load ())
              == application_job_queue_pressure_state_t::running
            && permits_in_use (owner.credits.load ()) >= owner.pause_threshold_permit_count) {
            owner.credits.fetch_or (paused_credit, std::memory_order_acq_rel);
            owner.pause_started_at = now;
            owner.pause_accounted_at = now;
            ++owner.paused_transition_count;
            changed = true;
        }
        // Releases can race the reservation owner. Once paused is published,
        // re-read credits so a release preceding that publication cannot lose
        // the resume boundary; later releases observe paused themselves.
        if (pressure_state (owner.credits.load ()) == application_job_queue_pressure_state_t::paused
            && permits_in_use (owner.credits.load ()) <= owner.resume_threshold_permit_count) {
            owner.cumulative_pause_duration_ns = saturated_add (
              owner.cumulative_pause_duration_ns, duration_ns (owner.pause_accounted_at, now));
            owner.credits.fetch_and (~paused_credit, std::memory_order_acq_rel);
            owner.pause_started_at = {};
            owner.pause_accounted_at = {};
            ++owner.running_transition_count;
            changed = true;
        }
        if (!changed)
            return std::nullopt;
        ++owner.pressure_sequence;
        pressure_transition_t transition;
        transition.state = pressure_state (owner.credits.load ());
        transition.sequence = owner.pressure_sequence;
        transition.sockets.reserve (owner.receive_flow_sockets.size ());
        for (const auto &[_, socket] : owner.receive_flow_sockets)
            transition.sockets.push_back (socket);
        return transition;
    }

    static permit_batch_t reserve_locked (const std::shared_ptr<state_t> &owner,
                                          std::uint32_t count)
    {
        const auto before = owner->credits.fetch_add (count, std::memory_order_acq_rel);
        owner->peak_permits_in_use =
          std::max (owner->peak_permits_in_use, permits_in_use (before) + count);
        return permit_batch_t (owner, count);
    }

    static std::optional<std::uint64_t>
    update_local_waiters_locked (state_t &owner, origin_t origin, int delta) noexcept
    {
        if (origin != origin_t::local)
            return std::nullopt;
        owner.local_waiters += delta;
        if (owner.local_waiters == 0)
            owner.local_backlog_logged = false;
        if (!owner.local_backlog_logged
            && static_cast<std::uint64_t> (owner.local_waiters)
                 > owner.configuration.effective_max_queued_application_jobs) {
            owner.local_backlog_logged = true;
            return static_cast<std::uint64_t> (owner.local_waiters);
        }
        return std::nullopt;
    }

    static void account_wait_locked (state_t &owner, const waiter_state_t &waiter) noexcept
    {
        update_local_waiters_locked (owner, waiter.origin, -1);
        if (waiter.measurement_epoch != owner.measurement_epoch)
            return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds> (
          std::chrono::steady_clock::now () - waiter.started_at);
        const auto amount =
          elapsed.count () <= 0 ? std::uint64_t{0} : static_cast<std::uint64_t> (elapsed.count ());
        owner.capacity_wait_duration_ns =
          amount > std::numeric_limits<std::uint64_t>::max () - owner.capacity_wait_duration_ns
            ? std::numeric_limits<std::uint64_t>::max ()
            : owner.capacity_wait_duration_ns + amount;
    }

    static bool
    take_oldest_waiter_locked (state_t &owner,
                               std::list<std::shared_ptr<waiter_state_t>> &ready) noexcept
    {
        if (owner.waiters.empty ())
            return false;
        const auto first = owner.waiters.begin ();
        (*first)->terminal = true;
        account_wait_locked (owner, **first);
        ready.splice (ready.end (), owner.waiters, first);
        publish_waiters_locked (owner);
        return true;
    }

    static void release_credits (std::shared_ptr<state_t> owner,
                                 permit_phase_t phase,
                                 std::uint32_t count) noexcept
    {
        const auto amount = phase == permit_phase_t::reserved ? static_cast<std::uint64_t> (count)
                                                              : queued_credit * count;
        std::list<std::shared_ptr<waiter_state_t>> ready;
        permit_batch_t grants;
        std::optional<pressure_transition_t> transition;
        auto before = owner->credits.load (std::memory_order_acquire);
        for (;;) {
            const auto remaining = permits_in_use (before) - count;
            if ((before & waiting_credit) != 0
                || ((before & paused_credit) != 0
                    && remaining <= owner->resume_threshold_permit_count)) {
                std::lock_guard lock (owner->mutex);
                owner->credits.fetch_sub (amount, std::memory_order_acq_rel);
                settle_released_capacity_locked (owner, ready, grants, transition);
                break;
            }
            if (owner->credits.compare_exchange_weak (
                  before, before - amount, std::memory_order_acq_rel, std::memory_order_acquire))
                return;
        }
        dispatch_pressure_transition (owner, std::move (transition));
        deliver_supply (ready, grants);
    }

    static void settle_released_capacity_locked (const std::shared_ptr<state_t> &owner,
                                                 std::list<std::shared_ptr<waiter_state_t>> &ready,
                                                 permit_batch_t &grants,
                                                 std::optional<pressure_transition_t> &transition)
    {
        const auto available = owner->configuration.effective_max_queued_application_jobs
                               - permits_in_use (owner->credits.load ());
        if (!owner->stopped && available != 0 && !owner->waiters.empty ()) {
            auto remaining = available;
            while (remaining != 0) {
                if (!take_oldest_waiter_locked (*owner, ready))
                    break;
                const auto &waiter = ready.back ();
                waiter->granted = std::min (waiter->requested, remaining);
                remaining -= waiter->granted;
            }
            if (!ready.empty ())
                grants = reserve_locked (owner, available - remaining);
        }
        transition = evaluate_pressure_locked (*owner);
    }

    static void settle_released_capacity (const std::shared_ptr<state_t> &owner) noexcept
    {
        std::list<std::shared_ptr<waiter_state_t>> ready;
        permit_batch_t grants;
        std::optional<pressure_transition_t> transition;
        {
            std::lock_guard lock (owner->mutex);
            settle_released_capacity_locked (owner, ready, grants, transition);
        }
        dispatch_pressure_transition (owner, std::move (transition));
        deliver_supply (ready, grants);
    }

    static void deliver_supply (std::list<std::shared_ptr<waiter_state_t>> &ready,
                                permit_batch_t &grants) noexcept
    {
        for (auto &waiter : ready) {
            std::optional<permit_batch_t> handoff (grants.take_batch (waiter->granted));
            try {
                waiter->callback (std::move (handoff));
            }
            catch (...) {
                if (handoff)
                    handoff->release_without_handler ();
            }
        }
    }

    static void
    dispatch_pressure_transition (const std::shared_ptr<state_t> &owner,
                                  std::optional<pressure_transition_t> transition) noexcept
    {
        if (!transition)
            return;
        for (const auto &socket : transition->sockets) {
            if (!socket)
                continue;
            const auto result = socket->apply (transition->state, transition->sequence);
            if (result == receive_flow_state_apply_result_t::failed
                || result == receive_flow_state_apply_result_t::invalid_state)
                record_flow_state_config_failure (owner);
        }
    }

    static void record_flow_state_config_failure (const std::shared_ptr<state_t> &owner) noexcept
    {
        {
            std::lock_guard lock (owner->mutex);
            if (owner->flow_state_config_failure_count
                != std::numeric_limits<std::uint64_t>::max ()) {
                ++owner->flow_state_config_failure_count;
            }
        }
        try {
            // The sink is immutable after construction, so it can be
            // invoked directly after the metric lock is released. Avoid
            // copying std::function here: this noexcept path must not turn
            // an allocation failure into terminate.
            if (owner->flow_state_config_failure_sink)
                owner->flow_state_config_failure_sink ();
        }
        catch (...) {
            // Diagnostics must never change queue state or flow behavior.
        }
    }

    static void
    deregister_receive_flow_socket (std::shared_ptr<state_t> owner,
                                    std::uint64_t registration_id,
                                    std::shared_ptr<receive_flow_socket_entry_t> entry) noexcept
    {
        {
            std::lock_guard lock (owner->mutex);
            const auto found = owner->receive_flow_sockets.find (registration_id);
            if (found != owner->receive_flow_sockets.end () && found->second == entry) {
                owner->receive_flow_sockets.erase (found);
            }
        }
        entry->close ();
    }

    static bool cancel_waiter (std::shared_ptr<state_t> owner,
                               std::shared_ptr<waiter_state_t> waiter) noexcept
    {
        batch_supply_callback_t callback;
        if (waiter->terminal.load (std::memory_order_acquire))
            return false;
        {
            std::lock_guard lock (owner->mutex);
            if (waiter->terminal)
                return false;
            const auto found = std::find (owner->waiters.begin (), owner->waiters.end (), waiter);
            if (found == owner->waiters.end ())
                return false;
            owner->waiters.erase (found);
            waiter->terminal = true;
            publish_waiters_locked (*owner);
            account_wait_locked (*owner, *waiter);
            callback = std::move (waiter->callback);
        }
        try {
            callback (std::nullopt);
        }
        catch (...) {
        }
        return true;
    }

    std::shared_ptr<state_t> _state;
};

/* Event-loop adapter for the queue's cancellable FIFO reservation.  The
 * callback owns only shared state, so a queue handoff racing service shutdown
 * cannot reach a destroyed service object. */
class application_supply_slot_t final
{
  private:
    struct state_t
    {
        std::mutex mutex;
        std::function<void ()> wake;
        std::optional<application_job_queue_t::waiter_t> waiter;
        std::optional<application_job_queue_t::permit_batch_t> permit;
        bool waiting = false;
        bool closed = false;
    };

  public:
    application_supply_slot_t (std::shared_ptr<application_job_queue_t> queue,
                               std::function<void ()> wake) :
        _queue (std::move (queue)), _state (std::make_shared<state_t> ())
    {
        if (!_queue)
            throw std::invalid_argument ("Application supply slot requires the host queue");
        _state->wake = std::move (wake);
    }

    ~application_supply_slot_t () { close (); }

    application_supply_slot_t (const application_supply_slot_t &) = delete;
    application_supply_slot_t &operator= (const application_supply_slot_t &) = delete;

    void ensure_waiter (std::uint32_t requested = 1)
    {
        std::optional<application_job_queue_t::waiter_t> previous;
        {
            std::lock_guard lock (_state->mutex);
            if (_state->closed || _state->permit || _state->waiting)
                return;
            previous = std::move (_state->waiter);
            _state->waiting = true;
        }
        previous.reset ();

        // The queue owns FIFO fairness for both immediate acquisition and
        // waiting. A supply already available to this ingress turn needs no
        // waiter allocation or wake notification back to that same turn.
        if (auto permit = _queue->try_reserve_supply_batch (requested)) {
            std::lock_guard lock (_state->mutex);
            _state->waiting = false;
            if (!_state->closed)
                _state->permit = std::move (permit);
            return;
        }

        const auto state = _state;
        auto waiter = _queue->wait_for_supply_batch (
          [state] (std::optional<application_job_queue_t::permit_batch_t> permit) {
              std::lock_guard lock (state->mutex);
              state->waiting = false;
              if (state->closed)
                  return;
              state->permit = std::move (permit);
              if (state->wake)
                  state->wake ();
          },
          application_job_queue_t::origin_t::remote, requested);
        {
            std::lock_guard lock (_state->mutex);
            if (_state->waiting && !_state->closed)
                _state->waiter.emplace (std::move (waiter));
        }
    }

    std::optional<application_job_queue_t::permit_batch_t> take_budget ()
    {
        std::optional<application_job_queue_t::waiter_t> completed;
        std::optional<application_job_queue_t::permit_batch_t> permit;
        {
            std::lock_guard lock (_state->mutex);
            permit = std::exchange (_state->permit, std::nullopt);
            if (!_state->waiting)
                completed = std::exchange (_state->waiter, std::nullopt);
        }
        completed.reset ();
        return permit;
    }

    std::optional<application_job_queue_t::permit_t> take ()
    {
        auto budget = take_budget ();
        if (!budget)
            return std::nullopt;
        return budget->take ();
    }

    bool has_supply () const
    {
        std::lock_guard lock (_state->mutex);
        return !_state->closed && _state->permit.has_value ();
    }

    void close () noexcept
    {
        std::optional<application_job_queue_t::waiter_t> waiter;
        std::optional<application_job_queue_t::permit_batch_t> permit;
        {
            std::lock_guard lock (_state->mutex);
            if (_state->closed)
                return;
            _state->closed = true;
            _state->waiting = false;
            _state->wake = {};
            waiter = std::move (_state->waiter);
            permit = std::move (_state->permit);
        }
        waiter.reset ();
        permit.reset ();
    }

  private:
    std::shared_ptr<application_job_queue_t> _queue;
    std::shared_ptr<state_t> _state;
};

} // namespace zlink::framework::runtime
