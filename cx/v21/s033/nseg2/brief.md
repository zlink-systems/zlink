# Task: .NET request-serial 1KiB — per-request segment times from syscall boundaries (diagnosis only)

## Situation
A first pass (/root/jobs/fx/cx/v21/s033/nseg/report.md, read it) mapped threads and code but reported every segment as "not measurable" because there is no request id. That is not acceptable: in request-serial exactly ONE request is in flight, so the socket syscalls of the two processes form a strict alternating sequence on one global clock (perf timestamps are system-wide). Use them as boundaries.

## Data
- Framework: /root/jobs/fx/cx/ratio045diag/zlink-framework-dotnet-request-serial-1024/ ; binding: /root/jobs/fx/cx/ratio045diag/zlink-dotnet-request-serial-1024/
- sched.data: perf record of sched_switch, sched_wakeup, raw_syscalls:sys_enter/sys_exit for both pids, 3 s. Re-script it as you need, e.g. `perf script -i sched.data -F comm,pid,tid,cpu,time,event,trace` (sys_enter shows NR and args, sys_exit shows NR and ret). window.json gives pids (source/target) and per-thread names. x86_64: 44 sendto, 45 recvfrom, 46 sendmsg, 47 recvmsg, 202 futex, 232 epoll_wait, 281 epoll_pwait, 1 write, 0 read (eventfd), 7 poll.
- Source tree for naming code: /root/worktree/zlink-ratio040 (main). CPU samples: source.nettrace/target.nettrace.

## Method (do this)
1. Identify the data-carrying socket syscalls: request bytes ~1.0–1.5 KB (Framework source sendto len 0x591 = 1425 was seen), reply similar. Find per process the Core IO thread and fd for: source request send (S_tx), target request receive (T_rx, sys_exit ret > 0 on that fd), target reply send (T_tx), source reply receive (S_rx). Ignore EAGAIN (ret -11) and zero-byte polls.
2. Build the sequence S_tx → T_rx → T_tx → S_rx → next S_tx. Drop cycles that break the order (report how many). For each cycle compute:
   - A = S_tx → T_rx (outbound transfer + target IO wake)
   - B = T_rx → T_tx (target: Core receive → Framework receive thread (.NET Long Running) → handler → reply submit → Core send)
   - C = T_tx → S_rx (return transfer + source IO wake)
   - D = S_rx → next S_tx (source: Core receive → completion → caller continuation → next request submit → Core send)
   Report p50, p90, mean for A–D, Framework and binding, and check A+B+C+D mean ≈ 1/throughput of the window.
3. Split B and D further using the thread hand-offs inside them: for each cycle, the ordered list of (sched_wakeup of thread X, X switches in, X's next mailbox/eventfd/futex syscall that wakes the next thread). E.g. in B: T_rx on IO thread → wake of Long Running receive thread → its run → handler → (wake of IO thread or write/eventfd) → T_tx. Report each sub-segment p50/p90 and how much of it is on-CPU (thread running) vs waiting (runnable-but-not-running after wakeup, or blocked). Name the code for each sub-segment from the CPU samples and source.
4. Name the two largest Framework-only sub-segments (larger than the binding's matching part), with µs and code.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only: do not modify any worktree or tracked file; no git operations; no spec edits; no GitHub operations. Do not read Core C headers (core/include).
- 리팩토링 점검: not applicable; state "코드 변경 없음".
- No new bench runs, no builds. Keep MemAvailable >= 12 GiB. Scripts and outputs only under /root/jobs/fx/cx/v21/s033/nseg2/.
- If after a real attempt the sequence cannot be built, show the concrete syscall excerpt that blocks it.

## Output: /root/jobs/fx/cx/v21/s033/nseg2/report.md
Korean, plain sentences. Start with a table: segment, Framework p50/p90, binding p50/p90, difference; then the two largest Framework-only sub-segments.
