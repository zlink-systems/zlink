Model gpt-6-luna, effort high: mechanical test refactor — replace fourteen hand-written record assemblies with the existing product factory; no design choice is delegated.

# Task: #1639 step 1 — the remaining test records that carry parts are built through from_parts

Earlier job briefs are history, not instructions.

## Situation
- Worktree /root/worktree/zlink-v21-s033/p1, branch perf/1639-cpp-p1-receive-record @ 5a931c498d (clean). The PM builds an earlier commit; edit only the files listed below.
- Factory: service_mailbox_record_t::from_parts, framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.hpp:169 (owner, domain, parts, source, correlation, generation, operation, bound_session, before_application_handler).
- The PM approved: every test record that sets parts is built with from_parts; local application records without parts stay as they are; no new product factory. Fourteen sites remain (T = framework/languages/cpp/tests/Zlink.Framework.UnitTests/), variable name after the line:
  - T/test_cpp_framework_reply_submit_claim.cpp:446 record
  - T/test_cpp_framework_relocation_seal_contract.cpp:1645 original, :1676 original, :2919 record, :3489 message
  - T/test_cpp_framework_mesh_node_vertical.cpp:889 node_send_record
  - T/test_cpp_framework_m6b_runtime.cpp:5561 record, :5632 record
  - T/test_cpp_framework_execution.cpp:1690 record
  - T/test_cpp_framework_claim_dispatch_cost.cpp:440 malformed, :508 original, :539 original, :744 record
  - T/test_cpp_framework_actor_maintenance_join.cpp:4615 malformed
  If a site sets parts only conditionally, or later than other steps a test depends on, leave it and say why.

## Do
1. At each site, replace the hand-written field assembly with one from_parts call carrying the same values; fields from_parts does not take stay as separate assignments after it. Keep every assertion and expected value unchanged.
2. Allocation-count tests (claim_dispatch_cost and any other site inside an allocation or cost sample): check that the from_parts call happens at the same point relative to the counted window as the old assembly (outside stays outside, inside stays inside), and that from_parts does not add a copy the old code avoided (it takes parts by value: pass with std::move where the old code moved). Write the check per site in the report.
3. bash scripts/format/format.sh --check cpp must pass (use format.sh cpp to fix). git diff --check must pass.

## Rules
- Do NOT build or run tests and take no lock (a Core release gate is measuring here; the PM builds).
- Edit only those test files. Do not start sub-agents and do not call any other model or AI tool.
- No git operations except git diff, git status, git log, git show. No commit, no push, no spec edits, no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not read other job folders.
- 리팩토링 점검: after the change, say whether any other hand assembly of this type remains under tests/ (file:line) — fix it in the same way if it is the same pattern.
- Time box about 1 hour. Keep your own output short.

## Output: /root/jobs/fx/cx/v21/s033/p1d/report.md (Korean, plain sentences)
(1) per site: file:line, change, counted-window check; (2) any further hits; (3) format result.
