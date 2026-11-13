# Task: structural redesign of the C++ Framework request submit path so the caller does binding-level work (≤ 6.6 µs/request), read-only

## Situation
The user keeps the target: every C++ cell Framework ÷ binding ≥ 0.6. request-backpressure 1KiB is 0.205 because the single source caller
thread is saturated (0.91 core, 17.48 µs/request; binding 3.69). Breakdown (/root/jobs/fx/cx/v21/s033/ccall/report.md, profile tree #3957 =
/root/worktree/zlink-3867h): binding submit 5.54 (binding 2.52), registration 4.42 (0.01), result observation 4.25 (0.05), encoding 2.15 (0.14),
other 1.11 (0.97). Allocation (malloc/free ~2.0), shared_ptr refcounts (~0.8) and mutex (~0.66) are spread across parts. Raw profile data and
scripts: /root/jobs/fx/cx/v21/s033/ccall/. Earlier analyses: /root/jobs/fx/cx/v21/s033/cprof/report.md, /root/jobs/fx/cx/v21/s033/crtc/report.md,
/root/jobs/fx/cx/v21/s033/csites/report.md (what #3957 already removed — do not redo it).

## Direction from the user
The user said this will not be solved by fine-tuning. Do NOT produce a list of small trims (#3919/#3957 already did that and moved
0.165 → 0.205). The Framework caller does ~4.7× the binding's work per request; design the request path so that per request the caller
does essentially what the binding caller does (encode payload into a prepared message, one submit) and everything else is paid once
(at bind/registration/first use) or amortized across many requests (batched registration, preallocated slot pools, prepared
envelope/header templates, completion delivered into a pre-reserved slot without per-request task/shared-state objects, etc.).
Name the structural change(s), the existing mechanisms they replace, and the spec constraints that shape them.

## Second direction from the user (decides disputes)
"Performance is essentially set by Core/bindings; the Framework only needs to cut its additional overhead as much as possible. Use the
bindings' features as much as possible and do not reinvent what the bindings provide."
So the primary deliverable is a table of every Framework mechanism on the request path (caller submit, target receive/handler/reply,
source completion) that duplicates something the C++ binding already provides (bindings/cpp: request/reply async, completion owner and
its pending map, poller/receive loop, timeouts/deadlines, routing ids, message/part ownership, completion delivery to coroutines/futures,
WRITABLE retry...). For each: Framework file:function, the binding feature (file:function) that already does it, µs/request it costs, and
whether the Framework can call the binding feature directly instead (what Framework-only contract, if any, still needs a thin layer, with
spec file:line). The redesign alternatives must be built from that table: Framework = binding calls + only the contract the binding lacks.

## Goal (design only, no code changes)
1. From the profile, list every unit of work the caller does per request in the Framework (file:function, µs, allocations, atomics/locks),
   and mark which the spec requires on the caller thread vs which could be done once (at registration/bind time), batched, moved off the
   critical thread without adding a hand-off on the critical path, or removed.
   Spec: framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md (§9 :329, §10 :375, §11 :490–568),
   08-messaging-hot-path.ko.md (§4 :141–213), 02-handler-turn-and-execution-gate.ko.md, and documents they reference. Keep every contract.
2. Binding submit 5.54 vs 2.52: explain the 3 µs the Framework spends more inside the binding/Core submit call (socket mutex, message
   construction, multipart parts, extra syscalls/wakeups?) with evidence, and how to close it using public binding APIs only.
3. Propose a concrete design that reaches ≤ 6.6 µs with arithmetic per item (expected µs saved, with evidence), ordered by size, grouped into
   2–4 PR-sized steps. For each step: files, removed mechanisms (before → after decision-owner counts; no new flags/counters without
   removals), tests to add, and the risk. If ≤ 6.6 µs is not reachable without a spec change, say exactly which clause (file:line) and what
   change would be needed, with the µs it would unlock.
4. Note the effect on request-serial and send-saturation cells (send caller is 4.7 µs vs binding ~1.3).

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only: do not modify any worktree or tracked file; no git operations; no spec edits; no GitHub operations.
  Do not read Core C headers (core/include).
- 리팩토링 점검: for each proposed change, state which existing mechanism it replaces and confirm no second parallel path remains.
- No builds and no benchmark runs (another job builds and measures now); use the existing profile data. Keep MemAvailable >= 12 GiB.
- Outputs only under /root/jobs/fx/cx/v21/s033/ccut/.

## Output: /root/jobs/fx/cx/v21/s033/ccut/report.md
Korean, plain sentences. Start with a 10-line summary: reachable or not, the steps with expected µs each, and the total.

## PM design constraints (read-only diagnosis first; PM picks one alternative before any implementation)
- Do not add decision points: terminal settlement (operation registry take) 1, deadline 1, admission 1, result delivery 1 stay; reduce only
  the intermediate state between them.
- Registration 4.4 µs: first examine a path where, when the caller is already inside the owner lane or there is no contention, the same
  registration owner handles it without an owner-turn round trip (without violating the 06 §5 lane boundary). Tabulate whether the deadline
  index and admission each allocate or lock per request.
- Result observation 4.3 µs: like #3957's C1 completion slot inside the request block, gather task, shared state and receiver completion
  objects into one per-request storage; target 0 heap allocations and 0 shared_ptr per request. User continuations never run on the
  receive-role stack (08-messaging-hot-path:309).
- Encoding and binding submit: whether the header plan cache re-checks on every request; what copies/locks only the Framework adds in the
  binding submit, function by function.
- For each alternative (compare 2–3): expected µs (from the measured part costs), ownership layers changed, effect on the common
  four-language contract, spec clauses (file:line).
