/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/execution/task_result.hpp"
#include <zlink/framework/detail/deadline_scheduler.hpp>

#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/execution/state_lane.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include <runtime/locations/location_repository.hpp>

#include <zlink/framework/contracts/locations/options.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <stop_token>
#include <thread>
#include <vector>

namespace zlink::framework::runtime
{

enum class owner_lease_claim_rejection_t
{
    conflict,
    generation_exhausted
};

class owner_lease_claim_rejected_error_t final : public std::runtime_error
{
  public:
    owner_lease_claim_rejected_error_t (owner_lease_claim_rejection_t rejection,
                                        std::chrono::steady_clock::time_point deadline_at,
                                        std::string message) :
        std::runtime_error (std::move (message)), _rejection (rejection), _deadline_at (deadline_at)
    {
    }

    owner_lease_claim_rejection_t rejection () const noexcept { return _rejection; }

    std::chrono::steady_clock::time_point deadline_at () const noexcept { return _deadline_at; }

  private:
    owner_lease_claim_rejection_t _rejection;
    std::chrono::steady_clock::time_point _deadline_at;
};

class location_runtime_t
{
    friend struct location_runtime_test_access_t;
    struct owner_lease_snapshot_t
    {
        std::optional<location_owner_token_t> token;
        std::optional<std::chrono::steady_clock::time_point> admission_deadline;
        bool healthy = false;
        std::optional<std::chrono::system_clock::time_point> renewed_at;
    };

    struct heartbeat_attempt_t;

    struct lease_renew_outcome_t
    {
        owner_lease_renew_result_t result{owner_lease_stale_t{}};
        std::optional<owner_lease_claim_rejection_t> rejection;
        std::string message;
    };

    struct heartbeat_owner_t
    {
        std::atomic_bool stop = false;
        std::mutex gate;
        std::condition_variable wake;
        std::shared_ptr<heartbeat_attempt_t> current;
    };

    struct heartbeat_attempt_t
    {
        explicit heartbeat_attempt_t (location_runtime_t *runtime,
                                      std::chrono::steady_clock::time_point started,
                                      std::chrono::steady_clock::time_point deadline,
                                      std::weak_ptr<heartbeat_owner_t> owner,
                                      std::stop_token cancellation = {}) :
            runtime (runtime),
            started_at (started),
            deadline_at (deadline),
            heartbeat (std::move (owner)),
            cancellation (cancellation)
        {
        }

        location_runtime_t *runtime;
        std::chrono::steady_clock::time_point started_at;
        std::chrono::steady_clock::time_point deadline_at;
        std::weak_ptr<heartbeat_owner_t> heartbeat;
        std::stop_token cancellation;
        std::function<void ()> expire;
        std::optional<task_t<lease_renew_outcome_t>> task;
    };

    struct descriptor_publication_t
    {
        explicit descriptor_publication_t (
          std::chrono::milliseconds timeout,
          std::function<std::chrono::steady_clock::time_point ()> clock) :
            now (std::move (clock)), deadline_at (now () + timeout)
        {
        }

        std::function<std::chrono::steady_clock::time_point ()> now;
        const std::chrono::steady_clock::time_point deadline_at;
        task_completion_source_t<location_write_result_t> observation;
        task_completion_source_t<void> drained;
        std::shared_ptr<detail::deadline_state_t> deadline =
          std::make_shared<detail::deadline_state_t> ();
        std::optional<task_t<location_write_result_t>> pending;

        // The deadline wins even when provider completion reaches us before
        // the scheduler delivers an already-due timer. No second expiry state.
        bool
        observe (std::optional<result_t<location_write_result_t>> result = std::nullopt,
                 std::optional<std::chrono::steady_clock::time_point> observed_at = std::nullopt)
        {
            const bool native_failure = result && !*result && result->error () == nullptr;
            if (!native_failure && (observed_at ? *observed_at : now ()) >= deadline_at)
                result = result_t<location_write_result_t>::failure (
                  framework_error_kind_t::deadline_exceeded,
                  "Location descriptor publication timed out");
            if (result) {
                deadline->cancelled.store (true, std::memory_order_release);
                detail::deadline_scheduler_t::instance ().wake ();
                observation.complete (*result);
            }
            return !observation.task ().await_ready ();
        }

        void finish (std::optional<result_t<location_write_result_t>> result = std::nullopt)
        {
            observe (std::move (result));
            drained.complete (result_t<void>::success ());
        }
    };

  public:
    struct observation_status_t
    {
        std::optional<std::string> last_error;
        bool owner_lease_healthy;
        std::optional<std::chrono::system_clock::time_point> owner_lease_renewed_at;
    };

    task_t<observation_status_t> observation_status_task () const
    {
        return _lane.run_task ([this] {
            const auto lease = std::atomic_load_explicit (&_owner_lease, std::memory_order_acquire);
            return observation_status_t{_last_error, lease->healthy, lease->renewed_at};
        });
    }

    explicit location_runtime_t (location_repository_t &store,
                                 location_options_t options = {},
                                 std::string owner_id = make_owner_id ()) :
        _store (&store), _options (options), _owner_id (std::move (owner_id))
    {
    }

    ~location_runtime_t () { stop (); }

    location_runtime_t (const location_runtime_t &) = delete;
    location_runtime_t &operator= (const location_runtime_t &) = delete;

    const std::string &owner_id () const noexcept { return _owner_id; }

    const location_options_t &options () const noexcept { return _options; }

    std::optional<location_owner_token_t> current_owner_token () const
    {
        const auto lease = std::atomic_load_explicit (&_owner_lease, std::memory_order_acquire);
        return owner_lease_usable (*lease, std::chrono::steady_clock::now ())
                 ? lease->token
                 : std::optional<location_owner_token_t>{};
    }

    task_t<std::optional<location_owner_token_t>> current_owner_token_task () const
    {
        return task_t<std::optional<location_owner_token_t>> (
          result_t<std::optional<location_owner_token_t>>::success (current_owner_token ()));
    }

    /* Draining marker (graceful-drain-handoff §3.1): peer rows written while
     * draining carry the typed flag; a started drain generation never flips
     * the flag back to false. */
    /* Metric surface binding (runtime-metrics §4.5): the host wires the
     * monitoring state so lease renew failures/lateness and write conflicts
     * emit catalog instruments; unset keeps the zero-cost path. */
    void bind_monitoring (
      std::shared_ptr<framework::detail::monitoring_runtime_state_t> monitoring) noexcept
    {
        _monitoring = std::move (monitoring);
    }

    void set_draining (bool value) noexcept
    {
        if (value) {
            _draining.store (true, std::memory_order_release);
        }
    }

    bool draining () const noexcept { return _draining.load (std::memory_order_acquire); }

    /* MeshNode services publish their descriptor state directly. Kept as an
     * internal drain synchronization point while legacy host code converges. */
    bool republish_peer_rows_draining () { return true; }

    bool owner_lease_healthy () const noexcept
    {
        return std::atomic_load_explicit (&_owner_lease, std::memory_order_acquire)->healthy;
    }

    bool owner_lease_usable () const noexcept
    {
        const auto lease = std::atomic_load_explicit (&_owner_lease, std::memory_order_acquire);
        return owner_lease_usable (*lease, std::chrono::steady_clock::now ());
    }

    std::optional<std::chrono::system_clock::time_point> owner_lease_renewed_at () const
    {
        return std::atomic_load_explicit (&_owner_lease, std::memory_order_acquire)->renewed_at;
    }

    std::optional<std::string> last_error () const
    {
        return _lane.run_checked ([this] { return _last_error; }).get ();
    }

    void start (zlink::routing_id_t node_rid, std::stop_token cancellation = {})
    {
        bool expected = false;
        if (!_started.compare_exchange_strong (expected, true)) {
            return;
        }
        static_cast<void> (node_rid);
        const auto operation_started_at = std::chrono::steady_clock::now ();
        try {
            if (cancellation.stop_requested ())
                std::rethrow_exception (
                  detail::make_cancellation_exception ("location runtime startup was cancelled"));
            const auto deadline_at = operation_started_at + _options.owner_lease_renew_timeout;
            renew_owner_lease_once_at (operation_started_at, deadline_at, cancellation);
            if (cancellation.stop_requested ()) {
                release_cancelled_claim (deadline_at);
                std::rethrow_exception (
                  detail::make_cancellation_exception ("location runtime startup was cancelled"));
            }
        }
        catch (...) {
            _started.store (false, std::memory_order_release);
            throw;
        }
        auto heartbeat = std::make_shared<heartbeat_owner_t> ();
        std::atomic_store_explicit (&_heartbeat_state, heartbeat, std::memory_order_release);
        _heartbeat = std::thread ([this, heartbeat = std::move (heartbeat), operation_started_at] {
#ifndef NDEBUG
            runtime::infrastructure_wait_guard::infrastructure_scope_t scope (this);
#endif
            heartbeat_loop (std::move (heartbeat), operation_started_at);
        });
    }

    void stop () noexcept
    {
        const bool started = _started.exchange (false);
        if (started)
            stop_heartbeat ();
        infrastructure_result ([this] { return drain_descriptor_publications (); }).value ();
        if (started)
            static_cast<void> (cleanup_owner ());
    }

    /* Drain owner cleanup (graceful-drain-handoff §4-5): stops the lease
     * heartbeat, then removes this owner's lease and rows while the store
     * stays usable for the rest of teardown. Returns false when the store
     * rejects the cleanup (the drain worker maps it to OwnerCleanupFailed). */
    bool cleanup_owner (std::optional<std::chrono::steady_clock::time_point> requested_deadline_at =
                          std::nullopt) noexcept
    {
        const auto deadline_at = requested_deadline_at.value_or (
          std::chrono::steady_clock::now () + _options.owner_lease_renew_timeout);
        if (_started.exchange (false)) {
            stop_heartbeat ();
        }
        try {
            const auto token = current_owner_token_unchecked ();
            if (token) {
                auto await_cleanup = [&] (auto request) {
                    if (remaining_until (deadline_at) <= std::chrono::milliseconds::zero ())
                        throw detail::make_boundary_exception (detail::boundary_error_t::timed_out,
                                                               "owner lease cleanup timed out");
                    auto pending = request ();
                    const auto completed = detail::observe_task_result_for (
                      pending, remaining_until (deadline_at), std::stop_token{});
                    if (!completed)
                        throw detail::make_boundary_exception (detail::boundary_error_t::timed_out,
                                                               "owner lease cleanup timed out");
                    completed->value ();
                };
                await_cleanup ([&] { return _store->remove_all_by_owner (*token); });
                await_cleanup ([&] { return _store->release_owner_lease (*token); });
                _lane.run_checked ([this] { clear_owner_lease_on_lane (); }).get ();
            }
            return true;
        }
        catch (const std::exception &error) {
            record_store_error ();
            record_failure (error.what ());
            return false;
        }
    }

    owner_lease_renew_result_t renew_owner_lease_once (
      std::optional<std::chrono::steady_clock::time_point> requested_deadline_at = std::nullopt,
      std::stop_token cancellation = {})
    {
        const auto started_at = std::chrono::steady_clock::now ();
        return renew_owner_lease_once_at (
          started_at,
          requested_deadline_at.value_or (started_at + _options.owner_lease_renew_timeout),
          cancellation);
    }

    /* Store access failed (read or register): one error count per failure
     * (runtime-metrics §4.5 store.errors). The polling and write surfaces all
     * report here so the counter aggregates store health in one series. */
    void record_store_error () const
    {
        runtime_metrics_t metrics (_monitoring);
        if (metrics.enabled ()) {
            metrics.counter ("zlink.location.store.errors", "{error}", 1);
        }
    }

    void record_runtime_failure (std::string message) const
    {
        record_failure (std::move (message));
    }

    // Location runtime §5: provider failures leave publication unknown. Only a
    // confirmed conflicting claim is a startup error. The repository owns §10 reads.
    template <typename Start>
    task_t<bool>
    descriptor_write_confirmed (Start start,
                                std::function<std::chrono::steady_clock::time_point ()> now =
                                  std::chrono::steady_clock::now) const
    {
        auto operation = std::make_shared<descriptor_publication_t> (
          _options.owner_lease_renew_timeout, std::move (now));
        try {
            co_await _lane.run_task ([this, operation] {
                std::erase_if (_descriptor_publications, [] (const auto &pending) {
                    return pending->drained.task ().await_ready ();
                });
                _descriptor_publications.push_back (operation);
            });
        }
        catch (...) {
            operation->finish (detail::current_exception_result<location_write_result_t> ());
        }
        detail::deadline_scheduler_t::instance ().schedule (
          operation->deadline_at, operation->deadline, [weak = std::weak_ptr{operation}] {
              if (auto active = weak.lock ())
                  active->observe (std::nullopt, active->deadline_at);
          });
        if (operation->observe ()) {
            try {
                operation->pending.emplace (start ());
            }
            catch (...) {
                operation->pending.emplace (
                  detail::current_exception_result<location_write_result_t> ());
            }
            detail::observe_task_terminal (
              *operation->pending, [operation] (const result_t<location_write_result_t> &result) {
                  operation->finish (result);
              });
        } else {
            operation->finish ();
        }
        const auto result = co_await await_result (operation->observation.task ());
        if (!result) {
            if (result.error () == nullptr)
                co_return detail::propagate_failure<bool> (
                  result, "Location descriptor publication failed");
            record_store_error ();
            co_await _lane.run_task (
              [this, message = std::string (result.error ()->what ())] () mutable {
                  record_failure_on_lane (std::move (message));
              });
            co_return false;
        }
        if (result.value ().status == location_write_status_t::rejected_conflict)
            throw framework_exception_t (framework_error_kind_t::not_configured,
                                         "Location descriptor is held by another claim");
        co_return result.value ().status == location_write_status_t::stored;
    }

    // This is the existing lease admission deadline, used as the renewal observation.
    // Observers do not create a retry clock or a second lease state.
    auto owner_lease_observation () const
    {
        return std::atomic_load_explicit (&_owner_lease, std::memory_order_acquire)
          ->admission_deadline;
    }

    void wake_owner_lease_observers () const
    {
        if (auto heartbeat =
              std::atomic_load_explicit (&_heartbeat_state, std::memory_order_acquire)) {
            std::lock_guard lock (heartbeat->gate);
            heartbeat->wake.notify_all ();
        }
    }

    void wait_for_owner_lease_change (std::optional<std::chrono::steady_clock::time_point> observed,
                                      const task_t<void> &stopped,
                                      std::chrono::milliseconds polling_interval) const
    {
        constexpr auto *wait_site = "location/discovery-input";
        if (auto heartbeat =
              std::atomic_load_explicit (&_heartbeat_state, std::memory_order_acquire);
            heartbeat && !heartbeat->stop.load (std::memory_order_acquire)) {
            std::unique_lock lock (heartbeat->gate);
            runtime::infrastructure_wait_guard::condition_wait_for (
              heartbeat->wake, lock, polling_interval,
              [&] {
                  return stopped.await_ready () || heartbeat->stop.load (std::memory_order_acquire)
                         || owner_lease_observation () != observed;
              },
              wait_site, runtime::infrastructure_wait_guard::wait_relation_t::own_input);
        } else {
            (void) detail::observe_task_result_for (stopped, polling_interval, std::stop_token{});
        }
    }

    /* Discovered peer total observed on the auto-connect polling tick
     * (runtime-metrics §7.2: the polling diff doubles as the gauge source, so
     * observable freshness follows the tick cadence). */
    void observe_discovered_peers (std::size_t count) const
    {
        runtime_metrics_t metrics (_monitoring);
        if (metrics.enabled ()) {
            metrics.observable ("zlink.location.peers", "{peer}", static_cast<double> (count));
        }
    }

  private:
    static bool owner_lease_usable (const owner_lease_snapshot_t &lease,
                                    std::chrono::steady_clock::time_point now) noexcept
    {
        return lease.token.has_value () && lease.admission_deadline.has_value ()
               && now < *lease.admission_deadline;
    }

    template <typename Work> void update_owner_lease_on_lane (Work work) const
    {
        auto lease = *std::atomic_load_explicit (&_owner_lease, std::memory_order_acquire);
        work (lease);
        std::atomic_store_explicit (
          &_owner_lease, std::make_shared<const owner_lease_snapshot_t> (std::move (lease)),
          std::memory_order_release);
    }

    void clear_owner_lease_on_lane () const
    {
        update_owner_lease_on_lane ([] (auto &lease) {
            lease.token.reset ();
            lease.admission_deadline.reset ();
        });
    }

    std::optional<location_owner_token_t> current_owner_token_unchecked () const
    {
        return std::atomic_load_explicit (&_owner_lease, std::memory_order_acquire)->token;
    }

    owner_lease_renew_result_t
    renew_owner_lease_once_at (std::chrono::steady_clock::time_point started_at,
                               std::chrono::steady_clock::time_point deadline_at,
                               std::stop_token cancellation)
    {
        auto heartbeat = std::make_shared<heartbeat_owner_t> ();
        auto attempt = std::make_shared<heartbeat_attempt_t> (this, started_at, deadline_at,
                                                              heartbeat, cancellation);
        auto pending = heartbeat_renew_once_async (attempt);
        auto completed =
          detail::observe_task_result_for (pending, remaining_until (deadline_at), cancellation);
        if (!completed) {
            std::function<void ()> expire;
            {
                std::lock_guard lock (heartbeat->gate);
                heartbeat->stop.store (true, std::memory_order_release);
                expire = std::move (attempt->expire);
            }
            if (expire)
                expire ();
            completed = pending.result ();
        }
        auto outcome = completed->value ();
        if (outcome.rejection)
            throw owner_lease_claim_rejected_error_t{*outcome.rejection, deadline_at,
                                                     std::move (outcome.message)};
        return outcome.result;
    }

    static std::chrono::milliseconds
    remaining_until (std::chrono::steady_clock::time_point deadline_at,
                     std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ())
    {
        return now >= deadline_at
                 ? std::chrono::milliseconds::zero ()
                 : std::chrono::ceil<std::chrono::milliseconds> (deadline_at - now);
    }

    template <typename Work>
    static auto lease_lane (const std::shared_ptr<heartbeat_attempt_t> &attempt,
                            Work work) -> task_t<std::invoke_result_t<Work &>>
    {
        return attempt->runtime->_lane.run_task (std::move (work));
    }

    template <typename T>
    static task_t<std::optional<result_t<T>>>
    await_heartbeat_store (task_t<T> pending, std::weak_ptr<heartbeat_attempt_t> weak_attempt)
    {
        auto attempt = weak_attempt.lock ();
        auto heartbeat = attempt ? attempt->heartbeat.lock () : nullptr;
        if (!attempt || !heartbeat || heartbeat->stop.load (std::memory_order_acquire)
            || remaining_until (attempt->deadline_at) <= std::chrono::milliseconds::zero ())
            co_return std::nullopt;
        auto completion = std::make_shared<task_completion_source_t<std::optional<result_t<T>>>> ();
        auto ready = completion->task ();
        {
            std::lock_guard lock (heartbeat->gate);
            if (heartbeat->stop.load (std::memory_order_acquire))
                co_return std::nullopt;
            attempt->expire = [completion] {
                completion->complete (result_t<std::optional<result_t<T>>>::success (std::nullopt));
            };
        }
        detail::observe_task_terminal (
          pending, [weak_attempt, completion] (const result_t<T> &result) {
              if (auto active = weak_attempt.lock ()) {
                  completion->complete (result_t<std::optional<result_t<T>>>::success (
                    remaining_until (active->deadline_at) > std::chrono::milliseconds::zero ()
                      ? std::optional<result_t<T>>{result}
                      : std::nullopt));
              }
          });
        attempt.reset ();
        auto result = co_await ready;
        if (auto active = weak_attempt.lock ()) {
            if (auto heartbeat = active->heartbeat.lock ()) {
                std::lock_guard lock (heartbeat->gate);
                active->expire = {};
            }
        }
        co_return result;
    }

    static task_t<void> heartbeat_accept (std::shared_ptr<heartbeat_attempt_t> attempt,
                                          location_owner_token_t token,
                                          std::chrono::system_clock::time_point expires_at,
                                          std::chrono::system_clock::time_point store_now,
                                          std::chrono::steady_clock::time_point started_at)
    {
        auto *runtime = attempt->runtime;
        co_await lease_lane (attempt, [runtime, token = std::move (token), expires_at, store_now,
                                       started_at] () mutable {
            const auto admission_lifetime =
              expires_at > store_now + runtime->_options.owner_lease_fencing_margin
                ? expires_at - store_now - runtime->_options.owner_lease_fencing_margin
                : std::chrono::system_clock::duration::zero ();
            runtime->update_owner_lease_on_lane ([&] (auto &lease) {
                lease.token = std::move (token);
                lease.healthy = true;
                lease.renewed_at = store_now;
                lease.admission_deadline =
                  started_at
                  + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                    admission_lifetime);
            });
            runtime->_last_error.reset ();
            return true;
        });
        runtime->wake_owner_lease_observers ();
    }

    static task_t<void> heartbeat_failure (std::shared_ptr<heartbeat_attempt_t> attempt,
                                           std::string message)
    {
        auto *runtime = attempt->runtime;
        co_await lease_lane (attempt, [runtime, message = std::move (message)] () mutable {
            runtime->record_failure_on_lane (std::move (message));
            return true;
        });
    }

    static task_t<std::optional<owner_lease_found_t>>
    heartbeat_confirm (std::shared_ptr<heartbeat_attempt_t> attempt, std::string &failure)
    {
        auto *runtime = attempt->runtime;
        bool failed = false;
        try {
            if (attempt->cancellation.stop_requested ())
                co_return std::nullopt;
            if (auto heartbeat = attempt->heartbeat.lock ();
                heartbeat && heartbeat->stop.load (std::memory_order_acquire)
                || remaining_until (attempt->deadline_at) <= std::chrono::milliseconds::zero ())
                co_return std::nullopt;
            auto response = co_await await_heartbeat_store (
              runtime->_store->read_owner_lease (runtime->_owner_id), attempt);
            if (!response)
                co_return std::nullopt;
            if (!response->has_value ()) {
                failure = response->error () ? response->error ()->what ()
                                             : "owner lease confirmation read failed";
                runtime->record_store_error ();
                failed = true;
            } else {
                const auto read = response->value ();
                if (const auto *found = std::get_if<owner_lease_found_t> (&read))
                    co_return *found;
            }
        }
        catch (const std::exception &error) {
            runtime->record_store_error ();
            failure = error.what ();
            failed = true;
        }
        if (failed)
            co_await heartbeat_failure (attempt, failure);
        co_return std::nullopt;
    }

    static task_t<lease_renew_outcome_t>
    heartbeat_renew_once_async (std::weak_ptr<heartbeat_attempt_t> weak_attempt)
    {
        auto attempt = weak_attempt.lock ();
        if (!attempt)
            co_return lease_renew_outcome_t{};
        lease_renew_outcome_t outcome;
        auto *runtime = attempt->runtime;
        runtime_metrics_t metrics (runtime->_monitoring);
        const auto metrics_enabled = metrics.enabled ();
        const auto started_at = attempt->started_at;
        std::optional<std::string> unexpected_failure;
        try {
            const auto owner_token = co_await lease_lane (
              attempt, [runtime] { return runtime->current_owner_token_unchecked (); });
            if (auto heartbeat = attempt->heartbeat.lock ();
                heartbeat && heartbeat->stop.load (std::memory_order_acquire))
                co_return outcome;
            if (owner_token) {
                std::string failure = "owner lease renewal timed out";
                std::optional<owner_lease_renew_result_t> result;
                try {
                    if (remaining_until (attempt->deadline_at)
                        <= std::chrono::milliseconds::zero ()) {
                        co_await heartbeat_failure (attempt, failure);
                        co_return outcome;
                    }
                    auto response = co_await await_heartbeat_store (
                      runtime->_store->renew_owner_lease (*owner_token,
                                                          runtime->_options.owner_lease_ttl),
                      attempt);
                    if (response)
                        result = response->value ();
                }
                catch (const std::exception &error) {
                    failure = error.what ();
                }
                if (result) {
                    if (const auto *renewed = std::get_if<owner_lease_renewed_t> (&*result)) {
                        if (const char *trace = std::getenv ("ZLINK_CPP_AUTO_CONNECT_TRACE");
                            trace != nullptr && *trace != '\0') {
                            const auto completed_at = std::chrono::steady_clock::now ();
                            std::cerr << "zlink owner-lease renew" << " monotonicMs="
                                      << std::chrono::duration_cast<std::chrono::milliseconds> (
                                           completed_at.time_since_epoch ())
                                           .count ()
                                      << " durationMs="
                                      << std::chrono::duration_cast<std::chrono::milliseconds> (
                                           completed_at - started_at)
                                           .count ()
                                      << " renewIntervalMs="
                                      << runtime->_options.owner_lease_renew_interval.count ()
                                      << " ttlMs=" << runtime->_options.owner_lease_ttl.count ()
                                      << '\n';
                        }
                        co_await heartbeat_accept (attempt, *owner_token, renewed->lease_expires_at,
                                                   renewed->store_now, started_at);
                        outcome.result = *renewed;
                        co_return outcome;
                    }
                    co_await lease_lane (attempt, [runtime] {
                        runtime->clear_owner_lease_on_lane ();
                        return true;
                    });
                } else {
                    auto confirmed = co_await heartbeat_confirm (attempt, failure);
                    if (confirmed && confirmed->token.owner_id == owner_token->owner_id
                        && confirmed->token.lease_generation == owner_token->lease_generation) {
                        co_await heartbeat_accept (attempt, confirmed->token,
                                                   confirmed->lease_expires_at,
                                                   confirmed->store_now, started_at);
                        outcome.result =
                          owner_lease_renewed_t{confirmed->lease_expires_at, confirmed->store_now};
                        co_return outcome;
                    }
                    if (metrics_enabled)
                        metrics.counter ("zlink.location.owner_lease.renew.failures", "{failure}",
                                         1);
                    co_await heartbeat_failure (attempt, std::move (failure));
                    co_return outcome;
                }
            }

            std::string failure = "owner lease claim timed out";
            std::optional<owner_lease_claim_result_t> claim;
            try {
                if (auto heartbeat = attempt->heartbeat.lock ();
                    heartbeat && heartbeat->stop.load (std::memory_order_acquire))
                    co_return outcome;
                if (remaining_until (attempt->deadline_at) <= std::chrono::milliseconds::zero ()) {
                    co_await heartbeat_failure (attempt, failure);
                    co_return outcome;
                }
                auto response = co_await await_heartbeat_store (
                  runtime->_store->claim_owner_lease (runtime->_owner_id,
                                                      runtime->_options.owner_lease_ttl),
                  attempt);
                if (response)
                    claim = response->value ();
            }
            catch (const std::exception &error) {
                failure = error.what ();
            }
            if (!claim) {
                if (!attempt->cancellation.stop_requested ()) {
                    auto confirmed = co_await heartbeat_confirm (attempt, failure);
                    if (confirmed) {
                        co_await heartbeat_accept (attempt, confirmed->token,
                                                   confirmed->lease_expires_at,
                                                   confirmed->store_now, started_at);
                        outcome.result =
                          owner_lease_renewed_t{confirmed->lease_expires_at, confirmed->store_now};
                        co_return outcome;
                    }
                }
                if (metrics_enabled)
                    metrics.counter ("zlink.location.owner_lease.renew.failures", "{failure}", 1);
                co_await heartbeat_failure (attempt, std::move (failure));
                co_return outcome;
            }
            if (const auto *claimed = std::get_if<owner_lease_claimed_t> (&*claim)) {
                co_await heartbeat_accept (attempt, claimed->token, claimed->lease_expires_at,
                                           claimed->store_now, started_at);
                outcome.result =
                  owner_lease_renewed_t{claimed->lease_expires_at, claimed->store_now};
                co_return outcome;
            }
            if (std::holds_alternative<owner_lease_generation_exhausted_t> (*claim)) {
                co_await heartbeat_failure (attempt, "owner lease generation is exhausted");
                outcome.rejection = owner_lease_claim_rejection_t::generation_exhausted;
                outcome.message = "owner lease generation is exhausted";
                co_return outcome;
            }
            bool scheduled_heartbeat = false;
            if (auto heartbeat = attempt->heartbeat.lock ()) {
                std::lock_guard lock (heartbeat->gate);
                scheduled_heartbeat = heartbeat->current == attempt;
            }
            if (scheduled_heartbeat) {
                auto confirmed = co_await heartbeat_confirm (attempt, failure);
                if (confirmed && confirmed->token.owner_id == runtime->_owner_id) {
                    co_await heartbeat_accept (attempt, confirmed->token,
                                               confirmed->lease_expires_at, confirmed->store_now,
                                               started_at);
                    outcome.result =
                      owner_lease_renewed_t{confirmed->lease_expires_at, confirmed->store_now};
                    co_return outcome;
                }
                co_await heartbeat_failure (attempt, "owner lease claim was rejected");
            }
            outcome.rejection = owner_lease_claim_rejection_t::conflict;
            outcome.message = "owner lease claim was rejected";
        }
        catch (const std::exception &error) {
            runtime->record_store_error ();
            unexpected_failure = error.what ();
        }
        if (unexpected_failure)
            co_await heartbeat_failure (attempt, std::move (*unexpected_failure));
        co_return outcome;
    }

    void release_cancelled_claim (std::chrono::steady_clock::time_point deadline_at) noexcept
    {
        try {
            auto token = current_owner_token_unchecked ();
            if (!token) {
                const auto remaining = remaining_until (deadline_at);
                if (remaining <= std::chrono::milliseconds::zero ()) {
                    record_failure ("owner lease cancellation read timed out");
                } else {
                    auto read_task = _store->read_owner_lease (_owner_id);
                    const auto read_response = read_task.result_for (remaining);
                    if (!read_response) {
                        record_failure ("owner lease cancellation read timed out");
                    } else if (!read_response->has_value ()) {
                        record_store_error ();
                        record_failure (read_response->error ()
                                          ? read_response->error ()->what ()
                                          : "owner lease cancellation read failed");
                    } else if (const auto *found =
                                 std::get_if<owner_lease_found_t> (&read_response->value ());
                               found != nullptr && found->token.owner_id == _owner_id) {
                        token = found->token;
                    }
                }
            }
            if (token) {
                const auto remaining = remaining_until (deadline_at);
                if (remaining <= std::chrono::milliseconds::zero ()) {
                    record_failure ("owner lease cancellation release timed out");
                } else {
                    auto release_task = _store->release_owner_lease (*token);
                    const auto released = release_task.result_for (remaining);
                    if (!released) {
                        record_failure ("owner lease cancellation release timed out");
                    } else if (!released->has_value ()) {
                        record_store_error ();
                        record_failure (released->error ()
                                          ? released->error ()->what ()
                                          : "owner lease cancellation release failed");
                    }
                }
            }
        }
        catch (const std::exception &error) {
            record_store_error ();
            record_failure (error.what ());
        }
        _lane.run_checked ([this] { clear_owner_lease_on_lane (); }).get ();
    }

    static std::string make_owner_id ()
    {
        static std::atomic_uint64_t counter{1};
        const auto now = std::chrono::steady_clock::now ().time_since_epoch ().count ();
        const auto random = std::random_device{}();
        return "cpp-location-owner-" + std::to_string (now) + "-" + std::to_string (random) + "-"
               + std::to_string (counter.fetch_add (1));
    }

    void stop_heartbeat () noexcept
    {
        auto heartbeat = std::atomic_load_explicit (&_heartbeat_state, std::memory_order_acquire);
        if (!heartbeat)
            return;
        {
            std::lock_guard lock (heartbeat->gate);
            heartbeat->stop.store (true, std::memory_order_release);
            heartbeat->wake.notify_all ();
        }
        if (_heartbeat.joinable ())
            runtime::infrastructure_wait_guard::join (_heartbeat, "location/heartbeat");
        std::shared_ptr<heartbeat_attempt_t> attempt;
        std::function<void ()> expire;
        {
            std::lock_guard lock (heartbeat->gate);
            attempt = std::move (heartbeat->current);
            if (attempt)
                expire = std::move (attempt->expire);
        }
        if (attempt) {
            if (expire)
                expire ();
            if (attempt->task)
                static_cast<void> (attempt->task->result ());
        }
        std::atomic_store_explicit (&_heartbeat_state, std::shared_ptr<heartbeat_owner_t>{},
                                    std::memory_order_release);
    }

    void heartbeat_loop (std::shared_ptr<heartbeat_owner_t> heartbeat,
                         std::chrono::steady_clock::time_point initial_operation_started_at)
    {
        heartbeat_loop (
          std::move (heartbeat), initial_operation_started_at, &std::chrono::steady_clock::now,
          [] (auto &condition, auto &lock, auto timeout, auto ready, const char *site) {
              runtime::infrastructure_wait_guard::condition_wait_for (
                condition, lock, timeout, std::move (ready), site,
                runtime::infrastructure_wait_guard::wait_relation_t::own_input);
          });
    }

    template <typename Now, typename Wait>
    void heartbeat_loop (std::shared_ptr<heartbeat_owner_t> heartbeat,
                         std::chrono::steady_clock::time_point initial_operation_started_at,
                         Now now,
                         Wait wait)
    {
        constexpr auto *wait_site = "location/heartbeat-input";
        auto operation_started_at = initial_operation_started_at;
        while (!heartbeat->stop.load (std::memory_order_acquire)) {
            const auto scheduled_at = operation_started_at + _options.owner_lease_renew_interval;
            std::unique_lock lock (heartbeat->gate);
            if (!heartbeat->current
                || (heartbeat->current->task && heartbeat->current->task->await_ready ())) {
                wait (
                  heartbeat->wake, lock, remaining_until (scheduled_at, now ()),
                  [&] { return heartbeat->stop.load (std::memory_order_acquire); }, wait_site);
                if (heartbeat->stop.load (std::memory_order_acquire))
                    break;
                heartbeat->current.reset ();
            } else {
                auto attempt = heartbeat->current;
                const auto remaining = remaining_until (attempt->deadline_at);
                if (remaining <= std::chrono::milliseconds::zero ()) {
                    auto expire = std::move (attempt->expire);
                    lock.unlock ();
                    if (expire)
                        expire ();
                    else {
                        lock.lock ();
                        runtime::infrastructure_wait_guard::condition_wait_for (
                          heartbeat->wake, lock, _options.owner_lease_renew_interval,
                          [&] { return heartbeat->stop.load (std::memory_order_acquire); },
                          wait_site,
                          runtime::infrastructure_wait_guard::wait_relation_t::own_input);
                    }
                    continue;
                }
                runtime::infrastructure_wait_guard::condition_wait_for (
                  heartbeat->wake, lock, remaining,
                  [&] {
                      return heartbeat->stop.load (std::memory_order_acquire)
                             || (attempt->task && attempt->task->await_ready ());
                  },
                  wait_site, runtime::infrastructure_wait_guard::wait_relation_t::own_input);
                continue;
            }
            const auto started_at = now ();
            const auto lateness = started_at - scheduled_at;
            operation_started_at = started_at;
            auto attempt = std::make_shared<heartbeat_attempt_t> (
              this, started_at, started_at + _options.owner_lease_renew_timeout, heartbeat);
            heartbeat->current = attempt;
            lock.unlock ();
            if (const runtime_metrics_t metrics (_monitoring);
                metrics.enabled () && lateness > lateness.zero ())
                metrics.histogram ("zlink.location.owner_lease.renew.lateness", "s",
                                   std::chrono::duration<double> (lateness).count ());
            attempt->task.emplace (heartbeat_renew_once_async (attempt));
            detail::observe_task_terminal (*attempt->task,
                                           [weak_heartbeat = std::weak_ptr{heartbeat}] (
                                             const result_t<lease_renew_outcome_t> &) {
                                               if (auto owner = weak_heartbeat.lock ()) {
                                                   std::lock_guard lock (owner->gate);
                                                   owner->wake.notify_all ();
                                               }
                                           });
        }
    }

    task_t<void> drain_descriptor_publications () const
    {
        auto operations =
          co_await _lane.run_task ([this] { return std::exchange (_descriptor_publications, {}); });
        for (const auto &operation : operations)
            operation->observe (detail::result_access_t::failure<location_write_result_t> (
              detail::make_cancellation_exception ("Location descriptor publication stopped")));
        // The caller's deadline cannot end opaque I/O. Preserve every repository
        // coroutine and its input until its actual terminal, before provider teardown.
        for (const auto &operation : operations)
            co_await operation->drained.task ();
    }

    void record_failure (std::string message) const
    {
        _lane.run_checked ([&] { record_failure_on_lane (std::move (message)); }).get ();
    }

    void record_failure_on_lane (std::string message) const
    {
        update_owner_lease_on_lane ([] (auto &lease) { lease.healthy = false; });
        _last_error = std::move (message);
    }

    location_repository_t *_store;
    location_options_t _options;
    std::string _owner_id;
    std::atomic_bool _draining = false;
    std::shared_ptr<framework::detail::monitoring_runtime_state_t> _monitoring;
    std::atomic_bool _started = false;
    std::thread _heartbeat;
    std::shared_ptr<heartbeat_owner_t> _heartbeat_state;
    // Lane teardown finishes accepted turns before their state is destroyed.
    mutable std::shared_ptr<const owner_lease_snapshot_t> _owner_lease{
      std::make_shared<const owner_lease_snapshot_t> ()};
    mutable std::optional<std::string> _last_error;
    mutable std::vector<std::shared_ptr<descriptor_publication_t>> _descriptor_publications;
    offload_executor_t _lane_executor;
    mutable state_lane_t _lane{_lane_executor};
};

} // namespace zlink::framework::runtime
