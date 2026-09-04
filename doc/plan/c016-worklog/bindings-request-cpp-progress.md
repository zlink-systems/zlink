START: detached worktree confirmed; scope bindings/cpp/**; core/build and core/build-dev preserved read-only
AUDIT: REQUEST async treated every nonzero ID as admitted; completion owner lacked WRITABLE request phase; C++ README and multi REQREP runner retained pending-admission assumptions
IMPLEMENT: shared raw request submit helper and REQUEST completion-entry WRITABLE retry state added; immediate admission keeps borrowed zero-copy path
TEST: new sleep-free public REQUEST regression passes 5/5 (HWM retry/reply, connect-before-bind, mixed SEND token, close terminal); existing request/optimization tests pass
PERF-CODE: multi REQREP runner now models async-operation ownership across pre-admission WRITABLE waits instead of nonzero-ID pending acceptance
SMOKE: full C++ contract 16/16 and samples 7/7 pass; single 15/15 and multi 120/120 complete with all requested patterns nonzero
VERIFY: bindings/cpp-only diff confirmed; git diff --check and perf shell syntax pass; core/build symlinks untouched
EXIT:0
