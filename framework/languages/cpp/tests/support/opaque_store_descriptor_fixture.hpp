/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_records.hpp"
#include <algorithm>
#include <utility>

namespace zlink::framework::test::opaque_store
{
using namespace runtime;

class unanswered_descriptor_store_t final : public location_store_t
{
  public:
    enum class phase_t
    {
        initial_read,
        write,
        confirmation_read
    };
    explicit unanswered_descriptor_store_t (phase_t phase) : phase (phase) {}

    task_t<store_read_result_t> read (store_key_t key) override
    {
        if (armed
            && ((phase == phase_t::initial_read && !held)
                || (phase == phase_t::confirmation_read && wrote && !held))) {
            held = true;
            co_await release.task ();
        }
        co_return co_await inner.read (std::move (key));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        if (armed
            && std::any_of (request.mutations.begin (), request.mutations.end (),
                            [] (const auto &mutation) {
                                return std::holds_alternative<store_delete_t> (mutation);
                            }))
            ++delete_writes;
        if (armed && phase == phase_t::write && !held) {
            held = true;
            // Keep the public provider's input alive through the suspended write.
            input = request;
            co_await release.task ();
            input_stable = std::get<store_put_t> (request.mutations.front ()).bytes
                           == std::get<store_put_t> (input.mutations.front ()).bytes;
        }
        auto result = co_await inner.write (std::move (request));
        if (armed && phase == phase_t::confirmation_read && !wrote) {
            wrote = true;
            co_return result_t<store_write_result_t>::failure (framework_error_kind_t::unavailable,
                                                               "descriptor write response lost");
        }
        co_return result;
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }

    in_memory_location_store_t inner;
    task_completion_source_t<void> release;
    phase_t phase;
    int delete_writes = 0;
    bool armed = false;
    bool held = false;
    bool wrote = false;
    store_write_request_t input;
    bool input_stable = true;
};

struct release_descriptor_on_exit_t
{
    task_completion_source_t<void> &release;
    ~release_descriptor_on_exit_t () { release.complete (result_t<void>::success ()); }
};

inline mesh_node_descriptor_t review_mesh_descriptor (const location_owner_token_t &owner)
{
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "review-mesh";
    descriptor.rid = zlink::routing_id_t::from ("review-node");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "inproc://review-node";
    descriptor.security_identity = "review-node";
    descriptor.activation_concurrency.limit = 1;
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.object_role = object_role_t::none;
    descriptor.state = framework_runtime_state_t::serving;
    return descriptor;
}
}
