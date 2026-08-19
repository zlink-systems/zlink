/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"
#include "../Support/relocation_client_support.hpp"

namespace
{

/* SF-F2: the client owns this scenario's public requests and assertions.
 *
 * Variant 1 (long-running): capture is held open on the source past several
 * owner-lease renew intervals (renew interval is 1s in this harness), then
 * released -- the relocation must still complete and preserve state.
 *
 * Variant 2 (failed call is not auto-resumed): a fresh object's relocation
 * is made to fail through an explicit target failure before the
 * relay-ready reply (df-fail-transfer-in: the target's restore() throws
 * before target admission commits). Like SpotActorTransfer's join_target,
 * this node's /actors/{id}/relocate always replies accepted=true
 * synchronously (deferred, asynchronous relocation) -- the failure is only
 * observable through the target's transfer_in_failed evidence and the
 * Actor's location staying at the source, not through the relocate() HTTP
 * reply. The failed operation must keep its source location/state with no
 * automatic retry. A distinct, fresh object ("a new call the Application
 * starts") is then relocated and must complete normally -- demonstrating
 * the failed operation's ID never silently completes at the target while
 * a genuinely new operation does.
 *
 * KNOWN DEFECT, not designed around: per Spec 28's explicit-failure rule,
 * an explicit failure reply must restore the source's memory/queue, and
 * the Actor must remain live and servable on the source afterward. In this
 * harness, issuing a plain request back to the failed actor right after
 * transfer_in_failed (get_actor_ref/probe on node_a) hangs indefinitely --
 * reproduced manually, never returning even past client_support.hpp's 10s
 * default timeout (so that timeout is not actually bounding the full
 * request-response wait; only the wrapper timeout added below does). This
 * IS the contract under test, so the probe stays in the scenario with a
 * hard client-side timeout so the harness fails fast instead of hanging;
 * the feature-map row records this as an open, reported defect rather
 * than working around it. */
inline void run_sf_f2_scenario (const options_t &options)
{
    namespace df = zlink::e2e::store_failure_relocation::client;
    using namespace zlink::e2e::store_failure_relocation;

    const auto &node_a = options.relocation_node_a_url;
    const auto &node_b = options.relocation_node_b_url;

    // Variant 1: long-running relocation.
    {
        const auto spot =
          df::create_spot_until_placed_on (node_b, "sf-f2-long-" + sf_client::unique_marker ("v1"),
                                           "df-b");
        const auto actor =
          df::create_actor_until_placed_on (node_a, "actor-df-hold-" + sf_client::unique_marker ("v1"),
                                            actor_type_hold_capture, 32, "df-a");
        const auto &actor_id = actor.actor_id;

        auto relocate_task = std::async (std::launch::async, [&] {
            return df::relocate (node_a, actor_id, "SF-F2", spot.spot_id);
        });
        df::wait_evidence (node_a, {"|" + actor_id + "|capture_held|"});
        // Hold well past several owner-lease renew intervals (1s each in
        // this harness) but under the relocate call's deadline.
        std::this_thread::sleep_for (std::chrono::milliseconds (3500));
        df::release_capture_gate (node_a, actor_id);

        const auto result = relocate_task.get ();
        sf_client::ensure (result.accepted, "SF-F2 long-running relocation was not accepted");
        df::wait_evidence (node_b, {"|" + actor_id + "|transfer_in|", "|" + actor_id + "|joined|"});
        const auto probed = df::probe (node_b, actor_id, "SF-F2", "after-long-running");
        sf_client::ensure (probed.node_rid == "df-b",
                           "SF-F2 long-running relocation did not land on the target");
        sf_client::ensure (probed.payload_length == 32,
                           "SF-F2 long-running relocation lost payload length");
    }

    // Variant 2: explicit target failure keeps the source, no auto-resume;
    // a distinct new call completes normally.
    {
        const auto spot =
          df::create_spot_until_placed_on (node_b, "sf-f2-fail-" + sf_client::unique_marker ("v2"),
                                           "df-b");
        const auto failed_actor =
          df::create_actor_until_placed_on (node_a,
                                            "actor-df-fail-in-" + sf_client::unique_marker ("v2"),
                                            actor_type_fail_transfer_in, 24, "df-a");
        const auto &failed_actor_id = failed_actor.actor_id;

        const auto failed_result = df::relocate (node_a, failed_actor_id, "SF-F2", spot.spot_id);
        sf_client::ensure (failed_result.accepted,
                           "SF-F2 relocate() reply changed shape -- expected synchronous "
                           "accepted=true (deferred), the failure surfaces asynchronously");
        df::wait_evidence (node_b, {"|" + failed_actor_id + "|transfer_in_failed|"});

        // This IS the contract under test (Spec 28: an explicit failure
        // reply must restore the source's memory/queue and the Actor must
        // remain live and servable on the source afterward) -- bounded
        // hard so a defect here fails the scenario instead of hanging it.
        const auto ref_after_failure = df::call_with_hard_timeout<df::actor_ref_res_t> (
          [&] { return df::get_actor_ref (node_a, failed_actor_id); },
          std::chrono::seconds (15), "SF-F2 get_actor_ref(source) after explicit target failure");
        sf_client::ensure (ref_after_failure.node_rid == "df-a",
                           "SF-F2 failed operation should keep the source location");
        const auto probed_source = df::call_with_hard_timeout<probe_state_res_t> (
          [&] { return df::probe (node_a, failed_actor_id, "SF-F2", "after-failed-call"); },
          std::chrono::seconds (15), "SF-F2 probe(source) after explicit target failure");
        sf_client::ensure (probed_source.payload_length == 24,
                           "SF-F2 failed operation should keep its source state");

        const auto new_actor =
          df::create_actor_until_placed_on (node_a,
                                            "actor-df-new-call-" + sf_client::unique_marker ("v2"),
                                            actor_type_stateful, 24, "df-a");
        const auto &new_actor_id = new_actor.actor_id;
        const auto new_result = df::relocate (node_a, new_actor_id, "SF-F2", spot.spot_id);
        sf_client::ensure (new_result.accepted, "SF-F2 new call after a failure was not accepted");
        // Wait for the join to complete (not just the payload arrival) before
        // asserting the committed location, exactly like variant 1: the
        // location commit precedes the joined callback, so `joined` evidence
        // is the earliest point the target ref is stable to read.
        df::wait_evidence (node_b, {"|" + new_actor_id + "|transfer_in|",
                                    "|" + new_actor_id + "|joined|"});
        const auto ref_new = df::get_actor_ref (node_b, new_actor_id);
        sf_client::ensure (ref_new.node_rid == "df-b",
                           "SF-F2 new call did not complete at the target");
    }

    std::cout << "scenario SF-F2 passed\n";
}

} // namespace
