2026-09-05 KST START: read-only pass 2; C/C++ Multi PUBSUB runner and receive-path comparison begun.
2026-09-05 KST RUNNER: both drain each ready socket to EAGAIN; noted C per-recv deadline/100ms poll cap vs C++ post-recv deadline/full-remaining poll wait.
2026-09-05 KST CALLS: 64B profiles show 2 subscribe calls/message plus drain miss in both; no Core part-count query, equal normalized init/close, C++ net +1 external size query/message.
2026-09-05 KST LOSSY: found runner/environment confounds: C recalculates server auto-HWM after connect per size, C++ only before bind; reports show 1MiB/4MiB mismatch and start load 0.18 vs 4.17.
2026-09-05 KST DONE: summary written; binding no-go, runner parity correction recommended as a separate measurement track.
EXIT:0
