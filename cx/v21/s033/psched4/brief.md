Model gpt-6.1-sol, effort high: one decision moved from a string test to a typed kind, across producers.

# Task: decide the concurrent (Node/Channel) application owner from the typed owner kind, not the owner string (#1639 scheduler PR, PM review)

Earlier job briefs are history, not instructions. Worktree /root/worktree/zlink-v21-s033/psched, branch perf/1639-cpp-sched-drain, HEAD 471d0a8c75. Read `git diff origin/main` and /root/jobs/fx/cx/v21/s033/psched3/report.md first.

## Why (PM review)
`service_mailbox_t::concurrent_application_owner (std::string_view owner)` (framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:232) returns `owner == "node" || owner.starts_with ("channel:")`. If the owner key format changes, records silently take the gated path. The typed kind already exists: `host::owner_kind_t` and `service_mailbox_t::application_owner (host::owner_kind_t kind, std::string_view id)` (service_mailbox.cpp:105) which builds the key. The decision must use the typed kind, and stay in exactly one place.

## Do
1. Find every caller of concurrent_application_owner (service_mailbox.cpp:164 enqueue, :247 try_claim_application_record, mesh_node_host_service.cpp:3415 submit_owner, :3947 drain_application_owner) and where the owner string at each site comes from. Trace each producer of application records back to the point where the kind is known.
2. Carry the kind with the owner where it is created, so the decision reads a typed value. Prefer the smallest carrier that reaches all four sites: for example the owner queue (owner_queue_t) records the kind once at its first enqueue from a typed field on the record, and the ready callback / drain receive the kind from the mailbox or from their own typed construction (the Node service knows it serves the Node kind; the Channel service knows the Channel kind). Do not parse the string anywhere. Keep a single function that maps kind → concurrent (Node, Channel → true; Spot, Actor → false), and delete the string version.
3. A record whose kind is not set must not silently become gated or concurrent: decide it explicitly, fail loudly in debug or use the existing invalid_argument path, and say which in the report.
4. Update the tests that construct owners by string so they set the kind (grep tests for "node" and "channel:" owners used with the application domain). Keep every assertion and negative control unchanged otherwise.
5. Give the decision-owner table before/after: sites that decide concurrent vs gated (target: one), and the count of new fields/parameters added and removed (reject anything that adds a flag without removing one; a typed kind replacing a string test is the exchange).

## Rules
- Do not build or run tests: this machine's measurement lock is held for hours; the PM compiles. Write code only, then read your diff line by line: brackets, call arguments, includes, every name/signature you use (grep it), every virtual override. Run `bash scripts/format/format.sh --check cpp` and fix what it reports.
- Read and write only that worktree and /root/jobs/fx/cx/v21/s033/psched4/. No spec edits, no git operations except diff/status/log/show, no GitHub, no sub-agents or other AI tools. Do not read core/include. SIGTERM/SIGINT only; no background jobs with &.
- Time box about 2 hours. Keep output short.

## Output: /root/jobs/fx/cx/v21/s033/psched4/report.md (Korean, plain sentences)
The carrier you chose and why, every changed site (file:line), the unset-kind decision, the decision-owner table, and the diff as psched4/change.diff (`git diff` against HEAD).
