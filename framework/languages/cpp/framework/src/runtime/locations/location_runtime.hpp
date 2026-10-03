/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

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
                                      std::chrono::steady_clock::time_point deadline,
                                      std::weak_ptr<heartbeat_owner_t> owner,
                                      std::stop_token cancellation = {}) :
            runtime (runtime),
            deadline_at (deadline),
            heartbeat (std::move (owner)),
            cancellation (cancellation)
        {
        }

        location_runtime_t *runtime;
        std::chrono::steady_clock::time_point deadline_at;
        std::weak_ptr<heartbeat_owner_t> heartbeat;
        std::stop_token cancellation;
        std::function<void ()> expire;
        std::optional<task_t<lease_renew_outcome_t>> task;
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
            return observation_status_t{_last_error, _owner_lease_healthy, _owner_lease_renewed_at};
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
        return current_owner_token_task ().result ().value ();
    }

    task_t<std::optional<location_owner_token_t>> current_owner_token_task () const
    {
        return _lane.run_task ([this] {
            return owner_lease_usable_on_lane () ? _owner_token
                                                 : std::optional<location_owner_token_t>{};
        });
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
        return _lane.run_checked ([this] { return _owner_lease_healthy; }).get ();
    }

    bool owner_lease_usable () const noexcept
    {
        return _lane.run_checked ([this] { return owner_lease_usable_on_lane (); }).get ();
    }

    std::optional<std::chrono::system_clock::time_point> owner_lease_renewed_at () const
    {
        return _lane.run_checked ([this] { return _owner_lease_renewed_at; }).get ();
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
        try {
            if (cancellation.stop_requested ())
                std::rethrow_exception (
                  detail::make_cancellation_exception ("location runtime startup was cancelled"));
            const auto deadline_at =
              std::chrono::steady_clock::now () + _options.owner_lease_renew_timeout;
            renew_owner_lease_once (deadline_at, cancellation);
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
        _heartbeat_state = std::make_shared<heartbeat_owner_t> ();
        _heartbeat = std::thread ([this, heartbeat = _heartbeat_state] {
#ifndef NDEBUG
            runtime::infrastructure_wait_guard::infrastructure_scope_t scope (this);
#endif
            heartbeat_loop (std::move (heartbeat));
        });
    }

    void stop () noexcept
    {
        if (!_started.exchange (false))
            return;
        stop_heartbeat ();
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
                _lane
                  .run_checked ([this] {
                      _owner_token.reset ();
                      _owner_lease_admission_deadline.reset ();
                  })
                  .get ();
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
        const auto deadline_at = requested_deadline_at.value_or (
          std::chrono::steady_clock::now () + _options.owner_lease_renew_timeout);
        auto heartbeat = std::make_shared<heartbeat_owner_t> ();
        auto attempt =
          std::make_shared<heartbeat_attempt_t> (this, deadline_at, heartbeat, cancellation);
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
    bool owner_lease_usable_on_lane () const noexcept
    {
        return _owner_token.has_value () && _owner_lease_admission_deadline.has_value ()
               && std::chrono::steady_clock::now () < *_owner_lease_admission_deadline;
    }

    std::optional<location_owner_token_t> current_owner_token_unchecked () const
    {
        return _lane.run_checked ([this] { return _owner_token; }).get ();
    }

    static std::chrono::milliseconds
    remaining_until (std::chrono::steady_clock::time_point deadline_at)
    {
        const auto now = std::chrono::steady_clock::now ();
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
        co_await lease_lane (
          attempt, [runtime, token = std::move (token), expires_at, store_now, started_at] {
              const auto admission_lifetime =
                expires_at > store_now + runtime->_options.owner_lease_fencing_margin
                  ? expires_at - store_now - runtime->_options.owner_lease_fencing_margin
                  : std::chrono::system_clock::duration::zero ();
              runtime->_owner_token = token;
              runtime->_owner_lease_healthy = true;
              runtime->_owner_lease_renewed_at = store_now;
              runtime->_owner_lease_admission_deadline =
                started_at
                + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                  admission_lifetime);
              runtime->_last_error.reset ();
              return true;
          });
    }

    static task_t<void> heartbeat_failure (std::shared_ptr<heartbeat_attempt_t> attempt,
                                           std::string message)
    {
        auto *runtime = attempt->runtime;
        co_await lease_lane (attempt, [runtime, message = std::move (message)] () mutable {
            runtime->_owner_lease_healthy = false;
            runtime->_last_error = std::move (message);
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
        const auto started_at = std::chrono::steady_clock::now ();
        std::optional<std::string> unexpected_failure;
        try {
            const auto snapshot =
              co_await lease_lane (attempt, [runtime, started_at, metrics_enabled] {
                  const auto due_at =
                    metrics_enabled && runtime->_last_renew_started_at
                      ? std::optional{*runtime->_last_renew_started_at
                                      + runtime->_options.owner_lease_renew_interval}
                      : std::optional<std::chrono::steady_clock::time_point>{};
                  if (metrics_enabled)
                      runtime->_last_renew_started_at = started_at;
                  return std::pair{runtime->_owner_token, due_at};
              });
            if (snapshot.second && started_at > *snapshot.second) {
                metrics.histogram (
                  "zlink.location.owner_lease.renew.lateness", "s",
                  std::chrono::duration<double> (started_at - *snapshot.second).count ());
            }
            if (auto heartbeat = attempt->heartbeat.lock ();
                heartbeat && heartbeat->stop.load (std::memory_order_acquire))
                co_return outcome;
            if (snapshot.first) {
                std::string failure = "owner lease renewal timed out";
                std::optional<owner_lease_renew_result_t> result;
                try {
                    if (remaining_until (attempt->deadline_at)
                        <= std::chrono::milliseconds::zero ()) {
                        co_await heartbeat_failure (attempt, failure);
                        co_return outcome;
                    }
                    auto response = co_await await_heartbeat_store (
                      runtime->_store->renew_owner_lease (*snapshot.first,
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
                        co_await heartbeat_accept (attempt, *snapshot.first,
                                                   renewed->lease_expires_at, renewed->store_now,
                                                   started_at);
                        outcome.result = *renewed;
                        co_return outcome;
                    }
                    co_await lease_lane (attempt, [runtime] {
                        runtime->_owner_token.reset ();
                        runtime->_owner_lease_admission_deadline.reset ();
                        return true;
                    });
                } else {
                    auto confirmed = co_await heartbeat_confirm (attempt, failure);
                    if (confirmed && confirmed->token.owner_id == snapshot.first->owner_id
                        && confirmed->token.lease_generation == snapshot.first->lease_generation) {
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
        _lane
          .run_checked ([this] {
              _owner_token.reset ();
              _owner_lease_admission_deadline.reset ();
          })
          .get ();
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
        auto heartbeat = _heartbeat_state;
        if (!heartbeat)
            return;
        heartbeat->stop.store (true, std::memory_order_release);
        heartbeat->wake.notify_all ();
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
        _heartbeat_state.reset ();
    }

    void heartbeat_loop (std::shared_ptr<heartbeat_owner_t> heartbeat)
    {
        constexpr auto *wait_site = "location/heartbeat-input";
        while (!heartbeat->stop.load (std::memory_order_acquire)) {
            std::unique_lock lock (heartbeat->gate);
            if (!heartbeat->current) {
                runtime::infrastructure_wait_guard::condition_wait_for (
                  heartbeat->wake, lock, _options.owner_lease_renew_interval,
                  [&] { return heartbeat->stop.load (std::memory_order_acquire); }, wait_site,
                  runtime::infrastructure_wait_guard::wait_relation_t::own_input);
                if (heartbeat->stop.load (std::memory_order_acquire))
                    break;
            } else if (heartbeat->current->task && heartbeat->current->task->await_ready ()) {
                const auto scheduled_at = heartbeat->current->deadline_at
                                          - _options.owner_lease_renew_timeout
                                          + _options.owner_lease_renew_interval;
                runtime::infrastructure_wait_guard::condition_wait_for (
                  heartbeat->wake, lock, remaining_until (scheduled_at),
                  [&] { return heartbeat->stop.load (std::memory_order_acquire); }, wait_site,
                  runtime::infrastructure_wait_guard::wait_relation_t::own_input);
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
            auto attempt = std::make_shared<heartbeat_attempt_t> (
              this, std::chrono::steady_clock::now () + _options.owner_lease_renew_timeout,
              heartbeat);
            heartbeat->current = attempt;
            lock.unlock ();
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

    void record_failure (std::string message) const
    {
        _lane
          .run_checked ([&] {
              _owner_lease_healthy = false;
              _last_error = std::move (message);
          })
          .get ();
    }

    location_repository_t *_store;
    location_options_t _options;
    std::string _owner_id;
    std::atomic_bool _draining = false;
    std::shared_ptr<framework::detail::monitoring_runtime_state_t> _monitoring;
    std::optional<std::chrono::steady_clock::time_point> _last_renew_started_at;
    std::atomic_bool _started = false;
    std::thread _heartbeat;
    std::shared_ptr<heartbeat_owner_t> _heartbeat_state;
    offload_executor_t _lane_executor;
    mutable state_lane_t _lane{_lane_executor};
    mutable bool _owner_lease_healthy = false;
    mutable std::optional<std::chrono::system_clock::time_point> _owner_lease_renewed_at;
    mutable std::optional<std::chrono::steady_clock::time_point> _owner_lease_admission_deadline;
    mutable std::optional<location_owner_token_t> _owner_token;
    mutable std::optional<std::string> _last_error;
};

} // namespace zlink::framework::runtime
