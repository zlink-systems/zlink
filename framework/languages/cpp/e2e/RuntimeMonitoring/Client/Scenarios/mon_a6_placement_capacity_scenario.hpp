/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "mon_a1_socket_events_scenario.hpp"

#include <chrono>
#include <iostream>

namespace zlink::framework::e2e::runtime_monitoring::client
{

inline nlohmann::json post_json (const std::string &base_url,
                                 const std::string &path,
                                 int &status)
{
    auto http = zlink::http_client::client_t::create ()
                  .base_url (base_url)
                  .timeout (std::chrono::milliseconds (3000))
                  .build ();
    const auto result = http.post (path).submit_raw ().result ();
    ensure (result.has_value (), "MON-A6 HTTP request did not complete");
    status = result.value ().status;
    return nlohmann::json::parse (result.value ().body);
}

inline nlohmann::json get_json (const std::string &base_url,
                                const std::string &path,
                                int &status)
{
    auto http = zlink::http_client::client_t::create ()
                  .base_url (base_url)
                  .timeout (std::chrono::milliseconds (3000))
                  .build ();
    const auto result = http.get (path).submit_raw ().result ();
    ensure (result.has_value (), "MON-A6 HTTP query did not complete");
    status = result.value ().status;
    return nlohmann::json::parse (result.value ().body);
}

inline void wait_placement (const std::string &base_url,
                            std::size_t actors,
                            std::size_t spots,
                            bool available)
{
    nlohmann::json last_snapshot;
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::milliseconds (5000);
    while (std::chrono::steady_clock::now () < deadline) {
        const auto snapshot = runtime_snapshot (base_url);
        last_snapshot = snapshot;
        const auto &placement = snapshot.at ("placement");
        if (placement.at ("activeActorCount").get<std::size_t> () == actors
            && placement.at ("activeSpotCount").get<std::size_t> () == spots
            && placement.at ("isAvailable").get<bool> () == available)
            return;
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    throw std::runtime_error ("MON-A6 placement snapshot did not converge: "
                              + last_snapshot.dump ());
}

inline void run_mon_a6_placement_capacity_scenario (const client_options_t &options)
{
    int status = 0;
    std::string actor_one_id;
    nlohmann::json actor_one;
    for (int index = 0; index < 64; ++index) {
        const auto candidate = "mon-a6-actor-" + std::to_string (index);
        const auto created = post_json (options.service_url,
                                        "/actor/create?actorId=" + candidate,
                                        status);
        if (status < 400 && created.value ("providerRid", "") == "svc-a") {
            actor_one_id = candidate;
            actor_one = created;
            break;
        }
        if (status < 400)
            post_json (options.service_url, "/actor/delete?actorId=" + candidate, status);
    }
    ensure (!actor_one_id.empty (), "MON-A6 could not place the first Actor on svc-a");
    ensure (status < 400 && actor_one.value ("status", "") == "created",
            "MON-A6 first Actor creation failed status=" + std::to_string (status)
              + " body=" + actor_one.dump ());
    std::string spot_one_id;
    nlohmann::json spot_one;
    for (int index = 0; index < 64; ++index) {
        const auto candidate = "mon-a6-spot-" + std::to_string (index);
        const auto created = post_json (options.service_url,
                                        "/spot/create?spotId=" + candidate,
                                        status);
        if (status < 400 && created.value ("providerRid", "") == "svc-a") {
            spot_one_id = candidate;
            spot_one = created;
            break;
        }
        if (status < 400)
            post_json (options.service_url,
                       "/admin/subject/close?spotId=" + candidate,
                       status);
    }
    ensure (!spot_one_id.empty (), "MON-A6 could not place the first Spot on svc-a");
    ensure (status < 400 && spot_one.contains ("spotId"),
            "MON-A6 first Spot creation failed");
    wait_placement (options.service_url, 1, 1, true);

    const auto actor_location = get_json (
      options.service_url,
      "/location/object?kind=actor&id=" + actor_one_id,
      status);
    ensure (status == 200 && actor_location.value ("globalId", "") == actor_one_id
              && actor_location.value ("state", "") == "ready"
              && actor_location.value ("stableType", "") == monitoring_actor_type,
            "MON-A6 exact Actor location query mismatch: "
              + actor_location.dump ());
    const auto spot_location = get_json (
      options.service_url,
      "/location/object?kind=spot&id=" + spot_one_id,
      status);
    ensure (status == 200 && spot_location.value ("globalId", "") == spot_one_id
              && spot_location.value ("state", "") == "ready"
              && spot_location.value ("stableType", "") == spot_channel,
            "MON-A6 exact Spot location query mismatch: "
              + spot_location.dump ());

    std::string actor_fill_id;
    for (int index = 0; index < 64; ++index) {
        const auto candidate = "mon-a6-fill-actor-" + std::to_string (index);
        const auto created = post_json (options.service_url,
                                        "/actor/create?actorId=" + candidate,
                                        status);
        if (status < 400 && created.value ("providerRid", "") == "svc-a") {
            actor_fill_id = candidate;
            break;
        }
        if (status < 400)
            post_json (options.service_url, "/actor/delete?actorId=" + candidate, status);
    }
    ensure (!actor_fill_id.empty (), "MON-A6 could not place the second Actor on svc-a");
    std::string spot_fill_id;
    nlohmann::json spot_fill;
    for (int index = 0; index < 64; ++index) {
        const auto candidate = "mon-a6-fill-spot-" + std::to_string (index);
        const auto created = post_json (options.service_url,
                                        "/spot/create?spotId=" + candidate,
                                        status);
        if (status < 400 && created.value ("providerRid", "") == "svc-a") {
            spot_fill_id = candidate;
            spot_fill = created;
            break;
        }
        if (status < 400)
            post_json (options.service_url,
                       "/admin/subject/close?spotId=" + candidate,
                       status);
    }
    ensure (!spot_fill_id.empty (), "MON-A6 could not place the second Spot on svc-a");
    wait_placement (options.service_url, 2, 2, false);

    const auto actor_page_one = get_json (
      options.service_url,
      "/location/objects?kind=actor&pageSize=1",
      status);
    ensure (status == 200 && actor_page_one.at ("items").size () == 1
              && actor_page_one.contains ("continuationToken"),
            "MON-A6 bounded Actor page did not return a continuation token: "
              + actor_page_one.dump ());
    const auto actor_page_two = get_json (
      options.service_url,
      "/location/objects?kind=actor&pageSize=1&continuationToken="
        + actor_page_one.at ("continuationToken").get<std::string> (),
      status);
    ensure (status == 200 && actor_page_two.at ("items").size () == 1,
            "MON-A6 Actor continuation page mismatch: "
              + actor_page_two.dump ());

    std::string actor_recovery_id;
    std::string actor_other_id;
    int actor_overflow_status = 0;
    nlohmann::json actor_overflow_response;
    for (int index = 0; index < 64; ++index) {
        const auto candidate = "mon-a6-recovery-actor-" + std::to_string (index);
        const auto over = post_json (options.service_url,
                                     "/actor/create?actorId=" + candidate,
                                     status);
        actor_overflow_status = status;
        actor_overflow_response = over;
        if (status == 409 && over.value ("error", "") == "capacity_exceeded") {
            actor_recovery_id = candidate;
            break;
        }
        if (status < 400 && actor_other_id.empty ())
            actor_other_id = candidate;
    }
    ensure (!actor_recovery_id.empty (),
            "MON-A6 Actor capacity failure was not typed status="
              + std::to_string (actor_overflow_status) + " body="
              + actor_overflow_response.dump ());
    std::string spot_recovery_id;
    std::string spot_other_id;
    int spot_overflow_status = 0;
    nlohmann::json spot_overflow_response;
    for (int index = 0; index < 64; ++index) {
        const auto candidate = "mon-a6-recovery-spot-" + std::to_string (index);
        const auto over = post_json (options.service_url,
                                     "/spot/create?spotId=" + candidate,
                                     status);
        spot_overflow_status = status;
        spot_overflow_response = over;
        if (status == 409 && over.value ("error", "") == "capacity_exceeded") {
            spot_recovery_id = candidate;
            break;
        }
        if (status < 400 && spot_other_id.empty ())
            spot_other_id = candidate;
    }
    ensure (!spot_recovery_id.empty (),
            "MON-A6 Spot capacity failure was not typed status="
              + std::to_string (spot_overflow_status) + " body="
              + spot_overflow_response.dump ());

    post_json (options.service_url, "/actor/delete?actorId=" + actor_one_id, status);
    post_json (options.service_url,
               "/admin/subject/close?spotId="
                 + spot_one.at ("spotId").get<std::string> (),
               status);
    wait_placement (options.service_url, 1, 1, true);

    const auto actor_two = post_json (options.service_url,
                                      "/actor/create?actorId=" + actor_recovery_id,
                                      status);
    ensure (status < 400 && actor_two.at ("status") == "created",
            "MON-A6 Actor creation did not recover after removal");
    const auto spot_two = post_json (options.service_url,
                                     "/spot/create?spotId=" + spot_recovery_id,
                                     status);
    ensure (status < 400 && spot_two.contains ("spotId"),
            "MON-A6 Spot creation did not recover after removal");
    wait_placement (options.service_url, 2, 2, false);
    post_json (options.service_url,
               "/actor/delete?actorId=" + actor_recovery_id,
               status);
    post_json (options.service_url,
               "/actor/delete?actorId=" + actor_fill_id,
               status);
    post_json (options.service_url,
               "/admin/subject/close?spotId="
                 + spot_two.at ("spotId").get<std::string> (),
               status);
    post_json (options.service_url,
               "/admin/subject/close?spotId=" + spot_fill_id,
               status);
    if (!actor_other_id.empty ())
        post_json (options.service_url,
                   "/actor/delete?actorId=" + actor_other_id,
                   status);
    if (!spot_other_id.empty ())
        post_json (options.service_url,
                   "/admin/subject/close?spotId=" + spot_other_id,
                   status);
    std::cout << "scenario MON-A6 passed\n";
}

} // namespace zlink::framework::e2e::runtime_monitoring::client
