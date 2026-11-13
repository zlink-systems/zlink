# Task: add length-boundary and golden tests for the C++ native service-wire writer (issue #3867, PM review of PR #3884)

Worktree /root/worktree/zlink-3867a, branch perf/3867-submit-alloc (stacked on PR #3884's perf/3867-wire-writer). Read `git log -3`, the PR #3884 review (`gh api repos/zlink-systems/zlink/pulls/3884/reviews -q '.[].body'`) and framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp (`native_wire_writer_t`, `allocate_wire_buffer`, the `*_message` encoders and the `*_into` templates).

## Goal
Test-only change in framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_service_wire_codec.cpp. Production code must not change. For each case, compare the native encoder (`*_message`) byte for byte with an independent expected byte array written in the test (not produced by the vector encoder), and also with the vector encoder:
1. text8 fields at 255 bytes accepted and at 256 bytes rejected with `service_wire_error_t`, for: channel name (channel request and channel send headers), source and target SpotId (Spot request/send header), source and target Actor ID (Actor request/send header).
2. RID bytes8 fields at 255 bytes accepted, 256 rejected: target node RID (Spot and Actor headers), bound-session source Session RID (Actor header with bound_session_source), bound session send Actor owner node RID. For the bound-session source RID also respect the existing `ridBytes` limit; if it is below 255, test that limit and limit+1 instead and say so in the report.
3. Metadata combinations on the send path: no metadata, empty-vs-present metadata, and metadata at a few lengths (1, 255, 256, 4096) through the native send-part assembly that `send_with_header_result` uses. Reach it only through existing test seams in the unit tests; if no seam reaches it without production changes, test the encoders that build each part plus the part order, and explain in the report.
4. Traced application envelope (flow_id/flow_origin set, application payload flow version 2): an independent golden byte array for a small payload and for a multipart payload, native and vector.
5. Rejected inputs leave no partial message: for an over-length field, the encoder throws and nothing is returned.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Test file only. Commit locally on top of the current HEAD; do not push.
- Build C++ only via `/root/jobs/fx/cx/cpp-build-slot.sh <build-dir> --parallel 3` (read its usage), reusing /root/jobs/fx/cx/v21/s033/salloc/build. Check MemAvailable >= 14 GiB before compiling.
- Run the codec test target, then the C++ unit and contract labels via `/root/jobs/fx/cx/measure-shared.sh ctest --test-dir /root/jobs/fx/cx/v21/s033/salloc/build -L 'framework-unit|framework-contract' --parallel 3 --output-on-failure`.
- Run `bash scripts/format/format.sh --check cpp` from the worktree root and fix.
- No measurement needed.

## Output: /root/jobs/fx/cx/v21/s033/wtest/report.md (exact path)
Cases added (with the expected-byte source for each), test and format results, commit SHA, anything not finished.
