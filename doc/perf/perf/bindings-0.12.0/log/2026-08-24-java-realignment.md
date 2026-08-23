# Java binding 0.13.0 계약 재정렬

## 결과 요약

Java binding의 송신·요청 완료 경로를 Core 0.13.0 계약에 맞췄다.

- `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/Native.java`와
  `NativeLayouts.java`에서 `send_ready` downcall과 event mirror를 제거하고
  `zlink_send_async`, `zlink_send_complete_handler`,
  `zlink_send_async_cancel`, send-complete event/options mirror를 추가했다.
- `SendCompletionRegistry`는 socket당 Core completion handler 하나와
  binding-owned opaque token 기반 strong pending table을 소유한다. Core가
  `zlink_send_async` 안에서 inline completion을 실행해도 token table이 먼저
  등록되며, callback은 stage를 정확히 한 번 완료하는 일만 수행한다.
- PAIR와 DEALER/ROUTER routed send는 `CompletionStage<Void>`를 반환한다.
  timeout은 Core per-operation option으로 전달하고, `TIMED_OUT`/`TERMINAL`은
  `terminal_errno`를 담은 `ZlinkSubmitException`으로 완료한다.
- PUB/XPUB publish는 `void` synchronous submit으로 바꿨다. PUB의 lossy
  동작은 Core에 맡기고, NODROP에서 즉시 발생하는 backpressure와 연결 오류를
  `ZlinkSubmitException`으로 전달한다. PUB/XPUB에는 `zlink_send_async`를
  호출하지 않는다.
- DEALER/ROUTER request는 binding timeout scheduler나 completion executor 없이
  Core reply callback이 반환 stage를 직접 완료한다.
- `RoutedAdmission`, `PublisherAdmission`, `StreamAdmission`과 그 wiring 및
  관련 내부 테스트를 삭제했다. Kotlin wrapper는 수정하지 않았고,
  `submit().await()` compile 경로를 유지했다.

Core는 재빌드하지 않았으며 기존 `core/build/lib/libzlink.so.0.13.0`을 사용했다.
Java tree는 send API를 JNI C 함수로 노출하는 구조가 아니라 FFM downcall을
사용하고, 기존 C bridge는 message-data helper만 제공하므로 send-completion용
별도 JNI source 변경은 필요하지 않았다.

## 소유권과 callback 검증

- `SendCompletionRegistry`는 Core 호출 전에 source `Message`를 native array로
  move하고, immediate rejection이면 source ownership을 복원한다.
- request의 native part array는 기존 direct request-part 경로와 같이 성공·거절
  후 남은 native slot을 닫는다.
- Core callback ABI `(subject, event, userdata)`를 FFM upcall signature에
  맞췄다. inproc PAIR probe에서 실제 completion callback으로 stage가 완료되고
  payload가 수신되는 것을 확인했다.
- ROUTER multipart abort와 DEALER generic-target-fail의 Core 결함 때문에
  `RoutedMultipartAdmissionContractTest`의 async record assertion은 한 part로
  유지하고 결함 설명 주석을 남겼다. `core/`는 수정하지 않았다.

## Zero-thread 점검

송신·routed send·request·publish 경로에는 binding-owned admission thread,
queue, scheduler, retry loop가 없다. 남은 executor는 다음 기존 callback delivery
기능에만 사용된다.

| 위치 | 목적 | 이번 작업에서의 판단 |
|---|---|---|
| `NativeCallbackSupport` / `SocketCallbackSupport` | receive와 STREAM packet callback을 JVM executor에서 전달 | 유지; send/request 완료와 무관 |
| `NativeMonitorSocket` | monitor event callback 전달 | 유지; monitor lifecycle 기능 |
| `OutboundRecordAttemptGate` | 짧은 outbound record 시도 직렬화 | thread/queue가 아닌 synchronized gate; 유지 |
| `RequestSubmitLoop` | 기존 raw ROUTER reply의 EINTR 재시도 | admission 재시도가 아닌 동기 syscall helper; 별도 flag |

Core가 소유하는 async mailbox/deadline thread가 JVM callback을 호출하며,
Java callback은 completion 전달만 한다.

## Grep proof

```text
$ rg -n -i "send[_-]?ready|routed_send_ready|SendReady|sendReady" \
    bindings/java --glob '!build/**'
(no matches)

$ rg -n "RoutedAdmission|PublisherAdmission|StreamAdmission|ScheduledExecutorService" \
    bindings/java/src/main/java
(no admission/scheduler matches)
```

`ExecutorService` 검색 결과는 위 표의 receive/STREAM callback executor와
monitor callback executor뿐이며 `SendCompletionRegistry`, `CoreRequestSupport`,
`RoutedRequestSupport`에는 executor 또는 scheduler가 없다.

## 테스트와 빌드

| 검증 | 결과 |
|---|---|
| Java 22 main classpath compile | 성공; `Unsafe` 기존 경고 7개 |
| Java 22 module compile | 성공; `Unsafe` 기존 경고 8개 |
| Java 22 Java test source compile | 성공; 31 test classes 생성 |
| 실제 inproc PAIR send-completion probe | 성공: `send_completion_probe: complete` |
| 수동 JUnit 대상 테스트 | 성공: routed one-part request와 PUB/XPUB synchronous surface 각 1 test |
| Kotlin samples/contract manual compile | 성공; Kotlin wrapper 수정 없음 |
| `./gradlew build --no-daemon` | 실행 불가: read-only `/home/hep7hep7/.gradle/...gradle-8.10.2-bin.zip.lck` |
| cached Gradle `build --offline --no-daemon` | 실행 불가: sandbox daemon server socket `Operation not permitted` |

Gradle test task는 daemon/wrapper 단계에서 시작되지 않았으므로 binding JUnit
suite의 Gradle 실행 결과는 없다. Java 22 직접 compile과 inproc probe는 이
환경 제약과 분리된 확인이다.

요청한 new-vs-pre-existing baseline을 위해 `bindings/java`와 Java spec만
path-limited `git stash --include-untracked`하려 했으나 repository의 `.git`이
read-only라 stash가 return code 1로 거절됐다. stash list와 worktree는 변경되지
않았으며, 다른 agent 경로도 건드리지 않았다. 따라서 이번 환경에서 전체
baseline suite를 stash 전후로 실행해 실패 수를 비교할 수는 없었다. 확인된
실패는 모두 Gradle/sandbox 실행 차단이며 Java 직접 compile과 inproc probe에서
새 compile/runtime failure는 확인되지 않았다.

## 필수 perf smoke matrix

공식 명령은 최종 상태에서 각각 실행했지만 Gradle wrapper lock 단계에서
중단됐다. 결과 파일이 없고 `status: complete`도 없으므로 아래 항목은 모두
**미실행(blocked)**으로 기록한다.

### Single

명령:

```text
./perf/single/run_benchmarks.sh --pattern ALL --transports tcp \
  --msg-sizes 64 --duration 1 --runs 1
```

`ALL` 확장 pattern은 `PAIR`, `PUBSUB`, `DEALER_DEALER`, `DEALER_ROUTER`,
`DEALER_ROUTER_REQREP`, `ROUTER_ROUTER`, `ROUTER_ROUTER_REQREP`, `SPOT`이다.

| Pattern | Transport | Payload | Duration / runs | 상태 | 사유 |
|---|---|---:|---:|---|---|
| PAIR | tcp | 64B | 1s / 1 | blocked | Gradle wrapper lock |
| PUBSUB | tcp | 64B | 1s / 1 | blocked | Gradle wrapper lock |
| DEALER_DEALER | tcp | 64B | 1s / 1 | blocked | Gradle wrapper lock |
| DEALER_ROUTER | tcp | 64B | 1s / 1 | blocked | Gradle wrapper lock |
| DEALER_ROUTER_REQREP | tcp | 64B | 1s / 1 | blocked | Gradle wrapper lock |
| ROUTER_ROUTER | tcp | 64B | 1s / 1 | blocked | Gradle wrapper lock |
| ROUTER_ROUTER_REQREP | tcp | 64B | 1s / 1 | blocked | Gradle wrapper lock |
| SPOT | tcp | 64B | 1s / 1 | blocked | Gradle wrapper lock |

### Multi

명령:

```text
./perf/multi/run_benchmarks.sh --pattern ALL --transports tcp --duration 1
```

기본 `ALL` pattern은 `MULTI_DEALER_DEALER`,
`MULTI_DEALER_ROUTER_SENDSEND`, `MULTI_DEALER_ROUTER_REQREP`,
`MULTI_ROUTER_ROUTER_SENDSEND`, `MULTI_ROUTER_ROUTER_REQREP`, `MULTI_PUBSUB`,
`MULTI_STREAM`이다.

| Pattern | Transport | Duration | 상태 | 사유 |
|---|---|---:|---|---|
| MULTI_DEALER_DEALER | tcp | 1s | blocked | Gradle wrapper lock |
| MULTI_DEALER_ROUTER_SENDSEND | tcp | 1s | blocked | Gradle wrapper lock |
| MULTI_DEALER_ROUTER_REQREP | tcp | 1s | blocked | Gradle wrapper lock |
| MULTI_ROUTER_ROUTER_SENDSEND | tcp | 1s | blocked | Gradle wrapper lock |
| MULTI_ROUTER_ROUTER_REQREP | tcp | 1s | blocked | Gradle wrapper lock |
| MULTI_PUBSUB | tcp | 1s | blocked | Gradle wrapper lock |
| MULTI_STREAM | tcp | 1s | blocked | Gradle wrapper lock |

별도 direct perf probe로 `PAIR / tcp / 64B / 1s`도 시도했으나 sandbox의 TCP
socket 생성이 `operation_not_permitted`로 거절됐다. 이 probe에는 benchmark
report가 생성되지 않았다.

## Flagged items

1. Gradle daemon 및 TCP socket 생성이 현재 sandbox 정책으로 차단되어 공식
   JUnit/perf report를 `status: complete`로 만들 수 없다.
2. Core의 routed multipart 결함 두 건은 Java test를 1-part로 제한해 기록했으며
   Core 수정 후 multipart assertion을 복원해야 한다.
3. `.git` read-only 때문에 stash 기반 baseline 비교를 완료하지 못했다. 다른
   agent의 `core/`, Rust, .NET, Node 변경은 건드리지 않았다.
