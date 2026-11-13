# kairos design — C++ request/send path restructuring (#3867, #3868)

Evidence: /root/jobs/fx/cx/v21/s033/cprof/report.md (main ee11390257, request-backpressure 1K), #3884 (merged) and #3888 (open) results.
Status: draft for comparison with the sol (ultra) advice and the zlink design. Not implemented.

## 1. What the profile actually says

The target is 0.6 × binding = 132,280 msg/s for request-backpressure 1K. That allows at most **7.56 µs/message on every serial resource** in each process.

| Serial resource | Framework µs/msg | Binding µs/msg | Over budget |
|---|---|---|---|
| Source application submitter | 23.4 (≈19–20 after #3884+#3888) | 3.7 | ×2.6 |
| Source Core IO (one socket's IO thread) | 19.5 | 2.5 | ×2.6 |
| Source Framework receive role | 18.4 | — | ×2.4 |
| Target Core IO/1 | 18.9 | 3.3 | ×2.5 |
| Target receive role | 17.0 | — | ×2.2 |
| Target dispatch workers (2) | 11.0–11.7 each | 3.3 (handler loop) | ×1.5 |

Also, the whole-process CPU budget at 0.6 is 60.5 µs/message (8 CPUs). Source uses 108 and target 83.

Conclusion 1: **the gap is not one hop.** Four or five different threads each carry 2–3× the per-message work they can afford. Speeding up the submitter alone moves the bottleneck to Core IO or the receive role within a few µs. Any design must cut all of them.

Conclusion 2: **the submitter itself has almost no context switches (0.036/msg).** Its 23 µs is CPU on its own thread: allocation 7.6, bookkeeping 5.1, Core call 5.0, codec 2.6, hand-off wake 1.5, task 0.8, driver 0.9. Removing "thread hand-offs" does not shrink that number. The hand-offs cost CPU on the *other* threads: source 2.46 and target 1.31 switches/msg, 9.4 futex calls/msg on the source.

Conclusion 3: **Core IO does ~7× the binding's syscalls per message** with the same Core request API (`request(...).async()` in both benches): sendto 0.88 vs 0.12, recvfrom 1.14 vs 0.31, read+write 2.0 vs 0.05 (eventfd/mailbox signalling), epoll_wait 1.9 vs 0.35. The binding bench batches about 8 messages per sendto because its submitter outruns the IO thread. The Framework wakes Core per message, and Core flushes per message. Part of this disappears automatically once the submitter is faster. The read/write wake traffic is a Framework-side pattern: poller-driven completion drains and mailbox commands per request.

## 2. Design K — four parts, all on the existing contracts

### K1. One allocation and one table per request on the submitter
- **Request record:** the public task state, admission source, receiver/projection, registry entry and wire part list live in one object, taken from a per-node free list (owned by the node lane, so there is no new lock). Lifetime is unchanged: the record returns to the free list when the terminal is claimed and the dispatcher seat is released (spec 01 §10.1: "dispatcher seat kept until callback returns").
- **Single pending entry (spec 01 §10.1):** the Framework registry entry is the only Framework pending. The binding's `request_completion_bundle_t` (admitted + reply async results) is a second per-request object with its own `std::map`-style insertion under `completion_owner_t::_mutex`. A binding callback-style submit — a context pointer plus a function pointer, with no async_result states — removes it. That is a binding API addition (binding minor version with Core, per the release rule). The native completion entry stays binding-owned; only its result representation changes.
- **Codec:** cache the encoded envelope prefix per (channel, packet name, content type). The deadline field stays the same text on the wire, but format it from a per-second cache instead of `gmtime_r/strftime` per message (0.37 µs). The service wire already writes into native messages (#3884).
- **Expected submitter cost:** allocation 7.6 → ~1.5, bookkeeping 5.1 → ~2.0, Core call 5.0 → ~2.5, codec 2.6 → ~1.2, hand-off 1.5 → ~0.5, task 0.8, driver 0.9. Total **≈ 9.4 µs**, against a 7.56 µs budget.

### K2. Completion delivery without the 8-worker hop where the spec allows it
- Spec 01 §11: a "new execution turn" does not mean another thread. Spec 08 §4.4: a resource that is not the receive role may run the turn directly; the receive role may run it only after handing the role over at a receive-rotation boundary.
- **Source:** completions for callers that have no Framework lane (an application thread, like the benchmark driver) are delivered in batches per receive rotation: the receive role hands off to an idle completion worker, then runs the batch itself. This replaces one pool wake per message with one hand-off per rotation. Completion workers currently burn 26 µs/msg in total.
- **Target:** the dispatch workers (11 µs each) run handler turns after the gate. Batch the I4 hand-off per rotation (already the spec unit) instead of one post per record.
- **Expected:** source futex 9.4 → ~2/msg; the receive role and completion pool drop from ~44 → ~15 µs/msg combined (not on the submitter, but this is the CPU budget and the next bottleneck).

### K3. Let Core batch
- With K1 the submitter outpaces the IO thread, and Core's existing mailbox coalescing starts batching (the binding bench's 8 msgs/sendto shows Core can).
- Remove per-request mailbox commands that the Framework adds on top of the data send: the poller re-arm for request completions, and any per-request `pollroute`/admission query on the IO thread. These must be found in the hop map (sol ultra and zlink); I have not traced them line by line yet.
- **Expected:** Core IO 19.5 → ~6–8 µs/msg.

### K4. Same on the target
- The receive role decodes the envelope and service wire, claims the gate and posts the handler. Apply the K1 codec and record pooling and the K2 per-rotation batching there.

## 3. Ceiling arithmetic

- Submitter with K1: ≈9.4 µs. Scaled to ratio040's measurement basis (×1.117) that is ≈10.5 µs, so U = 10^6 / (220,467 × 10.5) ≈ **0.43**.
- To reach 0.6, the submitter must be at 7.56 µs (ratio040 basis), i.e. ≈6.8 µs in profile terms. The binding itself spends 3.7 µs there. **The Framework may add only ~3 µs per message** for envelope, service wire, deadline registration, admission and public task, with no batching.
- I do not see a design inside the current contracts that gets those parts under 3 µs per message one at a time. The only structural lever that changes the arithmetic is **amortisation**: deadline, admission and Core submit work per *batch* of requests instead of per request. A batch cannot appear in the public request API — each call is one request — but it can appear inside the node lane: the lane drains several submitted requests in one turn, registers them together, and submits them to Core as one multi-message submission. Whether spec 08 §3 (E1–E4 per message) and the binding API allow a multi-request submit is the first question for the review.

## 4. Expected outcome and recommendation

| Cell | Now | K1–K4 estimate | With lane batching (if allowed) |
|---|---|---|---|
| request-backpressure 1K | 0.165 (≈0.19 with #3888) | ≈0.40–0.45 | ≈0.55–0.65 |
| send-saturation 1K | 0.301 | ≈0.45–0.5 | ≈0.6+ |
| request-serial 1K/4K | 0.52–0.56 | ≥0.6 (latency, K2 removes the pool hop) | — |
| 4K cells | 0.25–0.57 | ≥0.6 likely | — |

Recommendation:
1. Implement K1 and K2 first (inside current contracts, with measurable steps).
2. In parallel, decide whether lane-level batching (K-batch) is allowed by spec 08 §3 and the binding API. If it is not, C++ request-backpressure and send-saturation 1K stay near 0.4–0.5. That is a target decision for 대표님/the PM, because those two cells compare against a binding that does almost nothing per message.

## 5. Open questions for the comparison
- Exact per-request mailbox and poller commands on the source Core IO thread (K3) — from the hop maps.
- Whether a callback-style binding request submit already exists (avoiding the bundle) — `raw_route_port_t::request` callback overload still calls `.async()`.
- Whether spec 08 §3 binds E1–E4 to one message or allows a lane turn to process several.
