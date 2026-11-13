Model gpt-6.1-sol, effort high: two blocking review findings need an ownership choice (where the service-header cache stays valid, how a reply source carries only its reply target) — design judgment, not mechanical edits.

# Task: #1639 step 1 PR #4155 — fix the PM's two blocking review findings and one cleanup

Earlier job briefs are history, not instructions.

## Situation
- Worktree /root/worktree/zlink-v21-s033/p1, branch perf/1639-cpp-p1-receive-record @ 45ea3c721e (one commit, PR #4155). It builds and passes all C++ Framework tests on the PM's machine. Step 1 design and counts: /root/jobs/fx/cx/v21/s033/p1/report.md.
- The PM's review (full text, read it first): /root/jobs/fx/cx/v21/s033/p1r/input/pr-review.md. Summary:
  1. service_mailbox_record_t caches _service_header/_application_kind on first service_header() call, but non-const parts() lets callers replace parts afterwards (test m6c_runtime :4732 does, then re-processes; channel_host_service.cpp:377 replaces parts too). The cache can go stale. Keep the cache's validity condition in ONE place in code: e.g. replace whole parts only through one replace_parts(...) that clears the cache, and narrow non-const parts() to element edits; or hand the ingress classification along instead of caching on the record. The cache fields are public and receive_record.cpp:131 calls record._service_header.reset() directly — make the test re-classify through the public path instead.
  2. reply_source() (service_mailbox.cpp:72) copies the whole received_t including parts, then closes it. Callers: public_host_runtime.cpp:6835, 6884, 6923, 7254, 7287; raw_stateful_dispatch.cpp:1292. The previous code copied only the token. Move only routing id, reply token and route generation (if the binding received_t has no public API for that, carry a reply-target value on the record instead). A test must show allocation/copy count did not grow.
  3. Cleanup: raw_mesh_node_owner.hpp:131 adds `friend struct receive_record_test_access_t`; the class already has `raw_mesh_node_owner_test_access_t` (:536). Use the existing one only.

## Do
1. Fix 1 with the option that leaves fewer rules (count decision sites before/after: places that decide whether the cached classification is valid; places that classify). Keep the negative control `ZLINK_RECEIVE_RECORD_NEGATIVE=classification` meaningful — state how it still fails.
2. Fix 2. Read the binding C++ received_t / reply token declarations (bindings/cpp, not core/include) to pick the cheapest way. Add or extend a count test (existing allocation_counter / native_call_counter support) asserting reply_source does not copy parts; give its negative control.
3. Fix 3; move whatever receive_record_test_access_t provided into raw_mesh_node_owner_test_access_t and update the test.
4. Update every caller and test touched by the API change. grep tests, samples and cross-language too (framework/languages/cpp/{framework,tests,samples,cross-language}).
5. bash scripts/format/format.sh --check cpp and git diff --check must pass.
6. Before handing back: for every call you rewrote, count bracket pairs and argument count against the declaration (nothing can be compiled here; the last uncompiled diff lost a closing parenthesis).

## Rules
- Do NOT build or run tests and take no lock: a Core release gate is measuring on this machine. The PM compiles on their machine.
- Do not start sub-agents and do not call any other model or AI tool.
- No git operations except git diff, git status, git log, git show. No commit, no push, no spec edits, no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not read other job folders except p1/report.md.
- No new queue, lock, timer, flag or thread. No test-only product API (the PM rejected adding product API for test convenience).
- 리팩토링 점검: list any other record field that caches a value derived from parts, and any other whole-record copy on the reply path, with file:line.
- Time box about 3 hours. Keep your own output short.

## Output: /root/jobs/fx/cx/v21/s033/p1r/report.md (Korean, plain sentences)
(1) fix 1: chosen option, why, decision sites before/after, file:line; (2) fix 2: what is moved now, copies/allocations before/after, new test and its negative control; (3) fix 3; (4) every caller updated; (5) bracket/argument check list; (6) format result; (7) 리팩토링 점검.
