# Java Bingo 종료·runner 판정·ZoneWorld G4 수정 결과

2026-09-05. **전체 작업은 미완료다.** Bingo의 실제 실패를 Java binding 공개 API로
재현했다. Binding 수정·package 재빌드 금지에 따라 해당 결함을 Framework에서 보상하지
않았다. Runner 종료 판정과 ZoneWorld 오류 보존, durable sender의 logical owner 종료
판정은 Java 범위에서 수정했다. 최종 Bingo 2회·ZoneWorld 2회·aggregate는 모두 exit 1이다.
G4는 focused와 두 전체 ZoneWorld 실행에서 실제 `Unavailable`로 통과했다.

증거 디렉터리: `/tmp/java-bingo-fix-evidence/`. `main`에서 작업했으며 commit하지 않았다.
기존 변경과 동시에 진행되는 다른 작업은 유지했다. 지정된
`framework/languages/java/AGENTS.md`는 없어서 루트·Framework 규칙을 적용했다.
Core·binding·다른 언어·공유 sample·보호 문서는 수정하지 않았다.

## 1. Bingo teardown — binding owner의 미수정 결함

첫 재현에서 `session-b`, `play-b`가 `ForceStopped/TeardownFailed`였고, 상세 진단에서는
`play-a`도 같은 원인으로 실패했다. 기존 flow 로그는 application transition을 기록하지만
종료 예외 stack을 기록하지 않았다. 종료 실패 경계에 임시 stack trace만 추가해 원인을
확인한 뒤 **임시 코드는 모두 제거했다**.

| 원인 위치 | 확인한 동작 |
|---|---|
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaContext.java:44` | Framework adapter가 `shutdown()` 뒤 `close()`를 호출한다. |
| `bindings/java/src/main/java/systems/zlink/runtime/core/NativeContext.java:280` | 이미 shutdown된 context의 completion pump를 닫는다. |
| `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionPump.java:222,327` | `close()`가 control wake socket에 send하고 `SubmitResult.TERMINATED`를 던진다. |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:1968,2233` | Context close 예외를 teardown 실패로 전달한다. 계약에 맞는 실패 표기다. |

보존 stack: `bingo-diagnosis-roles/session-b.log:491`, `play-b.log:386`, `play-a.log:555`.
Spot `onClosing` 또는 authority Delete/capacity 경합이 이번 실패 원인이라는 증거는 없다.
따라서 .NET B1–B3를 추정으로 이식하지 않았다.

Framework를 사용하지 않는 [공개 API 재현](/tmp/java-bingo-fix-evidence/ContextShutdownRepro.java)은
ROUTER/DEALER request를 실제 수신해 binding completion pump를 만든 뒤 socket을 닫는다.
이어 `context.shutdown(); context.close();`를 호출하면 `TERMINATED`, errno `156384765`로
실패한다. `close()`만 호출하는 대조군은 성공한다. 예외 class와 stack이 Bingo와 같다.
수정은 binding의 shutdown/close owner가 이미 종료된 context의 completion pump 정리를
완결하도록 해야 한다. Framework에서 선행 shutdown을 생략하는 대안은 지원되는 공개 API
조합의 결함을 우회하므로 적용하지 않았다.

```bash
cd framework/languages/java
env -u ZLINK_LIBRARY_PATH TMPDIR=/dev/shm/zlink-tmp-java \
  flock -w7200 /tmp/zlink-jvm-gate.lock \
  java --enable-native-access=ALL-UNNAMED \
  -cp 'samples/java/Bingo/Server/Play/build/install/Play/lib/*' \
  /tmp/java-bingo-fix-evidence/ContextShutdownRepro.java
# 대조군: 위 명령 끝에 close-only 추가
```

- 소유 계층: **Java binding의 Context/completion pump lifecycle**. Framework는 실패를 관찰한다.
- Spec 조항: host relocation flow **§14, :771–826**의 callback → local cleanup → transport 정리와
  `ForceStopped/TeardownFailed`; binding `Context.java:48–51`의 shutdown 후 socket 정리 계약,
  `bindings/doc/spec/async-execution-model.ko.md:119–120`의 context termination waiter 정리.
- 교차언어 대조: .NET Framework backend는 binding `DisposeAsync`에 위임하고, .NET binding
  `Runtime/Handles/Context.cs:153–165`는 shutdown/term을 소유한다. Java의 native control wake
  pump 경로에서만 확인한 구조적 차이다. .NET B1–B3와 원인이 다르다.
- 변경 분류: **B — binding 기존 결함 확인, 수정 금지 범위이므로 미수정**.
- 수정 전/후 규칙 수: **변경 없음**. Framework catch·재시도·종료 순서 우회를 추가하지 않았다.

## 2. Runner — 기존 종료 증거 검사를 실패 소유자로 유지

감독의 과거 aggregate exit 0과 달리, 현재 checkout의 수정 전 Bingo 재현은
`bingo-before.exit=1`이었다. 기존 `runner-common.sh`는 이미 `TeardownFailed`를 실패로
전달했다. 과거 aggregate의 exit 0 원인을 이번 실행으로 확정했다고 주장하지 않는다.

현재 코드에서 확인한 누락은 공통 cleanup의 PID 배열 내부에만 있던 검증과, ZoneWorld의
별도 `SIGKILL` cleanup이다. `samples/runner-common.sh:251`의 기존 evidence 검사를 PID
배열 바깥으로 옮겼다. `samples/java/ZoneWorld/run_sample.sh:49`는 공통 cleanup을 호출한다.
PID 배열이 없는 EXIT trap에 같은 `TeardownFailed` 역할 로그를 넣은 독립 재현에서 수정 전
exit 0, 수정 후 exit 1을 확인했다(`runner-no-pids-before.log`, `runner-no-pids-after.log`).
같은 역할 로그에 재시작을 append하므로 start 시 줄 위치를 기록해 현재 process의 증거를
검사한다. 정상 TERM은 재시작 전에 같은 verifier로 검사하고 의도적 KILL만 제외한다.
CLI·proxy는 host role 목록에 넣지 않으며 `zone-node-3`와 replacement도 빠뜨리지 않는다.

G4 수정 전 로그에는 client TimeoutException이 있는데도 runner가 `scenario ZW-G4 passed`를
출력했다. `run_sample.sh:245`에서 기존 client 성공 표식을 확인해야 runner 표식을 발행하도록
했다. Client의 `Unavailable` assertion은 그대로다. 새 보고 파일이나 별도 판정기는 없다.

`samples/run_samples.sh`의 하드코딩된 `.artifacts` native library override를 제거해 호출자의
환경을 보존한다. 요청대로 `ZLINK_LIBRARY_PATH`가 unset이면 설치 binding의 library를 쓴다.
기존 `SampleReleaseGateContractTest:114`의 특정 override 강제 조건도 이 환경 계약으로
바꿨다. Package version과 binary는 변경하지 않았다.

- 소유 계층: **Java sample runner의 기존 `zlink_sample_verify_framework_termination`**.
- Spec 조항: host relocation flow **§14**의 `Stopped/None`; ZoneWorld **§9/ZW-G4**의 client 증거.
- 교차언어 대조: 감독이 지정한 .NET `8e76335988`, Node `8159b15752`의 host 정상 종료 조건과
  동일하게 Java host 역할을 판정한다. 의도적 crash와 client CLI만 예외다.
- 변경 분류: **B — 검증 누락 수정**. Native library 선택은 요청된 환경에 대한 **A 계약 적응**.
- 수정 전/후 규칙 수: **종료 판정 2 → 1**. ZoneWorld의 별도 강제 종료 성공 경로를 없애고
  기존 verifier를 재사용한다. PID 배열 유무는 판정 조건에서 제거했다.

## 3. ZoneWorld 오류 보존과 durable target lifecycle

Sample `Server/.../zone/actors/PlayerActor.java:220–230`은 이제 `UNAVAILABLE`만 `Unavailable`로
변환한다. `DEADLINE_EXCEEDED`, `SHUTTING_DOWN`은 각각 `DeadlineExceeded`, `ShuttingDown`이다.
.NET `PlayerActor.cs:132–136`의 `failed.Kind.ToString()`과 Node `player-actor.ts:96`처럼 실제
kind를 보존한다.

오류 통합을 제거한 첫 G4에서 source는 Core `NOT_CONNECTED`를 받았지만 계속 replay했다.
`zoneworld-g4-unmasked.log` 및 `/dev/shm/zlink-tmp-java/tmp.MWwRnDmBDf/logs/zone-node-1.log`
`:11434`에 native terminal, `:31310`에 `Failed kind=DEADLINE_EXCEEDED`가 있다.
Client는 `Unavailable`을 받지 못하고 timeout했다. 이는 G4 PASS가 아니다.

원인은 `ZLinkJavaDurableRequest.java`의 typed failure replay가 logical owner 종료와 physical
disconnect를 구분하지 않는 데 있었다. `ZLinkJavaRawMeshNode.java:6992–7003`에서 기존
Location expectation, connection intent와 admitted peer 정보를 조회한다. Expectation/intent가
없고 admitted peer도 없으면 sender `ZLinkJavaDurableRequest.java:57`가 `Unavailable`로 끝낸다.
재시도 deadline 판정을 attempt로 모아 logical terminal 검사와 경쟁하는 retry 분기를 없앴다.
기존 10 ms replay schedule, operation deadline과 frozen wire identity는 유지한다.

Peer 부재만 terminal로 삼는 대안은 transient disconnect replay를 깨므로 채택하지 않았다.
선택한 대안은 D-093 규칙 1의 기존 logical owner 판정 조회다. Monitor 상태, timer, poller,
connection generation 표나 retry budget은 추가하지 않았다. `hasPeerExpectation`은 기존
known-channel 정리에도 재사용해 같은 사실의 별도 판정을 두지 않았다.
기존 missing-route 테스트에는 Location expectation을 명시해 transient admission 대기를
검사하도록 했다. 원래의 deadline 경과·Unavailable assertion은 유지하고, expectation과
intent를 제거한 경우는 별도 테스트에서 즉시 terminal로 검증한다.

수정 후 focused G4에서 actor `g4-crash-974c19`는 `zoneworld-g4-final-roles/zone-node-1.log:22292`의
`Failed kind=UNAVAILABLE`로 끝났다. Client assertion과 replacement fresh placement가 통과했고
sample exit는 0이다. 현재 5개 host 역할 모두 `Stopped/None`이며 이전 `zone-node-2` process만
시나리오가 의도적으로 crash시켰다.

- 소유 계층: **Framework Location/auto-connect가 target 기대를 소유하고 durable sender가
  operation terminal을 소유한다**. Core는 physical completion을 소유한다.
- Spec 조항: `03-spot-actor/08-routing.ko.md:322–331`, actor model **§5 sender replay**,
  ZoneWorld **§9**, 감독 결정 **D-093 규칙 1**.
- 교차언어 대조: D-093/.NET 진단과 동일한 logical lifecycle 조건을 적용한다. Node의 단일
  ActorJoin timeout/peer 조회 경로와 Java durable replay는 구조가 다르다. Sample의 오류 kind
  보존은 .NET/Node와 일치한다.
- 변경 분류: **B — Java durable sender 기존 결함과 sample 오류 은폐 수정**.
- 수정 전/후 규칙 수: **Unavailable 의미 판정 2곳 → 1곳**, replay deadline 판정 경로 **2 → 1**.
  Sample이 deadline/shutdown을 owner 종료로 재분류하던 규칙을 제거했다.

## 검증 결과

모든 최종 Gradle 명령은 `TMPDIR=/dev/shm/zlink-tmp-java`, `unset ZLINK_LIBRARY_PATH`,
`flock -w7200 /tmp/zlink-jvm-gate.lock`으로 실행했다. Sample은 추가로
`flock -w7200 /tmp/zlink-samples-gate.lock`을 획득했다. Aggregate는 이 작업의 Java 범위를
지정한 `ZLINK_SAMPLE_LANGUAGES=java bash samples/run_samples.sh` 1회다. 최초 Bingo 진단은 기존 sample 전용
Gradle lock으로 시작했으며, 이후 실행은 요청된 JVM lock도 적용했다.

| 검증 | 결과 | 증거 |
|---|---|---|
| Bingo 수정 전 | exit 1, session-b/play-b TeardownFailed | `bingo-before.log`, `bingo-before-roles/` |
| Bingo 상세 진단 | exit 1, session-b/play-a/play-b binding context close 실패 | `bingo-diagnosis.log`, `bingo-diagnosis-roles/` |
| Binding 공개 API shutdown→close | FAIL, TERMINATED/156384765 | `context-shutdown-repro.log` |
| Binding 공개 API close 단독 | PASS | `context-close-only-repro.log` |
| Durable sender·mesh focused | 20/20 PASS | `durable-focused.log` |
| Runner 종료 contract | PASS | `runner-focused.log` |
| PID 배열 없는 EXIT trap, 동일 TeardownFailed 로그 | 수정 전 exit 0 → 수정 후 exit 1 | `runner-no-pids-before.log`, `runner-no-pids-after.log` |
| Unmasked G4 | 실제 FAIL, DeadlineExceeded와 client timeout; 당시 runner는 잘못된 PASS 표식 | `zoneworld-g4-unmasked.log` |
| 요청된 core test + contractTest 명령 1회 | core 1222/1224 PASS, 2 FAIL; contract는 core 실패로 실행 안 됨 | `full-unit-contract.log`, `full-core-results/` |
| 미실행 contractTest 별도 완료 | 기존 native override 강제 assertion 1 FAIL; 해당 환경 검사 수정 | `contract-only.log` |
| Core 실패 2개 focused 재검증 | descriptor 교체 PASS, operation registry cancellation FAIL | `gate-failures-focused.log` |
| Native 선택 contract 수정 후 | PASS | `native-selection-contract.log` |
| 수정 후 G4 focused | exit 0, client Unavailable PASS, host 5/5 Stopped/None | `zoneworld-g4-final.log`, `zoneworld-g4-final-roles/` |
| 최종 Bingo 1/2 | exit 1; 업무 완료, host 4/7 Stopped/None; session-a/session-b/play-b TeardownFailed | `bingo-final-1.log`, `bingo-final-1-roles/` |
| 최종 Bingo 2/2 | exit 1; 업무 완료, host 4/7 Stopped/None; session-a/play-a/play-b TeardownFailed | `bingo-final-2.log`, `bingo-final-2-roles/` |
| 최종 ZoneWorld 1/2 | exit 1; G4 Unavailable 및 host 5/5 정상; B8·본 실행 gateway TeardownFailed | `zoneworld-final-1.log`, `zoneworld-final-1-{g4,b8,main}-roles/` |
| 최종 ZoneWorld 2/2 | exit 1; G4 Unavailable 및 host 5/5 정상; B8·본 실행 gateway TeardownFailed | `zoneworld-final-2.log`, `zoneworld-final-2-{g4,b8,main}-roles/` |
| Java aggregate 1회 | exit 1; release contract/fake-backend/TicTacToe 통과 후 Bingo session-b/play-b TeardownFailed에서 중단 | `aggregate-final.log:198–206`, `aggregate-final-20260905-165751-73638/` |
| Shell syntax·최종 diff 검사 | PASS | `bash -n`, `git diff --check` |

각 ZoneWorld 실행의 B8 host는 4/5, 본 실행의 host 로그는 5/6이 `Stopped/None`이다.
나머지는 gateway의 `TeardownFailed`다. 본 실행의 여섯 로그에는 G3에서 정상 종료한
zone-node-2와 별도 replacement가 함께 포함된다. B8 실패 때문에 두 실행 모두
`zoneworld=completed` 표식이 보류됐다. Gateway 종료 예외의 상세 stack은 수집하지 않았으므로
Bingo와 같은 binding 원인이라고 단정하지 않는다.

설치 binding JAR SHA256은 `f958b5117a5b0e388297ef97831ddb64998a2b1339f844d6a95fcda432b49fdc`,
내장 `libzlink.so` SHA256은 요청된
`1c7887c36dd2f1fe133fe3a39ccbfeb9ea42d4b051b302e3ecacf160e31b116d`다. 최종 aggregate 뒤에도
두 hash가 동일했다. Package 재빌드는 없다.

## 변경 파일

Java `samples/runner-common.sh`, `samples/run_samples.sh`, ZoneWorld runner와 `PlayerActor.java`;
core runtime의 `ZLinkJavaDurableRequest.java`, `ZLinkJavaRawMeshNode.java`;
테스트 `ZLinkJavaDurableRequestTest`, `ZLinkJavaRawMeshNodeDurableReplayTest`,
`SampleRunnerTerminationContractTest`, `SampleReleaseGateContractTest`; 이 문서.

## BLOCKERS

1. **Bingo 정상 종료 미해결**: Java binding의 shutdown 후 completion pump close가
   `TERMINATED`를 던진다. Binding owner 수정과 package 갱신이 필요하며 이번 작업에서는 금지다.
2. **전체 unit gate 미통과**: `ZLinkServiceOperationRegistryTest.java:109`는 cancellation 뒤
   callback count가 1이어야 한다고 검사하지만 0이다. Registry `:195–196`은 completion을
   별도 실행에 게시한다. 이 파일들은 수정하지 않았으며 별도 원인 확인이 필요하다.
   Mesh descriptor 교체의 최초 실패는 focused 재검증에서 재현되지 않았다.
3. **ZoneWorld 전체 정상 종료 미달**: 두 실행 모두 G4는 통과했지만 B8·본 실행 gateway가
   `TeardownFailed`다. 상세 stack에 따른 owner 진단이 남는다. Client G4 assertion과 종료
   assertion을 완화하지 않았다.
4. **요청된 전체 green gate 미달**: Bingo ×2, ZoneWorld ×2와 aggregate의 정상 종료 조건 및
   전체 core unit 0-failure 조건은 달성하지 못했다. Aggregate는 Bingo에서 중단해 뒤 sample을
   실행하지 않았다. 위 owner 결함을 해결한 뒤 최종 gate를 다시 통과시켜야 한다.
