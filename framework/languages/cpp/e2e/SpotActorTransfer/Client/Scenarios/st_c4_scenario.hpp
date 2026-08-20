/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-C4: this file owns the scenario orchestration and its public assertions.
 *
 * Direct-Transfer Integrity Failure has two variants in the common doc:
 *   - checksum-mismatch: a chunk arrives with a checksum that does not match
 *     its bytes.
 *   - exact-identity-conflict: a different length/checksum arrives for the
 *     same exact relocation identity.
 *
 * checksum-mismatch is NOT implemented as a role-server E2E variant here --
 * the target-side contract clause it exercises (spec 28 §12: "On a checksum
 * mismatch, the target doesn't proceed to CAS, doesn't restore from a
 * partially assembled payload, and responds with an explicit failure
 * reply") is covered directly at the unit level instead --
 * verify_relocation_assembly_rejects_checksum_mismatch in
 * tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp feeds
 * relocation_state_assembly_t::accept() (relocation_transfer.hpp) a chunk
 * whose bytes don't hash to its own manifest's checksum_crc32c and asserts
 * the conflict result with nothing left to restore from.
 *
 * The E2E variant needs a chunk to actually arrive corrupted on the wire
 * between two live role-server processes, which needs a fault-injection
 * seam this harness doesn't have yet. The seam location is known now,
 * though, which the old version of this comment didn't have: in
 * mesh_node_runtime.cpp's relocate_application_actor, the
 * .send_state_chunk lambda (around line 1165) holds the fully-built
 * protocol::relocation_state_t in the source process, in memory, before
 * encoding -- flipping one byte of chunk.chunk_data there is a clean,
 * wire-framing-free corruption with the exact identity intact (so the
 * target's assembly links it as a conflict, not an ignored mismatched-
 * identity chunk). What's missing is a way to arm that one-shot, scoped to
 * a single Join, from the e2e client process: it would need a new
 * debug-only method on public_host_runtime_t together with a way to reach
 * it from Client/main.cpp, which -- because the client and the ActorNode
 * are separate processes -- means either a new field on the framework
 * host's public API surface (fw::app_t, framework/include) or a proxy
 * sitting on the actor-a<->actor-b mesh connection. Both were considered
 * and set aside for now: a new public API method that exists only to arm
 * test corruption crosses a contract boundary shared with every other
 * language binding for a single scenario's benefit, and a proxy is unsafe
 * here because ST-C4 runs inside the batched "all" run
 * (run_e2e.sh:246) sharing one live actor-a<->actor-b mesh connection with
 * a dozen other scenarios -- unlike ST-F3A's session-route proxy, which
 * gets its own isolated run. Recorded honestly in feature-map.ko.md rather
 * than faked; see that file for the same note.
 *
 * exact-identity-conflict IS implemented below: two Join calls for the
 * very same Actor to the very same target Spot are started concurrently
 * through the public Join HTTP API. Empirically (confirmed by evidence
 * logs, not assumed): exactly one call gets the normal "accepted,
 * deferred" synchronous reply and later an asynchronous
 * join_completion_accepted; the other gets an explicit, SYNCHRONOUS
 * rejection in its own HTTP reply -- accepted=false, error_kind
 * "FrameworkError:4" (framework_error_kind_t::rejected; this node's
 * error_kind_name() only gives NotFound/Unavailable/TimeoutException
 * their own names, everything else falls to "FrameworkError:<kind>"), and
 * the evidence text "Actor join is already reserved or moving" -- with no
 * retry and no partial restore. */
inline void scenario_runner_t::run_st_c4_scenario ()
{
    const auto spot = create_spot_until_placed_on (
      _nodes.b, "spot-identity-conflict-" + unique_suffix (),
      "actor-b");
    const auto actor = create_actor_until_placed_on (
      _nodes.a, "actor-identity-conflict-" + unique_suffix (),
      e2e::actor_type_stateful, 84, "actor-a");
    const auto &actor_id = actor.actor_id;
    const auto &spot_id = spot.spot_id;

    auto join_first = std::async (
      std::launch::async,
      [&] () -> std::optional<e2e::join_target_res_t> {
          try {
              return join_actor (_nodes.a, actor_id, {"ST-C4", spot_id});
          }
          catch (const std::exception &) {
              return std::nullopt;
          }
      });
    auto join_second = std::async (
      std::launch::async,
      [&] () -> std::optional<e2e::join_target_res_t> {
          try {
              return join_actor (_nodes.a, actor_id, {"ST-C4", spot_id});
          }
          catch (const std::exception &) {
              return std::nullopt;
          }
      });

    const auto first = join_first.get ();
    const auto second = join_second.get ();
    require (first.has_value () && second.has_value (),
             "ST-C4 a concurrent Join call failed at the transport level instead "
             "of returning an explicit application-level result.");

    /* Exactly one call is accepted (deferred, asynchronous relocation);
     * the other is rejected synchronously, in its own HTTP reply, with the
     * exact-identity conflict's error kind -- not a timeout, not a silent
     * drop, and not both accepted. */
    const int accepted_replies =
      (first->accepted ? 1 : 0) + (second->accepted ? 1 : 0);
    require (accepted_replies == 1,
             "ST-C4 expected exactly one concurrent Join call to be accepted, got "
               + std::to_string (accepted_replies));
    const auto &loser_reply = first->accepted ? *second : *first;
    // error_kind_name() maps framework_error_kind_t::rejected (4) to the
    // "FrameworkError:4" default case -- only NotFound/Unavailable/
    // TimeoutException get dedicated names in this node's handler.
    require (loser_reply.error_kind == "FrameworkError:4",
             "ST-C4 the loser's synchronous rejection had an unexpected error kind: "
               + loser_reply.error_kind);

    // The on_join_completed() "accepted" evidence line is not reliably
    // observed on this node even for a Join that demonstrably completes
    // (message_flow's own source_cleanup fires and the target holds the
    // Actor) -- so the completion signal used here is source_cleanup, the
    // same terminal-completion marker ST-B1/ST-C2/etc. already rely on.
    wait_evidence (_nodes.a, {"message_flow|" + actor_id + "|source_cleanup|"});
    const auto source_evidence = get_evidence (_nodes.a);

    /* The loser's failure must be the exact-identity conflict rejection,
     * not a timeout or an unrelated error, and it must carry no automatic
     * retry/resend evidence. */
    require (
      evidence_contains (
        source_evidence,
        "ST-C4|" + actor_id
          + "|join_failed|FrameworkError:4:Actor join is already reserved or moving"),
      "ST-C4 the loser's failure was not the expected exact-identity rejection.");

    /* No node may show evidence of restoring from a partial or conflicting
     * payload, and the single winner must have committed cleanly to the
     * target with no ambiguity about current location. */
    require_no_contains (
      get_evidence (_nodes.b), "|" + actor_id + "|conflict_restore|",
      "ST-C4 target restored from a conflicting/partial payload.");
    const auto ref = get_actor_ref (_nodes.b, actor_id);
    require (ref.node_rid == "actor-b",
             "ST-C4 the single accepted Join did not commit to the target.");
    const auto probe = probe_actor (_nodes.b, actor_id, {"ST-C4", "after-identity-conflict"});
    require (probe.marker == "after-identity-conflict",
             "ST-C4 target Actor did not process requests after the single accepted Join.");
}

} // namespace
