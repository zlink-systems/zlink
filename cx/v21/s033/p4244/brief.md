Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4244 — Core integration tests bind to port 0 instead of PID-derived fixed ports

Worktree /root/worktree/zlink-v21-s033/p4244 (branch fix/4244-core-test-port0, current origin/main). Commit locally; do not push.
Read first: `gh` is NOT allowed; the issue text is below. Read core/tests/testutil.hpp, core/tests/testutil.cpp and every user of ENDPOINT_1..3 / PORT_* / test_port_offset (`grep -rn "ENDPOINT_[0-9]\|PORT_[0-9]\|test_port_offset\|ZLINK_TEST_PORT_OFFSET" core/ scripts/ .github/`).

## Issue
`test_connect_rid_alias_binding` failed once in a parallel ctest run: `zlink_bind (server, ENDPOINT_1)` errno 98 (Address already in use). testutil.hpp:46-81 builds ENDPOINT_1..3/PORT_6 from base 5556..5561 plus `(pid % 1000) * 10` (or ZLINK_TEST_PORT_OFFSET). Any other process on that port breaks bind.
Done when: tests that need a TCP endpoint bind to `tcp://127.0.0.1:*` and read the bound endpoint (ZLINK_LAST_ENDPOINT / the existing test helper for it) for connect/rebind. Tests that must rebind the same port after close keep the first port they observed. Do not retry-to-pass or widen offsets. A single failure is a defect.

## Do
1. Convert every user to port 0 + last-endpoint. Then delete test_port_offset, the fixed bases, ENDPOINT_n/PORT_n macros and ZLINK_TEST_PORT_OFFSET handling if nothing uses them anymore (also from scripts/CI env if set there). Net rule count must not grow: list removed vs added helpers/macros in the report.
2. Fail-before (behavioural): before your change, occupy the fixed port the test will compute (e.g. run the test binary with ZLINK_TEST_PORT_OFFSET=0 while a small python listener holds 127.0.0.1:5556) and show the EADDRINUSE failure; after the change the same occupied port must not matter (test passes). Paste both outputs and the commands.
3. Run the four integration tests 20 times each, then the full Core ctest once with -j4 (Release). `bash scripts/format/format.sh --check cpp` (or the Core format target the repo uses for core/ — find it).

## Rules
- Compile only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), Release, -j3, build dir /root/worktree/zlink-v21-s033/p4244/build-p4244, 25 GB cap; remove the build dir at the end.
- Do NOT run scripts/dev/internal-docs.sh, git pull/fetch/push, GitHub, sub-agents or other AI tools. Keep raw logs in /root/jobs/fx/cx/v21/s033/p4244/logs; never delete them.
- Test-only change: do not touch Core product code. Korean, short.

## Output
/root/jobs/fx/cx/v21/s033/p4244/report.md: commit, changed files with reason, removed/added helpers, fail-before/pass-after outputs with commands, repeat counts, full-suite result, remaining problems.
