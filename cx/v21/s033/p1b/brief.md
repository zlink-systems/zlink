Model gpt-6.1-sol, effort high: the receive_record count test must be re-routed through a public path or the existing test-only probe boundary without weakening its assertions — a design judgment, not only mechanical edits.

# Task: #1639 step 1 — make the C++ Framework unit tests compile against the new receive record

Earlier job briefs are history, not instructions.

## Situation
- Worktree /root/worktree/zlink-v21-s033/p1, branch perf/1639-cpp-p1-receive-record @ 561680f5cc. Product code compiles (PM's -k 0 build on main merged in). Step 1 design and what changed: /root/jobs/fx/cx/v21/s033/p1/report.md.
- The PM's -k 0 build: 28 errors, all in 7 test files. List with file:line: /root/jobs/fx/cx/v21/s033/p1b/input/pm-k0-report.md. Four kinds:
  1. service_mailbox_record_t built by brace initialisation in old test code (reply_submit_claim :322 :583 :614; m6a_runtime :1751–1834).
  2. parts / reply_token used as fields; they are now accessor functions (m6a_runtime :3093 :3096 :3156 :3417; mesh_metrics :563 :566; client_server_runtime :1340; route_mesh_weight_update :780).
  3. instance_spot_activation :1405–1407 syntax error.
  4. New test receive_record :397 calls private raw_mesh_node_owner_t::send_control.

## Do
1. Fix kinds 1–3 by changing only how records are built or read (use the record's constructor/factory or accessors that the product already has). Keep every assertion and expected value unchanged; do not delete or skip tests.
2. Kind 4: observe the same count through a public path or the existing ZLINK_FRAMEWORK_TEST_CREATION_PROBE boundary. Do not add friend declarations, public wrappers, or any new private access. The negative control must still fail when the counted step is reintroduced; say in the report how.
3. grep every test and sample under framework/languages/cpp for the same stale patterns (brace-built service_mailbox_record_t, .parts / .reply_token / ->parts / ->reply_token on that type) and fix them too, so the next build has no repeat of these kinds. Type-check each hit by reading its declaration; other types with a field named parts are not in scope.
4. bash scripts/format/format.sh --check cpp must pass (use format.sh cpp to fix).

## Rules
- Do NOT build or run tests, and take no lock: a Core release gate is measuring on this machine. The PM compiles on their machine after the supervisor pushes. Work by reading declarations carefully.
- Edit only test and sample files in the worktree; product code (framework/languages/cpp/framework/src) only if a test truly cannot be written otherwise — then stop and write why in the report instead of editing.
- Do not start sub-agents and do not call any other model or AI tool.
- No git operations except git diff, git status, git log, git show. No commit, no push, no spec edits, no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not read other job folders except p1/report.md.
- 리팩토링 점검: list test helpers that now duplicate a product record factory, with file:line.
- Time box about 2 hours. Keep your own output short.

## Output: /root/jobs/fx/cx/v21/s033/p1b/report.md (Korean, plain sentences)
(1) per error: file:line, change; (2) extra hits fixed in step 3; (3) receive_record test: new observation path and how the negative control still fails; (4) format result; (5) 리팩토링 점검.
