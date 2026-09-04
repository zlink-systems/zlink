# Bucket B — Java DeliveryDispatch offer deadline 수정 결과

## 결론

Java DeliveryDispatch의 첫 배차와 재배차는 이제 Tracking 상태 publish가 완료된 뒤에 offer row를
만들고, 바로 이어 courier actor send를 제출한다. 따라서 선행 `DeliveryStatusChangedReq` 지연은
courier의 900 ms 응답 시간에 포함되지 않는다.

이 변경은 공통 sample 계약 §7.2의 “Courier A가 `OfferDeliveryMsg`를 받은 뒤 deadline 안에
결정을 보내지 않으면 만료한다”는 조건에 맞춘 것이다
(`framework/doc/framework/common/sample/deliverydispatch/README.ko.md:292-296`). 공개 API나
assertion은 변경하지 않았다.

## 수정

- `framework/languages/java/samples/java/DeliveryDispatch/Server/Dispatch/src/main/java/systems/zlink/samples/deliverydispatch/server/dispatch/DispatchWorker.java:35-40`
  - 첫 배차의 `offers.offer(...)`를 status publish 완료 콜백 안으로 이동했다.
- 같은 파일 `:65-82`
  - 재배차도 `Reassigned` status publish 완료 뒤에 offer를 시작한다.
- 같은 파일 `:91-97`
  - `startOffer(...)`가 row와 deadline을 기록한 직후 기존 public actor client 경로로 one-way
    send를 제출한다. deadline 생성과 send 사이에 await 또는 별도 I/O가 없다.
- `framework/languages/java/samples/java/DeliveryDispatch/Server/Dispatch/build.gradle.kts:25-27,41-43`
  - 기존 sample Gradle 구성에 JUnit Jupiter test 의존성과 test task 설정을 추가했다.

`DeliveryOfferStore`의 책임과 API는 바꾸지 않았다. `offer(...)`가 호출 시각을 기준으로 deadline을
만드는 기존 동작을 그대로 사용하되, 호출 위치를 실제 courier send 직전으로 옮겼다.

## 타 언어 parity 확인

읽기 전용으로 확인한 C++, .NET, Node 구현도 offer 상태/deadline을 Dispatch가 소유하고 one-way
courier send와 같은 worker 흐름에서 관리한다. 다만 현재 세 구현 모두 Java 수정 전과 마찬가지로
deadline을 status publish보다 먼저 기록한다.

- C++: `framework/languages/cpp/samples/DeliveryDispatch/Server/Dispatch/main.cpp:205-207,243-246`
- .NET: `framework/languages/dotnet/samples/DeliveryDispatch/Server/Dispatch/DispatchWorker.cs:138-141,200-203`
- Node: `framework/languages/node/samples/DeliveryDispatch.Ts/Server/DispatchCenter/dispatch-worker.ts:63-83`

이번 작업의 허용 범위 밖이므로 세 언어는 수정하지 않았다. Java는 §7.2의 수신 후 응답 시간
조건을 직접 만족하도록 status publish 지연을 deadline에서 제외했다.

## 회귀 테스트

추가 파일:
`framework/languages/java/samples/java/DeliveryDispatch/Server/Dispatch/src/test/java/systems/zlink/samples/deliverydispatch/server/dispatch/DispatchWorkerDeadlineTest.java:24-133`

첫 배차와 재배차를 각각 검증한다. 각 경우 status reply를 1,000 ms 보류한다. 이는
`SampleTimings.CourierDecisionTimeout` 900 ms보다 길다. 보류 중에는 만료된 offer가 없고 courier
send도 시작되지 않았음을 확인한다(`:59-62`). reply 완료 뒤에는 send 시각을 기록하고, 생성된
attempt 번호와 send 시각 기준 남은 deadline이 900 ms 전체 구간인지 확인한다(`:64-76`).

실행 결과:

```text
TMPDIR=/dev/shm/zlink-tmp-java flock -w7200 /tmp/zlink-jvm-gate.lock \
  ../../gradlew --settings-file standalone.settings.gradle.kts \
  --no-daemon --no-parallel --max-workers=1 :Server:Dispatch:test

BUILD SUCCESSFUL
```

## sample 3회 실행

세 실행 모두 다음 환경과 명령을 사용했다.

```text
TMPDIR=/dev/shm/zlink-tmp-java
ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/.artifacts/wsl/install/zlink-core/0.17.0/lib/libzlink.so
flock -w7200 /tmp/zlink-jvm-gate.lock ./run_sample.sh
```

| 실행 | `deliverydispatch-reassignment=completed` | `deliverydispatch-server-evidence=completed` | `deliverydispatch=completed` | teardown | 전체 exit |
|---|---|---|---|---|---|
| 1 | 통과 | 통과 | 통과 | courier-session `TEARDOWN_FAILED` | 1 |
| 2 | 통과 | 통과 | 통과 | courier-session `TEARDOWN_FAILED` | 1 |
| 3 | 통과 | 통과 | 통과 | courier-session `TEARDOWN_FAILED` | 1 |

기능 marker 세 개는 3회 모두 완료됐다. 각 실행은 알려진 pre-existing courier-session 종료 문제로
`ZLINK_FRAMEWORK_TERMINATION outcome=FORCE_STOPPED reason=TEARDOWN_FAILED`를 기록했고, cleanup이
실패했으므로 cleanup 뒤에 출력되는 `deliverydispatch-placement=completed`에는 도달하지 않았다.

## BLOCKERS

- Java 기능 수정과 회귀 검증의 blocker는 없다.
- 전체 sample exit 0 판정은 기존 courier-session `TEARDOWN_FAILED`가 막고 있다. 기능 marker는
  이 종료 실패와 별도로 3회 모두 통과했다.
- C++/.NET/Node에도 status publish 전 deadline을 시작하는 같은 순서가 남아 있으나 이번 작업의
  수정 금지 범위이므로 별도 parity 후속 작업이 필요하다.
