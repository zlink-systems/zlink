START detached@d5cb9d4739; scope/constraints confirmed; existing untracked core/build-main-readonly preserved
IMPLEMENT request DONTWAIT now attempts once, converts EAGAIN to SEND writable wait token, and removes REQUEST payload pending machinery
HEADERS PENDING_MAX documented ABI-retained/ignored; REQUEST writable-retry contract mirrored to c/cpp/go/rust raw headers
DEV_BUILD initial compile completed after updating removed pending-runtime unit expectations
TEST added public sleep-free request writable suite covering DEALER/ROUTER HWM, ROUTER/ROUTER RID, connect-before-bind, mixed SEND/REQUEST tokens, timeout boundary, removal/close terminal cleanup, PENDING_MAX no-op, and missing route
TEST_UPDATED phase3 request suite removed obsolete pre-admission retention cases and changed zero-weight REQUEST to BACKPRESSURED+token; zmp request suite green
DEV_GATE latest dev build passed with JOBS=4; full ctest excluding hotpath_gate passed 140/140 with -j2
REPEAT_GATE changed REQUEST-related suite passed 5/5 runs; new public suite contains no sleep calls
MIRROR_DIFF all 8 core headers match c/cpp/go/rust mirrors (32 cmp); git diff --check passed; no tracked changes outside allowed paths
IMPACT exact rg completed; language bindings need REQUEST WRITABLE retry parity and stale PENDING_MAX comments; framework call sites inherit binding behavior
HOTPATH_GATE release-gate tree and core/build/bin/hotpath_bench absent, so gate will be skipped and recorded as blocker
RELEASE_BUILD final JOBS=4 release --lib-only LTO build passed; core/build/lib/libzlink.so produced
SUMMARY wrote core-request-contract-b-summary.md with changes, contract table, gates, measurement decision, SPEC proposals, impact rg, and blockers
EXIT:0
