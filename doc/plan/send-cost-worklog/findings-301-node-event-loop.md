# Node raw event loop — 조사 결과

## 1. raw request 한 건이 지나는 경로

1. source 프로세스는 `main()`에서 transport를 만들고 active trigger를
   `core.runActive()`에 넘긴다
   (`framework/bench/grpc/node/client/main.js:228-243`).
   `implementation=zlink-node`는 framework transport가 아니라
   `createRawTransport()`를 고른다 (`:203-208`). `framework-transport.js`는
   `zlink-framework-node`일 때만 선택되는 Nest route client이고
   (`framework/bench/grpc/node/client/main.js:206-208`,
   `framework/bench/grpc/node/client/framework-transport.js:45-66`), raw 행에는
   참여하지 않는다.
2. `request-backpressure` trigger는 `runActive()`에서
   `requestBackpressure()`로 들어간다
   (`framework/bench/grpc/node/client/bench-core.js:201-204`). raw transport가
   `requestSubmission`을 제공하므로 그 전용 branch가 선택된다 (`:261-262`).
3. 매 iteration은 payload를 만들고 `metrics.begin()`으로 in-flight를 하나
   증가시킨 뒤 (`:263-271`; 증가는 `SourceMetrics.begin()`의 `:58-62`),
   raw `requestSubmission(0, payload)`을 호출한다 (`:271`). 이 호출은
   `submitRequest()`로 전달된다
   (`framework/bench/grpc/node/client/main.js:182-186`).
4. raw transport는 request ROUTER socket 하나를 만들고 remote routing id를
   고정한다 (`framework/bench/grpc/node/client/main.js:138-145`).
   `submitRequest()`는 routed `request`, envelope, protobuf body, timeout을
   만든 뒤 native `submit()`을 호출한다 (`:155-160`). 반환값의 `result`와
   `admitted`는 caller에 그대로 돌려주고, `reply`는 reply parts의 마지막
   body를 decode한 뒤 모든 part를 close하는 Promise다 (`:161-171`).
5. caller는 그 `reply`를 별도 async task로 pending set에 넣는다
   (`framework/bench/grpc/node/client/bench-core.js:276-286`). reply가 settle한
   뒤에만 header를 검사하고 metrics를 complete한다 (`:278-283`; 감소는
   `SourceMetrics.complete()`의 `:65-76`).
6. raw caller 자신은 `POLLCOMPLETION` poller를 만들거나 socket을 등록하지
   않는다. 그래서 public owner가 없는 동안에는 Node binding의 runtime watch가
   completion queue를 drain한다: watch는 pending entry가 있을 때 시작되고
   (`bindings/node/src/zlink/runtime/messaging/completion_owner.ts:736-743`),
   callback은 public owner가 없을 때 `drain()`을 호출한다 (`:745-759`).
   반대로 public poller에 `PollCompletion`으로 등록하면 completion owner가
   그 poller로 이전하며 (`bindings/node/src/zlink/runtime/eventing/poller.ts:237-260`),
   `wait()`이 readiness에서 그 owner를 drain한다 (`:193-205`).

## 2. event loop가 1.5%만 도는 이유

- **어디** — raw request-backpressure branch의
  `framework/bench/grpc/node/client/bench-core.js:261-290`, 특히
  `:287-289`이다. raw request 생성은
  `framework/bench/grpc/node/client/main.js:155-171`에 있다.

- **무엇이 막는가** — 정상 제출(`result !== Backpressured`)은 `while`을
  빠져나가거나 event loop에 양보하지 않는다. 이 branch의 유일한 `await`는
  `Backpressured`일 때의 `submission.admitted`다 (`bench-core.js:287-289`).
  reply task는 만들어 pending에 넣을 뿐 (`:276-286`), active 구간에는
  completion poller 진행이나 `setImmediate` yield가 없다. 따라서
  backpressure를 처음 받으면 source는 새 request 제출을 멈추고 admission
  Promise를 await한다. 그 동안 reply/admission completion은 raw caller가 아닌
  binding runtime watch가 event loop turn에서 drain한다
  (`completion_owner.ts:736-759`). 관측된 낮은 ELU는 이 **admission await로
  제출 loop가 정지하고 runtime watch의 wake만 기다리는 구조**와 일치한다.
  다만 0.0124~0.0153이라는 정확한 비율 자체는 코드 상수가 아니라 측정값이다.

- **정본은 어떻게 하는가** — Node 정본은 모든 requester socket을 단 하나의
  `POLLCOMPLETION` poller에 등록한다
  (`bindings/node/perf/multi/perf_multi_socket_reqrep.ts:76-80`). 한 socket
  sweep에서 reply task와 backpressure admission task를 각각 보관한다
  (`:144-155`). sweep 뒤에는 모두 blocked일 때만 deadline 이하 최대 50 ms를
  기다리고, 그 외에는 0 ms로 poller를 한 번 진행한 후 항상
  `await sleepImmediate()`로 continuation을 양보한다 (`:159-167`; timeout
  helper `:47-52`). C도 submitted work가 있으면 0 ms, 없으면
  `poll_timeout_until(deadline, 50)`으로 같은 completion poller를 진행한다
  (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:576-603`; helper
  `:123-134`). C의 quantum은 completion 진행 시점만 정할 뿐 outstanding
  request 상한은 만들지 않는다 (`:509-525`).

- **고치는 방법** — raw request socket을 정확히 하나의 public
  `POLLCOMPLETION` poller에 등록하고, raw request-backpressure loop를
  socket 하나의 bounded turn으로 바꾼다. 매 turn은 (1) request를 submit하고
  reply/admission을 각각 pending으로 유지하고, (2) submission이 있었으면
  `wait(..., 0)`, 막힌 상태라면 `pollTimeoutUntil(deadline, 50)`으로 그
  poller를 한 번 진행하고, (3) `setImmediate` turn을 양보해야 한다. active
  종료 뒤에도 같은 poller로 reply와 admission을 함께 drain해야 한다. 이는
  정본의 single-socket 경우와 같고, 새 in-flight 상한이나 benchmark 제한을
  만들지 않는다.

- **위험** — `PollCompletion` 등록은 completion owner를 public poller로
  이전한다. 같은 socket에 별도 completion poller를 더 만들거나, 종료 때
  owner를 회복하기 전에 socket/context를 닫으면 소유권과 pending Promise
  정리가 깨질 수 있다. binding은 이미 public owner가 다른 경우 실패시킨다
  (`bindings/node/src/zlink/runtime/messaging/completion_owner.ts:505-522`),
  poller drain은 그 owner만 수행한다 (`:524-540`). 따라서 등록·close 순서와
  reply/admission 양쪽의 종료 drain을 정본과 같이 검증해야 한다.

## 3. run3만 ELU 1.0으로 튄 이유

확인 가능한 사실은 두 가지다.

1. `peak_in_flight`는 submit 직전 `begin()`에서 증가하고
   (`framework/bench/grpc/node/client/bench-core.js:268`, `:58-62`), reply
   continuation이 실행될 때만 감소한다 (`:278-283`, `:65-76`). 따라서
   run3의 65,537은 그 수만큼 begin이 reply continuation의 감소보다 먼저
   진행됐다는 뜻이다.
2. 이 branch는 `Backpressured`일 때만 await한다
   (`framework/bench/grpc/node/client/bench-core.js:287-289`). 그러므로 run3의
   ELU 1.0은 active 구간에 admission await로 idle 상태에 머무르지 않고,
   synchronous submit loop가 event loop turn을 계속 점유한 경우와 부합한다.

그러나 왜 그 run에서 admission await가 늦었거나 없었는지는 이 source와 제공된
5-run 표만으로 확정할 수 없다. 대상 client에는 65,536/65,537을 분기하는 상수나
HWM 설정이 없고, run별 첫 `Backpressured` 시점, submit result 열, applied
Auto-HWM/queue snapshot도 없다. 따라서 `65,537 = 어떤 Auto-HWM 값 + 1` 또는
특정 Core 결함이라고 결론 내릴 근거는 없다.

## 4. send 경로가 ELU 1.00으로 포화인 것과 같은 원인인가

같은 원인이라고 확인할 수 없다. send-saturation은 request reply completion을
수거하지 않는다. `sendWorkers()`의 각 logical stream은 `sendSubmission()`을
호출하고, `Backpressured`일 때만 `admitted`를 await하며
(`framework/bench/grpc/node/client/bench-core.js:322-345`), 그 외에는 submit
직후 metrics를 complete한다 (`:332-339`). raw send도 request/reply Promise가
아닌 `send(...).submit()`만 반환한다
(`framework/bench/grpc/node/client/main.js:174-194`).

따라서 send의 ELU 1.00은 reply completion poller 부재가 아니라, 정상 send가
await/yield 없이 submit loop를 계속 실행하는 구조와 직접 맞는다. request와 send는
모두 `Backpressured`에서만 admission을 await한다는 공통점은 있지만, request에는
별도 reply completion queue가 있어 정본의 completion-poller turn이 필요하고,
send에는 그 reply stage가 없다.

## 5. 확인 못 한 것

- 5개 run 각각의 source result, 첫 `Backpressured` 시각·횟수, submit result
  sequence, applied Auto-HWM과 queue snapshot이 보존되어 있지 않다. 그래서
  run3만 admission await를 피한 직접 원인과 65,537의 정확한 경계를 확인하지
  못했다.
- raw request socket에 public poller를 실제로 등록했을 때 close 시 owner가
  runtime watch로 복귀하는 순서와, active/drain 구간의 정확한 결과는 구현과
  focused contract test 전에는 확인할 수 없다.
- 이 조사에서는 빌드·테스트·벤치를 실행하지 않았고, 제품 코드나 스펙 문서는
  변경하지 않았다.
