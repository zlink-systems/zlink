Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4350 measurement-server noise — binding A/A. MEASUREMENT ONLY.

Worktree /root/worktree/zlink-v21-s033/aa-noise, detached at main 6d508d3166. No code changes, no commits, no push/PR.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (measurement conditions, lock, raw-file rules; the call-shape rule does not apply — both sides are the same binding).

## Why
Since the quiet guard was tightened (12:05 UTC) single runs still jump: C++ binding send-saturation 4K one run 355,151 vs 238–258k; C++ L0 request-serial 4K one run 7,725 vs ~11,650. All four ladder languages use the same G5 rule, so we need this server's baseline run-to-run noise and the cause of the jumps.

## Do
1. Build the Java bench in THIS worktree (framework/bench/grpc/java/build.sh), verify JARs newer than sources.
2. Copy /root/jobs/fx/cx/v21/s033/lad-java-l2b/tools to aa-noise/tools; point JOB/raw/worktree at aa-noise; use ONLY the binding implementation (zlink-java raw server + raw client) on both "sides" — i.e. run binding twice per run index as A and B.
3. One lock session: request-serial 4K × 10 runs (A and B each), send-saturation 4K × 10 runs (A and B each). Standard conditions (source CPU 0-7, target 8-15, warmup 30 s, active 5 s).
4. For EVERY run record, in raw/run-env/<cell>-<A|B>-run<N>.txt: start/end UTC; scaling_cur_freq of all 16 CPUs sampled every 1 s during the active window; /proc/loadavg;     PID CLS %CPU STAT COMMAND
3803491 IDL  7.5 Ssl  /usr/lib/jvm/java-25-openjdk-amd64/bin/java --enable-native-access=ALL-UNNAMED -classpath /root/worktree/zlink-v21-s033/lad-java/framework/bench/grpc/java/client/build/install/bench-client/lib/* systems.zlink.bench.withgrpc.client.BenchClient --implementation zlink-framework-java --scenario send-saturation --payload-size 1024 --request-window 100 --submit-concurrency 8 --latency-sample-limit 200000 --warmup-seconds 30 --drain-bound-ms 30000 --request-timeout-ms 30000 --route-ready-ms 30000 --trigger-url http://127.0.0.1:5252 --stats-url http://127.0.0.1:5253 --target-endpoint tcp://127.0.0.1:5254 --target-stats-url http://127.0.0.1:5255 --run-id 4350par-L0-run1 --cell-id zlink-framework-java-send-saturation-1024 --raw-socket router --output /root/jobs/fx/cx/v21/s033/lad-java-par/raw/fresh/L0/run1/zlink-framework-java-send-saturation-1024 --report-file report.txt
3803214 IDL  5.0 Ssl  /usr/lib/jvm/java-25-openjdk-amd64/bin/java --enable-native-access=ALL-UNNAMED -classpath /root/worktree/zlink-v21-s033/lad-java/framework/bench/grpc/java/zlink-framework-server/build/install/bench-zlink-framework-server/lib/* systems.zlink.bench.withgrpc.frameworkserver.ZLinkFrameworkBenchServer --endpoint tcp://127.0.0.1:5254 --metrics-url http://127.0.0.1:5255
3792931 IDL  3.4 Sl   /usr/lib/node_modules/@openai/codex/node_modules/@openai/codex-linux-x64/vendor/x86_64-unknown-linux-musl/bin/codex exec -C /root/worktree/zlink-v21-s033/l3868 -m gpt-6.1-sol -c model_reasoning_effort="high" --dangerously-bypass-approvals-and-sandbox -o /root/zlink/.artifacts/codex/l3868-l1/summary.md -
 905967 IDL  2.8 Sl+  claude --dangerously-skip-permissions
    119  TS  2.0 S    [kcompactd0]
 859558  TS  1.3 Ssl  /usr/bin/containerd
2922245 IDL  1.3 Sl   /usr/lib/node_modules/@openai/codex/node_modules/@openai/codex-linux-x64/vendor/x86_64-unknown-linux-musl/bin/codex exec -C /root/worktree/zlink-v21-s033/lad-node -m gpt-6-luna -c model_reasoning_effort="high" --dangerously-bypass-approvals-and-sandbox -o /root/zlink/.artifacts/codex/lad-node-l1b/summary.md -
2593381 IDL  1.2 Sl   /usr/lib/node_modules/@openai/codex/node_modules/@openai/codex-linux-x64/vendor/x86_64-unknown-linux-musl/bin/codex exec -C /root/worktree/zlink-v21-s033/lad-dotnet -m gpt-6-luna -c model_reasoning_effort="high" --dangerously-bypass-approvals-and-sandbox -o /root/zlink/.artifacts/codex/lad-dotnet-l1b/summary.md -
2432982  TS  1.1 Sl   /usr/lib/node_modules/@openai/codex/node_modules/@openai/codex-linux-x64/vendor/x86_64-unknown-linux-musl/bin/codex exec -C /root/worktree/zlink-v21-s033/lad-java -m gpt-6-luna -c model_reasoning_effort="high" --dangerously-bypass-approvals-and-sandbox -o /root/zlink/.artifacts/codex/lad-java-par/summary.md -
1242106  TS  0.9 Ssl  /usr/sbin/tailscaled --state=/var/lib/tailscale/tailscaled.state --socket=/run/tailscale/tailscaled.sock --port=41641
3791001 IDL  0.4 S    /bin/bash /root/jobs/fx/cx/quiet/guard.sh
    140  TS  0.2 S    [kswapd0]
3803751 IDL  0.2 Sl   /root/.bun/bin/bun /root/.claude/dc/dc.ts kairos wait
1114913  TS  0.2 Ssl  /usr/bin/redis-server 127.0.0.1:6379
3804659 IDL  0.1 S    python3 /root/jobs/fx/cx/v21/s033/r4190m/tools/cell_snap.py before {"runId":"4350par-L0-run1","cellId":"zlink-framework-java-send-saturation-1024","pattern":"send-saturation","payloadBytes":1024,"phase":"warmup","durationMs":30000,"requestWindow":100,"submitConcurrency":8}
  43163  TS  0.0 Ssl  /usr/bin/dockerd -H fd:// --containerd=/run/containerd/containerd.sock
     17  TS  0.0 I    [rcu_preempt]
1824799 IDL  0.0 S    /bin/bash /root/jobs/fx/cx/measure-gate-keeper.sh
3579842  TS  0.0 Sl   /root/worktree/zlink-3868-csendb/framework/languages/cpp/build/csendb/test_cpp_framework_fanout_subscription
3799705 IDL  0.0 Sl   /usr/lib/node_modules/@openai/codex/node_modules/@openai/codex-linux-x64/vendor/x86_64-unknown-linux-musl/bin/codex-code-mode-host
2847392 IDL  0.0 Ss   bash /root/jobs/fx/cx/v21/s033/lad-dotnet-l1b/tools/run-and-continue.sh
 759473  TS  0.0 Zs   [codex] <defunct>
    384  TS  0.0 S    [xfsaild/nvme0n1p3]
 906219 IDL  0.0 S    /bin/bash /root/jobs/fx/cx/mem-guard.sh once mid-active; /proc/interrupts and /proc/softirqs deltas over the active window; and the guard log lines (/root/jobs/fx/cx/quiet/guard.log) whose UTC time falls in the run.
5. Report per cell: median, G5 spread (largest |run − median| ÷ median) for A, B and A∪B (20 runs); A/B median ratio; list every run more than 5% from the 20-run median with its environment evidence (frequency drop, other processes, interrupts). State plainly what differs in the outlier runs versus normal ones, or that nothing recorded differs.


## Second attempt (kairos, 11-13 15:45 UTC)
- The first attempt (aa-noise/first-attempt/report.md) ran out of time: its Java build was paused by the quiet guard while other measurements held the lock, and it never measured. Reuse its tools/ (copy into aa-noise2/tools) and its build if the JARs are newer than the sources; otherwise build again (the guard pauses builds only while a measurement holds the lock; just wait).
- Method change (PM 11-13 15:29): warmup 10 s for Java (was 30 s), active 5 s. Keep 10 A runs + 10 B runs for request-serial 4K and send-saturation 4K, with per-run environment evidence as above. Take the lock once for all 40 runs (about 15 min).
- Write report.md right after the lock hold. Time cap 3 h.

## Output
/root/jobs/fx/cx/v21/s033/aa-noise2/report.md (Korean, short tables, every number cites its raw file). Remove nothing.
