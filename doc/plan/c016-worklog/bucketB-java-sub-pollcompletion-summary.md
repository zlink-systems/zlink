# Java SUB receive poller의 POLLCOMPLETION 등록 제거

## 결과

Java Framework의 SUB socket은 receive poller에 `POLLIN`만 등록한다. Completion queue를
소유하는 ROUTER·DEALER·STREAM 경로는 기존처럼
`POLLIN|POLLOUT|POLLCOMPLETION`을 등록한다.

## 변경

- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaSocketReceivePoller.java:24-61`
  - socket의 completion queue 소유 여부를 생성자에서 받아 등록 event를 선택한다.
  - completion queue를 소유하지 않으면 `POLLIN`만 등록한다.
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaSubscriberSocket.java:16-18`
  - SUB socket의 receive poller를 completion queue 비소유자로 생성한다.
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaSocketReceiveOwnerTest.java:27-35`
  - `subscriberReadinessDoesNotClaimAnUnsupportedCompletionQueue`를 추가했다.
  - 실제 SUB socket의 readiness 등록을 실행하고 `NOT_SUPPORTED`가 발생하지 않음을 검증한다.

## 계약과 언어 간 동작 일치

- `core/doc/spec/core/05-polling.ko.md:85-90`: `POLLCOMPLETION`은
  PAIR·DEALER·ROUTER·STREAM의 socket-local completion queue readiness다.
- `core/doc/spec/core/05-polling.ko.md:282-284`: 지원하지 않는 source event 등록은
  `ZLINK_CONFIG_NOT_SUPPORTED`/`ENOTSUP`이며, `POLLCOMPLETION` 등록 가능 socket도 위 네
  종류로 제한한다.
- .NET 선례: commit `4644af9d03` (`framework/dotnet: poll SUB sockets without claiming
  completion ownership`). .NET도 SUB receive readiness에서 completion ownership을
  요구하지 않고 `PollIn`만 요청한다.

## 검증

Focused test:

```text
cd framework/languages/java
TMPDIR=/dev/shm/zlink-tmp-java flock -w7200 /tmp/zlink-jvm-gate.lock \
  ./gradlew --no-daemon :zlink-framework-core:test \
  --tests '*ReceiveOwner*' --tests '*Subscriber*'

BUILD SUCCESSFUL in 17s
ZLinkJavaSocketReceiveOwnerTest: 4 tests, 0 failures, 0 errors, 0 skipped
```

ZoneWorld smoke:

```text
cd framework/languages/java/samples/java/ZoneWorld
TMPDIR=/dev/shm/zlink-tmp-java ZLINK_SAMPLE_KEEP_RUN_DIR=1 \
  ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/.artifacts/wsl/install/zlink-core/0.17.0/lib/libzlink.so \
  flock -w7200 /tmp/zlink-jvm-gate.lock ./run_sample.sh ZW-G1

scenario ZW-G1 passed
runDir=/dev/shm/zlink-tmp-java/tmp.j053CP6o6r
```

`fanout location tick failed` 횟수:

| process | 변경 전 | 변경 후 |
|---|---:|---:|
| zone-node-1 | 3,262 | 0 |
| zone-node-2 | 3,006 | 0 |
| zone-node-3 | 2,144 | 0 |

변경 후 세 로그에서 `NOT_SUPPORTED`, `NotSupported`, `ENOTSUP` 문자열도 모두 0건이다.

## BLOCKERS

없음.
