/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "mon_a1_socket_events_scenario.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <iostream>
#include <thread>

namespace zlink::framework::e2e::runtime_monitoring::client
{

inline nlohmann::json channel_snapshot (const nlohmann::json &snapshot)
{
    for (const auto &channel : snapshot.at ("channels")) {
        if (channel.at ("name") == route_mesh_channel)
            return channel;
    }
    throw std::runtime_error ("RouteMesh channel snapshot is missing");
}

inline nlohmann::json wait_channel_weight (const std::string &base_url,
                                           int expected_weight)
{
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (10);
    nlohmann::json last_snapshot;
    do {
        last_snapshot = runtime_snapshot (base_url);
        const auto channel = channel_snapshot (last_snapshot);
        const auto expected_ready = expected_weight > 0;
        if (channel.at ("isReady").get<bool> () == expected_ready
            && (expected_ready || channel.at ("readyTargetCount").get<std::uint64_t> () == 0))
            return channel;
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    } while (std::chrono::steady_clock::now () < deadline);
    throw std::runtime_error (
      "MON-A3 channel weight did not converge: " + last_snapshot.dump ());
}

inline void set_mesh_weight (const std::string &base_url, int weight)
{
    auto client = zlink::http_client::client_t::create ()
                    .base_url (base_url)
                    .timeout (std::chrono::milliseconds (3000))
                    .build ();
    const auto changed =
      client.post ("/admin/mesh-weight?weight=" + std::to_string (weight))
        .submit_raw ()
        .result ();
    ensure (changed && changed.value ().status < 400,
            "MON-A3 public runtime-options call failed: "
              + (changed ? std::to_string (changed.value ().status)
                         + " body=" + changed.value ().body
                         : std::string ("transport")));
}

inline void run_mon_a3_spot_events_scenario (const client_options_t &options)
{
    // The fixture has three RouteMesh server members.  Change the complete
    // membership so the public target-count transition has an unambiguous
    // zero-target state.
    set_mesh_weight (options.service_url, 0);
    set_mesh_weight (options.filtered_service_url, 0);
    set_mesh_weight (options.throw_service_url, 0);
    const auto zero = wait_channel_weight (options.filtered_service_url, 0);
    ensure (!zero.at ("isReady").get<bool> ()
              && zero.at ("readyTargetCount").get<std::uint64_t> () == 0,
            "MON-A3 local weight 0 readiness is inconsistent");

    set_mesh_weight (options.service_url, 100);
    set_mesh_weight (options.filtered_service_url, 100);
    set_mesh_weight (options.throw_service_url, 100);
    const auto restored =
      wait_channel_weight (options.filtered_service_url, 100);
    ensure (restored.at ("isReady").get<bool> (),
            "MON-A3 restored channel is not selectable");
    std::cout << "scenario MON-A3 passed\n";
}

} // namespace zlink::framework::e2e::runtime_monitoring::client
