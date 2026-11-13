Model gpt-6.1-sol, effort high: supervisor review of an uncommitted product diff found a hot-path cost regression and an identity-semantics change; fixing them needs judgment on reply-path ownership.

# Task: #1639 step 1 PR #4155 — fix the supervisor's review findings on the uncommitted review-fix diff

Earlier job briefs are history, not instructions.

## Situation
- Worktree /root/worktree/zlink-v21-s033/p1, branch perf/1639-cpp-p1-receive-record @ 45ea3c721e plus an UNCOMMITTED diff (21 files, +263/−137) from the previous session. Keep that diff and work on top of it. Its report: input/p1r-report.md (read it first). The PM's review it answers: input/pr-review.md.
- The previous session made: replace_parts()/take_parts()/body_parts() so the service-header cache is cleared in one place; service_mailbox_reply_target_t holding optional routing id, reply token, route generation, correlation; reply_source() returning that target; reply functions in raw_mesh_node_owner taking `const service_mailbox_reply_target_t &`; friend merge.

## Supervisor review findings (fix all)
1. Hot-path cost regression. service_mailbox_reply_target_t has a non-explicit constructor from service_mailbox_record_t, and reply(), reply_failure(), reply_infrastructure() etc. now take `const service_mailbox_reply_target_t &`. Every existing call that passes a record now builds a temporary target per reply: optional<routing_id_t> copy plus reply_token_t copy (shared owner increment/decrement). Before, reply(record) copied nothing. This PR is a performance change; the immediate reply path must not gain copies or atomic operations. Fix so that replying with a record copies nothing (e.g. reply functions take a non-owning view of {routing id, token, route generation, correlation} built from either a record or a retained target, or overloads that forward references to one implementation). Only deferred replies (reply_source callers) may own a target. Make the target's constructor explicit so no implicit conversion can come back. Add a count test: an immediate reply through reply(record, ...) does zero allocations and zero token/RID copies in the counted window, with a negative control.
2. Identity semantics changed. Old checks used the LOGICAL `source_routing_id` (bytes): `source_routing_id.empty ()` guards and `source_routing_id == local.node_routing_id` for "is this my own node". The diff replaced them with the NATIVE routing_id() (falling back to logical only when native is absent), dropped the empty guards in prepare_relocation_ready_reply, reply_relocation_failed, reply and the actor-join path, and builds `zlink::routing_id_t::from (local.node_routing_id)` on every reply. Native RID and logical node identity are different values by design (the record's own comment: logical identity for topology/location, native for reply routing). Restore the decisions to their previous identity: the target must carry the logical source identity it needs (or the view must expose it) so local-node checks and empty guards keep their exact old meaning; native RID is used only for native reply routing. No per-reply construction of a routing_id_t for comparison. If you believe a check should change identity, do not change it — list it in the report with file:line and reason.
3. body_parts() returns `span(parts).subspan(1)`; that is undefined when parts is empty. Return an empty span when there is no prefix, and check every body_parts() caller for what it assumes.

## Do
- Fix 1–3, update callers and tests, keep the PM's original three requests satisfied.
- Report decision-site counts before/after for: identity used by each reply guard (logical vs native, per function), copies on immediate reply, copies on deferred reply.
- bash scripts/format/format.sh --check cpp and git diff --check must pass.
- For every call or declaration you change, count bracket pairs and arguments against the declaration (nothing can be compiled here).

## Rules
- Do NOT build or run tests and take no lock (a Core release gate is measuring here; the PM compiles).
- Do not start sub-agents and do not call any other model or AI tool.
- No git operations except git diff, git status, git log, git show. No commit, no push, no spec edits, no GitHub operations. Do not read Core C headers (core/include); binding C++ headers under bindings/cpp are fine. Do not run scripts/dev/internal-docs.sh. Do not read other job folders.
- No new queue, lock, timer, flag or thread; no test-only product API.
- 리팩토링 점검: any other place where native and logical identity are used interchangeably on the reply or ingress path, file:line.
- Time box about 2 hours. Keep your own output short.

## Output: /root/jobs/fx/cx/v21/s033/p1r2/report.md (Korean, plain sentences)
(1) fix 1 with counts and the new test/negative control; (2) fix 2: per reply function, identity used before/after the whole change, file:line; (3) fix 3 and caller assumptions; (4) bracket/argument check list for changed lines only; (5) format; (6) 리팩토링 점검.
