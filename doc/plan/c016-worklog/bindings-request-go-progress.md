START: detached worktree and scope verified; inspecting REQUEST/SEND completion ownership for D-B85.
IMPLEMENT: REQUEST now snapshots only after BACKPRESSURED, waits for its WRITABLE token, retries the same payload, then resumes normal REQUEST completion.
TESTS: added sleep-free 5x HWM retry, connect-before-bind, close cleanup, and mixed SEND/REQUEST token regressions; targeted run passes.
DOCS: PENDING_MAX is documented as ABI-retained and ignored; obsolete pending-request assumptions removed.
VERIFY: full bindings/go test+vet+raw guards passed; samples passed 7/7.
PERF: single 3/3 complete (15/15 lines, all nonzero); multi 24/24 complete (120/120 lines, all nonzero).
FINAL: git diff --check and gofmt checks pass; summary written with no blockers.
EXIT:0
