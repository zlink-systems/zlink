Model gpt-6-luna, effort medium: read-only reconnaissance across five bench drivers (mechanical mapping, no design choice).

# Task: #4190 — map where each bench driver submits, before switching to 8 concurrent submitters

Worktree /root/worktree/zlink-v21-s033/b4190 (detached at origin/main), READ ONLY: no edits, no build, no benchmark, no commit/branch/stash/reset. Do not run scripts/dev/internal-docs.sh or any git pull/fetch; no GitHub, no sub-agents or other AI tools.

## Decided condition (PM/실장님, spec PR coming for framework/bench/grpc/README.ko.md/.en.md §10.3, §5.1, §8.2)
- send-saturation and request-backpressure: 8 concurrent submitters, one submitter = one dedicated OS thread (Node: one event loop).
- request-serial: one sequential submitter, unchanged.
- Binding row and Framework row use the same driver structure and submitter count.

## Do
For each driver — the C reference bench (bindings/c/perf or framework/bench/grpc C path; find it), C++ (framework/bench/grpc/cpp/client/bench_cpp_client.cpp), .NET, Java, Node (find their clients under framework/bench/grpc/) — and for binding and Framework modes:
1. Where submissions happen now (file:line), how many threads/coroutines/tasks submit, how completion is observed (same thread or other), what the outstanding window / admission limit is and who owns it, and how the pacer (--target-rate, C++ only after #4181) interacts.
2. Where a submitter count would be configured (existing option? runner variable in run_local.sh or the language runner) and what shared state the submitters would contend on (counters, latency histograms, window semaphores) with file:line.
3. Which counters/metrics assume a single submitter (e.g. per-thread histograms merged at the end, sequence numbers, validators that check FIFO order) and would break or need merging.
4. A table per language: current structure → minimal change to N dedicated submitter threads, files to touch, estimated size, risks. Same table for the runner/metadata (record submitter count in results metadata like targetRate).

## Output: /root/jobs/fx/cx/v21/s033/b4190/report.md (Korean, short, every claim file:line)
