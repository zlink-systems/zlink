# Node L5~L7 진단 보고서

## 판정

벤치 fault다. 대상은 `BenchPayload` send를 찾아 handler까지 호출했지만, `CommandHandler.metrics`가 `undefined`여서 `this.metrics.record(...)`에서 예외가 났다. JSON/Protobuf codec 차이와 무관한 벤치 DI 등록 결함이다. Nest handler 활성화 경로는 constructor injection metadata로 인자를 구성한다(`framework/languages/node/packages/nestjs/src/providers.ts:562-604`). 기존 벤치는 `useFactory`로 handler 인스턴스를 등록했지만, Framework는 별도 scope 인스턴스를 만들고 JS constructor에는 `@Inject` token metadata가 없어 `new CommandHandler()`로 생성됐다. 오류 trace: `raw/diagnostic-logged/send-saturation-1024/run1/zlink-framework-node-send-saturation-1024/target.log`.

수정은 `EchoHandler`와 `CommandHandler`에 공통 `BENCH_SERVER_METRICS` token을 명시적으로 주입하고, 그 token에 기존 metrics 인스턴스를 `useValue`로 등록한다. 변경 위치: `framework/bench/grpc/node/zlink-framework-server/main.js:18,61-70,367-369`, `framework/bench/grpc/node/zlink-framework-server/bench-handlers.js:4,14-30,38`.

## 누락된 컨트롤

두 컨트롤 모두 L5_7 스위치 없이 실행했다. 각 실행은 별도 exclusive hold 안에서 했다.

| 컨트롤 | 결과 | Raw |
|---|---|---|
| send-saturation, 1K, 수정 전 | submitted=completed=138013, received=0, errors=0; count mismatch로 무효 | `raw/control/send-saturation-1024/run1/zlink-framework-node-send-saturation-1024/results.json` |
| request-serial, 1K, 수정 전 | target은 시작했지만 route-ready request probe 뒤 source stats endpoint가 준비되지 않음 | `raw/control/request-serial-1024/run1/zlink-framework-node-request-serial-1024/target.log`, `source.log` |
| send-saturation, 1K, 수정 후 | submitted=completed=received=217543, errors=0 | `raw/control-after-fix/send-saturation-1024/run1/zlink-framework-node-send-saturation-1024/results.json` |
| request-serial, 1K, 수정 후 | submitted=completed=received=21309, errors=0 | `raw/control-after-fix/request-serial-1024/run1/zlink-framework-node-request-serial-1024/results.json` |

대상 message-flow 진단은 `BenchPayload`와 `message_kind=send`를 확인했고 `reason=handler_exception`, `TypeError: Cannot read properties of undefined (reading 'record')`를 기록했다. `channel-dispatchers.ts:183-201`은 packet name으로 handler를 찾고 pipeline에 넘기며, `channel-dispatch-pipeline.ts:107-149`가 수신 후 handler 예외를 기록한다. `CommandHandler.handle()`의 실패 지점은 `bench-handlers.js:28-30`이다. 따라서 메시지는 도착했고, handler 선택과 JSON decode 전에 버려진 것이 아니다.

## Codec / handler 계약

일반 Framework 컨트롤은 client와 target이 `application/x-protobuf` serializer를 등록한다(`client/framework-transport.js:31-40`, `zlink-framework-server/main.js:351-356`). L5_7에서는 양쪽 등록을 생략하고 typed JSON 경로를 쓴다. `BenchPayload`는 객체이므로 기본 envelope content type은 `application/json`이다(`channel-envelope.ts:494-509,542-545`). Handler 등록에는 별도 content type 기대값이 없다. target은 envelope header의 content type으로 decoder/serializer를 선택한다(`channel-dispatchers.ts:183-199`, `channel-envelope.ts:332-355`). 진단 이벤트는 decode 실패가 아니라 handler 예외를 표시했다.

Unhandled send 기본 동작은 `LogAndDrop`이며, missing handler는 `no_handler`와 `drop`으로 보고된다(`channel-dispatch-pipeline.ts:132-139,500-503`). Message-flow 계약은 dispatch error의 `no_handler`, `decode_error`, `handler_exception`과 action을 정의한다(`framework/doc/framework/common/spec/server/06-observability/03-message-flow-tracing.ko.md:129-147`). 이번 trace는 `handler_exception`이므로 unhandled-send 정책이나 스펙 동작은 원인이 아니다.

## 수정 후 6개 L5_7 smoke

각 cell은 3초 warmup + 3초 active의 warm run과 counted run을 보관했고, 별도 exclusive hold 하나에서 수행했다(약 1분 11초). 각 run에서 errors=0 및 submitted=completed=received를 검사했다. 아래 수치는 counted 결과다.

| Pattern | Payload | submitted = completed = received | Errors | Raw |
|---|---:|---:|---:|---|
| send-saturation | 1K | 46320 | 0 | `raw/final-smoke/counted/send-saturation-1024/run1/zlink-framework-node-send-saturation-1024/results.json` |
| send-saturation | 4K | 17128 | 0 | `raw/final-smoke/counted/send-saturation-4096/run1/zlink-framework-node-send-saturation-4096/results.json` |
| request-serial | 1K | 8330 | 0 | `raw/final-smoke/counted/request-serial-1024/run1/zlink-framework-node-request-serial-1024/results.json` |
| request-serial | 4K | 7025 | 0 | `raw/final-smoke/counted/request-serial-4096/run1/zlink-framework-node-request-serial-4096/results.json` |
| request-backpressure | 1K | 26722 | 0 | `raw/final-smoke/counted/request-backpressure-1024/run1/zlink-framework-node-request-backpressure-1024/results.json` |
| request-backpressure | 4K | 12401 | 0 | `raw/final-smoke/counted/request-backpressure-4096/run1/zlink-framework-node-request-backpressure-4096/results.json` |

`npm test`: 22 passed, 0 failed. Smoke script also checked each run's `SCHED_OTHER` policy and source/target CPU affinity.

## 규칙 수 / commit

수정 전에는 handler provider factory와 Framework scope activator가 각각 handler 인스턴스를 만들던 두 경로가 있었다. 수정 후에는 한 경로(Nest constructor token resolution)만 metrics를 주입한다.

Commit: `bench(node): fix ladder L5_7 handler metrics DI (#4355)` — 로컬 commit만 수행.
