/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"
#include "../Support/relocation_client_support.hpp"

namespace
{

/* SF-F7: the client owns this scenario's public requests and assertions.
 *
 * The relocation node role is started with a small
 * relocation_payload_chunk_limit_bytes / relocation_in_flight_payload_budget_bytes
 * (see run_e2e.sh -- both are ordinary host-configurable
 * location_options_t fields, framework/languages/cpp/framework/include/
 * zlink/framework/contracts/locations/options.hpp), so KB-scale fixtures
 * genuinely exercise the chunk and in-flight-budget boundaries instead of
 * needing multi-megabyte payloads in an e2e run. */
inline void run_sf_f7_scenario (const options_t &options)
{
    namespace df = zlink::e2e::store_failure_relocation::client;
    using namespace zlink::e2e::store_failure_relocation;

    const auto &node_a = options.relocation_node_a_url;
    const auto &node_b = options.relocation_node_b_url;

    // Must match the relocationChunkLimitBytes / relocationInFlightBudgetBytes
    // this scenario's run_e2e.sh config passes to both relocation nodes.
    constexpr std::uint64_t chunk_limit_bytes = 4096;
    constexpr std::uint64_t in_flight_budget_bytes = 8192;

    const std::vector<std::pair<std::string, std::uint64_t>> fixtures{
      {"at-chunk-limit", chunk_limit_bytes},
      {"one-byte-over-chunk-limit", chunk_limit_bytes + 1},
      {"over-in-flight-budget", in_flight_budget_bytes + chunk_limit_bytes + 1},
    };

    for (const auto &[label, length] : fixtures) {
        const auto spot = df::create_spot_until_placed_on (
          node_b, "sf-f7-" + label + "-" + sf_client::unique_marker (""), "df-b");
        const auto actor = df::create_actor_until_placed_on (
          node_a, "actor-df-large-" + label + "-" + sf_client::unique_marker (""),
          actor_type_stateful, length, "df-a");
        const auto &actor_id = actor.actor_id;

        const auto expected_checksum = crc32c (deterministic_payload (length));

        relocate_res_t result;
        try {
            result = df::relocate (node_a, actor_id, "SF-F7", spot.spot_id,
                                   std::chrono::seconds (40));
        }
        catch (const std::exception &error) {
            sf_client::ensure (
              false, "SF-F7 " + label + " relocation should not fail on a size cap: "
                       + error.what ());
        }
        sf_client::ensure (result.accepted, "SF-F7 " + label + " relocation was not accepted");

        df::wait_evidence (node_b, {"|" + actor_id + "|transfer_in|"},
                           std::chrono::seconds (30));
        const auto probed = df::probe (node_b, actor_id, "SF-F7", label);
        sf_client::ensure (probed.node_rid == "df-b",
                           "SF-F7 " + label + " did not land on the target");
        sf_client::ensure (probed.payload_length == length,
                           "SF-F7 " + label + " length mismatch at the target");
        sf_client::ensure (probed.payload_checksum == expected_checksum,
                           "SF-F7 " + label + " checksum mismatch at the target");
    }

    std::cout << "scenario SF-F7 passed\n";
}

} // namespace
