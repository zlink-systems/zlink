/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* SF-D2: the client owns this scenario's public requests and assertions. */
inline void run_sf_d2_scenario (const options_t &options)
{
    warm_provider_connections (options, "SF-D2");
    sf_client::wait_connected (options.consumer_url, options.provider_a_endpoint);
    sf_client::wait_connected (options.consumer_url, options.provider_b_endpoint);
    const auto before = sf_client::connection_evidence (options.consumer_url);
    auto traffic = std::async (std::launch::async, [&options] {
        return drive_tolerant_requests (options, options.lease_ttl * 2 + options.heartbeat * 4);
    });
    sf_client::stop_store (options.redis_container);
    sf_client::post_empty (options.provider_b_url, "/admin/crash");
    sf_client::wait_down (options.provider_b_url);
    std::this_thread::sleep_for (options.lease_ttl + options.heartbeat);
    sf_client::restart_store (options.redis_container);
    const auto traffic_result = traffic.get ();
    sf_client::ensure (
      std::any_of (traffic_result.replies.begin (), traffic_result.replies.end (),
                   [] (const auto &reply) { return reply.provider_rid == "api-a"; }),
      "SF-D2 survivor served no outage traffic");

    sf_client::wait_peers (
      options.consumer_url, [] (const auto &peers) { return sf_client::has_rid (peers, "api-a"); },
      options.heartbeat * 8, "SF-D2 surviving provider did not re-register");
    sf_client::wait_peers (
      options.consumer_url, [] (const auto &peers) { return !sf_client::has_rid (peers, "api-b"); },
      stale_peer_timeout (options), "SF-D2 dead provider did not disappear");
    for (int i = 0; i < 8; ++i) {
        auto reply =
          sf_client::request_profile (options.consumer_url, "sf-d2-after-" + std::to_string (i));
        sf_client::ensure (reply.provider_rid == "api-a", "SF-D2 routed to dead provider");
    }
    const auto after = sf_client::connection_evidence (options.consumer_url);
    std::cerr
      << "SF-D2 survivor endpoint=" << options.provider_a_endpoint << " connected-before="
      << sf_client::connection_event_count (before, "Connected", options.provider_a_endpoint)
      << " connected-after="
      << sf_client::connection_event_count (after, "Connected", options.provider_a_endpoint)
      << " disconnected-before="
      << sf_client::connection_event_count (before, "Disconnected", options.provider_a_endpoint)
      << " disconnected-after="
      << sf_client::connection_event_count (after, "Disconnected", options.provider_a_endpoint)
      << '\n';
    sf_client::ensure (
      sf_client::connection_event_count (after, "Disconnected", options.provider_a_endpoint)
          == sf_client::connection_event_count (before, "Disconnected", options.provider_a_endpoint)
        && sf_client::connection_event_count (after, "Connected", options.provider_a_endpoint)
             == sf_client::connection_event_count (before, "Connected",
                                                   options.provider_a_endpoint),
      "SF-D2 survivor connection changed");
    sf_client::ensure (
      sf_client::connection_event_count (after, "Disconnected", options.provider_b_endpoint)
        > sf_client::connection_event_count (before, "Disconnected", options.provider_b_endpoint),
      "SF-D2 dead provider disconnect was not observed");
    std::cout << "scenario SF-D2 passed\n";
}

} // namespace
