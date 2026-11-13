# Java·Node L2 request-backpressure 원인과 production 수정안

작성 시점: 2026-11-13 20:16 UTC. 이 절까지 production 소스 수정 없음. 작업 branch `perf/4352-l2-bp-reply`, 시작 `df42a4df40`. 사용자의 “보고 후 동일 branch에서 구현” 지시에 따라 아래 B 분류의 내부 codec 수정만 진행한다. public API·spec 변경은 제안하지 않는다.

## 원인과 판정 범위

L2가 추가한 요청·응답 header의 **동기 encode·parse와 임시 객체 생성**이 source completion 처리와 target 수신 처리에 추가된다. reply를 잃거나 binding slot을 미반납하는 결함은 확인되지 않았다. 이 bench의 request-backpressure는 고정 100건 window가 아니라 binding admission에서만 제출을 중지하는 uncapped 경로다. 따라서 “window slot 재사용 지연”은 다음처럼 구분해야 한다.

| 언어 | production 원인 위치 | 완료 진행에 미치는 영향 |
|---|---|---|
| Java | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:1278-1354`, 특히 `:1324-1327`. native/read-only ByteBuffer의 필드마다 `char[]`를 만들고 byte별로 다시 복사한 뒤 compact String으로 다시 변환한다. request·reply 양쪽에서 반복된다. reply encode는 같은 파일 `:568-578`의 size/write 두 단계다. | source `ZLinkChannelCallRuntime.java:260-272`의 reply decode가 caller 결과 전달 전에 실행된다. binding `CompletionOwner.java:1199-1229`는 pending entry 제거와 retained 해제를 먼저 하고 future를 완료한다. drain lock도 `:460-471`에서 callback 전에 해제한다. header parse는 binding slot을 보유하지 않지만 completion callback CPU를 사용해 다음 완료 처리를 늦춘다. |
| Node | `framework/languages/node/packages/framework/src/runtime/channels/channel-envelope.ts:1042-1096`. canonical 문자열을 JS byte loop로 순회하며 escape를 수동 검증한 다음, escape가 있으면 JSON.parse가 같은 유효성을 다시 검사한다. canonical header `:926-1009`, reply encode `:217-238,560-725`가 event loop에서 동기 실행된다. | `channel-outbound-operations.ts:243-248`의 reply parse는 binding Promise 이후다. binding `completion_owner.ts`의 capture → settleIfJoined → removeIfSettled에서 Promise를 완료하고 pending map을 제거한 뒤 microtask가 parse한다(설치된 JS `completion_owner.js:493-506,582-589`). 추가 await/microtask hop은 없다. decode·encode CPU가 다음 poll/submit을 늦춘다. |

원인과 수정을 구분한다. 모든 envelope 비용을 없앨 수 있다는 판정은 아니다. 이번 수정은 위에서 확인한 **필드별 중간 복사와 JS byte/escape 재검사**를 제거한다. 남는 JSON 객체·String·wire bytes 생성 비용은 현 계약에 필요하다. 21.9%/28.9% 전체를 제거했다고 보고하지 않는다.

## scratch 재현과 profile 근거

- Java scratch `/root/worktree/zlink-v21-s033/l2bp-scratch/java` = `e8721a0776`, Node scratch `.../node` = `2bef18aa94`. production envelope 파일은 시작 branch와 동일하다. 측정 worktree의 코드·산출물은 사용하지 않았다.
- 새 빌드 로그·source보다 최신인 artifact 검사·HEAD/dirty/base/SHA256: `raw/java-build-initial.log`, `raw/java-build-fingerprint.json`, `raw/node-build-initial.log`, `raw/node-build-fingerprint.json`. call-by-call 대조는 `call-shape.md`.
- 모든 process와 thread는 `6-7,14-15`. guard가 설정한 SCHED_IDLE을 변경하지 않았다. `start.json`/`active.json`에 thread별 affinity·policy·wchan을 보존했다. lock을 획득하지 않았다. CPU·주변 부하·profiler 영향 때문에 throughput은 진단 값이며 기존 정식 측정과 같은 판정에 쓰지 않는다.
- Node `--cpu-prof` + `monitorEventLoopDelay`(1ms), Java async-profiler 4.6 `ctimer` CPU 1ms + wall 10ms + alloc 256KiB를 같은 run에서 수집했다. Java 유효 9초 warmup run은 각각 18.17초/18.07초, Node는 약 8초였다. 자식에는 19초 timeout을 적용했다.

| 언어·run | 관찰 | raw 근거 |
|---|---|---|
| Node L1/L2, warmup 3s·active 4s | L1 19,444.5 → L2 13,729.0 msg/s로 손실 방향과 약 29% 차이를 재현. 두 run 모두 submitted=completed=received, errors=abandoned=0. | `raw/node/baseline-L1-b/results.json`, `raw/node/baseline-L2/results.json`, 각 `active-target-stats.json` |
| Java L1/L2, warmup 9s·active 4s | L1 45,426.25 → L2 42,338.5 msg/s로 손실 방향은 재현. 제한 CPU의 손실률은 원래 21.9%를 재현하지 않는다. 각각 193,594/178,657건 count 일치, 오류·abandoned 0. | `raw/java/warm-L1-b/results.json`, `raw/java/warm-L2/results.json`, 각 `active-target-stats.json` |
| Java 최초 3s warmup | compiler와 native yield 경합 때문에 throughput 방향이 뒤집혔다. 원인 판정은 반복되는 codec/alloc stack으로 제한하고 warmup을 늘린 profile을 추가했다. | `raw/java/baseline-L1-b`, `raw/java/baseline-L2` |

각 CPU 비중은 active 구간의 inclusive sample이며 서로 포함되는 행은 합산하지 않는다.

| 언어·L2 경로 | source CPU | target CPU | 근거 |
|---|---:|---:|---|
| Java envelope 전체 | 7.94% | 21.36% | `raw/java/warm-L2/jfr-summary.json` |
| Java header decode | 5.19% | 15.57% | 같은 파일, `tryDecodeHeader`/`decodeHeader` |
| Java header encode | 1.85% | 5.15% | 같은 파일, `encodeHeaderBytesInternal` |
| Node envelope 전체 | 15.51% | 13.09% | `raw/node/baseline-L2/cpu-summary.json` |
| Node 요청 encode / 응답 decode | 11.58% / 3.70% | — | 같은 파일 |
| Node 요청 decode / 응답 encode | — | 5.23% / 7.29% | 같은 파일. encode에는 기존 protobuf serialize 호출도 포함되므로 순수 header 비용의 상한이다. |

Java L2 active allocation sample에서 source `char[]` 39,059,456B, target 37,748,736B가 `CanonicalReader.string:1324`에 귀속된다. L1에는 이 stack이 없다. 원본 `source.jfr`/`target.jfr`, `*-cpu.collapsed`, `*-alloc.collapsed`, `*-wall.collapsed`를 각 run 폴더에 보존했다. wall profile은 admission/futex·epoll·native yield 대기가 지배하고, envelope 아래 별도 wait가 나타나지 않는다. Node GC self는 L1 source/target 3.06%/5.89%, L2 3.69%/4.08%이고 runMicrotasks도 3.35%→3.65%다. GC 급증이나 새 hop보다 동기 codec 비용의 추가가 설명력이 높다. event-loop 지연 표본은 각 `event-loop-<pid>.json`에 있다.

응답만의 문제라고 단정할 근거도 없다. Node source에서는 요청 encode 비중이 응답 decode보다 크고, Java target에서는 요청 decode 비중이 응답 encode보다 크다. packet 크기 임계치·correlation map의 in-flight 비례 탐색·별도 completion 대기 상태를 가정해 수정하지 않는다.

## 결정 소유와 교차언어 대조

| 결정 | spec 소유 조항 | C++ | .NET | Java | Node | 수정 후 소유자 |
|---|---|---|---|---|---|---|
| header decode와 malformed/duplicate/UTF-8 거부 | `02-channel-transport/03-client-server-channel.ko.md` §5.3 application record 계약, `01-execution/08-messaging-hot-path.ko.md` §4.1 I3 | `framework/src/runtime/messaging/envelope_codec.cpp:687,1258-1324` | `Runtime/Messaging/ZLinkEnvelopeCodec.cs:377,414,933` | `ZLinkChannelEnvelope.java:634,846,1213` | `channel-envelope.ts:889,926,1634` | 각 언어의 기존 envelope codec 그대로 |
| reply header 구성과 encode | 같은 §5.3, hot-path §4.3 W4 | `channel_reply_writer.cpp:12-44`, `envelope_codec.cpp:527` | `ZLinkEnvelopeCodec.cs:188` | `ZLinkChannelDispatchReporter.java:378-397`, `ZLinkChannelEnvelope.java:181,568` | `channel-dispatchers.ts:409-412`, `channel-envelope.ts:217` | 기존 codec·reply writer 그대로 |
| admission/physical completion·pending 제거 | `01-execution/01-submit-and-completion.ko.md` §10-11, hot-path §3 E5 | Core/binding; codec가 결정하지 않음 | Core/binding; codec가 결정하지 않음 | binding CompletionOwner, Framework `ZLinkChannelCallRuntime.java:260` | binding CompletionOwner, Framework `channel-outbound-operations.ts:243` | Core/binding 그대로; framework에 새 상태·poller·재시도 없음 |

위 C++ 경로는 `framework/languages/cpp/framework/src/`, .NET은 `framework/languages/dotnet/src/Zlink.Framework/`, Java는 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/` 기준이다. C++는 string_view/retained decode, .NET은 Utf8JsonReader+span을 사용한다. Java/Kotlin은 같은 JVM codec을 사용한다. 네 언어의 header·correlation·metadata·오류 의미는 동일하며, 이번 차이는 내부 문자열 처리 비용이다. 새로운 동작을 정하지 않으므로 D(spec gap)가 아니다.

## production 수정안

| 언어 | 채택안 | 비교한 대안과 선택 이유 | 규칙·상태 수 |
|---|---|---|---|
| Java | `CanonicalReader.string`에서 native/read-only 필드를 reader-local byte scratch로 bulk copy하고 기존 Latin-1 String 생성으로 모은다. 같은 header의 다음 필드는 scratch를 재사용한다. String은 독립 소유이며 buffer position·limit, invalid-input 복원, fallback을 유지한다. | header 전체를 heap으로 복사하면 literal scan은 싸지지만 항상 header 크기의 추가 allocation이 생긴다. shared cache/ThreadLocal은 수명·동시성 규칙을 추가한다. 작은 reader-local scratch는 기존 필드별 char[]를 대체하고 전역 상태를 추가하지 않는다. | ASCII String 생성 판정 위치 2(배열/char[])→1(byte[]). header validation 소유 1→1. 필드별 char[] 제거, reader-local byte[] 1개 사용; 새 lock/queue/flag 없음. |
| Node | canonical 문자열 경계 탐색은 native `Buffer.indexOf`, raw control/ASCII 판정은 native RegExp로 처리한다. escape의 의미 검사는 JSON.parse 하나가 맡고 canonical 인식 실패는 기존 strict reader로 보낸다. UTF-8·duplicate·flow-off·correlation 계약과 객체 shape는 유지한다. | JSON.parse로 header 전체를 읽으면 duplicate 정책과 기존 무복사 계약이 달라진다. 필드/헤더 cache는 dynamic correlation에 miss와 보유 상태를 만든다. 기존 reader 안에서 byte loop를 대체하면 별도 정책이 없다. | escape 유효성 판정 위치 2(수동 switch+JSON.parse)→1(JSON.parse). header validation 소유 1→1. 새 cache/queue/flag/lock 없음. |

분류는 두 언어 모두 **B: 기존 내부 codec의 중복 작업/임시 allocation 비용 수정**이다. API·wire·오류 의미와 completion/admission 소유를 바꾸지 않는다. Java는 heap/direct/read-only 비교 allocation 회귀 검사와 golden/canonical/malformed/cache 테스트를 수행한다. Node는 escaped quote/backslash/control/UTF-8/truncation/duplicate/flow-off 및 envelope 비용·reply 관련 contract test를 수행한다. allocation/CPU 관찰은 실제 빌드 산출물로 전후를 비교하며, 새로 발견된 실패는 재실행 통과로 무시하지 않는다. 두 언어 format check와 관련 framework gate를 실행하고 언어별로 로컬 commit한다.
