/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"
#include "../Support/relocation_client_support.hpp"

namespace
{

/* SF-F11: the client owns this scenario's public requests and assertions.
 *
 * Common variant only: A's direct transfer fails explicitly before the
 * relay-ready reply (df-fail-transfer-out: capture() throws immediately on
 * the source, before any chunk is sent), and B's independent relocation is
 * then run with a distinct deterministic payload. Like every relocate()
 * call in this harness, the HTTP reply itself is always accepted=true
 * synchronously (deferred, asynchronous relocation) -- the explicit
 * failure is only observable through transfer_out_failed evidence and A's
 * location staying at the source, not through the relocate() HTTP reply.
 * This scenario does not implement the doc's additional "supported-
 * language cancels a pending waiter" sub-variant -- the doc itself scopes
 * that sub-variant to "supported languages" only ("runs only where the
 * exact interface supports it"), and this harness does not add a
 * waiter-cancellation seam to the trimmed relocation node role; the
 * feature-map row records this. */
inline void run_sf_f11_scenario (const options_t &options)
{
    namespace df = zlink::e2e::store_failure_relocation::client;
    using namespace zlink::e2e::store_failure_relocation;

    const auto &node_a = options.relocation_node_a_url;
    const auto &node_b = options.relocation_node_b_url;

    constexpr std::uint64_t payload_length_a = 20;
    constexpr std::uint64_t payload_length_b = 36;

    const auto spot =
      df::create_spot_until_placed_on (node_b, "sf-f11-" + sf_client::unique_marker (""), "df-b");

    const auto actor_a = df::create_actor_until_placed_on (
      node_a, "actor-df-fail-out-" + sf_client::unique_marker (""), actor_type_fail_transfer_out,
      payload_length_a, "df-a");
    const auto &actor_a_id = actor_a.actor_id;

    const auto actor_b = df::create_actor_until_placed_on (
      node_a, "actor-df-stateful-" + sf_client::unique_marker (""), actor_type_stateful,
      payload_length_b, "df-a");
    const auto &actor_b_id = actor_b.actor_id;

    const auto result_a = df::relocate (node_a, actor_a_id, "SF-F11", spot.spot_id);
    sf_client::ensure (result_a.accepted,
                       "SF-F11 relocate() reply changed shape -- expected synchronous "
                       "accepted=true (deferred), the failure surfaces asynchronously");
    df::wait_evidence (node_a, {"|" + actor_a_id + "|transfer_out_failed|"});

    const auto ref_a = df::get_actor_ref (node_a, actor_a_id);
    sf_client::ensure (ref_a.node_rid == "df-a",
                       "SF-F11 A should be restored from source memory after the transfer failure");
    const auto probed_a = df::probe (node_a, actor_a_id, "SF-F11", "after-transfer-failure");
    const auto expected_checksum_a = crc32c (deterministic_payload (payload_length_a));
    sf_client::ensure (probed_a.payload_length == payload_length_a
                         && probed_a.payload_checksum == expected_checksum_a,
                       "SF-F11 A's payload changed after the transfer failure");

    const auto result_b = df::relocate (node_a, actor_b_id, "SF-F11", spot.spot_id);
    sf_client::ensure (result_b.accepted, "SF-F11 B's independent relocation was not accepted");
    df::wait_evidence (node_b, {"|" + actor_b_id + "|transfer_in|"});
    const auto probed_b = df::probe (node_b, actor_b_id, "SF-F11", "after-b-relocation");
    const auto expected_checksum_b = crc32c (deterministic_payload (payload_length_b));
    sf_client::ensure (probed_b.node_rid == "df-b",
                       "SF-F11 B did not land on the target");
    sf_client::ensure (probed_b.payload_length == payload_length_b
                         && probed_b.payload_checksum == expected_checksum_b,
                       "SF-F11 B's target checksum does not exactly match B -- possible mixing "
                       "with A's bytes");

    std::cout << "scenario SF-F11 passed\n";
}

} // namespace
