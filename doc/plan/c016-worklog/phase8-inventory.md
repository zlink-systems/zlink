# 0.16.0 Phase 8 stale-API inventory

Scope and method: read-only search of the user-specified Core and binding guide,
reference, README, godoc, and public sample paths.  `framework/**` was neither
read nor reported; no build or test was run.  A counts occurrences matched by
the Phase 9 regex verbatim; B counts confirmed semantic-stale statements
(callback completion, DATA reply receive, caller retry queue, old pending
name, STREAM selection, two-lane wording, or no-completion-lane wording).
`FP` means the regex matched `subscription_event` or a non-API sample helper,
not a stale public API.  No historical fixture/changelog hit was found.  The
exact Phase 9 search was:

```bash
rg -n -i -e 'zlink_send_async|zlink_send_async_cancel|zlink_send_(async_)?options_t|zlink_request_options_t|zlink_send_complete_handler|zlink_reply_handler_fn|zlink_stream_packet_handler|transport_pair_(id|generation)|ZLINK_OPT_SEND_PENDING_MAX_(MSGS|BYTES)|RoutedSend|RequestCallback|RequestReplyCompletion|request_seq|RequestSeq|TrySend|send_async|sendAsync|onPacket|on_packet|onEvent|on_event|onFire|on_fire|0\.15\.1' \
  core/doc/guide core/doc/reference core/README* core/examples \
  bindings/doc/guide/{cpp,dotnet,go,java,node,python,rust} \
  bindings/doc/reference/{c,cpp,dotnet,go,java,node,python,rust} \
  bindings/c/samples bindings/*/README*
```

## 1. Summary

### Core: update required

| File pair (each language unless noted) | A | B | ko/en | Disposition |
|---|---:|---:|---|---|
| `core/doc/guide/02-core-api.{ko,en}.md` | 1 | 1 | pair | update |
| `03-0-socket-patterns.{ko,en}.md` | 1 | 1 | pair | update |
| `03-3-dealer.{ko,en}.md` | 4 | 3 | pair | update |
| `03-4-router.{ko,en}.md` | 11 | 5 | pair | update |
| `03-5-stream.{ko,en}.md` | ko 6 / en 7 | 5 | broken | update |
| `08-routing-id.{ko,en}.md` | 2 | 1 | pair | update |
| `09-message-api.{ko,en}.md` | 0 | 1 | pair | update |
| `scenarios.{ko,en}.md` | 1 | 0 | pair | update |
| `core/doc/reference/03-socket-lifecycle.{ko,en}.md` | 18 | 12 | pair | update, highest priority |
| `05-raw-receive.{ko,en}.md` | 3 | 2 | pair | update |
| `06-pair.{ko,en}.md` | 4 | 1 | pair | update |
| `11-dealer.{ko,en}.md` | 9 | 6 | pair | update |
| `12-router.{ko,en}.md` | 17 | 5 | pair | update |
| `13-stream.{ko,en}.md` | 6 | 5 | pair | update |
| `15-polling.{ko,en}.md` | 2 | 2 | pair | update |
| `18-errors.{ko,en}.md` | 1 | 1 | pair | update |

No matching stale API was found in `core/README*` or `core/examples`.

### Binding guides / README: update required

| File pair | A | B | ko/en | Disposition |
|---|---:|---:|---|---|
| `bindings/doc/guide/cpp/index.{ko,en}.md` | 2 | 2 | pair | update |
| `dotnet/index.{ko,en}.md` | 2 | 3 | pair | update |
| `go/index.{ko,en}.md` | 0 | 2 | pair | update |
| `java/index.{ko,en}.md` | 2 | 3 | pair | update |
| `node/index.{ko,en}.md` | 2 | 3 | pair | update |
| `python/index.{ko,en}.md` | 0 | 5 | pair | update |
| `rust/index.{ko,en}.md` | 2 | 3 | pair | update |
| `bindings/go/README.godoc.md` | 0 | 1 | n/a | update only if its listed stream API is meant as guide contract |
| `bindings/node/README.md` | 0 stale; 5 FP | 0 | n/a | **current / keep** |
| `bindings/node/README.typedoc.md` | 0 | 2 | n/a | update |
| `bindings/python/README.md` | 0 stale; 1 FP | 0 | n/a | keep |

### Binding references: update required

All entries below are paired `ko,en` files and their quoted public surface is
absent from the corresponding current binding contracts.

| Files | A per file | B per file | Disposition |
|---|---:|---:|---|
| `cpp/02-messaging`, `cpp/03-sockets`, `cpp/04-eventing` | 8, 9, 6 | 3, 5, 4 | update |
| `dotnet/02-messaging`, `dotnet/03-sockets`, `dotnet/04-eventing` | 13, 17, 7 | 5, 8, 4 | update |
| `go/02-messaging`, `go/03-sockets`, `go/04-eventing`, `go/05-errors` | 17, 10, 6, 4 | 7, 6, 4, 1 | update |
| `java/02-messaging`, `java/03-sockets`, `java/04-eventing` | 15, 21, 10 | 6, 10, 5 | update |
| `node/02-messaging`, `node/03-sockets`, `node/04-eventing` | 17, 13, 5 | 7, 8, 3 | update |
| `python/02-messaging`, `python/03-sockets`, `python/04-eventing` | 6, 12, 8 | 3, 7, 4 | update |
| `rust/02-messaging`, `rust/03-sockets`, `rust/04-eventing`, `rust/05-errors` | 9, 10, 6, 0 | 5, 6, 4, 1 | update |
| `go/README.{ko,en}.md`, `java/README.{ko,en}.md` | 2, 2 | 1, 1 | update |

The following reference README regex hits are false positives from
`subscription_event`, and require no Phase 8 edit: `cpp/README.{ko,en}:41-42`,
`dotnet/README.{ko,en}:41-44`, `node/README.{ko,en}:39-40`,
`python/README.{ko,en}:46-48`, and `rust/README.{ko,en}:49`.  The second Go
README hit (`:54-55`) is also `subscription_event`; its `:33-35` callback /
channel text is stale.  Java README `:42` names
`RequestCallbackSubmitOperation`, which is stale.

`bindings/c/samples/sample_common.h:230` and
`bindings/c/samples/{stream_recv_sample.c:9,stream_packet_recv_sample.c:9}`
are false positives (`subscription_event` and local `callback_signal_t`);
`bindings/c/samples/dealer_router_recv_sample.c:23` already uses a
`reply_token`.  No C sample is a stale-API edit target.

## 2. File-by-file evidence and classification

### Authoritative public-surface comparison

The removal judgment is source-backed, rather than inferred from the docs:

- `core/include/zlink/socket/api.h:39-65,225-254,258-276,312-326` exports
  completion ID/record/recv, part send/request/reply with `reply_token`, and
  packet receive; it contains none of the old async/callback/request-sequence
  functions or option structs.
- `core/include/zlink_enum.h:109-112` exports
  `ZLINK_OPT_PENDING_MAX_MSGS/BYTES`; the old `SEND_PENDING` names are absent.
- `core/include/zlink_enum.h:175-180` exports STREAM RAW/PACKET receive mode.
- `bindings/cpp/include/zlink/Contracts/Messaging/{received.hpp:24-85,operation_contracts.hpp:53-56,181-201}`,
  `bindings/dotnet/src/Zlink/Contracts/Messaging/{Received.cs:58,OperationContracts.cs:28-48,ReplyToken.cs:10-46}`,
  `bindings/go/contracts/{messaging.go:12-13,eventing.go:141-142,sockets.go:38-40}`,
  `bindings/java/src/main/java/systems/zlink/contracts/messaging/{Received.java:578-580,RequestSubmitOperation.java:37,ReplyToken.java:8-52}`,
  `bindings/node/src/zlink/contracts/messaging/{operations.ts:17-23,received.ts:22-76}`,
  `bindings/python/src/zlink/contracts/messaging/{received.py:11-59,156-171}`,
  and `bindings/rust/src/contracts/messaging/{received.rs:11-130,operations.rs:73-142}`
  are the current completion/reply-token surfaces.  A search of those public
  roots found no `RequestSeq`, `request_seq`, `RequestCallback`,
  `RequestReplyCompletion`, `TrySend`, `sendAsync`, `onPacket`, `onEvent`, or
  `onFire` API declaration.

### Core guide and reference evidence

| File:line (one representative quote; same stale model applies to paired translation) | A/B | Judgment and comparison |
|---|---|---|
| `core/doc/guide/02-core-api.en.md:30` — “complete through `zlink_reply_handler_fn`.” | A/B | Removed callback; replace with completion pull. `socket/api.h:321-326`. |
| `03-0-socket-patterns.en.md:26` — “STREAM may use `zlink_recv_handler()` or `zlink_stream_packet_handler()`.” | A/B | Both callback selection paths removed; mode setter and pull receive replace them. `socket/api.h:312-318`, `zlink_enum.h:175-180`. |
| `03-3-dealer.en.md:155` — “delivers the reply via callback.” | B | Also `:243-247` receives a request reply as DATA; both contradict completion queue. `spec/core/socket/06-dealer.en.md:48,153`. |
| `03-4-router.en.md:149-150` — “completes through a reply callback … `request_seq != 0`.” | A/B | Old callback and sequence token. Current API uses `zlink_reply_token_t`; see `socket/api.h:41-42,270-274`. |
| `03-5-stream.en.md:63` — “packet callback: `zlink_stream_packet_handler()`.” | A/B | Callback API removed; this chapter must explain the immutable RAW/PACKET choice. `socket/api.h:312-318`. |
| `08-routing-id.en.md:28-34` — `uint64_t request_seq` passed to `zlink_router_recv_part`. | A | Reply token supersedes sequence output. `socket/api.h:270-274`. |
| `09-message-api.en.md:34` — “Request/reply completion callbacks own … parts.” | B | Completion record owns reply storage until `zlink_completion_close`. `socket/api.h:50-65,324-326`. |
| `scenarios.en.md:14` — STREAM recommendation is `zlink_stream_packet_handler`. | A | Removed API; link to packet mode/receive instead. |
| `core/doc/reference/03-socket-lifecycle.en.md:122-160` — old async/cancel API section and `ZLINK_OPT_SEND_PENDING_MAX_*`. | A/B | Complete removed surface, including old option name and callback completion. Current header anchors above. |
| `03-socket-lifecycle.en.md:183-200` — “paired DEALER/ROUTER completion lane.” | B | Single-lane plan §3.4 says flow-state support is by socket type, not completion-lane ownership. |
| `05-raw-receive.en.md:64-81` — `zlink_stream_packet_handler` as one receive mode. | A/B | Replace callback mode with `zlink_stream_recv_packet`, and document first bind/connect selection. |
| `06-pair.en.md:8,45` — `zlink_send_async` / `zlink_send_complete_handler`. | A/B | Removed async callback surface; use part-send completion ID/queue. |
| `11-dealer.en.md:103-120` — “`zlink_reply_handler_fn` completion” and request “via callback.” | A/B | Reply is REQUEST completion, not callback or `ZLINK_POLLIN` DATA. `spec/core/socket/06-dealer.en.md:48,153`. |
| `12-router.en.md:95-101,151-156,181-221` — physical `transport_pair_*` and `request_seq`. | A/B | Physical pair/generation and request sequence have no public header declaration; use RID + reply token. |
| `13-stream.en.md:67-76` — `zlink_stream_packet_handler`. | A/B | Removed; use explicit receive mode and `zlink_stream_recv_packet`. |
| `15-polling.en.md:95,163` — completion installed with `zlink_send_complete_handler`. | A/B | Poller must drain `zlink_completion_recv` after `ZLINK_POLLCOMPLETION`. |
| `18-errors.en.md:26` — request result delivered by `zlink_reply_handler_fn`. | A/B | Result is a `zlink_completion_t` REQUEST record. |

The corresponding `.ko.md` files contain the same old surface at the same
nearby lines (not a historical explanation), so all are modification targets.

### Binding guide/reference evidence

The detailed binding chapters converge on the same three removed patterns:

| Language files:line | Quote | Judgment |
|---|---|---|
| `cpp/guide/index.en.md:120-124`; `reference/02-messaging.en.md:172-185`; `03-sockets.en.md:176-185,283-290`; `04-eventing.en.md:22-41,171-187` | “`submit(callback)` … delivers the reply through the callback”; `request_seq`; `set_packet_handler`; `on_event`/`on_fire`. | All callback/sequence surfaces absent; update guide and the three reference chapters. |
| `dotnet/guide/index.en.md:138-139`; `reference/02-messaging.en.md:192-207`; `03-sockets.en.md:173-183,272-288`; `04-eventing.en.md:20-40,135-151` | “actual reply arrives later via the callback”; `RequestSeq`, `TrySendCompletionControl`, `OnPacket`, `OnEvent`, `OnFire`. | Absent public members; current `ReplyToken` and `Task` contract cited above. |
| `go/guide/index.en.md:158,335`; `reference/02-messaging.en.md:189-201`; `03-sockets.en.md:140-213,277-296`; `04-eventing.en.md:24-44,221-240`; `05-errors.en.md:30-32` | `RequestReplyCompletion`, callback terminals, `OnSendReady`, `OnPacket`, `OnEvent`, `OnFire`. | Absent public types/callback registrations. Current `PollCompletion` is `contracts/eventing.go:141-142`. |
| `java/guide/index.en.md:159-160,302,359`; `reference/02-messaging.en.md:195-214`; `03-sockets.en.md:181-191,272-302`; `04-eventing.en.md:20-40,175-202`; `README.en.md:42` | `RequestCallbackSubmitOperation`, callback reply, `requestSeq`, `onPacket`, `onEvent`, `onFire`. | Removed. Current `RequestSubmitOperation.submit()` returns `CompletionStage` at `RequestSubmitOperation.java:37`. |
| `node/guide/index.en.md:116-117,199,246`; `reference/02-messaging.en.md:189-220`; `03-sockets.en.md:134,189-201,291-299`; `04-eventing.en.md`; | callback request, `requestSeq`, `setSendReadyHandler`, `setPacketHandler`, `onEvent`/`onFire`. | Removed callback bridge. `contracts/messaging/operations.ts:17-23` describes Core-completion Promise. |
| `python/guide/index.en.md:73-98,137-140`; `reference/02-messaging.en.md:10,89,141,243-292`; `03-sockets.en.md:141,182-292`; `04-eventing.en.md` | `RequestCallbackOp`, `request_seq`, `on_packet`, callback request reply. | Removed; replace with `ReplyToken` and completion-backed awaitable path. |
| `rust/guide/index.en.md:108,250`; `reference/02-messaging.en.md:61-80,198-216`; `03-sockets.en.md:101,155,227-244`; `04-eventing.en.md`; `05-errors.en.md:26` | `request_seq`, “callback-only”, `on_packet`, callback-driven completion. | Contradicts `operations.rs:141-142`: Future resolves from socket completion queue. |

Each row applies equally to the adjacent `.ko.md` translation; exact Korean
anchors are the same chapter/nearby translated paragraph (for example,
`bindings/doc/reference/{go,java,node}/02-messaging.ko.md:205-217`,
`bindings/doc/reference/python/03-sockets.ko.md:301-308`, and
`bindings/doc/reference/rust/02-messaging.ko.md:215-232`).

### Confirmed current / false-positive material

- `bindings/node/README.md:33-35` already says to select
  `StreamRecvMode.Raw` or `.Packet` before bind/connect; `:83-86` correctly
  says completion, monitor, timer and packet delivery are pull-only and that
  no binding retry queue exists.  Its regex hits at `:29,68,103` are
  `SubscriptionEvent` only.
- `bindings/c/samples/dealer_router_recv_sample.c:23` uses `reply_token`, and
  `stream_packet_recv_sample.c:71` uses `zlink_stream_recv_packet`; keep both.
- `bindings/node/README.typedoc.md:34-37` is not current: “Send completion
  and request reply callbacks are installed once per socket” and the retained
  stream/monitor/timer callbacks contradict the pull-only public model. It is
  a Phase 8 README target, not a historical note.
- No `0.15.1` occurrence was found in the scoped paths.

## 3. Phase 8's nine required guide decisions

“Exists” below means a current, correct description, not merely a stale
mention.  Normative spec anchors are included to make empty guide cells clear.

| Required decision | Current guide/reference coverage | Phase 8 guide work |
|---|---|---|
| 1. blocking vs nonblocking/awaitable send | No correct user-guide chapter. Old lifecycle and binding guides describe callback terminals. Normative: `core/doc/spec/core/socket/README.en.md:942-1012`. | New/rewrite Core lifecycle and every binding guide operation section. |
| 2. Core, not caller, owns pre-admission retry | Only Node README correctly says “no … retry queue” at `bindings/node/README.md:83-86`; no Core guide says why. Normative: `README.en.md:1004-1012`. | Add Core explanation; propagate language ownership wording. |
| 3. shared `PENDING_MAX_MSGS/BYTES`, no old name | No correct guide. `core/doc/reference/03-socket-lifecycle.{ko,en}:122-160` uses old `SEND_PENDING`. Normative: `README.en.md:1339-1342`. | Rewrite C option section and language projection notes. |
| 4. admission is not peer delivery | No correct guide statement. Normative: `README.en.md:1030`. | Add to Core send guide and language awaitable text. |
| 5. drain completion queue after poller readiness | No correct C example; `reference/15-polling` has obsolete handler language. Normative: `README.en.md:1193` and draft `:344-346`. | Add C poller/drain sample to lifecycle/polling guide. |
| 6. choose STREAM RAW or PACKET before bind/connect | Node README covers it (`:33-35`). Core/binding chapters still present callback selection. Normative: `08-stream.en.md:94-97`. | Rewrite Core STREAM chapter and all language STREAM sections; preserve Node text. |
| 7. cancellation boundaries | No correct guide. Old lifecycle advertises `zlink_send_async_cancel`. Draft: `core-socket-send-recv-completion-0.16.0-spec-draft.ko.md:806-821`. | Add per-language pre-submit / framework-queue / Core-owned boundary. |
| 8. reply via completion/Task/Future, not DATA recv | No correct guide: Core DEALER/ROUTER guides explicitly say callback or DATA receive; binding references say callback. Normative: `06-dealer.en.md:48,153`. | Rewrite request/reply examples across Core and all bindings. |
| 9. public `PollCompletion` owner keeps `wait()` looping | No current guide. Node README says public poller temporarily owns drain (`:83-85`) but does not state the continuous wait-loop rule. Draft: `...spec-draft.ko.md:923-936`. | New binding guide note for every language that exposes public poller ownership. |

## 4. ko/en parity findings

- **Confirmed broken:** `core/doc/guide/03-5-stream.{ko,en}.md` has different
  Phase-9-regex occurrence counts (ko 6, en 7).  Update both from the same
  RAW/PACKET pull model; do not copy only the English callback-removal text.
- **Paired but jointly stale:** every pair in the Core and binding summary
  tables has the same old public model in both languages.  Equal occurrence
  count is not a parity pass; both translations must be updated together.
- **No unpaired file was found** among the scoped matching guide/reference
  chapters.  The language-specific README/godoc files are inherently unpaired.

## 5. BLOCKERS / QUESTIONS

1. `doc/plan/dealer-router-single-lane-design.ko.md:550-584` makes
   `zlink_socket_set_receive_flow_state` public API shape stable but changes
   the interpretation to type support, not Completion-lane ownership.  The
   current Core header comment at `core/include/zlink_enum.h:243` still says
   “Paired DEALER/ROUTER completion-lane receive-flow observation.”  This is
   source/comment drift, not safe to resolve in a Phase 8 guide edit; confirm
   the code-comment owner before choosing final wording.
2. The draft’s public-poller rule (`core-socket-send-recv-completion-0.16.0-spec-draft.ko.md:923-936`)
   names language runtime behavior.  The current Node README partially covers
   drain ownership but not the required continued `wait()` loop.  Confirm the
   corresponding public-poller behavior for C++/.NET/Go/Java/Python/Rust before
   writing language-specific examples; no build/test was authorized here.
3. The search found no historical fixture/changelog under the requested paths.
   If the intended Phase 8 edit scope excludes reference catalogs in favour of
   guide-only pages, the binding `02/03/04` reference chapters should be kept
   in this inventory but scheduled separately rather than silently omitted.
