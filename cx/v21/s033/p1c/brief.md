Model gpt-6-luna, effort high: mechanical test refactor — replace eight hand-written record assemblies with the existing product factory; no design choice is delegated.

# Task: #1639 step 1 — test helpers build service_mailbox_record_t through from_parts

Earlier job briefs are history, not instructions.

## Situation
- Worktree /root/worktree/zlink-v21-s033/p1, branch perf/1639-cpp-p1-receive-record @ 4b15c79159 (clean). The PM is building this commit; do not touch anything else.
- Factory: service_mailbox_record_t::from_parts, framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.hpp:169 (owner, domain, parts, source, correlation, generation, operation, bound_session, before_application_handler).
- The PM asked (AGENTS.md §3, refactoring is part of the change) that these eight test helpers, which assemble the same fields by hand, use from_parts instead. T = framework/languages/cpp/tests/Zlink.Framework.UnitTests/:
  - T/test_cpp_framework_mailbox_fifo_cost.cpp:25 record()
  - T/test_cpp_framework_receive_allocations.cpp:57 infrastructure_record()
  - T/test_cpp_framework_claim_dispatch_cost.cpp:96 enqueue() (raw_request branch)
  - T/test_cpp_framework_actor_maintenance_join.cpp:3592 actor_record()
  - T/test_cpp_framework_relocation_seal_contract.cpp:190 spot_send() (keep its source_original setting after the factory call)
  - T/test_cpp_framework_relocation_seal_contract.cpp:1596 record lambda
  - T/test_cpp_framework_receive_allocations.cpp:393 enqueue lambda (keep its envelope_header setting after the factory call)
  - T/test_cpp_framework_local_enqueue_allocations.cpp:790 enqueue lambda

## Do
1. In each helper, replace the hand-written field assembly with one from_parts call carrying the same values; fields from_parts does not take stay as separate assignments after it. Keep every assertion and expected value unchanged.
2. Allocation-count tests (receive_allocations, local_enqueue_allocations, claim_dispatch_cost, mailbox_fifo_cost): check that the from_parts call happens at the same point relative to the counted window as the old assembly (outside stays outside, inside stays inside), and that from_parts does not add a copy the old code avoided (it takes parts by value: pass with std::move where the old code moved). Write the check per helper in the report.
3. bash scripts/format/format.sh --check cpp must pass (use format.sh cpp to fix). git diff --check must pass.

## Rules
- Do NOT build or run tests and take no lock (a Core release gate is measuring here; the PM builds).
- Edit only those test files. Do not start sub-agents and do not call any other model or AI tool.
- No git operations except git diff, git status, git log, git show. No commit, no push, no spec edits, no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not read other job folders.
- 리팩토링 점검: after the change, say whether any other hand assembly of this type remains under tests/ (file:line) — fix it in the same way if it is the same pattern.
- Time box about 1 hour. Keep your own output short.

## Output: /root/jobs/fx/cx/v21/s033/p1c/report.md (Korean, plain sentences)
(1) per helper: file:line, change, counted-window check; (2) any further hits; (3) format result.
