# 10. Revision Candidates (Not A Contract)

> [Common contract table of contents](README.en.md)
>
> **The items in this chapter aren't a contract.** They aren't an
> implementation basis, and once promotion is decided, they move to
> that chapter's contract body and the 5 languages are updated
> together (the README change procedure).

| ID | Title | Motivation | To Decide |
| --- | --- | --- | --- |
| R1 | Expose the error body on typed failure | `submit<T>()` discards the response body at status ≥ 400, requiring a `submitRaw()` workaround to read the API error payload — a practical pitfall | The form that carries status+headers+raw body in the failure value (an exception field vs. a failure envelope), the 5-language expression |
| R3′ | The total retry deadline option | (R3's backoff+jitter was promoted and completed on 2026-07-12 — Chapter 6 §6.2) A total deadline spanning the whole retry is still not in the contract (only cpp enforces a total budget on both paths — a language deviation) | Whether to introduce a total deadline option (e.g. `totalTimeout`), the direction for unifying with the cpp deviation |
| R4 | multipart binary file | `multipartFile`'s content is a string, so binary upload isn't possible | Adding a byte-argument overload vs. a file-path argument, the 5-language signature |
| R5 | Deeper kotlin coroutine work | Cancellation doesn't propagate to the underlying request, streaming is only a callback sink (no `Flow`), java's blocking `fetch` and kotlin's suspend `fetch` share a name | The `suspendCancellableCoroutine` propagation scope, whether to add a `Flow<ByteArray>` download, cleaning up the `fetch` naming |
| R7 | Reconsidering the one-shot verb path | one-shot builds/tears down the transport stack per request (dotnet recreates the handler → socket exhaustion risk). This path is also why the 7 client/builder verbs are duplicated | Keep + document a warning vs. reuse an internal shared transport vs. remove |
| R8 | Switching cpp to true async I/O | The current approach offloads sync Beast to a thread pool (sync-over-threadpool) — a worker is occupied for the whole request duration and the default scheduler is serial | Scope/schedule of a Beast async-exchange switch (large), relationship with a short-term mitigation (splitting thread count/scheduler) |
| R9 | Unifying the request cancellation surface | Only dotnet takes a `CancellationToken`; cpp/java/node have no means to cancel an in-flight request (timeout is the only boundary). Same root as kotlin's non-propagated cancellation (R5) | Scope of exposing a language-idiomatic cancellation means (node `AbortSignal`, java future cancel, cpp cancellation token), the error kind on cancel |
| R10 | An observability hook (interceptor) | With no request/response interceptor, there's no place to hook token refresh, common logging, zlink flow-tracing header propagation, or metrics. The biggest unification gap with the framework core (message-flow tracing) | Hook point (before request/after response/on failure), signature, the flow-correlation header standard |
| R11 | Multi-value response header | Since the response header is `map<string,string>`, a repeated same name (`Set-Cookie`, etc.) collapses. The cookie jar handles it internally, but a raw consumer loses information | Extending the `headers` type (adding a multi-value accessor vs. changing the map type — compatibility), the 5-language expression |
| R12 | Making the streaming surface language-idiomatic | The download sink/upload provider is a sync callback — no backpressure, inconsistent with language idiom (node async iterator, dotnet `IAsyncEnumerable`, kotlin `Flow`). A generalization of R5 | Whether to keep the callback in parallel, the per-language idiomatic type mapping, the cpp counterpart |
| R13 | A hosting/DI integration helper | With no ASP.NET Core DI, NestJS module, or Spring bean registration helper, there's no touchpoint with the framework hosting guide flow. It's natural for the DI container to manage the client lifetime rule (§2.4) | A separate package vs. inclusion in the core, per-language scope (dotnet/node first), the configuration binding form |
| R14 | A conditional-polling terminal (`poll`) | There's demand for "repeat at interval X, up to N times/period T until the response satisfies a condition," such as waiting for job completion or checking status. Unconditional repetition (loop) is a scheduling concern and defaults to being outside the transport contract, but can be subsumed as a special form of poll | The terminal signature (condition predicate/interval/bound), the return form (final response vs. history), the boundary with retry (Chapter 6), whether cancellation (R9) must precede it, whether to include unconditional repetition |

Don't grow the columns of this table to record a registration/
promotion history — leave it in the plan (in progress) or commit
message. **Removed numbers**: R2 (timeout `DeadlineExceeded`), R3
(backoff+jitter), and R6 (header casing) were promoted to the contract
body (Chapter 6 §6.2 / Chapter 4 §4.3) and removed. R3's remaining
issue is split out as R3′.
