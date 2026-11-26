/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/location_runtime.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include <gtest/gtest.h>

namespace zlink::framework::runtime
{
struct mesh_node_host_service_test_access_t
{
    static void selection_services (mesh_node_host_service_t &service, service_provider_t &provider)
    {
        service._services = &provider;
    }

    static void configure (mesh_node_host_service_t &service,
                           std::shared_ptr<location_repository_t> store,
                           location_runtime_t &runtime,
                           bool published = false)
    {
        service._location_store = std::move (store);
        service._location_runtime = &runtime;
        service._location_store->configure_reclaim_margin (std::chrono::milliseconds::zero ());
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "publication-mesh";
        descriptor.rid = zlink::routing_id_t::from ("local");
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "inproc://publication-mesh";
        descriptor.security_identity = "default";
        descriptor.activation_concurrency.limit = 1;
        if (published) {
            const auto owner = runtime.current_owner_token ().value ();
            descriptor.owner_id = owner.owner_id;
            descriptor.lease_generation = owner.lease_generation;
            EXPECT_EQ (location_write_status_t::stored,
                       service._location_store
                         ->update_mesh_node (descriptor, location_write_intent_t::new_claim)
                         .result ()
                         .value ()
                         .status);
            service._published_mesh_nodes.push_back ({descriptor.mesh_name, descriptor.rid});
        }
        service._published_mesh_descriptors.push_back (std::move (descriptor));
    }
    static void recovery_descriptors (mesh_node_host_service_t &service,
                                      location_repository_t &store,
                                      location_runtime_t &runtime,
                                      const mesh_node_descriptor_t &descriptor)
    {
        service._location_store = std::shared_ptr<location_repository_t> (&store, [] (auto *) {});
        service._location_runtime = &runtime;
        service._published_mesh_descriptors.push_back (descriptor);
        service._published_mesh_nodes.push_back ({descriptor.mesh_name, descriptor.rid});
        auto other = descriptor;
        other.rid = zlink::routing_id_t::from ("second-descriptor");
        EXPECT_EQ (location_write_status_t::stored,
                   store.update_mesh_node (other, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status);
        service._published_mesh_descriptors.push_back (other);
        service._published_mesh_nodes.push_back ({other.mesh_name, other.rid});
    }
    static bool publication_mutex_available (mesh_node_host_service_t &service)
    {
        if (!service._descriptor_publish_mutex.try_lock ())
            return false;
        service._descriptor_publish_mutex.unlock ();
        return true;
    }
    // This adapter also compiles against the original synchronous implementation
    // for the RED run. Only its return shape is normalized, never its scheduling.
    template <typename Service>
    static task_t<bool> publish (Service &service,
                                 std::stop_token cancellation,
                                 std::chrono::steady_clock::time_point deadline)
    {
        if constexpr (requires {
                          service.republish_after_store_recovery (
                            location_write_intent_t::new_claim, cancellation, deadline);
                      })
            return service.republish_after_store_recovery (location_write_intent_t::new_claim,
                                                           cancellation, deadline);
        else
            return task_t<bool> (result_t<bool>::success (
              service.republish_after_store_recovery (location_write_intent_t::new_claim)));
    }
};
} // namespace zlink::framework::runtime
