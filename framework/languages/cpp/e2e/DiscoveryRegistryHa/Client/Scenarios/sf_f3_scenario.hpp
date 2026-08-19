/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"
#include "../Support/relocation_client_support.hpp"

namespace
{

/* SF-F3: the client owns this scenario's public requests and assertions.
 *
 * The relocation node role wires the Relocation Store to its own Redis
 * container (see run_e2e.sh), separate from the Location Store's -- the
 * production framework already exposes these as distinct store interfaces
 * (add_location_store vs add_relocation_store /
 * zlink::framework::redis::redis_relocation_store_t), so "stop only the
 * Relocation Store" is directly expressible without a fault-injection
 * seam of any kind: it is just two containers, one of them stopped.
 *
 * Scope note: this scenario proves the core claim -- Relocate succeeds via
 * direct transfer while the Relocation Store is down, because the payload
 * itself never goes through the store. It does NOT additionally implement
 * the doc's pending-request-terminal-reconstruction sub-variant (a request
 * that completes after relocation, whose caller-visible result depends on
 * a store record and is only reconstructable after the store recovers) --
 * that needs bound-session/backlog machinery this trimmed-down relocation
 * node deliberately does not carry (SpotActorTransfer's ST-F3 exercises
 * that exact backlog path already). The feature-map row records this
 * honestly. */
inline void run_sf_f3_scenario (const options_t &options)
{
    namespace df = zlink::e2e::store_failure_relocation::client;
    using namespace zlink::e2e::store_failure_relocation;

    const auto &node_a = options.relocation_node_a_url;
    const auto &node_b = options.relocation_node_b_url;

    const auto spot =
      df::create_spot_until_placed_on (node_b, "sf-f3-" + sf_client::unique_marker (""), "df-b");
    const auto actor = df::create_actor_until_placed_on (
      node_a, "actor-df-store-outage-" + sf_client::unique_marker (""), actor_type_stateful, 48,
      "df-a");
    const auto &actor_id = actor.actor_id;

    sf_client::ensure (!options.relocation_redis_container.empty (),
                       "SF-F3 requires a dedicated relocation-store Redis container");
    sf_client::docker_action (options.relocation_redis_container, "stop -t 0");

    const auto expected_checksum = crc32c (deterministic_payload (48));

    bool relocated = false;
    try {
        const auto result = df::relocate (node_a, actor_id, "SF-F3", spot.spot_id);
        relocated = result.accepted;
    }
    catch (const std::exception &error) {
        sf_client::ensure (false,
                           std::string ("SF-F3 Relocate should succeed through direct transfer "
                                        "during a Relocation Store outage: ")
                             + error.what ());
    }
    sf_client::ensure (relocated,
                       "SF-F3 Relocate did not succeed while the Relocation Store was down");

    df::wait_evidence (node_b, {"|" + actor_id + "|transfer_in|", "|" + actor_id + "|joined|"});
    const auto probed = df::probe (node_b, actor_id, "SF-F3", "during-store-outage");
    sf_client::ensure (probed.node_rid == "df-b",
                       "SF-F3 target request did not succeed during the Relocation Store outage");
    sf_client::ensure (probed.payload_checksum == expected_checksum,
                       "SF-F3 target state was not restored correctly during the store outage");

    // Bring the store back so cleanup and any later scenario in the same
    // run see a healthy container.
    sf_client::docker_action (options.relocation_redis_container, "start");

    std::cout << "scenario SF-F3 passed\n";
}

} // namespace
