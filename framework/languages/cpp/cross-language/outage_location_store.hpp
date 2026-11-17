/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/locations/provider_location_repository.hpp"

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
                    co_return fw::store_found_t{fw::store_value_t{
                      put->bytes, {"outage-observation"}, std::nullopt,
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
    explicit outage_location_store_t (std::shared_ptr<fw::location_store_t> inner,
                                       std::string actor_id,
                                       std::string mesh,
                                       std::string node,
                                       bool target) :
        inner (std::move (inner)), repository (*this->inner),
        actor_key (runtime::actor_authority_key (actor_id)), target (target),
        mesh (std::move (mesh)), node (std::move (node))
    {
        outage_observation_store_t capture;
        runtime::provider_location_repository_t codecs (capture);
        codecs.read_authority_core (actor_key).result ().value ();
        authority_store_key = capture.observed_key;
        codecs.remove_mesh_node (
          fw::mesh_node_descriptor_key_t{this->mesh, zlink::routing_id_t::from (this->node)},
          fw::location_owner_token_t{"observation", 1}).result ().value ();
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

        const bool descriptor = contains_put (request, descriptor_store_key);
        const bool authority = contains_put (request, authority_store_key);
        auto result = co_await inner->write (std::move (request));
        if (!target && authority && expected_recovered_lease.load () != 0
            && std::holds_alternative<fw::store_write_applied_t> (result)) {
            const auto snapshot = co_await repository.read_authority_core (actor_key);
            if (const auto *current = std::get_if<fw::authority_snapshot_t> (&snapshot)) {
                runtime::actor_authority_detail::relocation_capture_identity_t identity;
                const auto actor = runtime::decode_direct_actor_authority_payload (
                  current->payload, &identity);
                if (actor && actor->has_relocation_state
                    && actor->relocation_phase
                         == runtime::actor_authority_detail::captured_relocation_phase) {
                    const auto expected = static_cast<std::uint64_t> (
                      expected_recovered_lease.load ());
                    if (identity.source_owner_lease_generation != expected
                        || identity.coordinator_lease_generation != expected
                        || identity.source_owner_id != expected_recovered_owner
                        || identity.coordinator_owner_id != expected_recovered_owner
                        || current->owner.owner_id != expected_recovered_owner
                        || current->owner.lease_generation
                             != expected_recovered_lease.load ())
                        throw std::runtime_error (
                          "relocation source/coordinator did not use the recovered lease");
                    relocation_fence_observed.store (true);
                }
            }
        }
        if (recovering.load () && std::holds_alternative<fw::store_write_applied_t> (result)) {
            if (descriptor && !descriptor_released.load ()) {
                descriptor_held.store (true);
                co_await descriptor_release.task ();
            }
            if (authority && !cas_released.load ()) {
                cas_held.store (true);
                co_await cas_release.task ();
            }
            if (descriptor)
                descriptor_delivered.store (true);
            if (authority)
                cas_delivered.store (true);
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
    std::atomic<std::int64_t> expected_recovered_lease{0};
    std::string expected_recovered_owner;
    std::atomic_bool relocation_fence_observed{false};
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

    static bool contains_put (const fw::store_write_request_t &request,
                               const fw::store_key_t &key)
    {
        return std::any_of (request.mutations.begin (), request.mutations.end (),
                            [&] (const auto &mutation) {
                                const auto *put = std::get_if<fw::store_put_t> (&mutation);
                                return put && put->key.value == key.value;
                            });
    }

    fw::store_key_t authority_store_key, descriptor_store_key;
};
