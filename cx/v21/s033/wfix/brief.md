# Task: two review fixes on commit 013a924289 (issue #3867, native service-wire writer)

Worktree /root/worktree/zlink-3867w, branch perf/3867-wire-writer. Read the previous job report /root/jobs/fx/cx/v21/s033/wire/report.md and `git show HEAD` first.
C = framework/languages/cpp/framework/src/runtime/.

## Fix 1 — writer bounds must hold in Release builds
`native_wire_writer_t` in `C/protocol/service_wire_codec.cpp` checks its precomputed size only with `assert`, which is compiled out in Release (NDEBUG). If a size formula and the bytes written ever disagree, Release writes past the binding message or returns a message with unwritten trailing bytes.
- Make both checks unconditional: `push_back`/`insert` throw `service_wire_error_t` (a short message naming the writer) when the write would pass the allocated size, and `take()` throws the same error when written size != allocated size. Check before writing. Remove the `<cassert>` include if no longer used.
- Keep it one compare per call; no new flag, mode or counter.
- Add a unit test in `test_cpp_framework_service_wire_codec.cpp` that proves the check holds in the test build configuration regardless of NDEBUG: reach the writer only through existing seams if possible; if the writer is not reachable from tests, add the narrowest test-access seam that exists in the codebase style (see how other codec internals are tested) and justify it in the report.

## Fix 2 — audit copies of application_payload_t
The commit changed `application_payload_t` multipart storage from a shared holder to an owned vector, so a copy now duplicates the part list (one vector allocation plus `message_t::copy()` per part) where it used to bump one reference count.
- List every place in framework/languages/cpp/framework/src that copies an `application_payload_t` from an lvalue (by-value parameters fed an lvalue, captures by copy, container push of an lvalue, `auto x = payload`). For each give file:line, whether it is on a per-message path (request, reply, send, multicast fanout per target, channel fanout), and whether the source is used afterwards.
- Where the source is not used afterwards, change it to a move. Where a per-message path really needs two owners, report it; do not reintroduce the shared holder.
- Put the table in the report.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Amend nothing: add one new commit on top of 013a924289. Do not push.
- Build C++ only via `/root/jobs/fx/cx/cpp-build-slot.sh <build-dir> --parallel 3` (read its usage). Reuse the build dir /root/jobs/fx/cx/v21/s033/wire/build (it already has ZLINK_FRAMEWORK_CPP_TEST_PROBES=ON). Every affected target must compile with -k 0.
- Run the C++ unit and contract labels via `/root/jobs/fx/cx/measure-shared.sh ctest --test-dir /root/jobs/fx/cx/v21/s033/wire/build -L 'framework-unit|framework-contract' --parallel 3 --output-on-failure`.
- Run `bash scripts/format/format.sh --check cpp` from the worktree root and fix.
- No A/B measurement is needed for this change.
- Keep MemAvailable >= 12 GiB.

## Output: /root/jobs/fx/cx/v21/s033/wfix/report.md (exact path)
What changed and why, the copy-site table, test and format results, new commit SHA, anything not finished.
