2026-09-04 START HEAD=70a9998998138cc3db8258711cbf986e4ced113c detached; scope=bindings/go/**; existing untracked core/build and core/build-dev preserved; core read-only.
2026-09-04 BASELINE internal/native: FAIL raw header allowlist hash (eventing/api.h mirror already differs from stale 0.17.0 allowlist); runtime tests otherwise not reached. No core mutation.
2026-09-04 IMPLEMENTED managed SEND retry: immutable packet snapshot, nonzero token/context/target correlation, POLLOUT+WRITABLE queue drain, exact packet resubmit, cancellation tombstone, REQUEST path retained.
2026-09-04 TEST targeted public-operation HWM flow green; TestManagedSendRetriesExactPacketAfterWritable count=5 green; CompletionWritable public value=3 checks green.
2026-09-04 REVIEW fixed settled-close race, poller integer-slot stack safety, REQUEST event projection during mixed drains, runtime fatal waiter completion, and scheduler-yield/no-timer completion pump.
