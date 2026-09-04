START: detached worktree confirmed; scope bindings/c only, core builds preserved, perf read-only per explicit constraint.
IMPLEMENTED: added five-pass C public-API REQUEST WRITABLE regression coverage and made the completion-only test use blocking admission.
NARROW_TEST: request writable and completion poller contracts passed (2/2); new contract executes all scenarios five times.
FULL_C_GATE: contract tests 10/10 and samples 6/6 passed with local Core and ZLINK_BUILD_JOBS=3.
PERF_SINGLE: complete 3/3, nonzero; DR_REQREP 316343, RR_REQREP 299993, DR 617231 msg/s (tcp, 1024B, 2s).
CONSTRAINT_NOTE: single runner unexpectedly auto-built symlink target /home/hep7hep7/project/zlink/core/build; all later perf uses --reuse-build.
PERF_MULTI: complete 6/6, nonzero; 8 clients, tcp, 1024/65536B, 2s, DEALER_ROUTER_REQREP 284570/19028, ROUTER_ROUTER_REQREP 175625/18195, DEALER_DEALER 949958/115134 msg/s.
FINAL_GATE: post-update C contract 10/10 and samples 6/6 passed; REQUEST regression standalone 5/5; git diff --check clean.
SUMMARY: wrote bindings-request-c-summary.md; no in-scope blockers, stale perf README handed off to the separate perf owner.
EXIT:0
