# Java ClientServer Server-only readiness 수정 결과

Java의 Serving 상태인 Server-only host는 listener가 시작되고 weight가 양수이면
`READY`, `isReady=true`, `readyTargetCount=1`을 반환한다. Weight가 0이면
`DEGRADED`, `false`, `0`이다. Ready Server가 없는 Client-only도 `DEGRADED`,
`false`, `0`이다. Client+Server는 local Ready Server를 한 번 집계한다.
공개 API와 Kotlin 구현은 변경하지 않았다. 최종 전체 gate는 실패 0개이며,
Java 7개와 Kotlin 7개 sample도 모두 통과했다.

소유 계층: Framework의 socket registry가 local·remote target과 connection 사실을 소유하고,
ClientServer monitoring view가 host 상태와 target 수를 공개 topology 상태로 투영한다.

소유 spec: `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:174,194`의
topology 범위·Ready 정의와 `languages/java/interfaces/monitoring.ko.md:229,249`의 status·동일 process weight 규칙이다.
`languages/dotnet/interfaces/10-topology-monitoring.ko.md:379`도 local·remote의 positive-weight Ready Server 집계를 명시한다.
Outbound 역할·Ready connection 선택은 `02-channel-transport/03-client-server-channel.ko.md:17,218,262`를 따른다.
위 상대 spec 경로의 root는 `framework/doc/framework/common/spec/server/`다.

교차언어 대조 결과: .NET의 `ZLinkClientServerRuntimeService.cs:39,80,98,139`는 local Server를 포함한
snapshot의 Ready 수와 readiness를 공개 status로 전달한다. Java는 registry의 listener descriptor와
socket weight를 사용한다는 구조적 차이가 있다. Kotlin monitoring 계약 `languages/kotlin/interfaces/monitoring.ko.md:5,32`는
Java status와 accessor를 그대로 사용한다. Kotlin 계약 테스트와 JVM 전체 gate로 이 projection을 검증했다.

변경 분류: **B — 기존 결함**. 감독이 지정한 원인·수정 범위와 regression/gate 지시를 구현 승인으로 적용했다.
구현 전에 원인, 소유 spec, 교차언어 대조와 B 분류를 보고했다.

## 원인과 수정

수정 전 `ZLinkChannelSocketRegistry.java:573–599`는 `clientServerConnections`만 순회하여
local Server descriptor를 누락했다. `ZLinkTopologyRuntimeViews.java:67–68`은
`server || !client || readyTargetCount > 0`으로 Server 역할만 있어도 readiness를 허용했다.
따라서 weight 100의 Server-only는 Ready이면서 count 0이고, weight 0도 Ready였다.

`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/`
아래 수정은 다음과 같다.

- `ZLinkChannelSocketRegistry.java:580`은 기존 local descriptor를 remote target 목록과 함께 snapshot으로 만든다.
  중복 제거에는 기존 `clientServerLogicalIdentity`를 사용한다. 같은 identity의 자기 연결이 있어도 local Server를 한 번 집계한다.
  Local weight는 `:609`에서 실제 server socket의 현재 값을 읽으므로 runtime weight 변경도 반영한다.
- `ZLinkChannelSocketRegistry.java:1612`의 기존 target snapshot constructor가 connection·Serving 상태를 한 번 계산한다.
  Public projection은 저장된 Ready 값과 positive weight를 사용한다.
- `ZLinkTopologyRuntimeViews.java:61,67`은 Ready target 수를 한 번 집계하고
  `hostServing && readyTargetCount > 0`으로 topology readiness를 결정한다.
- `ZLinkChannelRuntime.java:1125`는 outbound의 `UNAVAILABLE`/`NOT_FOUND` 분류 입력을
  registry의 실제 connection 조회인 `ZLinkChannelSocketRegistry.java:569`에서 얻는다.
  Monitoring 목록에 local target을 합쳐도 미준비 자기 연결이 이 조회에서 가려지지 않는다.
  Client 역할 검사와 오류 분류·Ready 대기·선택은 기존 호출 경로에 유지한다.
  Monitoring snapshot의 사용하지 않는 `connectionReady` field는 제거했다.

Local descriptor는 `ZLinkChannelRuntime.java:552–570`의 channel 설정 완료 뒤 생성된다.
`ZLinkChannelRuntimeConfigurator.java:75–84`가 router 등록, bind, handler 등록과 request loop 시작을 완료하고,
`ZLinkChannelSocketRegistry.java:662–704`가 실제 listener endpoint를 읽어 descriptor를 저장한다.
따라서 역할 선언만으로 local Ready target이 만들어지지 않는다. Descriptor는 socket 종료 경로에서 제거된다.

대안으로 view에 Server-only 보정 분기를 두는 방식을 검토했다. Target 집계와 readiness에 별도 역할 규칙이
생기므로, 기존 snapshot에 local Server를 포함하고 view가 그 결과를 읽는 방식을 선택했다.
새 runtime 상태, timer, retry, option, codec이나 public helper는 추가하지 않았다.

수정 전/후 규칙 수: **host Serving 아래 readiness 허용 분기 3 → 1**
(`server`, `!client`, positive Ready target → positive Ready target).
Snapshot의 저장 field도 **4 → 3**이며, connection 준비 사실은 기존 registry에만 남는다.

## 변경 파일과 회귀 테스트

- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelSocketRegistry.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelRuntime.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkTopologyRuntimeViews.java`
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/channels/ZLinkTopologyRuntimeViewsTest.java`
- `framework/languages/java/zlink-framework-spring-boot-starter/src/test/java/systems/zlink/framework/spring/ZLinkClientServerReadinessTest.java`
- 이 결과 문서

새 회귀 테스트는 실제 Spring host를 public configurer로 시작한다. Public monitoring·listener·socket options·client만 사용하며,
internal DTO, raw frame, 수동 codec이나 내부 구조 assertion을 사용하지 않는다.

| 테스트 위치 | 검증 |
|---|---|
| `ZLinkClientServerReadinessTest.java:38` | Server-only weight 100/0의 host Serving, listener bind, role, state, isReady, count와 local target weight |
| `:63` | 유효한 manual endpoint를 가진 Client-only에 Ready Server가 없으면 Degraded/false/count 0 |
| `:82` | Client+Server weight 100/0의 Ready 대상 수 1/0 및 공개 observe 초기 status |
| `:110` | Public runtime socket weight를 100 → 0 → 100으로 변경했을 때 local readiness와 count |
| `:136` | Ready Server-only에서 outbound send/request의 `NOT_CONFIGURED` 유지 |
| 기존 `ZLinkTopologyRuntimeViewsTest.java:19` | Ready Server fixture를 등록한 뒤 host relocation 시 IsReady=false, Stopping, target count 유지 |

기존 projection unit test의 역할 선언만 있던 fixture에는 server와 Serving descriptor를 등록했다.
기존 readiness·relocation assertion은 유지하고 target count assertion을 추가했다.
수정 전 유효한 공개 설정으로 실행한 회귀 테스트는 **6개 중 4개 실패**였다.
Server-only와 Client+Server의 weight 100 count 누락 및 weight 0의 잘못된 Ready를 재현했다.

## 검증과 gate

증거 root는 `/tmp/zlink-java-cs-server-ready-20260906/`다.
최종 전체 gate는 **exit 0, 1,641개 중 통과 1,627개·skip 14개·실패 0개**다.
집계 기준은 Gradle HTML report이며 `final-full-gate-html-result.json`에 보존했다.
Redis suite의 skip 14개가 포함된다. XML만 합산한 수는 1,628개이므로 전체 개수에는 HTML 집계를 사용한다.

| 검증 | 결과 | 증거 |
|---|---|---|
| 수정 전 공개 회귀 | 2 pass / 4 fail | `focused-before.log`, `focused-before.xml` |
| 최종 readiness 7 + ReadyWait 5 + topology projection 2 | PASS 14/14 | `final-focused.log` |
| Kotlin exact public contract | PASS 17/17 | `final-focused.log`, Kotlin `contractTest` report |
| 최종 `./gradlew --no-daemon test --rerun --continue` | PASS, failed 0, skipped 14 | `final-full-gate.log`, `final-full-gate-results/`, `final-full-gate-html/` |
| Kotlin framework unit — 전체 gate에 포함 | PASS 75/75 | `final-full-gate-html/zlink-framework-kotlin/index.html` |
| Sample release contract + public-manager fake backend | PASS 22/22 + 1/1 | `samples.log`, testkit reports |
| Java 7 + Kotlin 7 samples | PASS 14/14, aggregate exit 0 | `samples.log`, `samples-result.json` |

모든 Gradle test는 `flock --exclusive --close /tmp/zlink-java-gate.lock` 아래 실행했다.
최종 전체 gate를 시작하기 직전 `/proc/loadavg`는 `9.91 9.57 9.68`이었다.
Samples는 다음 순서로 공용 sample lock과 Java lock을 얻는다.

```bash
flock --exclusive --close /tmp/zlink-samples-gate.lock \
  flock --exclusive --close /tmp/zlink-java-gate.lock \
  env TMPDIR=/dev/shm/zlink-tmp-java ZLINK_JAVA_STREAM_TRACE=1 \
  ZLINK_SAMPLE_FAILURE_LOG_ROOT=/tmp/zlink-java-cs-server-ready-20260906/sample-logs \
  bash samples/run_samples.sh
```

실행 디렉터리는 `framework/languages/java`다. Samples의 기존 Normal/Detailed message-flow와 file log를 사용한다.
첫 실행부터 aggregate log와 실패 시 role log 보존 경로를 지정했다. Sample runtime·runner는 수정하지 않았다.
TicTacToe, Bingo, DeliveryDispatch, GameQuest, ShoppingMall, SupportChat, ZoneWorld를 두 언어에서 모두 실행했다.
Java와 Kotlin ZoneWorld는 선택 시나리오로 축소하지 않은 기본 `all` 실행이며,
`zoneworld=completed`와 정상 runner 종료를 확인했다. Aggregate의 최종 출력은 `All Java/Kotlin samples passed`다.

첫 전체 gate에는 기존 실패 두 건이 있었다. Outbound monitoring 의존을 제거하고 snapshot의 불필요한 field를
제거한 뒤 최종 gate를 다시 실행했다. 두 테스트의 코드·fixture·timeout·assertion은 변경하지 않았으며 최종 gate에서는 통과했다.

- `ZLinkClientServerM6ARuntimeTest.java:641`의 `sameProcessServerUsesStoreDiscoveryAndExactDealerRouterAdmission`:
  store runtime 시작 직후 기대한 outbound dealer가 null이었다. 단독 진단에서도 실패했다.
  이 테스트는 변경한 target monitoring snapshot과 오류 분류 경로를 호출하지 않는다.
- `ZLinkCanonicalRelocationStateMachineTest.java:499`의 `actorJoinPrepareRetriesNotConnectedWithinOriginalDeadline`:
  `target rejected canonical relocation: 17`로 실패했다. ClientServer monitoring을 사용하지 않는 Actor relocation 경로다.

첫 실패는 `full-gate.log`, `full-gate-results/`, 단독 진단은 `related-diagnostic-kotlin.log`에 보존했다.
이번 수정이 이 간헐 실패들의 원인을 해결했다고 판정하지 않는다.
최종 gate와 sample gate에 남은 실패는 없다.

추가 진단에서 Client+Server의 초기 status는 local Ready target과 미연결 임시 target을 함께 제공했다.
`admission-diagnostic.xml`은 초기 관측 뒤 다음 status를 기다리다 timeout한 기록이며, 실제 admission 종료 상태를 입증하지 않는다.
공개 회귀는 계약에서 요구하는 Ready 대상 수와 topology 상태를 검증한다. 전체 `targets` 길이를 Ready 대상 수로 간주하지 않는다.

검증에 사용한 binding은 `gradle/libs.versions.toml`의 `0.17.0` local Maven package다.
실제 resolved jar 경로는 `admission-diagnostic.log`에 확인했다.

- Jar: `.artifacts/wsl/maven/systems/zlink/zlink/0.17.0/zlink-0.17.0.jar`
- Jar SHA256: `5f377df340e2ab22217558846c9d263d9f54f34c27010c5454e478a458a59e60`
- Bundled Linux native와 `core/build-dev/lib/libzlink.so.0.17.0` SHA256은 동일:
  `64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43`

작업 branch는 `main`이며 commit은 하지 않았다. Core, binding, protected spec/doc, 타 언어 source는 수정하지 않았다.
동시에 진행 중인 타 언어 변경과 기존 untracked 디렉터리는 보존했다.
