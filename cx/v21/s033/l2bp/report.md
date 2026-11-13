# Java·Node L2 request-backpressure 원인과 production 수정

최초 수정안은 production 변경 전에 작성했다(`raw/report-before-implementation.md`). 원래 사다리 커밋으로 scratch build한 뒤 조사했으며, local branch는 `perf/4352-l2-bp-reply`다. push·PR·GitHub·sub-agent·internal-docs·measure.lock 사용 없음. raw는 삭제하지 않았다.

## 원인

L2가 더하는 요청·응답 JSON header encode/parse가 source completion과 target receive/reply의 동기 CPU 작업을 늘린다. reply 유실·pending entry 누수·binding slot 미반납은 발견하지 못했다. 이 bench의 request-backpressure는 고정 100건 window가 아니라 binding admission에서만 제출을 멈추는 uncapped 경로다.

| 언어 | 제거 가능한 production 비용(변경 전 df42a4df40 기준) | 완료 진행과의 관계 |
|---|---|---|
| Java | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:1324-1327`: native/read-only 필드마다 char[] 생성 → byte별 재복사 → compact String 재변환 | `ZLinkChannelCallRuntime.java:260-272`의 reply decode가 결과 전달 전에 동기 실행된다. binding은 `bindings/java/.../CompletionOwner.java:1199-1229`에서 pending 제거·retained 해제를 먼저 하고 future를 완료하며, drain lock도 `:460-471`에서 callback 전에 푼다. slot 보유가 아니라 다음 완료 처리를 늦추는 CPU/allocation 비용이다. |
| Node | `framework/languages/node/packages/framework/src/runtime/channels/channel-envelope.ts:1674-1679,1721-1727`: 생성한 hex correlation을 재검증하고 ASCII를 JS charCodeAt loop로 검사한다. `:679-684`는 kind가 이미 정한 deadline 유무를 별도 Map key로 다시 유지한다. | binding `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:207,635,767`은 Promise 완료와 pending 제거를 처리한 뒤 framework reply microtask가 실행된다. `channel-outbound-operations.ts:243-248`의 header parse와 encode CPU가 다음 poll/submit을 늦춘다. 추가 await/hop은 없다. |

Node 전체 header decode는 변경하지 않는다. 첫 Buffer.indexOf/RegExp string-reader 후보는 단독 CPU profile에서 slice·RegExp·GC 비용이 늘어 기각했다(`raw/node/codec-after`, `raw/node-envelope-native-rejected.ts`). 최종 수정안을 먼저 갱신한 기록은 `raw/report-before-node-revision.md`다. 위의 중복 작업을 줄이는 수정이며 **원래 21.9%/28.9% 손실 전체를 제거했다고 판정하지 않는다.** 남는 JSON String·객체·wire buffer 비용은 현 계약에 필요하다.

## 재현과 profile

| 항목 | 조건·근거 |
|---|---|
| scratch | Java `e8721a0776`: `/root/worktree/zlink-v21-s033/l2bp-scratch/java`; Node `2bef18aa94`: `.../node`. 다른 측정 worktree/산출물은 사용하지 않음 |
| 새 빌드·fresh·해시 | `raw/java-build-initial.log`, `raw/java-build-fingerprint.json`, `raw/node-build-initial.log`, `raw/node-build-fingerprint.json`; 경로 대조 `call-shape.md` |
| 실행 격리 | 모든 process/thread CPU `6-7,14-15`; guard가 정한 SCHED_IDLE을 변경하지 않음. 각 run의 `start.json`, `active.json`에 affinity/policy/wchan 보존. 공식 throughput/7% 판정에 사용하지 않음 |
| Java profiler | async-profiler 4.6: ctimer CPU 1ms + wall 10ms + alloc 256KiB. source/target JFR와 cpu/alloc/wall collapsed stack 보존 |
| Node profiler | --cpu-prof + event-loop delay(1ms), CPU/memory/ELU 표본. `*.cpuprofile`, `event-loop-<pid>.json`, `cpu-summary.json` 보존 |
| run 길이 | Java 9s warmup + 4s active: 전 18.17/18.07s, 수정 후 16.79s. Node 약 8s. profiled child마다 19s timeout, 원본 lifecycle.json 보존 |

| 진단 실행 | 결과 | raw |
|---|---|---|
| Node L1 → L2, 1K | 19,444.5 → 13,729.0 msg/s, 손실 방향과 약 29% 차이 재현. count 일치·errors=abandoned=0 | `raw/node/baseline-L1-b`, `raw/node/baseline-L2` |
| Java L1 → L2, 1K | 45,426.25 → 42,338.5 msg/s, 손실 방향 재현. 제한 CPU에서 원래 21.9% 손실률은 재현하지 않음. count 193,594/178,657 일치·errors=abandoned=0 | `raw/java/warm-L1-b`, `raw/java/warm-L2` |
| Java 수정 후 L2 | 228,958건 submitted=completed=received, errors=abandoned=0. throughput은 잡음 때문에 개선률 판정에 사용하지 않음 | `raw/java/fixed-L2` |

Java 최초 3s warmup에서는 compiler/native yield 경합으로 throughput 방향이 뒤집혔으므로 9s warmup profile을 추가했다. 이전 시도와 실패한 준비 실행도 모두 보존했다.

아래는 active 구간의 inclusive CPU sample 비중이다. 포함 관계가 있는 행을 합산하지 않는다.

| L2 경로 | source | target | 근거 |
|---|---:|---:|---|
| Java 전체 envelope | 7.94% | 21.36% | `raw/java/warm-L2/jfr-summary.json` |
| Java header decode / encode | 5.19% / 1.85% | 15.57% / 5.15% | 같은 파일 |
| Node 전체 envelope | 15.51% | 13.09% | `raw/node/baseline-L2/cpu-summary.json` |
| Node 요청 encode / 응답 decode | 11.58% / 3.70% | — | 같은 파일 |
| Node 요청 decode / 응답 encode | — | 5.23% / 7.29% | 같은 파일. encode에는 기존 protobuf serialize도 포함되어 순수 header 비용의 상한 |

Java wall은 futex/admission·epoll·native yield가 지배하고 envelope 아래 별도 wait는 없다. Node GC self는 L1 source/target 3.06%/5.89%, L2 3.69%/4.08%; runMicrotasks는 3.35%→3.65%다. 새 hop/GC 급증보다 동기 codec 작업 추가가 설명력이 높다. correlation lookup의 in-flight 비례 탐색·reply 크기 임계치 결함은 확인하지 못했다.

backpressure에서만 차이가 큰 이유는 호출 빈도와 header의 반복 가능성이다. Java의 repeatable command header는 기존 단순-header cache를 사용할 수 있고, Node의 correlation/flow가 없는 command는 prepared bytes를 그대로 반환한다(`ZLinkChannelEnvelope.java:523`, `channel-envelope.ts:711`). request/reply는 correlation이 매번 달라 새 header bytes와 decoded String이 필요하다. serial에서는 왕복 대기가 이 CPU 비용을 가리고, uncapped backpressure에서는 이미 바쁜 source/target 처리율과 queue 체류 시간에 추가 비용이 반영된다. 이는 in-flight 개수에 비례하는 correlation 탐색이나 미반납 slot 상태를 추가할 이유가 아니다.

## 결정 소유와 수정안

| 결정 | spec | C++ | .NET | Java | Node |
|---|---|---|---|---|---|
| header parse·검증 | ClientServer Channel §5.3, hot path §4.1 I3 | `framework/src/runtime/messaging/envelope_codec.cpp:687,1258-1324` | `Runtime/Messaging/ZLinkEnvelopeCodec.cs:377,414,933` | `ZLinkChannelEnvelope.java:634,846,1213` | `channel-envelope.ts:889,926,1634` |
| reply encode | 같은 §5.3, hot path §4.3 W4 | `channel_reply_writer.cpp:12-44`, `envelope_codec.cpp:527` | `ZLinkEnvelopeCodec.cs:188` | `ZLinkChannelDispatchReporter.java:378-397` | `channel-dispatchers.ts:409-412` |
| physical completion/admission | Submit과 완료 §10-11, hot path §3 E5 | binding `completion_owner.cpp:521,628` | binding `CompletionOwner.cs:1220` | binding `CompletionOwner.java:460,1199` | binding `completion_owner.ts:207,635,767` |

spec root: `framework/doc/framework/common/spec/server/`; 해당 문서 `02-channel-transport/03-client-server-channel.ko.md`, `01-execution/08-messaging-hot-path.ko.md`, `01-execution/01-submit-and-completion.ko.md`. 위 framework C++ 경로는 `framework/languages/cpp/`, .NET은 `framework/languages/dotnet/src/Zlink.Framework/`, Java는 core `.../runtime/` 기준이다. binding C++는 `bindings/cpp/src/Runtime/Messaging/`, .NET은 `bindings/dotnet/src/Zlink/Runtime/Messaging/` 기준이다.

C++는 retained string_view, .NET은 Utf8JsonReader/span으로 같은 계약을 처리한다. Java/Kotlin은 같은 JVM codec을 사용한다. 이번 변경은 내부 representation/중복 검사만 줄이며 네 언어의 header·metadata·correlation·오류 의미를 바꾸지 않는다. public API·spec·binding 수정은 없다. 분류는 두 언어 모두 **B(기존 내부 비용 수정)**다.

| 언어 | 최종 production 수정안 | 대안·규칙 수 |
|---|---|---|
| Java | reader-local byte[] scratch 하나를 bulk copy에 재사용하고 heap/native String 생성을 기존 Latin-1 경로로 모음. String은 독립 소유, position/limit/fallback 유지 | 전체 header heap copy/ThreadLocal/cache는 추가 allocation·수명·동시성 규칙 때문에 제외. String 생성 판정 위치 2→1, validation owner 1→1. 필드별 char[] 제거, reader-local byte[] 추가. lock/queue/flag 없음 |
| Node | 생성 ID 재검증 제거, 외부/수신 ID의 ASCII 검증을 native RegExp로 처리. prepared-header boolean deadline Map 제거 | ID 형식 보장 2(생성+재검증)→1(생성), deadline 인덱스 2(kind+boolean)→1(kind). 외부 validation owner 1→1. Map 계층 1개와 header당 lookup 1회 제거; 새 상태 없음 |

## 검증과 현재 상태

| 항목 | 결과·근거 |
|---|---|
| Java 회귀 전/후 | old scratch 실패: native decode의 heap 대비 추가 allocation 256B/header. 수정 후 요청·응답 모두 ≤96B/header 통과. `raw/java-regression-before.xml`, `raw/java-regression-after.xml` |
| Java 관련 framework 검사 | 1,348 tests, 실패·skip 0. canonical·golden·cache·registered encoding 포함. `raw/java-focused-after.log` |
| Java allocation profile 전/후 | CanonicalReader.string char[] 표본 source 39,059,456B / target 37,748,736B → 두 곳 모두 0. `raw/java/warm-L2/*-alloc.collapsed`, `raw/java/fixed-L2/*-alloc.collapsed` |
| Node 원래 codec 계약 검사 | 확장된 새 6개 test가 원래 artifact에서 통과. 모든 ASCII/64-byte 경계, invalid UTF-8/control/escape/truncation, generated ID·deadline·flow 왕복 검증. `raw/node-final-contracts-before.log` |
| Node 최종 검사 | **강제 재빌드 대기.** 이전 185개 통과는 parser 후보 artifact였으므로 최종 변경의 통과로 사용하지 않음 |
| format | 두 언어 전체 check 통과. `raw/java-format-check.log`, `raw/node-format-check.log`. Node touched-file lint 통과. cache 설명 comment 갱신 후 targeted check 예정 |
| local commit | Java `ddc13b0615`; Node 최종 artifact 검사·test 후 별도 commit 예정 |

Java 수정 후 profile은 테스트한 main branch core JAR을 classpath 첫 항목으로, 처음 fresh build한 scratch bench/binding을 그대로 사용했다. source/JAR freshness·private field·해시 확인은 `raw/java-fixed-core-fingerprint.json`. Node는 최종 소스·산출물 signature와 mtime을 확인한 뒤 검사·profile을 진행한다. 측정 보호로 멈춘 빌드와 raw를 보존하며 보호 설정을 바꾸지 않는다.

리팩토링 점검: 비용/중복 소유 발견 3건(Java 임시 변환, Node ID 재검증, Node deadline 이중 인덱스)을 수정했다. 넘긴 항목 0. touched 경로의 execution-gate 중복 동기화 발견 0. 공식 동시 조건의 성능 재측정은 하지 않았으며, 잔여 L2 손실률과 7% 충족 여부는 판정하지 않는다.
