Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: Node ladder L5~L7 — target receives 0 while source sends complete. Before calling it a Framework defect, run the two missing controls and find where the message stops. Fix only if it is a bench fault.

Worktree /root/worktree/zlink-v21-s033/lad-node, branch exp/4350-node-ladder, tip 7d1afa98a9. Commit locally only; never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh. Never delete raw files.
Read first: /root/jobs/fx/cx/v21/s033/node-l57f/report.md (previous job: wiring checked, reproduce commands), common-rules.md (incl. LOAD TESTS DURING OTHER HOLDS), ladder-steps.md.

## Missing controls (supervisor review of node-l57f)
1. Main Framework path control: run the same smoke (send-saturation 1K, request-serial 1K) WITHOUT the L5_7 switches, i.e. the regular zlink-framework-node bench with the protobuf codec registered. If that delivers (received = completed), the difference is what L5_7 changed: the codec (protobuf registration removed → default JSON) and the DTO (plain BenchPayload class with a Buffer body).
2. Unhandled / undecodable dispatch: Framework drops a message that finds no handler silently by default (Java's bench comments say the same; check Node's equivalent setting, e.g. the unhandled dispatch action / message-flow logging in the Node Framework configuration, with file:line). Turn on the action or logging that reports unhandled and decode failures on the target, and record what the target does with each arriving message: no handler match (packet name / message type key differs between JSON and protobuf registration?), JSON decode failure (Buffer body serialized as {type:'Buffer',data:[...]} vs base64?), or never arrives.
Also check: the content type the source puts on the envelope at L5_7 and the content type the target handler registration expects.

## Do
Each smoke run inside your own exclusive hold (flock -x /root/jobs/fx/cx/measure.lock, at most 5 minutes per hold), pin source 0-5 / target 8-13; reuse node-l57f's commands.
Write /root/jobs/fx/cx/v21/s033/node-l57g/report.md: control results, where the message stops with file:line, verdict:
- bench fault (e.g. DTO/codec/registration mismatch the bench introduced): fix in one commit "bench(node): fix ladder L5_7 <what> (#4355)", npm test, smoke of all 6 cells (one short run each, own hold at most 10 min) with errors 0 and submitted = completed = received.
- Framework behaviour that the spec defines (silent drop of unhandled sends): quote the spec file:line, fix the bench so it registers what it sends, and note it.
- Framework defect (a decodable, handled message is lost; or a decode failure that the spec says must be reported is dropped silently): stop, write the exact reproduce command and production file:line; no workaround.
Time cap 2 h.
