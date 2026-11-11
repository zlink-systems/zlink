/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/execution/task_result.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
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
#include <type_traits>
#include <utility>
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
    struct heartbeat_attempt_t;

    struct heartbeat_owner_t
    {
        std::atomic_bool stop = false;
        std::mutex gate;
        std::condition_variable wake;
        bool pending_peer_reconcile = false;
        std::shared_ptr<heartbeat_attempt_t> current;
        task_completion_source_t<void> discovery_changed;

        void notify (bool reconcile = false)
        {
            task_completion_source_t<void> changed;
            {
                std::lock_guard lock (gate);
                pending_peer_reconcile = pending_peer_reconcile || reconcile;
                changed = std::exchange (discovery_changed, task_completion_source_t<void>{});
                wake.notify_all ();
            }
            // A continuation may register the next observation immediately.
            changed.complete (result_t<void>::success ());
        }
    };

    struct store_outage_t : std::enable_shared_from_this<store_outage_t>
    {
        store_outage_t (std::chrono::steady_clock::time_point observed_at,
                        std::chrono::steady_clock::time_point cutoff,
                        location_owner_token_t token,
                        std::shared_ptr<heartbeat_owner_t> owner) :
            observed_at (observed_at),
            cutoff (cutoff),
            token (std::move (token)),
            owner (std::move (owner))
        {
        }

        const std::chrono::steady_clock::time_point observed_at;
        const std::chrono::steady_clock::time_point cutoff;
        const location_owner_token_t token;
        // Retain the heartbeat lifecycle, including after runtime teardown.
        const std::shared_ptr<heartbeat_owner_t> owner;
        std::atomic_bool discarded = false;
        task_completion_source_t<serial_execution_queue_t::execution_operation_t> terminal;
        std::shared_ptr<task_t<void>> recovery;
        std::shared_ptr<task_t<bool>> publication;
        std::vector<std::function<void (serial_execution_queue_t::execution_admission_t)>>
          discard_requests;

        bool latch (std::chrono::steady_clock::time_point now)
        {
            if (owner->stop.load (std::memory_order_acquire))
                return discarded.load (std::memory_order_acquire);
            if (now >= cutoff && !discarded.exchange (true, std::memory_order_acq_rel)) {
                // The deadline owns the latch; each owner only posts its local cleanup.
                for (const auto &request : discard_requests)
                    request (
                      [episode = shared_from_this ()] (const serial_execution_queue_t &queue) {
                          // A request already delivered to an owner must not start
                          // object cleanup after stop transfers cleanup to the Host.
                          return episode->owner->stop.load (std::memory_order_acquire)
                                   ? serial_execution_queue_t::execution_status_t::allowed
                                   : execution_admission (episode, queue.execution_ready_at (), {});
                      });
                terminal.complete (
                  result_t<serial_execution_queue_t::execution_operation_t>::success (
                    {serial_execution_queue_t::execution_status_t::ownership_lost, cutoff}));
            }
            return discarded.load (std::memory_order_acquire);
        }
    };

    struct successful_lease_t
    {
        location_owner_token_t token;
        std::chrono::steady_clock::time_point started_at;
        std::chrono::steady_clock::time_point admission_deadline;
    };

    enum class execution_gate_state_t : std::uint64_t
    {
        open,
        admission_closed,
        episode
    };
    static constexpr std::uint64_t execution_gate_state_mask = 3;

    struct owner_deadlines_t
    {
        std::shared_ptr<detail::deadline_state_t> reservation =
          std::make_shared<detail::deadline_state_t> ();

        void cancel ()
        {
            auto pending = std::atomic_exchange_explicit (
              &reservation, std::shared_ptr<detail::deadline_state_t>{}, std::memory_order_acq_rel);
            if (pending)
                pending->cancelled.store (true, std::memory_order_release);
            detail::deadline_scheduler_t::instance ().wake ();
        }
    };

    struct owner_lease_snapshot_t
    {
        std::optional<location_owner_token_t> token;
        std::optional<std::chrono::steady_clock::time_point> admission_deadline;
        std::optional<successful_lease_t> last_successful_lease;
        std::shared_ptr<store_outage_t> outage;
        std::vector<std::weak_ptr<store_outage_t>> discarded_outages;
        std::shared_ptr<owner_deadlines_t> deadlines = std::make_shared<owner_deadlines_t> ();
        bool healthy = false;
        std::optional<std::chrono::system_clock::time_point> renewed_at;
    };

  public:
    using execution_recovery_t =
      std::function<task_t<serial_execution_queue_t::execution_operation_t> (
        authority_key_t, authority_snapshot_t, std::function<task_t<void> (authority_snapshot_t)>)>;

  private:
    struct execution_observer_t
    {
        std::function<void (serial_execution_queue_t::execution_admission_t)> discard;
        std::function<void (authority_key_t, authority_snapshot_t)> observe;
        std::function<task_t<void> (execution_recovery_t, std::chrono::steady_clock::time_point)>
          recover;
        std::function<void ()> resume;
    };
    struct lease_renew_outcome_t
    {
        owner_lease_renew_result_t result{owner_lease_stale_t{}};
        std::optional<owner_lease_claim_rejection_t> rejection;
        std::string message;
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
        std::shared_ptr<store_outage_t> requested_outage;
    };

    struct repository_operation_t
    {
        virtual ~repository_operation_t () = default;
        virtual void stop_observation () {}
        task_completion_source_t<void> submitted;
        task_completion_source_t<void> drained;
    };

    struct descriptor_publication_t : repository_operation_t
    {
        explicit descriptor_publication_t (
          std::chrono::milliseconds timeout,
          std::function<std::chrono::steady_clock::time_point ()> clock,
          std::optional<std::chrono::steady_clock::time_point> requested_deadline = std::nullopt) :
            now (std::move (clock)),
            deadline_at (std::min (
              now () + timeout,
              requested_deadline.value_or (std::chrono::steady_clock::time_point::max ())))
        {
        }

        std::function<std::chrono::steady_clock::time_point ()> now;
        const std::chrono::steady_clock::time_point deadline_at;
        task_completion_source_t<location_write_result_t> observation;
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

        void stop_observation () override
        {
            observe (detail::result_access_t::failure<location_write_result_t> (
              detail::make_cancellation_exception ("Location descriptor publication stopped")));
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
        std::optional<std::chrono::steady_clock::time_point> store_outage_observed_at;
        std::optional<std::chrono::steady_clock::time_point> store_outage_cutoff;
        bool store_outage_discarded;
    };

    task_t<observation_status_t> observation_status_task () const
    {
        return _lane->run_task ([this] {
            const auto lease =
              std::atomic_load_explicit (_owner_lease.get (), std::memory_order_acquire);
            const auto &outage = lease->outage;
            return observation_status_t{
              _last_error,
              lease->healthy,
              lease->renewed_at,
              outage ? std::make_optional (outage->observed_at) : std::nullopt,
              outage ? std::make_optional (outage->cutoff) : std::nullopt,
              outage && outage->discarded.load (std::memory_order_acquire)};
        });
    }

    explicit location_runtime_t (location_repository_t &store,
                                 location_options_t options = {},
                                 std::string owner_id = make_owner_id ()) :
        _store (&store), _options (options), _owner_id (std::move (owner_id))
    {
        _store->configure_reclaim_margin (_options.owner_lease_fencing_margin);
        auto access = std::make_shared<location_repository_t::access_t> ();
        access->admit = [lease = _owner_lease, clock = _lease_now] {
            const auto snapshot =
              std::atomic_load_explicit (lease.get (), std::memory_order_acquire);
            return !snapshot->outage || owner_lease_usable (*snapshot, (*clock) ());
        };
        access->observe = [lane = _lane, observers = _execution_discard_requests] (
                            authority_key_t key, authority_snapshot_t snapshot) {
            lane->try_post ([observers, key = std::move (key), snapshot = std::move (snapshot)] {
                for (const auto &request : *observers)
                    if (request.observe)
                        request.observe (key, snapshot);
            });
        };
        _store->configure_access (std::move (access));
    }

    ~location_runtime_t ()
    {
        stop ();
        _store->configure_access ({});
        // Borrowed lane handles can outlive the Runtime; callbacks using its fields cannot.
        _lane->close ();
    }

    location_runtime_t (const location_runtime_t &) = delete;
    location_runtime_t &operator= (const location_runtime_t &) = delete;

    std::function<std::chrono::steady_clock::time_point ()> execution_clock () const
    {
        return *_lease_now;
    }

    serial_execution_queue_t::execution_admission_t execution_policy () const
    {
        return [gate = _execution_gate, lease = _owner_lease,
                now = _lease_now] (const serial_execution_queue_t &queue) {
            const auto state = static_cast<execution_gate_state_t> (
              gate->load (std::memory_order_acquire) & execution_gate_state_mask);
            if (state == execution_gate_state_t::open)
                return serial_execution_queue_t::execution_status_t::allowed;
            if (state == execution_gate_state_t::admission_closed)
                return serial_execution_queue_t::execution_status_t::stopped;
            const auto snapshot =
              std::atomic_load_explicit (lease.get (), std::memory_order_acquire);
            for (const auto &retired : snapshot->discarded_outages)
                if (const auto episode = retired.lock ();
                    retention_eligible (episode, queue.confirmed_execution_ready_at ()))
                    return serial_execution_queue_t::execution_status_t::ownership_lost;
            if (!snapshot->outage)
                return owner_lease_usable (*snapshot, (*now) ())
                         ? serial_execution_queue_t::execution_status_t::allowed
                         : serial_execution_queue_t::execution_status_t::stopped;
            return execution_admission (snapshot->outage, queue.execution_ready_at (), *now);
        };
    }

    task_t<void> on_execution_discard (
      std::function<void (serial_execution_queue_t::execution_admission_t)> request,
      std::function<void (authority_key_t, authority_snapshot_t)> observe = {},
      std::function<task_t<void> (execution_recovery_t, std::chrono::steady_clock::time_point)>
        recover = {},
      std::function<void ()> resume = {})
    {
        return _lane->run_task ([this, request = std::move (request), observe = std::move (observe),
                                 recover = std::move (recover),
                                 resume = std::move (resume)] () mutable {
            _execution_discard_requests->push_back (
              {std::move (request), std::move (observe), std::move (recover), std::move (resume)});
        });
    }

    using descriptor_publisher_t = std::function<task_t<bool> (
      location_write_intent_t, std::stop_token, std::chrono::steady_clock::time_point)>;

    task_t<void> set_descriptor_publisher (descriptor_publisher_t publish,
                                           std::stop_token owner_cancellation = {})
    {
        if (!publish)
            return task_t<void> (result_t<void>::success ());
        return _lane->run_task (
          [this, publish = std::move (publish), owner_cancellation] () mutable {
              _descriptor_publisher = [publish = std::move (publish), owner_cancellation] (
                                        auto intent, auto cancellation, auto deadline) {
                  if (owner_cancellation.stop_requested ())
                      return task_t<bool> (result_t<bool>::success (false));
                  return publish (intent, cancellation, deadline);
              };
          });
    }

    task_t<bool>
    republish_descriptors (location_write_intent_t intent,
                           std::stop_token cancellation,
                           std::chrono::steady_clock::time_point deadline,
                           std::shared_ptr<task_completion_source_t<void>> submitted = {})
    {
        const auto heartbeat = std::atomic_load (&_heartbeat_state);
        if (heartbeat && heartbeat->stop.load (std::memory_order_acquire)) {
            if (submitted)
                submitted->complete (result_t<void>::success ());
            co_return false;
        }
        if (!submitted) {
            const auto episode = co_await _lane->run_task ([this] {
                const auto lease =
                  std::atomic_load_explicit (_owner_lease.get (), std::memory_order_acquire);
                return lease->outage && lease->token
                           && lease->token->lease_generation
                                != lease->outage->token.lease_generation
                         ? lease->outage
                         : std::shared_ptr<store_outage_t>{};
            });
            if (episode) {
                auto publication =
                  co_await _lane->run_task ([episode] { return episode->publication; });
                if (!publication)
                    co_return false;
                co_return co_await detail::project_task_terminal<bool> (
                  *publication, [] (const result_t<bool> &result) { return result; });
            }
        }
        // The lane owns registration; this copy retains only the current invocation.
        auto publish = co_await _lane->run_task ([this] { return _descriptor_publisher; });
        if (!publish) {
            if (submitted)
                submitted->complete (result_t<void>::success ());
            co_return true;
        }
        auto pending = publish (intent, cancellation, deadline);
        // Startup and Fixed RID waiting retain the publisher's existing observation.
        if (!submitted)
            co_return co_await pending;
        // The existing publication owner also drains the Host callback, including
        // continuations that still borrow Host state after physical Store completion.
        auto confirmation = descriptor_write_confirmed (
          [pending = std::move (pending)] () mutable {
              return detail::project_task_terminal<location_write_result_t> (
                pending, [] (const result_t<bool> &result) {
                    if (!result && result.error ()
                        && result.error_kind () == framework_error_kind_t::not_configured)
                        return result_t<location_write_result_t>::success (
                          {location_write_status_t::rejected_conflict, 0, {}});
                    if (!result)
                        return detail::propagate_failure<location_write_result_t> (
                          result, "Location descriptor callback failed");
                    if (!result.value ())
                        return result_t<location_write_result_t>::failure (
                          framework_error_kind_t::unavailable, "Location descriptor unconfirmed");
                    return result_t<location_write_result_t>::success (
                      {location_write_status_t::stored, 0, {}});
                });
          },
          *_lease_now, cancellation, deadline);
        auto submissions = co_await _lane->run_task ([this] {
            auto tasks = std::make_shared<std::vector<task_t<void>>> ();
            for (const auto &pending : _descriptor_publications)
                tasks->push_back (pending->submitted.task ());
            return tasks;
        });
        for (auto &submission : *submissions)
            co_await submission;
        if (submitted)
            submitted->complete (result_t<void>::success ());
        co_return co_await confirmation;
    }

    const std::string &owner_id () const noexcept { return _owner_id; }

    const location_options_t &options () const noexcept { return _options; }

    std::optional<location_owner_token_t> current_owner_token () const
    {
        const auto observation = current_owner_observation ();
        return observation ? std::make_optional (*observation) : std::nullopt;
    }

    std::shared_ptr<const location_owner_token_t> current_owner_observation () const
    {
        const auto lease =
          std::atomic_load_explicit (_owner_lease.get (), std::memory_order_acquire);
        if (!owner_lease_usable (*lease, (*_lease_now) ()))
            return {};
        return {lease, &*lease->token};
    }

    auto bound_execution_owner () const
    {
        return [lease = _owner_lease, now = _lease_now] (
                 const serial_execution_queue_t &queue) -> std::optional<location_owner_token_t> {
            return bound_execution_owner (
              *std::atomic_load_explicit (lease.get (), std::memory_order_acquire), queue, *now);
        };
    }

    std::optional<location_owner_token_t>
    bound_execution_owner (const serial_execution_queue_t &queue) const
    {
        return bound_execution_owner (
          *std::atomic_load_explicit (_owner_lease.get (), std::memory_order_acquire), queue,
          *_lease_now);
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

    const std::shared_ptr<framework::detail::monitoring_runtime_state_t> &
    monitoring () const noexcept
    {
        return _monitoring;
    }

    task_t<void> register_metric_scope (std::string kind, std::string name)
    {
        return _lane->run_task (
          [this, kind = std::move (kind), name = std::move (name)] () mutable {
              const auto scope = std::pair{std::move (kind), std::move (name)};
              if (std::find (_metric_scopes.begin (), _metric_scopes.end (), scope)
                  == _metric_scopes.end ())
                  _metric_scopes.push_back (scope);
          });
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
        return std::atomic_load_explicit (_owner_lease.get (), std::memory_order_acquire)->healthy;
    }

    bool owner_lease_usable () const noexcept
    {
        const auto lease =
          std::atomic_load_explicit (_owner_lease.get (), std::memory_order_acquire);
        return owner_lease_usable (*lease, (*_lease_now) ());
    }

    std::optional<std::chrono::system_clock::time_point> owner_lease_renewed_at () const
    {
        return std::atomic_load_explicit (_owner_lease.get (), std::memory_order_acquire)
          ->renewed_at;
    }

    std::optional<std::string> last_error () const
    {
        return _lane->run_checked ([this] { return _last_error; }).get ();
    }

    void start (zlink::routing_id_t node_rid, std::stop_token cancellation = {})
    {
        bool expected = false;
        if (!_started.compare_exchange_strong (expected, true)) {
            return;
        }
        static_cast<void> (node_rid);
        auto heartbeat = std::make_shared<heartbeat_owner_t> ();
        std::atomic_store_explicit (&_heartbeat_state, heartbeat, std::memory_order_release);
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
        stop_heartbeat (started);
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
        const bool started = _started.exchange (false);
        stop_heartbeat (started);
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
                    if (!completed) {
                        detail::task_result_access_t::read (pending).value ();
                        throw detail::make_boundary_exception (detail::boundary_error_t::timed_out,
                                                               "owner lease cleanup timed out");
                    }
                    completed->value ();
                };
                await_cleanup ([&] { return _store->remove_all_by_owner (*token, deadline_at); });
                await_cleanup ([&] { return _store->release_owner_lease (*token, deadline_at); });
                _lane->run_checked ([this] { clear_owner_lease_on_lane (); }).get ();
            }
            return true;
        }
        catch (const std::exception &error) {
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

    void record_runtime_failure (std::string message) const
    {
        record_failure (std::move (message));
    }

    // Location runtime §5: provider failures leave publication unknown. Only a
    // confirmed conflicting claim is a startup error. The repository owns §10 reads.
    template <typename Start>
    task_t<bool> descriptor_write_confirmed (
      Start start,
      std::function<std::chrono::steady_clock::time_point ()> now = std::chrono::steady_clock::now,
      std::stop_token cancellation = {},
      std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt,
      std::stop_token owner_cancellation = {}) const
    {
        auto lane = _lane;
        auto operation = std::make_shared<descriptor_publication_t> (
          _options.owner_lease_renew_timeout, std::move (now), deadline);
        const auto cancel = [weak = std::weak_ptr{operation}] {
            if (auto active = weak.lock ())
                active->observe (result_t<location_write_result_t>::failure (
                  framework_error_kind_t::shutting_down,
                  "Location descriptor publication stopped"));
        };
        std::stop_callback stopped (cancellation, cancel);
        std::stop_callback owner_stopped (owner_cancellation, cancel);
        try {
            co_await lane->run_task (
              [this, operation] { register_repository_operation (operation); });
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
                auto pending = start ();
                if constexpr (std::is_same_v<decltype (pending), task_t<location_write_status_t>>) {
                    // Removal callers already ignore stale/no-row status. They
                    // need the same bounded observation and physical-I/O drain.
                    operation->pending.emplace (continue_task<location_write_result_t> (
                      std::move (pending), [] (location_write_status_t) {
                          return task_t<location_write_result_t> (
                            result_t<location_write_result_t>::success (
                              {location_write_status_t::stored, 0, {}}));
                      }));
                } else {
                    operation->pending.emplace (std::move (pending));
                }
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
        operation->submitted.complete (result_t<void>::success ());
        const auto result = co_await await_result (operation->observation.task ());
        if (!result) {
            if (result.error () == nullptr)
                co_return detail::propagate_failure<bool> (
                  result, "Location descriptor publication failed");

            if (result.error_kind () == framework_error_kind_t::shutting_down)
                co_return false;
            co_await lane->run_task (
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
        return std::atomic_load_explicit (_owner_lease.get (), std::memory_order_acquire)
          ->admission_deadline;
    }

    auto descriptor_publication_observation (
      std::optional<std::chrono::steady_clock::time_point> observed) const
    {
        const auto current = owner_lease_observation ();
        return std::pair{current, current != observed ? location_write_intent_t::new_claim
                                                      : location_write_intent_t::renew};
    }

    auto peer_reconcile_requester () const
    {
        return
          [owner = std::weak_ptr{std::atomic_load_explicit (
             &_heartbeat_state, std::memory_order_acquire)}] (const std::vector<std::uint8_t> &) {
              if (auto heartbeat = owner.lock ()) {
                  heartbeat->notify (true);
              }
          };
    }

    void wake_owner_lease_observers () const
    {
        if (auto heartbeat =
              std::atomic_load_explicit (&_heartbeat_state, std::memory_order_acquire)) {
            heartbeat->notify ();
        }
    }

    task_t<void> wait_for_owner_lease_change (
      std::optional<std::chrono::steady_clock::time_point> observed) const
    {
        auto heartbeat = std::atomic_load_explicit (&_heartbeat_state, std::memory_order_acquire);
        if (!heartbeat)
            return task_completion_source_t<void>{}.task ();
        std::lock_guard lock (heartbeat->gate);
        if (!heartbeat->stop.load (std::memory_order_acquire)
            && (owner_lease_observation () != observed || heartbeat->pending_peer_reconcile)) {
            heartbeat->pending_peer_reconcile = false;
            return task_t<void> (result_t<void>::success ());
        }
        // The preceding polling observation has ended. Retire its notification
        // registration instead of retaining timed-out observers until renewal.
        heartbeat->discovery_changed = task_completion_source_t<void>{};
        return heartbeat->discovery_changed.task ();
    }


  private:
    void record_lease_metric_on_lane (bool failure, double seconds = 0) const
    {
        runtime_metrics_t metrics (_monitoring);
        if (!metrics.enabled ())
            return;
        for (const auto &[kind, name] : _metric_scopes) {
            const std::map<std::string, std::string> tags{{"scope_kind", kind},
                                                          {"scope_name", name}};
            if (failure)
                metrics.counter ("zlink.location.owner_lease.renew.failures", "{failure}", 1, tags);
            else
                metrics.histogram ("zlink.location.owner_lease.renew.lateness", "s", seconds, tags);
        }
    }

    // Location runtime §5.1: this predicate only supplies the retention exception;
    // all existing admission and lifecycle rules still run in the execution gate.
    static bool
    retention_eligible (const std::shared_ptr<store_outage_t> &episode,
                        std::optional<std::chrono::steady_clock::time_point> ready_at) noexcept
    {
        return episode && ready_at && *ready_at < episode->observed_at;
    }

    static serial_execution_queue_t::execution_status_t execution_admission (
      const std::shared_ptr<store_outage_t> &episode,
      std::optional<std::chrono::steady_clock::time_point> ready_at,
      const std::function<std::chrono::steady_clock::time_point ()> &now) noexcept
    {
        if (!retention_eligible (episode, ready_at))
            return serial_execution_queue_t::execution_status_t::allowed;
        using status_t = serial_execution_queue_t::execution_status_t;
        if (episode->discarded.load (std::memory_order_acquire))
            return status_t::ownership_lost;
        if (episode->owner->stop.load (std::memory_order_acquire))
            return status_t::stopped;
        return now () < episode->cutoff ? status_t::allowed : status_t::ownership_lost;
    }

    static bool owner_lease_usable (const owner_lease_snapshot_t &lease,
                                    std::chrono::steady_clock::time_point now) noexcept
    {
        return lease.token.has_value () && lease.admission_deadline.has_value ()
               && now < *lease.admission_deadline;
    }

    static std::optional<location_owner_token_t>
    bound_execution_owner (const owner_lease_snapshot_t &lease,
                           const serial_execution_queue_t &queue,
                           const std::function<std::chrono::steady_clock::time_point ()> &now)
    {
        if (owner_lease_usable (lease, now ()))
            return lease.token;
        if (retention_eligible (lease.outage, queue.execution_ready_at ())
            && queue.execution_status () == serial_execution_queue_t::execution_status_t::allowed)
            return lease.outage->token;
        return std::nullopt;
    }

    static void schedule_owner_deadlines (
      const std::shared_ptr<owner_deadlines_t> &deadlines,
      const std::shared_ptr<std::atomic<std::uint64_t>> &gate,
      std::uint64_t generation,
      std::optional<std::chrono::steady_clock::time_point> admission,
      const std::shared_ptr<store_outage_t> &episode,
      const std::shared_ptr<std::function<std::chrono::steady_clock::time_point ()>> &now)
    {
        const auto at = (*now) ();
        if (admission && at >= *admission) {
            auto expected = generation | static_cast<std::uint64_t> (execution_gate_state_t::open);
            gate->compare_exchange_strong (
              expected,
              generation | static_cast<std::uint64_t> (execution_gate_state_t::admission_closed),
              std::memory_order_acq_rel, std::memory_order_acquire);
        }
        if (episode)
            episode->latch (at);
        std::optional<std::chrono::steady_clock::time_point> next;
        if (admission && at < *admission)
            next = admission;
        if (episode && !episode->discarded.load (std::memory_order_acquire)
            && !episode->owner->stop.load (std::memory_order_acquire))
            next = next ? std::min (*next, episode->cutoff) : episode->cutoff;
        if (!next)
            return;
        auto previous =
          std::atomic_load_explicit (&deadlines->reservation, std::memory_order_acquire);
        if (!previous)
            return;
        auto pending = std::make_shared<detail::deadline_state_t> ();
        if (!std::atomic_compare_exchange_strong_explicit (&deadlines->reservation, &previous,
                                                           pending, std::memory_order_acq_rel,
                                                           std::memory_order_acquire))
            return;
        detail::deadline_scheduler_t::instance ().schedule (
          *next, pending,
          [weak = std::weak_ptr{deadlines}, gate, generation, admission, episode, now] {
              if (auto active = weak.lock ())
                  schedule_owner_deadlines (active, gate, generation, admission, episode, now);
          });
    }

    template <typename Work>
    static void update_owner_lease_on_lane (
      const std::shared_ptr<std::shared_ptr<const owner_lease_snapshot_t>> &owner_lease,
      const std::shared_ptr<std::atomic<std::uint64_t>> &gate,
      const std::shared_ptr<std::function<std::chrono::steady_clock::time_point ()>> &now,
      Work work,
      const std::shared_ptr<std::vector<execution_observer_t>> &observers)
    {
        const auto previous =
          std::atomic_load_explicit (owner_lease.get (), std::memory_order_acquire);
        auto lease = *previous;
        lease.deadlines = std::make_shared<owner_deadlines_t> ();
        work (lease);
        std::erase_if (lease.discarded_outages,
                       [] (const auto &episode) { return episode.expired (); });
        if (previous->outage && previous->outage != lease.outage
            && previous->outage->discarded.load (std::memory_order_acquire))
            lease.discarded_outages.emplace_back (previous->outage);
        previous->deadlines->cancel ();
        const auto at = (*now) ();
        const auto state = lease.outage || !lease.discarded_outages.empty ()
                             ? execution_gate_state_t::episode
                           : !lease.admission_deadline || at >= *lease.admission_deadline
                             ? execution_gate_state_t::admission_closed
                             : execution_gate_state_t::open;
        const auto generation = ((gate->load (std::memory_order_relaxed) >> 2) + 1) << 2;
        const auto snapshot = std::make_shared<const owner_lease_snapshot_t> (std::move (lease));
        std::atomic_store_explicit (owner_lease.get (), snapshot, std::memory_order_release);
        gate->store (generation | static_cast<std::uint64_t> (state), std::memory_order_release);
        schedule_owner_deadlines (snapshot->deadlines, gate, generation,
                                  snapshot->admission_deadline, snapshot->outage, now);
        if ((previous->outage || !owner_lease_usable (*previous, at)) && !snapshot->outage
            && owner_lease_usable (*snapshot, at))
            for (const auto &observer : *observers)
                if (observer.resume)
                    observer.resume ();
    }

    template <typename Work> void update_owner_lease_on_lane (Work work) const
    {
        update_owner_lease_on_lane (_owner_lease, _execution_gate, _lease_now, std::move (work),
                                    _execution_discard_requests);
    }

    void clear_owner_lease_on_lane () const
    {
        update_owner_lease_on_lane ([] (auto &lease) {
            lease.token.reset ();
            lease.admission_deadline.reset ();
        });
    }

    void observe_store_outage_on_lane (std::chrono::steady_clock::time_point observed_at)
    {
        update_owner_lease_on_lane ([&] (auto &lease) {
            const auto owner =
              std::atomic_load_explicit (&_heartbeat_state, std::memory_order_acquire);
            if (owner->stop.load (std::memory_order_acquire) || lease.outage
                || !lease.last_successful_lease)
                return;
            auto cutoff = lease.last_successful_lease->admission_deadline;
            if (_options.store_outage_retention) {
                cutoff = lease.last_successful_lease->started_at;
                // Saturate each addition before converting milliseconds to clock ticks.
                for (const auto duration :
                     {_options.owner_lease_ttl, _options.owner_lease_fencing_margin,
                      _options.owner_lease_fencing_margin}) {
                    const auto available = std::chrono::duration_cast<std::chrono::milliseconds> (
                      std::chrono::steady_clock::time_point::max () - cutoff);
                    cutoff = duration > available ? std::chrono::steady_clock::time_point::max ()
                                                  : cutoff + duration;
                }
            }
            auto episode = std::make_shared<store_outage_t> (
              observed_at, cutoff, lease.last_successful_lease->token, owner);
            for (const auto &request : *_execution_discard_requests)
                episode->discard_requests.push_back (request.discard);
            lease.outage = episode;
            episode->latch (episode->observed_at);
        });
    }

    bool observe_successful_lease_on_lane (owner_lease_snapshot_t &lease,
                                           const location_owner_token_t &token,
                                           std::chrono::steady_clock::time_point started_at,
                                           std::chrono::steady_clock::time_point admission_deadline,
                                           bool renewal,
                                           const std::shared_ptr<store_outage_t> &requested_outage)
    {
        const auto episode = lease.outage;
        const bool discarded = episode && episode->latch ((*_lease_now) ());
        if (episode) {
            if (episode->owner->stop.load (std::memory_order_acquire) || episode != requested_outage
                || started_at < episode->observed_at || !lease.token
                || lease.token->owner_id != token.owner_id
                || (renewal && lease.token->lease_generation != token.lease_generation)
                || episode->token.owner_id != token.owner_id
                || (!renewal && episode->token.lease_generation == token.lease_generation))
                return false;
        }
        if (!lease.last_successful_lease || started_at >= lease.last_successful_lease->started_at)
            lease.last_successful_lease = successful_lease_t{token, started_at, admission_deadline};
        if (discarded)
            lease.outage.reset ();
        return episode && !discarded;
    }

    static task_t<void>
    heartbeat_communication_failure (std::shared_ptr<heartbeat_attempt_t> attempt)
    {
        auto *runtime = attempt->runtime;
        const auto observed_at = (*runtime->_lease_now) ();
        const auto heartbeat = attempt->heartbeat.lock ();
        if (attempt->cancellation.stop_requested ()
            || (heartbeat && heartbeat->stop.load (std::memory_order_acquire)
                && observed_at < attempt->deadline_at))
            co_return;
        co_await lease_lane (
          attempt, [runtime, observed_at] { runtime->observe_store_outage_on_lane (observed_at); });
    }

    std::optional<location_owner_token_t> current_owner_token_unchecked () const
    {
        return std::atomic_load_explicit (_owner_lease.get (), std::memory_order_acquire)->token;
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
            completed = detail::task_result_access_t::read (pending);
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
        return attempt->runtime->_lane->run_task (std::move (work));
    }

    template <typename T>
    static task_t<std::optional<result_t<T>>>
    await_heartbeat_store (task_t<T> pending, std::weak_ptr<heartbeat_attempt_t> weak_attempt)
    {
        auto attempt = weak_attempt.lock ();
        auto heartbeat = attempt ? attempt->heartbeat.lock () : nullptr;
        if (!attempt || !heartbeat || heartbeat->stop.load (std::memory_order_acquire)
            || remaining_until (attempt->deadline_at, (*attempt->runtime->_lease_now) ())
                 <= std::chrono::milliseconds::zero ())
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
                    remaining_until (active->deadline_at, (*active->runtime->_lease_now) ())
                        > std::chrono::milliseconds::zero ()
                      ? std::optional<result_t<T>>{result}
                      : std::nullopt));
              }
          });
        attempt.reset ();
        auto result = co_await std::move (ready);
        if (auto active = weak_attempt.lock ()) {
            if (auto heartbeat = active->heartbeat.lock ()) {
                std::lock_guard lock (heartbeat->gate);
                active->expire = {};
            }
        }
        co_return result;
    }

    void register_repository_operation (std::shared_ptr<repository_operation_t> operation) const
    {
        std::erase_if (_descriptor_publications, [] (const auto &pending) {
            return pending->drained.task ().await_ready ();
        });
        _descriptor_publications.push_back (std::move (operation));
    }

    template <typename T, typename Start>
    static task_t<T> retain_repository_operation (location_runtime_t *runtime,
                                                  std::shared_ptr<state_lane_t> lane,
                                                  std::shared_ptr<store_outage_t> episode,
                                                  Start start)
    {
        // Only physical submission enters the drain lane; logical recovery keeps
        // its caller's continuation context and the episode's original terminal.
        auto completion = std::make_shared<task_completion_source_t<T>> ();
        auto submitted =
          lane->run_task ([runtime, episode, start = std::move (start), completion] () mutable {
              if (episode->owner->stop.load (std::memory_order_acquire)) {
                  completion->complete (result_t<T>::failure (framework_error_kind_t::shutting_down,
                                                              "Location recovery stopped"));
                  return;
              }
              auto operation = std::make_shared<repository_operation_t> ();
              auto retained = std::make_shared<task_t<T>> (start ());
              runtime->register_repository_operation (operation);
              detail::observe_task_terminal (
                *retained, [operation, retained, completion] (const result_t<T> &result) {
                    completion->complete (result);
                    operation->drained.complete (result_t<void>::success ());
                });
              operation->submitted.complete (result_t<void>::success ());
          });
        detail::observe_task_terminal (submitted, [completion] (const result_t<void> &result) {
            if (!result)
                completion->complete (
                  detail::propagate_failure<T> (result, "Location recovery submission failed"));
        });
        return completion->task ();
    }

    static task_t<authority_read_result_t> confirm_preservation (
      task_t<authority_compare_exchange_result_t> pending,
      std::shared_ptr<store_outage_t> episode,
      std::shared_ptr<std::function<std::chrono::steady_clock::time_point ()>> now,
      std::function<task_t<void> (authority_snapshot_t)> apply,
      std::chrono::steady_clock::time_point deadline)
    {
        const auto result =
          co_await observe_until (std::move (pending), deadline, {}, [now] { return (*now) (); });
        if (!result)
            co_return detail::propagate_failure<authority_read_result_t> (
              result, "Location Preserve CAS failed");
        if (const auto *stored = std::get_if<authority_stored_t> (&result.value ())) {
            if (!episode->owner->stop.load (std::memory_order_acquire)
                && !episode->latch ((*now) ()))
                co_await apply (stored->snapshot);
            co_return authority_read_result_t{stored->snapshot};
        }
        co_return authority_read_result_t{authority_missing_t{}};
    }

    static task_t<serial_execution_queue_t::execution_operation_t>
    recover_execution (location_runtime_t *runtime,
                       std::shared_ptr<state_lane_t> lane,
                       std::shared_ptr<store_outage_t> episode,
                       std::shared_ptr<std::function<std::chrono::steady_clock::time_point ()>> now,
                       authority_key_t key,
                       authority_snapshot_t fence,
                       std::function<task_t<void> (authority_snapshot_t)> apply,
                       location_owner_token_t token,
                       std::shared_ptr<task_t<bool>> publication,
                       std::chrono::milliseconds timeout)
    {
        using status_t = serial_execution_queue_t::execution_status_t;
        using operation_t = serial_execution_queue_t::execution_operation_t;
        auto completion = std::make_shared<task_completion_source_t<operation_t>> ();
        auto terminal = completion->task ();
        auto episode_terminal = episode->terminal.task ();
        detail::observe_task_terminal (
          episode_terminal,
          [completion] (const result_t<operation_t> &result) { completion->complete (result); });
        const auto started = (*now) ();
        if (episode->owner->stop.load (std::memory_order_acquire))
            return task_t<operation_t> (
              result_t<operation_t>::success ({status_t::stopped, started}));
        if (episode->latch (started))
            return task_t<operation_t> (
              result_t<operation_t>::success ({status_t::ownership_lost, started}));
        const bool preserve = token.lease_generation != fence.owner.lease_generation;
        // §5.1 D19: equality is admissible; only the original episode owns T.
        if (preserve && started + timeout > episode->cutoff)
            return terminal;
        auto read = std::make_shared<task_t<authority_read_result_t>> (
          preserve ? confirm_preservation (
                       retain_repository_operation<authority_compare_exchange_result_t> (
                         runtime, lane, episode,
                         [runtime, key, fence, token, started, timeout] {
                             auto mutation = authority_put_t::preserve (fence, token);
                             mutation.operation_deadline = started + timeout;
                             return runtime->_store->compare_exchange_authority_core (
                               key, fence.store_version, std::move (mutation));
                         }),
                       episode, now, std::move (apply), started + timeout)
                   : retain_repository_operation<authority_read_result_t> (
                       runtime, lane, episode, [runtime, key = std::move (key)] () mutable {
                           return runtime->_store->read_authority_core (std::move (key));
                       }));
        const auto confirm = [weak = std::weak_ptr{completion}, episode = std::weak_ptr{episode},
                              now, read = std::weak_ptr{read},
                              publication = std::weak_ptr{publication}, fence = std::move (fence),
                              token, preserve] {
            auto active = weak.lock ();
            const auto current_episode = episode.lock ();
            const auto current_read = read.lock ();
            const auto current_publication = publication.lock ();
            if (!active || !current_episode || !current_read || active->task ().await_ready ())
                return;
            const auto observed_at = (*now) ();
            if (current_episode->owner->stop.load (std::memory_order_acquire)) {
                active->complete (
                  result_t<operation_t>::success ({status_t::stopped, observed_at}));
                return;
            }
            if (current_episode->latch (observed_at)) {
                active->complete (
                  result_t<operation_t>::success ({status_t::ownership_lost, observed_at}));
                return;
            }
            std::optional<result_t<bool>> published;
            if (current_publication && current_publication->await_ready ()) {
                published = detail::task_result_access_t::read (*current_publication);
                if (!*published && published->error ()
                    && published->error_kind () == framework_error_kind_t::not_configured) {
                    active->complete (
                      result_t<operation_t>::success ({status_t::ownership_lost, observed_at}));
                    return;
                }
            }
            if (!current_read->await_ready ())
                return;
            const auto result = detail::task_result_access_t::read (*current_read);
            if (!result) {
                active->complete (detail::propagate_failure<operation_t> (
                  result, "Location recovery authority confirmation failed"));
                return;
            }
            const auto *current = std::get_if<authority_snapshot_t> (&result.value ());
            const bool matches =
              current && current->same_incarnation (fence)
              && (preserve || current->store_version == fence.store_version)
              && current->allocation.state == placement_allocation_state_t::active
              && current->owner.owner_id == token.owner_id
              && current->owner.lease_generation == token.lease_generation
              && current->allocation.target.node_lifecycle_generation
                   == fence.allocation.target.node_lifecycle_generation;
            if (!matches) {
                active->complete (
                  result_t<operation_t>::success ({status_t::ownership_lost, observed_at}));
                return;
            }
            if (current_publication && !published)
                return;
            if (published && !*published) {
                active->complete (detail::propagate_failure<operation_t> (
                  *published, "Location descriptor publication failed"));
                return;
            }
            if (published && !published->value ()) {
                active->complete (
                  result_t<operation_t>::failure (framework_error_kind_t::unavailable,
                                                  "Location descriptor publication unconfirmed"));
                return;
            }
            active->complete (result_t<operation_t>::success ({status_t::allowed, observed_at}));
        };
        detail::observe_task_terminal (*read, [confirm] (const auto &) { confirm (); });
        if (publication)
            detail::observe_task_terminal (*publication, [confirm] (const auto &) { confirm (); });
        // The logical terminal releases these observations even when opaque I/O
        // remains retained by the independent stop drain.
        detail::observe_task_terminal (terminal, [read, publication] (const auto &) {});
        return terminal;
    }

    static task_t<void>
    join_execution_recovery (std::shared_ptr<std::vector<task_t<void>>> recoveries,
                             std::shared_ptr<task_t<void>> previous)
    {
        // Earlier reads must terminate too, including reads whose local object retired.
        // A previous read error is retried by the current pass, not carried forward.
        if (previous)
            co_await detail::project_task_terminal<void> (
              *previous, [] (const result_t<void> &) { return result_t<void>::success (); });
        result_t<void> terminal = result_t<void>::success ();
        for (auto &recovery : *recoveries) {
            const auto result = co_await await_result (std::move (recovery));
            if (!result)
                terminal = detail::propagate_failure<void> (result, "Location recovery failed");
        }
        terminal.value ();
    }

    static task_t<void> finish_execution_recovery (
      std::shared_ptr<task_t<void>> recovery,
      std::shared_ptr<state_lane_t> lane,
      std::shared_ptr<std::shared_ptr<const owner_lease_snapshot_t>> owner_lease,
      std::shared_ptr<std::atomic<std::uint64_t>> gate,
      std::shared_ptr<std::vector<execution_observer_t>> observers,
      std::shared_ptr<store_outage_t> episode,
      location_owner_token_t token,
      std::shared_ptr<std::function<std::chrono::steady_clock::time_point ()>> now)
    {
        const auto result = co_await await_result (detail::project_task_terminal<void> (
          *recovery, [] (const result_t<void> &result) { return result; }));
        // An unconfirmed read retains the original T until a later pass succeeds or T fires.
        if (!result)
            co_await episode->terminal.task ();
        auto completed = std::make_shared<task_completion_source_t<void>> ();
        auto posted = lane->try_post (
          [owner_lease, gate, observers, episode, token, now, recovery, completed] {
              if (!episode->owner->stop.load (std::memory_order_acquire)
                  && episode->recovery == recovery) {
                  const auto at = (*now) ();
                  episode->latch (at);
                  update_owner_lease_on_lane (
                    owner_lease, gate, now,
                    [&] (auto &lease) {
                        if (lease.outage != episode || !lease.token
                            || lease.token->owner_id != token.owner_id
                            || lease.token->lease_generation != token.lease_generation)
                            return;
                        lease.outage.reset ();
                        episode->terminal.complete (
                          result_t<serial_execution_queue_t::execution_operation_t>::success (
                            {serial_execution_queue_t::execution_status_t::allowed, at}));
                    },
                    observers);
              }
              completed->complete (result_t<void>::success ());
          },
          [completed] (std::exception_ptr error) {
              completed->complete (detail::result_access_t::failure<void> (std::move (error)));
          });
        if (!posted)
            throw framework_exception_t (framework_error_kind_t::shutting_down,
                                         "Location recovery lane stopped");
        co_await completed->task ();
    }

    static task_t<void> heartbeat_accept (std::shared_ptr<heartbeat_attempt_t> attempt,
                                          location_owner_token_t token,
                                          std::chrono::system_clock::time_point expires_at,
                                          std::chrono::system_clock::time_point store_now,
                                          std::chrono::steady_clock::time_point started_at,
                                          bool renewal = false)
    {
        auto *runtime = attempt->runtime;
        const auto recovering =
          co_await lease_lane (attempt, [runtime, token, expires_at, store_now, started_at, renewal,
                                         requested_outage = attempt->requested_outage] {
              const auto admission_lifetime =
                expires_at > store_now + runtime->_options.owner_lease_fencing_margin
                  ? expires_at - store_now - runtime->_options.owner_lease_fencing_margin
                  : std::chrono::system_clock::duration::zero ();
              const auto admission_deadline =
                started_at
                + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                  admission_lifetime);
              bool recovering = false;
              runtime->update_owner_lease_on_lane ([&] (auto &lease) {
                  recovering = runtime->observe_successful_lease_on_lane (
                    lease, token, started_at, admission_deadline, renewal, requested_outage);
                  lease.token = token;
                  lease.healthy = true;
                  lease.renewed_at = store_now;
                  lease.admission_deadline = admission_deadline;
              });
              runtime->_last_error.reset ();
              return recovering;
          });
        if (recovering) {
            const auto episode = attempt->requested_outage;
            std::shared_ptr<task_t<bool>> publication;
            if (episode->token.lease_generation != token.lease_generation) {
                publication =
                  co_await lease_lane (attempt, [episode] { return episode->publication; });
                if (!publication || (publication->await_ready () && [&] {
                        const auto result = detail::task_result_access_t::read (*publication);
                        return !result || !result.value ();
                    }())) {
                    auto submitted = std::make_shared<task_completion_source_t<void>> ();
                    publication = std::make_shared<task_t<bool>> (runtime->republish_descriptors (
                      location_write_intent_t::renew, attempt->cancellation, attempt->deadline_at,
                      submitted));
                    detail::observe_task_terminal (*publication, [submitted] (const auto &) {
                        submitted->complete (result_t<void>::success ());
                    });
                    co_await submitted->task ();
                    co_await lease_lane (
                      attempt, [episode, publication] { episode->publication = publication; });
                }
            }
            auto recovery = co_await lease_lane (attempt, [runtime, episode, token, publication] {
                auto recoveries = std::make_shared<std::vector<task_t<void>>> ();
                for (const auto &observer : *runtime->_execution_discard_requests)
                    if (observer.recover)
                        recoveries->push_back (observer.recover (
                          [runtime, lane = runtime->_lane, episode, now = runtime->_lease_now,
                           token, publication,
                           timeout = runtime->_options.owner_lease_renew_timeout] (
                            authority_key_t key, authority_snapshot_t fence,
                            std::function<task_t<void> (authority_snapshot_t)> apply) {
                              return recover_execution (
                                runtime, lane, episode, now, std::move (key), std::move (fence),
                                std::move (apply), token, publication, timeout);
                          },
                          episode->observed_at));
                    else if (publication)
                        recoveries->push_back (task_t<void> (result_t<void>::failure (
                          framework_error_kind_t::unavailable,
                          "Location execution owner has no recovery operation")));
                auto recovery = std::make_shared<task_t<void>> (
                  join_execution_recovery (std::move (recoveries), episode->recovery));
                episode->recovery = recovery;
                return recovery;
            });
            auto finishing = finish_execution_recovery (
              recovery, runtime->_lane, runtime->_owner_lease, runtime->_execution_gate,
              runtime->_execution_discard_requests, episode, token, runtime->_lease_now);
            if (recovery->await_ready () && detail::task_result_access_t::read (*recovery))
                co_await finishing;
        }
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

    static task_t<std::optional<owner_lease_read_result_t>>
    heartbeat_confirm (std::shared_ptr<heartbeat_attempt_t> attempt,
                       std::string &failure,
                       std::optional<location_owner_token_t> expected_token = std::nullopt)
    {
        auto *runtime = attempt->runtime;
        bool failed = false;
        try {
            if (attempt->cancellation.stop_requested ())
                co_return std::nullopt;
            if (auto heartbeat = attempt->heartbeat.lock ();
                heartbeat && heartbeat->stop.load (std::memory_order_acquire)
                || remaining_until (attempt->deadline_at, (*attempt->runtime->_lease_now) ())
                     <= std::chrono::milliseconds::zero ())
                co_return std::nullopt;
            auto response = co_await await_heartbeat_store (
              runtime->_store->read_owner_lease (runtime->_owner_id), attempt);
            if (!response) {
                co_await heartbeat_communication_failure (attempt);
                co_return std::nullopt;
            }
            if (!response->has_value ()) {
                failure = response->error () ? response->error ()->what ()
                                             : "owner lease confirmation read failed";

                failed = true;
            } else {
                auto read = response->value ();
                const bool ownership_lost =
                  co_await lease_lane (attempt, [runtime, expected_token, &read] {
                      const auto lease = std::atomic_load_explicit (runtime->_owner_lease.get (),
                                                                    std::memory_order_acquire);
                      const auto *found = std::get_if<owner_lease_found_t> (&read);
                      if (!expected_token || !lease->outage || !lease->token
                          || lease->token->owner_id != expected_token->owner_id
                          || lease->token->lease_generation != expected_token->lease_generation
                          || !found
                          || (found->token.owner_id == expected_token->owner_id
                              && found->token.lease_generation == expected_token->lease_generation))
                          return false;
                      // An exact different token is ownership loss, even if expired.
                      lease->outage->latch (lease->outage->cutoff);
                      runtime->record_failure_on_lane ("owner lease changed during recovery");
                      return true;
                  });
                if (!ownership_lost)
                    co_return read;
            }
        }
        catch (const std::exception &error) {
            failure = error.what ();
            failed = true;
        }
        if (failed) {
            co_await heartbeat_communication_failure (attempt);
            co_await heartbeat_failure (attempt, failure);
        }
        co_return std::nullopt;
    }

    static task_t<lease_renew_outcome_t>
    heartbeat_renew_once_async (std::weak_ptr<heartbeat_attempt_t> weak_attempt,
                                double lateness_seconds = 0)
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
            const auto owner_token =
              co_await lease_lane (attempt, [runtime, attempt, metrics_enabled, lateness_seconds] {
                  if (metrics_enabled && lateness_seconds > 0)
                      runtime->record_lease_metric_on_lane (false, lateness_seconds);
                  attempt->requested_outage =
                    std::atomic_load_explicit (runtime->_owner_lease.get (),
                                               std::memory_order_acquire)
                      ->outage;
                  return runtime->current_owner_token_unchecked ();
              });
            if (auto heartbeat = attempt->heartbeat.lock ();
                heartbeat && heartbeat->stop.load (std::memory_order_acquire))
                co_return outcome;
            if (owner_token) {
                std::string failure = "owner lease renewal timed out";
                std::optional<owner_lease_renew_result_t> result;
                try {
                    if (remaining_until (attempt->deadline_at, (*attempt->runtime->_lease_now) ())
                        <= std::chrono::milliseconds::zero ()) {
                        co_await heartbeat_communication_failure (attempt);
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
                                                   renewed->store_now, started_at, true);
                        outcome.result = *renewed;
                        co_return outcome;
                    }
                    const bool recovering = co_await lease_lane (attempt, [runtime, attempt] {
                        attempt->requested_outage =
                          std::atomic_load_explicit (runtime->_owner_lease.get (),
                                                     std::memory_order_acquire)
                            ->outage;
                        if (!attempt->requested_outage)
                            runtime->clear_owner_lease_on_lane ();
                        return static_cast<bool> (attempt->requested_outage);
                    });
                    if (recovering) {
                        auto exact = co_await heartbeat_confirm (attempt, failure, owner_token);
                        const bool expired = co_await lease_lane (attempt, [runtime, attempt,
                                                                            owner_token, &exact] {
                            const auto lease = std::atomic_load_explicit (
                              runtime->_owner_lease.get (), std::memory_order_acquire);
                            if (!exact || lease->outage != attempt->requested_outage
                                || !lease->token || lease->token->owner_id != owner_token->owner_id
                                || lease->token->lease_generation != owner_token->lease_generation
                                || !lease->admission_deadline)
                                return false;
                            // Admission is lifetime minus margin. A new claim needs
                            // lifetime plus margin, rechecked after the exact read.
                            auto reclaim_after = *lease->admission_deadline;
                            for (int margin = 0; margin != 2; ++margin) {
                                const auto available =
                                  std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::steady_clock::time_point::max () - reclaim_after);
                                reclaim_after =
                                  runtime->_options.owner_lease_fencing_margin > available
                                    ? std::chrono::steady_clock::time_point::max ()
                                    : reclaim_after + runtime->_options.owner_lease_fencing_margin;
                            }
                            const auto *found = std::get_if<owner_lease_found_t> (&*exact);
                            return (*runtime->_lease_now) () >= reclaim_after
                                   && (!found || found->lease_expires_at <= found->store_now);
                        });
                        if (!expired)
                            co_return outcome;
                    }
                } else {
                    co_await heartbeat_communication_failure (attempt);
                    auto exact = co_await heartbeat_confirm (attempt, failure, owner_token);
                    const auto *confirmed =
                      exact ? std::get_if<owner_lease_found_t> (&*exact) : nullptr;
                    if (confirmed && confirmed->token.owner_id == owner_token->owner_id
                        && confirmed->token.lease_generation == owner_token->lease_generation
                        && confirmed->lease_expires_at > confirmed->store_now) {
                        co_await heartbeat_accept (attempt, confirmed->token,
                                                   confirmed->lease_expires_at,
                                                   confirmed->store_now, started_at);
                        outcome.result =
                          owner_lease_renewed_t{confirmed->lease_expires_at, confirmed->store_now};
                        co_return outcome;
                    }
                    if (metrics_enabled)
                        co_await lease_lane (attempt, [runtime] {
                            runtime->record_lease_metric_on_lane (true);
                            return true;
                        });
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
                if (remaining_until (attempt->deadline_at, (*attempt->runtime->_lease_now) ())
                    <= std::chrono::milliseconds::zero ()) {
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
                    auto exact = co_await heartbeat_confirm (attempt, failure);
                    const auto *confirmed =
                      exact ? std::get_if<owner_lease_found_t> (&*exact) : nullptr;
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
                    co_await lease_lane (attempt, [runtime] {
                        runtime->record_lease_metric_on_lane (true);
                        return true;
                    });
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
                auto exact = co_await heartbeat_confirm (attempt, failure, owner_token);
                const auto *confirmed =
                  exact ? std::get_if<owner_lease_found_t> (&*exact) : nullptr;
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
                    auto release_task = _store->release_owner_lease (*token, deadline_at);
                    const auto released = release_task.result_for (remaining);
                    if (!released) {
                        record_failure ("owner lease cancellation release timed out");
                    } else if (!released->has_value ()) {
                        record_failure (released->error ()
                                          ? released->error ()->what ()
                                          : "owner lease cancellation release failed");
                    }
                }
            }
        }
        catch (const std::exception &error) {
            record_failure (error.what ());
        }
        _lane->run_checked ([this] { clear_owner_lease_on_lane (); }).get ();
    }

    static std::string make_owner_id ()
    {
        static std::atomic_uint64_t counter{1};
        const auto now = std::chrono::steady_clock::now ().time_since_epoch ().count ();
        const auto random = std::random_device{}();
        return "cpp-location-owner-" + std::to_string (now) + "-" + std::to_string (random) + "-"
               + std::to_string (counter.fetch_add (1));
    }

    void stop_heartbeat (bool started) noexcept
    {
        auto heartbeat = std::atomic_load_explicit (&_heartbeat_state, std::memory_order_acquire);
        if (!heartbeat)
            return;
        {
            std::lock_guard lock (heartbeat->gate);
            heartbeat->stop.store (true, std::memory_order_release);
        }
        infrastructure_result ([this] {
            return _lane->run_task ([this] {
                const auto lease =
                  std::atomic_load_explicit (_owner_lease.get (), std::memory_order_acquire);
                lease->deadlines->cancel ();
                if (lease->outage) {
                    lease->outage->terminal.complete (
                      result_t<serial_execution_queue_t::execution_operation_t>::success (
                        {serial_execution_queue_t::execution_status_t::stopped, {}}));
                }
            });
        }).value ();
        detail::deadline_scheduler_t::instance ().wake ();
        heartbeat->notify ();
        if (!started)
            return;
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
                static_cast<void> (detail::task_result_access_t::read (*attempt->task));
        }
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
                const auto remaining =
                  remaining_until (attempt->deadline_at, (*attempt->runtime->_lease_now) ());
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
            attempt->task.emplace (heartbeat_renew_once_async (
              attempt, std::chrono::duration<double> (lateness).count ()));
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
        auto operations = co_await _lane->run_task (
          [this] { return std::exchange (_descriptor_publications, {}); });
        for (const auto &operation : operations)
            operation->stop_observation ();
        // The caller's deadline cannot end opaque I/O. Preserve every repository
        // coroutine and its input until its actual terminal, before provider teardown.
        for (const auto &operation : operations)
            co_await operation->drained.task ();
    }

    void record_failure (std::string message) const
    {
        _lane->run_checked ([&] { record_failure_on_lane (std::move (message)); }).get ();
    }

    void record_failure_on_lane (std::string message) const
    {
        update_owner_lease_on_lane ([] (auto &lease) { lease.healthy = false; });
        _last_error = std::move (message);
    }

    location_repository_t *_store;
    location_options_t _options;
    std::string _owner_id;
    std::shared_ptr<std::function<std::chrono::steady_clock::time_point ()>> _lease_now =
      std::make_shared<std::function<std::chrono::steady_clock::time_point ()>> (
        &std::chrono::steady_clock::now);
    std::shared_ptr<std::atomic<std::uint64_t>> _execution_gate =
      std::make_shared<std::atomic<std::uint64_t>> (
        static_cast<std::uint64_t> (execution_gate_state_t::open));
    std::atomic_bool _draining = false;
    std::vector<std::pair<std::string, std::string>> _metric_scopes;
    std::shared_ptr<framework::detail::monitoring_runtime_state_t> _monitoring;
    std::atomic_bool _started = false;
    std::thread _heartbeat;
    std::shared_ptr<heartbeat_owner_t> _heartbeat_state = std::make_shared<heartbeat_owner_t> ();
    // Lane teardown finishes accepted turns before their state is destroyed.
    mutable std::shared_ptr<std::shared_ptr<const owner_lease_snapshot_t>> _owner_lease{
      std::make_shared<std::shared_ptr<const owner_lease_snapshot_t>> (
        std::make_shared<const owner_lease_snapshot_t> ())};
    mutable std::optional<std::string> _last_error;
    mutable std::vector<std::shared_ptr<repository_operation_t>> _descriptor_publications;

    // Owner subscriptions contain no retained object inventory or Ready evidence.
    descriptor_publisher_t _descriptor_publisher;
    std::shared_ptr<std::vector<execution_observer_t>> _execution_discard_requests =
      std::make_shared<std::vector<execution_observer_t>> ();
    mutable std::shared_ptr<state_lane_t> _lane = std::make_shared<state_lane_t> ();
};

} // namespace zlink::framework::runtime
