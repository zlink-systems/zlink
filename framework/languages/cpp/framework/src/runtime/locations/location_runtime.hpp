/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/execution/state_lane.hpp"
#include <runtime/locations/location_repository.hpp>

#include <zlink/framework/contracts/locations/options.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
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
  public:
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
        return _lane
          .run ([this] {
              return owner_lease_usable_on_lane () ? _owner_token
                                                   : std::optional<location_owner_token_t>{};
          })
          .get ();
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
        return _lane.run ([this] { return _owner_lease_healthy; }).get ();
    }

    bool owner_lease_usable () const noexcept
    {
        return _lane.run ([this] { return owner_lease_usable_on_lane (); }).get ();
    }

    std::optional<std::chrono::system_clock::time_point> owner_lease_renewed_at () const
    {
        return _lane.run ([this] { return _owner_lease_renewed_at; }).get ();
    }

    std::optional<std::string> last_error () const
    {
        return _lane.run ([this] { return _last_error; }).get ();
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
                throw detail::make_boundary_exception (detail::boundary_error_t::cancelled,
                                                       "location runtime startup was cancelled");
            const auto deadline_at =
              std::chrono::steady_clock::now () + _options.owner_lease_renew_timeout;
            renew_owner_lease_once (deadline_at, cancellation);
            if (cancellation.stop_requested ()) {
                release_cancelled_claim (deadline_at);
                throw detail::make_boundary_exception (detail::boundary_error_t::cancelled,
                                                       "location runtime startup was cancelled");
            }
        }
        catch (...) {
            _started.store (false, std::memory_order_release);
            throw;
        }
        _heartbeat_stop.store (false, std::memory_order_release);
        _heartbeat = std::thread ([this] { heartbeat_loop (); });
    }

    void stop () noexcept
    {
        if (!_started.exchange (false)) {
            return;
        }
        _heartbeat_stop.store (true, std::memory_order_release);
        _heartbeat_wake.notify_all ();
        if (_heartbeat.joinable ()) {
            _heartbeat.join ();
        }
        try {
            const auto token = current_owner_token_unchecked ();
            if (token) {
                _store->remove_all_by_owner (*token).result ().value ();
                _store->release_owner_lease (*token).result ().value ();
                _lane
                  .run ([this] {
                      _owner_token.reset ();
                      _owner_lease_admission_deadline.reset ();
                  })
                  .get ();
            }
        }
        catch (const std::exception &error) {
            record_store_error ();
            record_failure (error.what ());
        }
    }

    /* Drain owner cleanup (graceful-drain-handoff §4-5): stops the lease
     * heartbeat, then removes this owner's lease and rows while the store
     * stays usable for the rest of teardown. Returns false when the store
     * rejects the cleanup (the drain worker maps it to OwnerCleanupFailed). */
    bool cleanup_owner () noexcept
    {
        if (_started.exchange (false)) {
            _heartbeat_stop.store (true, std::memory_order_release);
            _heartbeat_wake.notify_all ();
            if (_heartbeat.joinable ()) {
                _heartbeat.join ();
            }
        }
        try {
            const auto token = current_owner_token_unchecked ();
            if (token) {
                _store->remove_all_by_owner (*token).result ().value ();
                _store->release_owner_lease (*token).result ().value ();
                _lane
                  .run ([this] {
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
        catch (...) {
            return false;
        }
    }

    owner_lease_renew_result_t renew_owner_lease_once (
      std::optional<std::chrono::steady_clock::time_point> requested_deadline_at = std::nullopt,
      std::stop_token cancellation = {})
    {
        runtime_metrics_t metrics (_monitoring);
        const auto metrics_enabled = metrics.enabled ();
        std::optional<std::chrono::steady_clock::time_point> due_at;
        if (metrics_enabled) {
            due_at = _lane
                       .run ([this] {
                           return _last_renew_started_at
                                    ? std::optional{*_last_renew_started_at
                                                    + _options.owner_lease_renew_interval}
                                    : std::optional<std::chrono::steady_clock::time_point>{};
                       })
                       .get ();
        }
        const auto started_at = std::chrono::steady_clock::now ();
        if (metrics_enabled) {
            _lane
              .run ([&] {
                  _last_renew_started_at = started_at;
                  if (due_at && started_at > *due_at) {
                      metrics.histogram (
                        "zlink.location.owner_lease.renew.lateness", "s",
                        std::chrono::duration<double> (started_at - *due_at).count ());
                  }
              })
              .get ();
        }
        const auto deadline_at =
          requested_deadline_at.value_or (started_at + _options.owner_lease_renew_timeout);
        const auto confirm_claim =
          [&] (std::string &failure) -> std::optional<owner_lease_found_t> {
            try {
                const auto remaining = remaining_until (deadline_at);
                if (remaining <= std::chrono::milliseconds::zero ())
                    return std::nullopt;
                auto read_task = _store->read_owner_lease (_owner_id);
                const auto read_response = read_task.result_for (remaining, cancellation);
                if (!read_response)
                    return std::nullopt;
                if (!read_response->has_value ()) {
                    failure = read_response->error () ? read_response->error ()->what ()
                                                      : "owner lease confirmation read failed";
                    record_store_error ();
                    record_failure (failure);
                    return std::nullopt;
                }
                const auto read = read_response->value ();
                if (const auto *found = std::get_if<owner_lease_found_t> (&read))
                    return *found;
            }
            catch (const std::exception &error) {
                record_store_error ();
                failure = error.what ();
                record_failure (failure);
            }
            return std::nullopt;
        };
        const auto accept_lease = [&] (const location_owner_token_t &owner,
                                       std::chrono::system_clock::time_point expires_at,
                                       std::chrono::system_clock::time_point store_now) {
            const auto admission_lifetime =
              expires_at > store_now + _options.owner_lease_fencing_margin
                ? expires_at - store_now - _options.owner_lease_fencing_margin
                : std::chrono::system_clock::duration::zero ();
            _lane
              .run ([&] {
                  _owner_token = owner;
                  _owner_lease_healthy = true;
                  _owner_lease_renewed_at = store_now;
                  _owner_lease_admission_deadline =
                    started_at
                    + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                      admission_lifetime);
                  _last_error.reset ();
              })
              .get ();
            return owner_lease_renew_result_t{owner_lease_renewed_t{expires_at, store_now}};
        };

        const auto token = current_owner_token_unchecked ();
        if (token) {
            std::optional<owner_lease_renew_result_t> result;
            std::string failure = "owner lease renewal timed out";
            try {
                const auto remaining = remaining_until (deadline_at);
                if (remaining <= std::chrono::milliseconds::zero ()) {
                    record_failure (std::move (failure));
                    return owner_lease_renew_result_t{owner_lease_stale_t{}};
                }
                auto renew_task = _store->renew_owner_lease (*token, _options.owner_lease_ttl);
                const auto renew_response = renew_task.result_for (remaining, cancellation);
                if (renew_response)
                    result = renew_response->value ();
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
                                  << _options.owner_lease_renew_interval.count ()
                                  << " ttlMs=" << _options.owner_lease_ttl.count () << '\n';
                    }
                    return accept_lease (*token, renewed->lease_expires_at, renewed->store_now);
                }
                _lane
                  .run ([this] {
                      _owner_token.reset ();
                      _owner_lease_admission_deadline.reset ();
                  })
                  .get ();
            } else {
                if (const auto confirmed = confirm_claim (failure);
                    confirmed && confirmed->token.owner_id == token->owner_id
                    && confirmed->token.lease_generation == token->lease_generation) {
                    return accept_lease (confirmed->token, confirmed->lease_expires_at,
                                         confirmed->store_now);
                }
                if (metrics_enabled)
                    metrics.counter ("zlink.location.owner_lease.renew.failures", "{failure}", 1);
                record_failure (std::move (failure));
                return owner_lease_renew_result_t{owner_lease_stale_t{}};
            }
        }

        std::optional<owner_lease_claim_result_t> claim;
        std::string failure = "owner lease claim timed out";
        try {
            const auto remaining = remaining_until (deadline_at);
            if (remaining <= std::chrono::milliseconds::zero ()) {
                record_failure (std::move (failure));
                return owner_lease_renew_result_t{owner_lease_stale_t{}};
            }
            auto claim_task = _store->claim_owner_lease (_owner_id, _options.owner_lease_ttl);
            const auto claim_response = claim_task.result_for (remaining, cancellation);
            if (claim_response)
                claim = claim_response->value ();
        }
        catch (const std::exception &error) {
            failure = error.what ();
        }
        if (!claim) {
            if (!cancellation.stop_requested ()) {
                if (const auto confirmed = confirm_claim (failure))
                    return accept_lease (confirmed->token, confirmed->lease_expires_at,
                                         confirmed->store_now);
            }
            if (metrics_enabled)
                metrics.counter ("zlink.location.owner_lease.renew.failures", "{failure}", 1);
            record_failure (std::move (failure));
            return owner_lease_renew_result_t{owner_lease_stale_t{}};
        }
        if (const auto *claimed = std::get_if<owner_lease_claimed_t> (&*claim)) {
            return accept_lease (claimed->token, claimed->lease_expires_at, claimed->store_now);
        }
        if (std::holds_alternative<owner_lease_generation_exhausted_t> (*claim)) {
            throw owner_lease_claim_rejected_error_t{
              owner_lease_claim_rejection_t::generation_exhausted, deadline_at,
              "owner lease generation is exhausted"};
        }
        throw owner_lease_claim_rejected_error_t{owner_lease_claim_rejection_t::conflict,
                                                 deadline_at, "owner lease claim was rejected"};
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
        return _lane.run ([this] { return _owner_token; }).get ();
    }

    static std::chrono::milliseconds
    remaining_until (std::chrono::steady_clock::time_point deadline_at)
    {
        const auto now = std::chrono::steady_clock::now ();
        return now >= deadline_at
                 ? std::chrono::milliseconds::zero ()
                 : std::chrono::ceil<std::chrono::milliseconds> (deadline_at - now);
    }

    void accept_conflicting_claim (std::chrono::steady_clock::time_point deadline_at) noexcept
    {
        const auto started_at = std::chrono::steady_clock::now ();
        try {
            const auto remaining = remaining_until (deadline_at);
            if (remaining <= std::chrono::milliseconds::zero ())
                return;
            auto read_task = _store->read_owner_lease (_owner_id);
            const auto read_response = read_task.result_for (remaining);
            if (!read_response) {
                record_failure ("owner lease conflict confirmation read timed out");
                return;
            }
            if (!read_response->has_value ()) {
                record_store_error ();
                record_failure (read_response->error ()
                                  ? read_response->error ()->what ()
                                  : "owner lease conflict confirmation read failed");
                return;
            }
            const auto read = read_response->value ();
            const auto *found = std::get_if<owner_lease_found_t> (&read);
            if (found == nullptr || found->token.owner_id != _owner_id)
                return;
            const auto admission_lifetime =
              found->lease_expires_at > found->store_now + _options.owner_lease_fencing_margin
                ? found->lease_expires_at - found->store_now - _options.owner_lease_fencing_margin
                : std::chrono::system_clock::duration::zero ();
            _lane
              .run ([&] {
                  _owner_token = found->token;
                  _owner_lease_healthy = true;
                  _owner_lease_renewed_at = found->store_now;
                  _owner_lease_admission_deadline =
                    started_at
                    + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                      admission_lifetime);
                  _last_error.reset ();
              })
              .get ();
        }
        catch (const std::exception &error) {
            record_store_error ();
            record_failure (error.what ());
        }
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
          .run ([this] {
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

    void heartbeat_loop ()
    {
        while (!_heartbeat_stop.load (std::memory_order_acquire)) {
            std::unique_lock lock (_heartbeat_gate);
            _heartbeat_wake.wait_for (lock, _options.owner_lease_renew_interval, [this] {
                return _heartbeat_stop.load (std::memory_order_acquire);
            });
            if (_heartbeat_stop.load (std::memory_order_acquire)) {
                break;
            }
            lock.unlock ();
            try {
                renew_owner_lease_once ();
            }
            catch (const owner_lease_claim_rejected_error_t &error) {
                record_failure (error.what ());
                if (error.rejection () == owner_lease_claim_rejection_t::conflict)
                    accept_conflicting_claim (error.deadline_at ());
            }
        }
    }

    void record_failure (std::string message) const
    {
        _lane
          .run ([&] {
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
    std::atomic_bool _heartbeat_stop = false;
    std::thread _heartbeat;
    std::mutex _heartbeat_gate;
    std::condition_variable _heartbeat_wake;
    offload_executor_t _lane_executor;
    mutable state_lane_t _lane{_lane_executor};
    mutable bool _owner_lease_healthy = false;
    mutable std::optional<std::chrono::system_clock::time_point> _owner_lease_renewed_at;
    mutable std::optional<std::chrono::steady_clock::time_point> _owner_lease_admission_deadline;
    mutable std::optional<location_owner_token_t> _owner_token;
    mutable std::optional<std::string> _last_error;
};

} // namespace zlink::framework::runtime
