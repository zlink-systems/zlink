/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "actor_maintenance_join_observations.hpp"
#include <zlink/framework/contracts/configuration/app.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>
#include <zlink/framework/contracts/monitoring/route_mesh_runtime.hpp>
#include <string_view>
#include <thread>

namespace zlink::framework::test::actor_maintenance_join
{
// Release application callbacks before host teardown, including assertion failure.
struct release_callbacks_t
{
    observations_t &e;
    ~release_callbacks_t ();
};

struct host_t
{
    fw::app_t app = fw::app_t::create ();
    std::thread thread;
    ~host_t ();
    void start ();
};

// Which Entry Spot a node registers: one accepting `actor_t`, none, or one accepting only
// `other_actor_t` (that node also registers the `other_actor_t` factory).
enum class entry_kind_t
{
    member,
    none,
    other
};
void observe_joined_batch_admission (observations_t &evidence,
                                     const fw::message_flow_event_t &event);
void configure (host_t &host,
                const std::shared_ptr<observations_t> &observations,
                const std::string &rid,
                std::shared_ptr<fw::location_store_t> store,
                std::shared_ptr<fw::relocation_store_t> relocations,
                std::chrono::milliseconds follow_duration = default_follow_duration,
                int spot_limit = 0,
                bool application_signaled = false,
                entry_kind_t entry = entry_kind_t::member,
                std::optional<int> placement_weight = std::nullopt);
bool ready (host_t &host);
bool move_to (fw::actor_client_t &sender,
              observations_t &evidence,
              std::string target,
              std::size_t count);
bool peer_ready (fw::route_mesh_runtime_t &routes,
                 std::string_view peer_rid = {},
                 std::size_t count = 1);
bool set_published_placement_weight (fw::service_provider_t &services,
                                     int weight,
                                     fw::service_provider_t *requester = nullptr);
} // namespace zlink::framework::test::actor_maintenance_join
