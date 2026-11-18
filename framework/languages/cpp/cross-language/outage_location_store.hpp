/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/provider_location_repository.hpp"
#include <cstdlib>
#include <functional>
#include <map>
#include <fstream>

struct outage_lease_profile_t
{
    std::map<std::string, std::string> values;
    outage_lease_profile_t ()
    {
        const auto path = std::getenv ("ZLINK_OUTAGE_LEASE_PROFILE");
        if (!path || !*path)
            throw std::runtime_error ("ZLINK_OUTAGE_LEASE_PROFILE is required");
        std::ifstream input (path);
        if (!input)
            throw std::runtime_error ("cannot read outage lease profile");
        std::string line;
        while (std::getline (input, line)) {
            if (!line.empty () && line.back () == '\r')
                line.pop_back ();
            const auto equal = line.find ('=');
            if (equal != std::string::npos && line.front () != '#')
                values.emplace (line.substr (0, equal), line.substr (equal + 1));
        }
        if (values.at ("storeOutageRetention") != "true")
            throw std::runtime_error ("outage profile requires StoreOutageRetention");
    }
    std::chrono::milliseconds duration (const std::string &key) const
    {
        return std::chrono::milliseconds (std::stoll (values.at (key)));
    }
};

// Observation uses the repository's own key and record codecs. The overlay is
// read-only: candidate writes never escape to Redis before their release.
class outage_observation_store_t final : public fw::location_store_t
{
  public:
    explicit outage_observation_store_t (fw::location_store_t *inner = nullptr,
                                         const fw::store_write_request_t *overlay = nullptr) :
        _inner (inner), _overlay (overlay)
    {
    }

    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
    {
        observed_key = key;
        if (_overlay) {
            for (const auto &mutation : _overlay->mutations) {
                if (const auto *put = std::get_if<fw::store_put_t> (&mutation);
                    put && put->key.value == key.value) {
                    co_return fw::store_found_t{
                      fw::store_value_t{put->bytes,
                                        {"outage-observation"},
                                        std::nullopt,
                                        std::chrono::system_clock::now ()}};
                }
                if (const auto *removed = std::get_if<fw::store_delete_t> (&mutation);
                    removed && removed->key.value == key.value)
                    co_return fw::store_missing_t{std::chrono::system_clock::now ()};
            }
        }
        if (_inner)
            co_return co_await _inner->read (std::move (key));
        co_return fw::store_missing_t{std::chrono::system_clock::now ()};
    }

    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t) override
    {
        throw std::logic_error ("outage observation cannot write");
        co_return fw::store_write_conflict_t{};
    }

    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        if (!_inner)
            throw std::logic_error ("key observation cannot scan");
        co_return co_await _inner->scan (std::move (request));
    }

    fw::store_key_t observed_key;

  private:
    fw::location_store_t *_inner;
    const fw::store_write_request_t *_overlay;
};

class outage_location_store_t final : public fw::location_store_t
{
  public:
    static std::int64_t now_nanos ()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds> (
                 std::chrono::steady_clock::now ().time_since_epoch ())
          .count ();
    }
    void timing_event (const std::string &stage, std::int64_t nanos)
    {
        if (events)
            events ("outage-timing|event=" + stage
                    + "|monotonicMs=" + std::to_string (nanos / 1000000.0));
    }
    void set_cutoff (std::chrono::steady_clock::time_point cutoff)
    {
        cutoff_nanos.store (
          std::chrono::duration_cast<std::chrono::nanoseconds> (cutoff.time_since_epoch ())
            .count ());
        const auto lifetime = profile.duration ("ttlMs") + 2 * profile.duration ("fencingMarginMs");
        timing_event ("t0",
                      cutoff_nanos.load ()
                        - std::chrono::duration_cast<std::chrono::nanoseconds> (lifetime).count ());
        timing_event ("cutoff", cutoff_nanos.load ());
    }
    void check_timing () const
    {
        if (failure.load () == 1)
            throw std::runtime_error (
              "deadline-exceeded|Preserve submission or confirmation exceeded cutoff");
        if (failure.load () == 2)
            throw std::runtime_error ("discarded|actor authority Delete during recovery");
        if (recovering.load () && (!descriptor_delivered.load () || !cas_delivered.load ())
            && now_nanos () >= cutoff_nanos.load ())
            throw std::runtime_error ("deadline-exceeded|confirmation wait reached cutoff");
    }
    void confirm (bool descriptor)
    {
        const auto now = now_nanos ();
        timing_event (descriptor ? "descriptor-confirmed" : "cas-confirmed", now);
        if (now >= cutoff_nanos.load ()) {
            failure.store (1);
            check_timing ();
        }
        if (descriptor)
            descriptor_delivered.store (true);
        else
            cas_delivered.store (true);
    }
    std::function<void (const std::string &)> events;
    outage_lease_profile_t profile;
    std::atomic<std::int64_t> cutoff_nanos{0}, cas_submitted_nanos{0};
    std::atomic<int> failure{0};
    explicit outage_location_store_t (std::shared_ptr<fw::location_store_t> inner,
                                      std::string actor_id,
                                      std::string mesh,
                                      std::string node,
                                      bool target) :
        inner (std::move (inner)),
        repository (*this->inner),
        actor_key (runtime::actor_authority_key (actor_id)),
        target (target),
        mesh (std::move (mesh)),
        node (std::move (node))
    {
        outage_observation_store_t capture;
        runtime::provider_location_repository_t codecs (capture);
        codecs.read_authority_core (actor_key).result ().value ();
        authority_store_key = capture.observed_key;
        codecs
          .remove_mesh_node (
            fw::mesh_node_descriptor_key_t{this->mesh, zlink::routing_id_t::from (this->node)},
            fw::location_owner_token_t{"observation", 1})
          .result ()
          .value ();
        descriptor_store_key = capture.observed_key;
    }

    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
    {
        fail_if_outage ();
        co_return co_await inner->read (std::move (key));
    }

    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        fail_if_outage ();
        co_return co_await inner->scan (std::move (request));
    }

    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
    {
        const auto submitted_at = now_nanos ();
        fail_if_outage ();
        if (target && !commit_released.load ()) {
            outage_observation_store_t overlay (inner.get (), &request);
            runtime::provider_location_repository_t projected (overlay);
            auto candidate = co_await projected.read_authority_core (actor_key);
            if (const auto *snapshot = std::get_if<fw::authority_snapshot_t> (&candidate);
                snapshot && snapshot->allocation.target.node_rid.value () == node) {
                auto before = co_await repository.read_authority_core (actor_key);
                if (const auto *source = std::get_if<fw::authority_snapshot_t> (&before);
                    source && source->allocation.target.node_rid.value () != node) {
                    staged.store (true);
                    co_await commit_release.task ();
                }
            }
        }

        if (recovering.load () && (!descriptor_delivered.load () || !cas_delivered.load ())
            && std::any_of (request.mutations.begin (), request.mutations.end (),
                            [&] (const auto &mutation) {
                                const auto *removed = std::get_if<fw::store_delete_t> (&mutation);
                                return removed && removed->key.value == authority_store_key.value;
                            })) {
            failure.store (2);
            check_timing ();
        }
        const bool descriptor = contains_put (request, descriptor_store_key);
        const bool authority = contains_put (request, authority_store_key);
        if (recovering.load () && authority && !cas_delivered.load ()) {
            cas_submitted_nanos.store (submitted_at);
            timing_event ("cas-submit", submitted_at);
            if (submitted_at
                  + std::chrono::duration_cast<std::chrono::nanoseconds> (
                      profile.duration ("renewTimeoutMs"))
                      .count ()
                > cutoff_nanos.load ()) {
                failure.store (1);
                check_timing ();
            }
        }
        auto result = co_await inner->write (std::move (request));
        if (recovering.load () && std::holds_alternative<fw::store_write_applied_t> (result)) {
            if (descriptor && !descriptor_released.load ()) {
                descriptor_held.store (true);
                co_await descriptor_release.task ();
            }
            if (authority && !cas_released.load ()) {
                cas_held.store (true);
                co_await cas_release.task ();
            }
            if (descriptor && !descriptor_delivered.load ())
                confirm (true);
            if (authority && !cas_delivered.load ())
                confirm (false);
        }
        co_return result;
    }

    fw::authority_snapshot_t authority ()
    {
        auto result = repository.read_authority_core (actor_key).result ().value ();
        const auto *snapshot = std::get_if<fw::authority_snapshot_t> (&result);
        if (!snapshot)
            throw std::runtime_error ("outage actor authority is missing");
        return *snapshot;
    }

    void begin_recovery ()
    {
        recovering.store (true);
        unavailable.store (false);
    }

    static void release (std::atomic_bool &released, fw::task_completion_source_t<void> &pending)
    {
        released.store (true);
        pending.complete (fw::result_t<void>::success ());
    }

    std::shared_ptr<fw::location_store_t> inner;
    runtime::provider_location_repository_t repository;
    fw::authority_key_t actor_key;
    std::atomic_bool unavailable{false}, failure_observed{false}, recovering{false};
    std::atomic_bool descriptor_held{false}, cas_held{false}, staged{false};
    std::atomic_bool descriptor_delivered{false}, cas_delivered{false};
    std::atomic_bool descriptor_released{false}, cas_released{false}, commit_released{false};
    fw::task_completion_source_t<void> descriptor_release, cas_release, commit_release;
    bool target;
    std::string mesh, node;

  private:
    void fail_if_outage ()
    {
        if (unavailable.load ()) {
            failure_observed.store (true);
            throw fw::framework_exception_t (fw::framework_error_kind_t::unavailable,
                                             "injected cross-language Store outage");
        }
    }

    static bool contains_put (const fw::store_write_request_t &request, const fw::store_key_t &key)
    {
        return std::any_of (request.mutations.begin (), request.mutations.end (),
                            [&] (const auto &mutation) {
                                const auto *put = std::get_if<fw::store_put_t> (&mutation);
                                return put && put->key.value == key.value;
                            });
    }

    fw::store_key_t authority_store_key, descriptor_store_key;
};
