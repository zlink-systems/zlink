/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/mesh_node_host_service_internal.hpp"

#include "runtime/execution/task_result.hpp"
#include "runtime/locations/location_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <utility>

namespace zlink::framework::runtime
{

location_write_intent_t mesh_node_host_service_detail::mesh_new_claim_intent (
  const std::vector<std::shared_ptr<detail::mesh_node_builder_state_t>> &registrations,
  const mesh_node_descriptor_t &descriptor)
{
    return std::any_of (registrations.begin (), registrations.end (),
                        [&descriptor] (const auto &registration) {
                            return registration->mesh_name == descriptor.mesh_name
                                   && registration->routing_id == descriptor.rid;
                        })
             ? location_write_intent_t::fixed_rid_new_claim
             : location_write_intent_t::new_claim;
}

using mesh_node_host_service_detail::mesh_new_claim_intent;

bool mesh_node_host_service_t::publish_descriptor_state (framework_runtime_state_t state) noexcept
{
    try {
        if (state == framework_runtime_state_t::draining) {
            for (const auto &node : _nodes) {
                node->request_stop ();
                node->native_node ().transport ().publish_draining ().result ().value ();
            }
        }
        std::unique_lock lock (_descriptor_publish_mutex);
        // A Preparing host has no runtime weight publication to drain. Startup
        // must still be able to report a competing claim during the shutdown read.
        if (_published_mesh_nodes.size () != _published_mesh_descriptors.size ()) {
            lock.unlock ();
            return write_mesh_descriptor_state (state);
        }
        while (_descriptor_publication) {
            auto pending = _descriptor_publication;
            lock.unlock ();
            (void) infrastructure_result ([&] { return pending->task (); });
            lock.lock ();
        }
        auto publication = detail::make_task_completion_source<bool> ();
        _descriptor_publication = publication;
        lock.unlock ();
        const bool written = write_mesh_descriptor_state (state);
        lock.lock ();
        _descriptor_publication.reset ();
        lock.unlock ();
        publication->complete (result_t<bool>::success (written));
        request_descriptor_publication ();
        return written;
    }
    catch (...) {
        return false;
    }
}

bool mesh_node_host_service_t::write_mesh_descriptor_state (
  framework_runtime_state_t state) noexcept
{
    try {
        // A setter can record new weights during Store I/O. Keep the mutex
        // available and merge the confirmed state into the latest revision.
        std::optional<location_owner_token_t> owner;
        std::vector<mesh_node_descriptor_t> descriptors;
        {
            std::lock_guard lock (_descriptor_publish_mutex);
            owner = current_location_owner ();
            if (!_location_store || !owner)
                return _published_mesh_descriptors.empty ();
            descriptors = _published_mesh_descriptors;
        }
        for (const auto &original : descriptors) {
            const auto same_key = [&original] (const auto &value) {
                return value.mesh_name == original.mesh_name && value.rid == original.rid;
            };
            auto current = original;
            location_page_request_t page;
            bool found = false;
            do {
                auto listed = infrastructure_result ([&] {
                                  return _location_store->list_mesh_nodes (current.mesh_name, page);
                              }).value ();
                const auto stored = std::find_if (
                  listed.items.begin (), listed.items.end (),
                  [&] (const mesh_node_descriptor_t &candidate) {
                      return candidate.rid.to_hex () == current.rid.to_hex ()
                             && candidate.lifecycle_generation == current.lifecycle_generation;
                  });
                if (stored != listed.items.end ()) {
                    current = *stored;
                    found = true;
                    break;
                }
                page.continuation_token = std::move (listed.continuation_token);
            } while (page.continuation_token);
            if (!found) {
                /* A draining host may already have lost its descriptor when
                 * an earlier lease cleanup fenced the row. Shutdown is
                 * idempotent in that state; relocation still requires the
                 * descriptor to be present so it keeps the store failure
                 * signal. */
                if (state == framework_runtime_state_t::draining) {
                    std::lock_guard lock (_descriptor_publish_mutex);
                    std::erase_if (_published_mesh_descriptors, same_key);
                    std::erase_if (_published_mesh_nodes, same_key);
                    continue;
                }
                return false;
            }
            if (current.state != state) {
                if (current.descriptor_revision == std::numeric_limits<std::uint64_t>::max ())
                    return false;
                current.state = state;
                current.owner_id = owner->owner_id;
                current.lease_generation = owner->lease_generation;
                ++current.descriptor_revision;
                const auto written = infrastructure_result ([&] {
                                         return _location_store->update_mesh_node (
                                           current, location_write_intent_t::renew);
                                     }).value ();
                if (written.status != location_write_status_t::stored)
                    return false;
            }
            std::lock_guard lock (_descriptor_publish_mutex);
            auto cached = std::find_if (_published_mesh_descriptors.begin (),
                                        _published_mesh_descriptors.end (), same_key);
            if (cached == _published_mesh_descriptors.end ())
                return false;
            // The Store row can predate pending local option changes.
            cached->state = current.state;
            cached->descriptor_revision =
              std::max (cached->descriptor_revision, current.descriptor_revision);
            cached->owner_id = std::move (current.owner_id);
            cached->lease_generation = current.lease_generation;
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

task_t<bool> mesh_node_host_service_t::republish_after_store_recovery (
  location_write_intent_t intent,
  std::stop_token cancellation,
  std::chrono::steady_clock::time_point deadline)
{
    if (!co_await publish_requested_descriptors (intent, cancellation, deadline, false))
        co_return false;
    {
        std::lock_guard lock (_dispatch_gate_mutex);
        if (!_descriptor_stop.stop_requested ()
            && (!_location_runtime || !_location_runtime->draining ()))
            _accept_application_dispatch.store (true, std::memory_order_release);
    }
    _dispatch_gate_changed.notify_all ();
    co_return true;
}

void mesh_node_host_service_t::request_descriptor_publication ()
{
    std::chrono::steady_clock::time_point deadline;
    {
        std::lock_guard lock (_descriptor_publish_mutex);
        if (_descriptor_stop.stop_requested () || _changed_mesh_descriptors.empty ())
            return;
        deadline = _location_runtime->execution_clock () ()
                   + _location_runtime->options ().owner_lease_renew_timeout;
    }
    (void) publish_requested_descriptors (location_write_intent_t::renew,
                                          _descriptor_stop.get_token (), deadline, true);
}

task_t<bool> mesh_node_host_service_t::publish_requested_descriptors (
  location_write_intent_t intent,
  std::stop_token cancellation,
  std::chrono::steady_clock::time_point deadline,
  bool background)
{
    std::shared_ptr<task_completion_source_t<bool>> publication;
    bool start = false;
    {
        std::lock_guard lock (_descriptor_publish_mutex);
        if (_descriptor_stop.stop_requested () || cancellation.stop_requested ())
            return task_t<bool> (result_t<bool>::success (false));
        const auto owner =
          _location_runtime ? _location_runtime->descriptor_publication_owner () : std::nullopt;
        if (!_location_store || !owner)
            return task_t<bool> (result_t<bool>::success (_published_mesh_descriptors.empty ()));
        const bool all_claimed =
          _published_mesh_nodes.size () == _published_mesh_descriptors.size ();
        if (background && !all_claimed)
            return task_t<bool> (result_t<bool>::success (false));
        // Discovery confirms startup and owner changes. Runtime option changes
        // and their retries are submitted by the setter and lease renew.
        if (all_claimed && (!background || _changed_mesh_descriptors.empty ())
            && std::all_of (_published_mesh_descriptors.begin (),
                            _published_mesh_descriptors.end (), [&] (const auto &descriptor) {
                                return descriptor.owner_id == owner->owner_id
                                       && descriptor.lease_generation == owner->lease_generation;
                            }))
            return task_t<bool> (result_t<bool>::success (true));
        if (!_descriptor_publication) {
            _descriptor_publication = detail::make_task_completion_source<bool> ();
            start = true;
        }
        publication = _descriptor_publication;
    }
    if (start) {
        auto run = [this, publication, intent, cancellation, deadline] {
            auto pending = publish_mesh_descriptors (intent, cancellation, deadline);
            detail::observe_task_terminal (
              pending, [publication] (const auto &result) { publication->complete (result); });
        };
        if (!background)
            run ();
        else if (!infrastructure_executor ()->try_submit_internal (std::move (run))) {
            {
                std::lock_guard lock (_descriptor_publish_mutex);
                _descriptor_publication.reset ();
            }
            publication->complete (result_t<bool>::success (false));
        }
    }
    return publication->task ();
}

task_t<bool>
mesh_node_host_service_t::publish_mesh_descriptors (location_write_intent_t intent,
                                                    std::stop_token cancellation,
                                                    std::chrono::steady_clock::time_point deadline)
{
    try {
        const auto now = _location_runtime->execution_clock ();
        for (;;) {
            std::vector<mesh_node_descriptor_t> descriptors;
            std::set<std::string> changed;
            {
                std::lock_guard lock (_descriptor_publish_mutex);
                descriptors = _published_mesh_descriptors;
                changed = _changed_mesh_descriptors;
            }
            auto written = co_await await_result (
              write_mesh_descriptors (descriptors, changed, intent, cancellation, deadline));
            {
                std::lock_guard lock (_descriptor_publish_mutex);
                // A setter and the end of a publication use the same turn to
                // record the revision or relinquish publication ownership.
                const bool newer = std::any_of (
                  descriptors.begin (), descriptors.end (), [this] (const auto &copied) {
                      return _changed_mesh_descriptors.contains (copied.mesh_name)
                             && !unchanged_published_descriptor (copied);
                  });
                if (!newer || cancellation.stop_requested () || _descriptor_stop.stop_requested ()
                    || now () >= deadline) {
                    _descriptor_publication.reset ();
                    co_return written;
                }
            }
        }
    }
    catch (...) {
        std::lock_guard lock (_descriptor_publish_mutex);
        _descriptor_publication.reset ();
        throw;
    }
}

task_t<bool> mesh_node_host_service_t::write_mesh_descriptors (
  const std::vector<mesh_node_descriptor_t> &descriptors,
  const std::set<std::string> &changed,
  location_write_intent_t intent,
  std::stop_token cancellation,
  std::chrono::steady_clock::time_point deadline)
{
    const auto owner =
      _location_runtime ? _location_runtime->descriptor_publication_owner () : std::nullopt;
    if (!owner)
        co_return descriptors.empty ();
    std::vector<task_t<bool>> publications;
    bool complete = true;
    for (const auto &original : descriptors) {
        if (cancellation.stop_requested () || _descriptor_stop.stop_requested ()) {
            complete = false;
            break;
        }
        auto descriptor = original;
        const auto key = mesh_node_descriptor_key_t{descriptor.mesh_name, descriptor.rid};
        bool first_publication;
        {
            std::lock_guard lock (_descriptor_publish_mutex);
            first_publication =
              std::find_if (_published_mesh_nodes.begin (), _published_mesh_nodes.end (),
                            [&key] (const auto &published) {
                                return published.mesh_name == key.mesh_name
                                       && published.rid == key.rid;
                            })
              == _published_mesh_nodes.end ();
        }
        if (!first_publication && !changed.contains (descriptor.mesh_name)
            && descriptor.owner_id == owner->owner_id
            && descriptor.lease_generation == owner->lease_generation)
            continue;
        // First publication retries still require a newly observed lease renewal.
        if (first_publication && intent == location_write_intent_t::renew) {
            complete = false;
            continue;
        }
        descriptor.owner_id = owner->owner_id;
        descriptor.lease_generation = owner->lease_generation;
        if (!first_publication
            && descriptor.descriptor_revision == std::numeric_limits<std::uint64_t>::max ()) {
            complete = false;
            continue;
        }
        if (!first_publication)
            ++descriptor.descriptor_revision;
        auto publication = _location_runtime->descriptor_write_confirmed (
          [store = _location_store, descriptor, first_publication,
           write_intent = first_publication ? mesh_new_claim_intent (_registrations, descriptor)
                                            : location_write_intent_t::renew] {
              return continue_task<location_write_result_t> (
                store->update_mesh_node (descriptor, write_intent),
                [first_publication] (location_write_result_t result) {
                    if (!first_publication
                        && result.status == location_write_status_t::ignored_stale)
                        result.status = location_write_status_t::rejected_conflict;
                    return task_t<location_write_result_t> (
                      result_t<location_write_result_t>::success (std::move (result)));
                });
          },
          _location_runtime->execution_clock (), cancellation, deadline,
          _descriptor_stop.get_token ());
        publications.push_back (continue_task<bool> (
          std::move (publication), [this, original, descriptor = std::move (descriptor), key,
                                    first_publication, cancellation] (bool written) mutable {
              if (!written || cancellation.stop_requested () || _descriptor_stop.stop_requested ())
                  return task_t<bool> (result_t<bool>::success (false));
              std::lock_guard lock (_descriptor_publish_mutex);
              auto *const current = unchanged_published_descriptor (original);
              if (current) {
                  *current = std::move (descriptor);
                  _changed_mesh_descriptors.erase (key.mesh_name);
              }
              if (first_publication)
                  _published_mesh_nodes.push_back (key);
              return task_t<bool> (result_t<bool>::success (true));
          }));
    }
    auto published = result_t<bool>::success (complete);
    for (auto &publication : publications) {
        const auto result = co_await await_result (std::move (publication));
        if (!result)
            published =
              detail::propagate_failure<bool> (result, "Mesh descriptor publication failed");
        else if (published)
            published = result_t<bool>::success (published.value () && result.value ());
    }
    // The callback owns every descriptor continuation, including after a rejection.
    co_return published.value () && !cancellation.stop_requested ()
      && !_descriptor_stop.stop_requested ();
}

mesh_node_descriptor_t *
mesh_node_host_service_t::unchanged_published_descriptor (const mesh_node_descriptor_t &copied)
{
    const auto cached =
      std::find_if (_published_mesh_descriptors.begin (), _published_mesh_descriptors.end (),
                    [&copied] (const auto &value) {
                        return value.mesh_name == copied.mesh_name && value.rid == copied.rid;
                    });
    // A concurrent state/weight publication owns its newer descriptor.
    if (cached == _published_mesh_descriptors.end ()
        || cached->descriptor_revision != copied.descriptor_revision
        || cached->owner_id != copied.owner_id
        || cached->lease_generation != copied.lease_generation)
        return nullptr;
    return &*cached;
}

std::optional<location_owner_token_t> mesh_node_host_service_t::current_location_owner () const
{
    if (!_location_runtime)
        return std::nullopt;
    return _location_runtime->current_owner_token ();
}

} // namespace zlink::framework::runtime
