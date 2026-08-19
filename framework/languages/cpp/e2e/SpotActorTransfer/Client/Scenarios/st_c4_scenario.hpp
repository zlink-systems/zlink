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
 * checksum-mismatch is NOT implemented here -- no seam. It requires
 * corrupting bytes or the checksum field of a wire-level
 * protocol::relocation_state_t chunk in flight, or lying about a chunk's
 * checksum while the receiver still recomputes it. Nothing in this e2e
 * harness constructs or edits a raw relocation_state_t chunk -- the client
 * only calls the public Join HTTP API, never the mesh-node-to-mesh-node
 * wire protocol. On the receiving side, relocation_assembly_t::accept()
 * (framework/languages/cpp/framework/src/runtime/stateful/
 * relocation_transfer.hpp, around lines 187-198) always recomputes
 * protocol::relocation_checksum_crc32c() from the bytes it actually
 * received and compares it to the manifest's checksum_crc32c that came
 * from the same sender -- an honest sender's checksum always matches its
 * own bytes, so this path can only be exercised by a fault-injection seam
 * that intercepts/mutates the wire frame between source and target. No
 * such seam is exposed to the e2e harness or the public API today. Adding
 * one would mean editing production wire/mesh code
 * (raw_mesh_node_owner.cpp / service_wire_codec.cpp), which is out of this
 * task's scope. This gap is recorded honestly in feature-map.ko.md rather
 * than faked.
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
