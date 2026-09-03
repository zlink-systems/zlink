# Phase 8 Core/binding documentation surgical-update summary

## Result

- Modified 116 existing documentation files; no repository file was created, removed, or renamed.
- Existing section/heading counts did not decrease in any modified file; paired Korean/English heading counts match.
- Aggregate numstat: 2,442 insertions / 2,467 deletions (deletion-to-insertion ratio 1.010, below the 1.2 stop threshold).
- No file lost 20% or more of its original lines.
- A `†` marks a permitted per-file `deletions > insertions` case: an old callback/handler/request-sequence API example block was replaced by a shorter current pull/completion API example. No section or heading was removed.

Reason codes: `CORE-GUIDE` = current C completion/retry/STREAM/monitor usage; `CORE-REF` = exact C signatures/lifecycle/pull reference; `B-GUIDE` = nine decisions and language terminal forms; `B-MSG` = reply token and terminal builder; `B-SOCKET` = socket/router/STREAM surface; `B-EVENT` = monitor/timer pull and poller ownership; `B-ERROR` = request terminal/error delivery; `B-README` = top-level contract summary.

## Modified-file line counts

| File | HEAD lines | Current lines | Numstat | Reason |
|---|---:|---:|---:|---|
| `bindings/doc/guide/cpp/index.en.md` | 311 | 320 | +35/-26 | B-GUIDE |
| `bindings/doc/guide/cpp/index.ko.md` | 307 | 316 | +34/-25 | B-GUIDE |
| `bindings/doc/guide/dotnet/index.en.md` | 333 | 345 | +38/-26 | B-GUIDE |
| `bindings/doc/guide/dotnet/index.ko.md` | 321 | 332 | +37/-26 | B-GUIDE |
| `bindings/doc/guide/go/index.en.md` | 355 | 369 | +28/-14 | B-GUIDE |
| `bindings/doc/guide/go/index.ko.md` | 349 | 363 | +27/-13 | B-GUIDE |
| `bindings/doc/guide/java/index.en.md` | 436 | 449 | +42/-29 | B-GUIDE |
| `bindings/doc/guide/java/index.ko.md` | 424 | 438 | +40/-26 | B-GUIDE |
| `bindings/doc/guide/node/index.en.md` | 310 | 322 | +32/-20 | B-GUIDE |
| `bindings/doc/guide/node/index.ko.md` | 305 | 316 | +31/-20 | B-GUIDE |
| `bindings/doc/guide/python/index.en.md` | 153 | 155 | +38/-36 | B-GUIDE |
| `bindings/doc/guide/python/index.ko.md` | 142 | 145 | +35/-32 | B-GUIDE |
| `bindings/doc/guide/rust/index.en.md` | 280 | 295 | +23/-8 | B-GUIDE |
| `bindings/doc/guide/rust/index.ko.md` | 278 | 292 | +22/-8 | B-GUIDE |
| `bindings/doc/reference/cpp/02-messaging.en.md` | 192 | 193 | +14/-13 | B-MSG |
| `bindings/doc/reference/cpp/02-messaging.ko.md` | 199 | 200 | +14/-13 | B-MSG |
| `bindings/doc/reference/cpp/03-sockets.en.md` | 342 | 344 | +19/-17 | B-SOCKET |
| `bindings/doc/reference/cpp/03-sockets.ko.md` | 351 | 353 | +18/-16 | B-SOCKET |
| `bindings/doc/reference/cpp/04-eventing.en.md` | 215 | 215 | +9/-9 | B-EVENT |
| `bindings/doc/reference/cpp/04-eventing.ko.md` | 223 | 223 | +9/-9 | B-EVENT |
| `bindings/doc/reference/cpp/05-errors.en.md` | 84 | 84 | +5/-5 | B-ERROR |
| `bindings/doc/reference/cpp/05-errors.ko.md` | 87 | 87 | +6/-6 | B-ERROR |
| `bindings/doc/reference/dotnet/02-messaging.en.md` | 214 | 214 | +10/-10 | B-MSG |
| `bindings/doc/reference/dotnet/02-messaging.ko.md` | 215 | 215 | +11/-11 | B-MSG |
| `bindings/doc/reference/dotnet/03-sockets.en.md` | 312 | 310 | +17/-19 | B-SOCKET†: packet callback block → shorter `ReceivePacket` pull example |
| `bindings/doc/reference/dotnet/03-sockets.ko.md` | 316 | 313 | +16/-19 | B-SOCKET†: packet callback block → shorter `ReceivePacket` pull example |
| `bindings/doc/reference/dotnet/04-eventing.en.md` | 201 | 201 | +9/-9 | B-EVENT |
| `bindings/doc/reference/dotnet/04-eventing.ko.md` | 202 | 202 | +9/-9 | B-EVENT |
| `bindings/doc/reference/dotnet/05-errors.en.md` | 99 | 99 | +5/-5 | B-ERROR |
| `bindings/doc/reference/dotnet/05-errors.ko.md` | 102 | 102 | +6/-6 | B-ERROR |
| `bindings/doc/reference/go/02-messaging.en.md` | 215 | 201 | +25/-39 | B-MSG†: callback/channel request block → shorter `Submit(ctx)` result example |
| `bindings/doc/reference/go/02-messaging.ko.md` | 235 | 218 | +26/-43 | B-MSG†: callback/channel request block → shorter `Submit(ctx)` result example |
| `bindings/doc/reference/go/03-sockets.en.md` | 320 | 312 | +24/-32 | B-SOCKET†: packet handler block → shorter `RecvPacket` pull example |
| `bindings/doc/reference/go/03-sockets.ko.md` | 343 | 333 | +24/-34 | B-SOCKET†: packet handler block → shorter `RecvPacket` pull example |
| `bindings/doc/reference/go/04-eventing.en.md` | 259 | 255 | +10/-14 | B-EVENT†: monitor/timer handler blocks → shorter pull examples |
| `bindings/doc/reference/go/04-eventing.ko.md` | 274 | 268 | +9/-15 | B-EVENT†: monitor/timer handler blocks → shorter pull examples |
| `bindings/doc/reference/go/05-errors.en.md` | 108 | 108 | +2/-2 | B-ERROR |
| `bindings/doc/reference/go/05-errors.ko.md` | 124 | 124 | +2/-2 | B-ERROR |
| `bindings/doc/reference/go/README.en.md` | 59 | 59 | +6/-6 | B-README |
| `bindings/doc/reference/go/README.ko.md` | 60 | 60 | +6/-6 | B-README |
| `bindings/doc/reference/java/02-messaging.en.md` | 220 | 220 | +16/-16 | B-MSG |
| `bindings/doc/reference/java/02-messaging.ko.md` | 227 | 226 | +16/-17 | B-MSG†: request callback text → shorter `CompletionStage` terminal text |
| `bindings/doc/reference/java/03-sockets.en.md` | 330 | 330 | +25/-25 | B-SOCKET |
| `bindings/doc/reference/java/03-sockets.ko.md` | 343 | 343 | +25/-25 | B-SOCKET |
| `bindings/doc/reference/java/04-eventing.en.md` | 228 | 228 | +11/-11 | B-EVENT |
| `bindings/doc/reference/java/04-eventing.ko.md` | 239 | 238 | +10/-11 | B-EVENT†: handler registration text → shorter monitor/timer pull text |
| `bindings/doc/reference/java/05-errors.en.md` | 93 | 93 | +5/-5 | B-ERROR |
| `bindings/doc/reference/java/05-errors.ko.md` | 99 | 99 | +6/-6 | B-ERROR |
| `bindings/doc/reference/java/README.en.md` | 47 | 47 | +3/-3 | B-README |
| `bindings/doc/reference/java/README.ko.md` | 47 | 47 | +3/-3 | B-README |
| `bindings/doc/reference/node/02-messaging.en.md` | 226 | 221 | +29/-34 | B-MSG†: callback request block → shorter Promise `submit()` example |
| `bindings/doc/reference/node/02-messaging.ko.md` | 240 | 229 | +28/-39 | B-MSG†: callback request block → shorter Promise `submit()` example |
| `bindings/doc/reference/node/03-sockets.en.md` | 325 | 319 | +27/-33 | B-SOCKET†: packet callback block → shorter `recvPacket` pull example |
| `bindings/doc/reference/node/03-sockets.ko.md` | 347 | 340 | +28/-35 | B-SOCKET†: packet callback block → shorter `recvPacket` pull example |
| `bindings/doc/reference/node/04-eventing.en.md` | 176 | 178 | +10/-8 | B-EVENT |
| `bindings/doc/reference/node/04-eventing.ko.md` | 186 | 187 | +10/-9 | B-EVENT |
| `bindings/doc/reference/node/05-errors.en.md` | 93 | 93 | +4/-4 | B-ERROR |
| `bindings/doc/reference/node/05-errors.ko.md` | 102 | 102 | +5/-5 | B-ERROR |
| `bindings/doc/reference/python/02-messaging.en.md` | 166 | 166 | +8/-8 | B-MSG |
| `bindings/doc/reference/python/02-messaging.ko.md` | 182 | 181 | +9/-10 | B-MSG†: `RequestCallbackOp` text → shorter awaited `submit()` terminal text |
| `bindings/doc/reference/python/03-sockets.en.md` | 322 | 325 | +35/-32 | B-SOCKET |
| `bindings/doc/reference/python/03-sockets.ko.md` | 338 | 340 | +36/-34 | B-SOCKET |
| `bindings/doc/reference/python/04-eventing.en.md` | 221 | 222 | +11/-10 | B-EVENT |
| `bindings/doc/reference/python/04-eventing.ko.md` | 234 | 235 | +12/-11 | B-EVENT |
| `bindings/doc/reference/python/05-errors.en.md` | 87 | 87 | +4/-4 | B-ERROR |
| `bindings/doc/reference/python/05-errors.ko.md` | 95 | 95 | +4/-4 | B-ERROR |
| `bindings/doc/reference/rust/02-messaging.en.md` | 223 | 216 | +22/-29 | B-MSG†: callback-only request block → shorter Future `submit()` example |
| `bindings/doc/reference/rust/02-messaging.ko.md` | 240 | 230 | +24/-34 | B-MSG†: callback-only request block → shorter Future `submit()` example |
| `bindings/doc/reference/rust/03-sockets.en.md` | 265 | 269 | +24/-20 | B-SOCKET |
| `bindings/doc/reference/rust/03-sockets.ko.md` | 283 | 288 | +28/-23 | B-SOCKET |
| `bindings/doc/reference/rust/04-eventing.en.md` | 237 | 233 | +23/-27 | B-EVENT†: monitor/timer callback blocks → shorter pull examples |
| `bindings/doc/reference/rust/04-eventing.ko.md` | 250 | 244 | +26/-32 | B-EVENT†: monitor/timer callback blocks → shorter pull examples |
| `bindings/doc/reference/rust/05-errors.en.md` | 91 | 87 | +8/-12 | B-ERROR†: callback error-delivery block → shorter Future terminal description |
| `bindings/doc/reference/rust/05-errors.ko.md` | 100 | 95 | +9/-14 | B-ERROR†: callback error-delivery block → shorter Future terminal description |
| `bindings/doc/reference/rust/README.en.md` | 54 | 54 | +3/-3 | B-README |
| `bindings/doc/reference/rust/README.ko.md` | 54 | 54 | +3/-3 | B-README |
| `bindings/go/README.godoc.md` | 67 | 73 | +7/-1 | B-README |
| `bindings/node/README.typedoc.md` | 38 | 38 | +5/-5 | B-README |
| `core/doc/guide/02-core-api.en.md` | 42 | 44 | +6/-4 | CORE-GUIDE |
| `core/doc/guide/02-core-api.ko.md` | 49 | 50 | +5/-4 | CORE-GUIDE |
| `core/doc/guide/03-0-socket-patterns.en.md` | 39 | 41 | +5/-3 | CORE-GUIDE |
| `core/doc/guide/03-0-socket-patterns.ko.md` | 48 | 50 | +5/-3 | CORE-GUIDE |
| `core/doc/guide/03-3-dealer.en.md` | 490 | 478 | +39/-51 | CORE-GUIDE†: request callback/DATA-reply blocks → shorter completion-queue examples |
| `core/doc/guide/03-3-dealer.ko.md` | 498 | 486 | +39/-51 | CORE-GUIDE†: request callback/DATA-reply blocks → shorter completion-queue examples |
| `core/doc/guide/03-4-router.en.md` | 365 | 354 | +44/-55 | CORE-GUIDE†: callback/request-sequence blocks → shorter reply-token/completion examples |
| `core/doc/guide/03-4-router.ko.md` | 370 | 359 | +44/-55 | CORE-GUIDE†: callback/request-sequence blocks → shorter reply-token/completion examples |
| `core/doc/guide/03-5-stream.en.md` | 326 | 369 | +116/-73 | CORE-GUIDE |
| `core/doc/guide/03-5-stream.ko.md` | 389 | 379 | +66/-76 | CORE-GUIDE†: packet callback examples → shorter current pull examples |
| `core/doc/guide/06-monitoring.en.md` | 32 | 32 | +6/-6 | CORE-GUIDE |
| `core/doc/guide/06-monitoring.ko.md` | 37 | 37 | +4/-4 | CORE-GUIDE |
| `core/doc/guide/08-routing-id.en.md` | 46 | 46 | +7/-7 | CORE-GUIDE |
| `core/doc/guide/08-routing-id.ko.md` | 54 | 55 | +8/-7 | CORE-GUIDE |
| `core/doc/guide/09-message-api.en.md` | 35 | 37 | +5/-3 | CORE-GUIDE |
| `core/doc/guide/09-message-api.ko.md` | 44 | 46 | +5/-3 | CORE-GUIDE |
| `core/doc/guide/scenarios.en.md` | 20 | 20 | +2/-2 | CORE-GUIDE |
| `core/doc/guide/scenarios.ko.md` | 25 | 25 | +2/-2 | CORE-GUIDE |
| `core/doc/reference/03-socket-lifecycle.en.md` | 209 | 215 | +68/-62 | CORE-REF |
| `core/doc/reference/03-socket-lifecycle.ko.md` | 204 | 212 | +68/-60 | CORE-REF |
| `core/doc/reference/05-raw-receive.en.md` | 93 | 88 | +23/-28 | CORE-REF†: handler receive block → shorter current pull API example |
| `core/doc/reference/05-raw-receive.ko.md` | 91 | 88 | +24/-27 | CORE-REF†: handler receive block → shorter current pull API example |
| `core/doc/reference/06-pair.en.md` | 46 | 47 | +6/-5 | CORE-REF |
| `core/doc/reference/06-pair.ko.md` | 47 | 48 | +6/-5 | CORE-REF |
| `core/doc/reference/11-dealer.en.md` | 192 | 177 | +67/-82 | CORE-REF†: callback/DATA-reply sections → shorter completion-queue reference |
| `core/doc/reference/11-dealer.ko.md` | 193 | 179 | +69/-83 | CORE-REF†: callback/DATA-reply sections → shorter completion-queue reference |
| `core/doc/reference/12-router.en.md` | 267 | 248 | +95/-114 | CORE-REF†: physical-pair/request-sequence blocks → shorter RID/reply-token reference |
| `core/doc/reference/12-router.ko.md` | 269 | 244 | +92/-117 | CORE-REF†: physical-pair/request-sequence blocks → shorter RID/reply-token reference |
| `core/doc/reference/13-stream.en.md` | 93 | 87 | +30/-36 | CORE-REF†: packet handler block → shorter `zlink_stream_recv_packet` reference |
| `core/doc/reference/13-stream.ko.md` | 96 | 89 | +32/-39 | CORE-REF†: packet handler block → shorter `zlink_stream_recv_packet` reference |
| `core/doc/reference/14-socket-monitor.en.md` | 135 | 128 | +20/-27 | CORE-REF†: monitor callback block → shorter `zlink_socket_monitor_recv` reference |
| `core/doc/reference/14-socket-monitor.ko.md` | 137 | 132 | +20/-25 | CORE-REF†: monitor callback block → shorter `zlink_socket_monitor_recv` reference |
| `core/doc/reference/15-polling.en.md` | 183 | 199 | +27/-11 | CORE-REF |
| `core/doc/reference/15-polling.ko.md` | 183 | 199 | +28/-12 | CORE-REF |
| `core/doc/reference/16-timers.en.md` | 88 | 79 | +8/-17 | CORE-REF†: timer-handler block → shorter `zlink_timer_recv` pull reference |
| `core/doc/reference/16-timers.ko.md` | 91 | 82 | +8/-17 | CORE-REF†: timer-handler block → shorter `zlink_timer_recv` pull reference |
| `core/doc/reference/18-errors.en.md` | 97 | 97 | +4/-4 | CORE-REF |
| `core/doc/reference/18-errors.ko.md` | 95 | 95 | +4/-4 | CORE-REF |

## Nine decisions and canonical locations

All nine decisions are also stated in each of the seven language guide pairs under `bindings/doc/guide/{cpp,dotnet,go,java,node,python,rust}/index.{en,ko}.md`, using that binding's current terminal forms.

1. Blocking versus nonblocking/awaitable send: `core/doc/reference/03-socket-lifecycle.{en,ko}.md`.
2. Core owns accepted pre-admission retry: `core/doc/reference/03-socket-lifecycle.{en,ko}.md`.
3. Shared `ZLINK_OPT_PENDING_MAX_MSGS/BYTES`: `core/doc/reference/03-socket-lifecycle.{en,ko}.md`.
4. Admission is not peer delivery: `core/doc/reference/03-socket-lifecycle.{en,ko}.md`.
5. Poll readiness requires draining the completion queue: `core/doc/reference/15-polling.{en,ko}.md`.
6. STREAM RAW/PACKET is selected before the first successful bind/connect: `core/doc/guide/03-5-stream.{en,ko}.md` and `core/doc/reference/13-stream.{en,ko}.md`.
7. Cancellation boundary (pre-submit/framework waiter versus Core-owned accepted operation): `core/doc/reference/03-socket-lifecycle.{en,ko}.md`.
8. Request reply arrives through completion/Task/Future, not ordinary DATA receive: `core/doc/guide/03-3-dealer.{en,ko}.md` and `core/doc/reference/{11-dealer,12-router}.{en,ko}.md`.
9. A public `PollCompletion` owner must keep `wait()` looping while terminal operations remain: `core/doc/reference/15-polling.{en,ko}.md` and every binding guide pair.

## Public-poller source verification

Decision 9 was confirmed in all seven implementations before documenting it:

- C++: `bindings/cpp/src/Runtime/Eventing/poller.cpp`
- .NET: `bindings/dotnet/src/Zlink/Runtime/Eventing/Poller.cs`
- Go: `bindings/go/internal/native/poller_timer.go`
- Java: `bindings/java/src/main/java/systems/zlink/runtime/eventing/NativePoller.java`
- Node: `bindings/node/src/zlink/runtime/eventing/poller.ts`
- Python: `bindings/python/src/zlink/_runtime/eventing/poller.py`
- Rust: `bindings/rust/src/runtime/eventing/poller.rs`

## Stale-token rescan

Command (the same Phase 9 expression over the existing scoped files, with path normalization before classification):

```bash
rg -n -i -e 'zlink_send_async|zlink_send_async_cancel|zlink_send_(async_)?options_t|zlink_request_options_t|zlink_send_complete_handler|zlink_reply_handler_fn|zlink_stream_packet_handler|transport_pair_(id|generation)|ZLINK_OPT_SEND_PENDING_MAX_(MSGS|BYTES)|RoutedSend|RequestCallback|RequestReplyCompletion|request_seq|RequestSeq|TrySend|send_async|sendAsync|onPacket|on_packet|onEvent|on_event|onFire|on_fire|0\.15\.1' core/doc/guide core/doc/reference core/README* core/examples bindings/doc/guide/{cpp,dotnet,go,java,node,python,rust} bindings/doc/reference/{c,cpp,dotnet,go,java,node,python,rust} bindings/c/samples bindings/*/README*
```

- Raw matches: 169.
- Confirmed stale matches: 0.
- False positives: 169, all from current subscription-event names such as `subscription_event`, `SubscriptionEvent`, `receiveSubscriptionEvent`, or `wait_for_subscription_event` matching the broad `onEvent` alternative.
- Extra removed C handler-symbol scan (`zlink_socket_monitor_handler|zlink_monitor_ignore_handler|zlink_timer_handler`): 0 matches.

## Validation

- `git diff --check` over the scoped documentation: passed.
- Modified-file heading preservation: passed for all 116 files.
- Korean/English paired heading-count parity: passed.
- Forbidden/frozen paths (`core/doc/spec`, `bindings/doc/spec`, `framework/doc`, `doc`) changed by this work: 0.
- Build and tests were not run, as explicitly required.

## Held items

- `core/include/zlink_enum.h` still has the inventory BLOCKER comment. It is outside the authorized documentation scope; guides describe receive-flow support by socket type and do not copy the stale completion-lane wording.
- Kotlin and plain JavaScript PACKET sample sources remain callback-based. The Core STREAM guide preserves both language tabs and explicitly says that a current pull-based sample is not available; it does not embed stale code. Node/TypeScript uses the current pull sample.
- No other Phase 8 guide/reference item is held.
