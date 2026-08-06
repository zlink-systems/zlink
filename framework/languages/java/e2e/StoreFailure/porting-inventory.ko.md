# StoreFailure Java 포팅 인벤토리

기준:
- `.NET`: `framework/languages/dotnet/e2e/StoreFailure`
- 공통 문서: `framework/doc/framework/common/e2e/config-6-store-failure-recovery.ko.md`

상태 의미:
- `done`: Java 파일이 실제로 존재하고 같은 책임을 수행한다.
- `partial`: 일부 책임만 새 location store 계약으로 전환됐다.
- `gap`: 아직 `.NET` 기준 의미까지 대응되지 않는다.
- `not-needed`: Java 구조에서는 별도 파일이 필요 없고, 근거를 비고에 적었다.

| .NET 기준 파일 | Java 대응 파일 | 분류 | 상태 | 비고 |
|----------------|----------------|------|------|------|
| `.gitignore` | `.gitignore` | config | done | 로그와 Gradle 산출물 제외. |
| `feature-map.ko.md` | `feature-map.ko.md` | feature-map | done | Config 6 Java Redis location store scenario 완료와 검증 로그를 기록한다. |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | 실행별 Redis location store를 기본으로 준비한다. 기본 실행, polling fallback, fail-static outage, grace exceeded, provider crash, graceful shutdown, short outage recovery, long outage recovery, status transition, store response delay와 `all` 실행은 Redis location store 기반이다. |
| `Shared/Messages.cs` | `Shared/src/main/java/systems/zlink/e2e/storefailure/shared/Contracts.java` | shared | done | baseline request/reply와 evidence wait DTO를 제공하고, status와 peer row는 consumer public endpoint의 응답으로 검증한다. |
| `Client/Program.cs` | `Client/src/main/java/systems/zlink/e2e/storefailure/client/Program.java` | client-entry | done | `SF-A1`/`SF-A2`/`SF-B1`/`SF-B2`/`SF-C1`/`SF-C2`/`SF-D1`/`SF-D2`/`SF-D3`/`SF-E1` dispatch만 남기고 오래된 DR-* dispatch를 제거했다. |
| `Client/Scenarios/SfA1BaselineScenario.cs` | `Client/src/main/java/systems/zlink/e2e/storefailure/client/scenarios/SfA1BaselineScenario.java` | scenario | done | provider peer row, runtime status, request success를 public HTTP endpoint로 검증한다. |
| `Client/Scenarios/SfA2PollingFallbackScenario.cs` | `Client/src/main/java/systems/zlink/e2e/storefailure/client/scenarios/SfA2PollingFallbackScenario.java` | scenario | done | polling-only consumer가 watch 없이 provider 추가/제거를 peer query와 request path로 반영하는지 검증한다. |
| `Client/Scenarios/SfB1FailStaticScenario.cs` | `Client/src/main/java/systems/zlink/e2e/storefailure/client/scenarios/SfB1FailStaticScenario.java` | scenario | done | Redis pause 중 기존 request 성공, unhealthy status, owner lease failure, recovery status를 public endpoint로 검증한다. |
| `Client/Scenarios/SfB2GraceExceededScenario.cs` | `Client/src/main/java/systems/zlink/e2e/storefailure/client/scenarios/SfB2GraceExceededScenario.java` | scenario | done | store failure grace를 넘긴 Redis pause 중 기존 request가 계속 성공하고 복구 후 status/peer row가 정상인지 검증한다. |
| `Client/Scenarios/SfC1CrashLeaseExpiryScenario.cs` | `Client/src/main/java/systems/zlink/e2e/storefailure/client/scenarios/SfC1CrashLeaseExpiryScenario.java` | scenario | done | `api-b` SIGKILL 뒤 owner lease 만료로 peer list에서 제외되고 request가 `api-a`로만 가는지 검증한다. |
| `Client/Scenarios/SfC2GracefulRemovalScenario.cs` | `Client/src/main/java/systems/zlink/e2e/storefailure/client/scenarios/SfC2GracefulRemovalScenario.java` | scenario | done | provider HTTP `/shutdown` 뒤 owner lease TTL 전에 peer row가 사라지고 request가 `api-a`로만 가는지 검증한다. |
| `Client/Scenarios/SfD1ShortOutageRecoveryScenario.cs` | `Client/src/main/java/systems/zlink/e2e/storefailure/client/scenarios/SfD1ShortOutageRecoveryScenario.java` | scenario | done | TTL보다 짧은 Redis pause/unpause 동안 request가 계속 성공하고 복구 후 status/peer row가 정상인지 검증한다. |
| `Client/Scenarios/SfD2LongOutageRecoveryScenario.cs` | `Client/src/main/java/systems/zlink/e2e/storefailure/client/scenarios/SfD2LongOutageRecoveryScenario.java` | scenario | done | 긴 Redis outage 중 `api-b`를 죽인 뒤, survivor `api-a` 재등록과 `api-b` 제외 및 request 지속 성공을 검증한다. |
| `Client/Scenarios/SfD3StatusTransitionScenario.cs` | `Client/src/main/java/systems/zlink/e2e/storefailure/client/scenarios/SfD3StatusTransitionScenario.java` | scenario | done | public runtime status가 healthy, outage, recovered 순서로 바뀌고 복구 뒤 last refresh 시간이 전진하는지 검증한다. |
| `Client/Scenarios/SfE1StoreDelayNonBlockingScenario.cs` | `Client/src/main/java/systems/zlink/e2e/storefailure/client/scenarios/SfE1StoreDelayNonBlockingScenario.java` | scenario | done | store response delay 중 일반 request p99가 baseline 기반 budget 안에 있고, 지연 해제 뒤 request가 성공하는지 검증한다. |
| `Server/Provider/StoreFailure.Provider.csproj` | `Server/Provider/build.gradle.kts` | build | done | Redis location store extension을 참조한다. |
| `Server/Provider/ProviderHostFactory.cs` | `Server/Provider/src/main/java/systems/zlink/e2e/storefailure/provider/ProviderApplication.java` | server-role | done | `useDiscovery()` 없이 Redis location store와 짧은 location timing option을 등록하고 provider channel server를 public framework 설정으로 연다. |
| `Server/Provider/ProviderEndpoints.cs` | `Server/Provider/src/main/java/systems/zlink/e2e/storefailure/provider/ProviderEndpoints.java` | endpoints | done | health/evidence/evidence wait/shutdown endpoint를 제공한다. provider runtime query는 현재 Config 6 검증에서 필요하지 않고 consumer public query가 observer 역할을 맡는다. |
| `Server/Consumer/StoreFailure.Consumer.csproj` | `Server/Consumer/build.gradle.kts` | build | done | Redis location store extension을 참조한다. |
| `Server/Consumer/ConsumerHostFactory.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/storefailure/consumer/ConsumerApplication.java` | server-role | done | Redis location store와 짧은 location timing option을 등록하고, polling-only mode와 delay mode에서는 같은 store를 감싼 공개 검증용 store를 사용한다. |
| `Server/Consumer/Program.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/storefailure/consumer/Program.java` | server-entry | done | consumer process entrypoint다. |
| `Server/Consumer/PollingOnlyLocationStore.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/storefailure/consumer/PollingOnlyLocationStore.java` | store | done | Redis store에 모든 I/O를 위임하되 optional change-stamp interface를 구현하지 않아 pure polling 경로를 만든다. |
| `Server/Consumer/DelayableLocationStore.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/storefailure/consumer/DelayableLocationStore.java` | store | done | Redis store에 모든 I/O를 위임하기 전에 설정된 delay를 비동기로 적용해 store 응답 지연을 주입한다. |
| `Server/Consumer/LocationStoreDelayState.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/storefailure/consumer/LocationStoreDelayState.java` | store | done | consumer admin endpoint가 설정한 delay 값을 store wrapper가 같은 state에서 읽는다. |
| `Server/Consumer/ConsumerEndpoints.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/storefailure/consumer/ConsumerEndpoints.java` | endpoints | done | request endpoint, public location runtime query status/peer endpoint, store delay admin endpoint를 제공한다. |
| `Server/Registry/*` | 없음 | server-role | not-needed | Config 6은 Redis location store 장애·복구를 검증하므로 embedded registry role을 사용하지 않는다. |
| `Server/Embedded/*` | 없음 | server-role | not-needed | 제거된 registry 계약에 의존하던 old DR role이라 소스에서 제거했다. |
| `Server/Probe/*` | 없음 | server-role | not-needed | registry topology probe 대신 public location runtime query를 사용한다. |

## 검증

- `../../gradlew --project-cache-dir /tmp/zlink-storefailure-gradle-cache --no-daemon compileJava --console=plain`
  - 결과: `BUILD SUCCESSFUL`
- `timeout 240s ./run_e2e.sh SF-A1`
  - 결과: `scenario SF-A1 passed providers=[api-a]`, `store-failure e2e result=passed`
  - 로그: `logs/20260703-205335-11858/`
- `timeout 300s ./run_e2e.sh SF-A2`
  - 결과: `scenario SF-A2 passed providers=[api-a]`, `scenario SF-A2 passed providers=[api-b]`,
    `scenario SF-A2 passed providers=[api-a]`, `store-failure e2e result=passed`
  - 로그: `logs/20260703-211659-69614/`
- `timeout 360s ./run_e2e.sh SF-B1`
  - 결과: `scenario SF-A1 passed providers=[api-a]`, `scenario SF-B1 passed providers=[api-a, api-b]`,
    `scenario SF-B1-RECOVERED passed providers=[api-b]`, `store-failure e2e result=passed`
  - 로그: `logs/20260703-212347-99803/`
- `timeout 180s ./run_e2e.sh SF-B2`
  - 결과: `scenario SF-A1 passed providers=[api-a]`, `scenario SF-B2 passed providers=[api-a]`,
    `scenario SF-B2-RECOVERED passed providers=[api-a]`, `store-failure e2e result=passed`
  - outage 중 RouteMesh snapshot에서 기존 A는 READY 상태를 유지하고 B는 target에 없음을 확인했다.
    Store 복구 뒤에는 Location query에서 B가 READY 상태로 편입된 것을 확인했다.
  - 로그: `logs/20260806-023951-3481315/`
- `timeout 360s ./run_e2e.sh SF-D1`
  - 결과: `scenario SF-A1 passed providers=[api-a]`, `scenario SF-D1 passed providers=[api-b, api-a]`,
    `scenario SF-D1-RECOVERED passed providers=[api-a]`, `store-failure e2e result=passed`
  - 로그: `logs/20260703-212806-15766/`
- `timeout 300s ./run_e2e.sh SF-C1`
  - 결과: `scenario SF-A1 passed providers=[api-a]`, `scenario SF-C1 passed providers=[api-a]`,
    `store-failure e2e result=passed`
  - 로그: `logs/20260703-210726-45574/`
- `timeout 300s ./run_e2e.sh SF-C2`
  - 결과: `scenario SF-A1 passed providers=[api-a]`, `scenario SF-C2 passed providers=[api-a]`,
    `store-failure e2e result=passed`
  - 로그: `logs/20260703-211120-55351/`
- `timeout 420s ./run_e2e.sh SF-D2`
  - 결과: `scenario SF-A1 passed providers=[api-a]`, `scenario SF-D2 passed providers=[api-a]`,
    `store-failure e2e result=passed`
  - 로그: `logs/20260703-214417-56996/`
- `timeout 360s ./run_e2e.sh SF-D3`
  - 결과: `scenario SF-D3-HEALTHY passed providers=[api-a]`,
    `scenario SF-D3-OUTAGE passed providers=[]`, `scenario SF-D3-RECOVERED passed providers=[api-b]`,
    `scenario SF-D3 passed`, `store-failure e2e result=passed`
  - 로그: `logs/20260703-213847-41499/`
- `timeout 900s ./run_e2e.sh all`
  - 결과: `SF-A1`/`SF-A2`/`SF-B1`/`SF-B2`/`SF-C1`/`SF-C2`/`SF-D1`/`SF-D2`/`SF-D3` 통과,
    `store-failure e2e result=passed`
  - 로그: `logs/20260703-214733-67106/`
- cleanup 후 재검증
  - `../../gradlew --project-cache-dir /tmp/zlink-storefailure-cleanup-gradle-cache --no-daemon :Client:compileJava :Server:Provider:compileJava :Server:Consumer:compileJava --console=plain`
    - 결과: `BUILD SUCCESSFUL`
  - `timeout 900s ./run_e2e.sh all`
    - 결과: `SF-A1`/`SF-A2`/`SF-B1`/`SF-B2`/`SF-C1`/`SF-C2`/`SF-D1`/`SF-D2`/`SF-D3` 통과,
      `store-failure e2e result=passed`
    - 로그: `logs/20260704-033113-35152/`
- `nice -n 10 ./gradlew --project-cache-dir /tmp/zlink-storefailure-gradle-cache --no-daemon --no-parallel --max-workers=1 -p e2e/StoreFailure :Shared:compileJava :Server:Consumer:compileJava :Client:compileJava --console=plain`
  - 결과: `BUILD SUCCESSFUL`
- `nice -n 10 timeout 420s ./run_e2e.sh SF-E1`
  - 결과: `scenario SF-E1 passed`, `scenario SF-E1 passed providers=[api-a]`,
    `store-failure e2e result=passed`
  - 로그: `logs/20260707-215129-3537304/`
- `nice -n 10 timeout 900s ./run_e2e.sh all`
  - 결과: `SF-A1`/`SF-A2`/`SF-B1`/`SF-B2`/`SF-C1`/`SF-C2`/`SF-D1`/`SF-D2`/`SF-D3`/`SF-E1` 통과,
    `store-failure e2e result=passed`
  - 로그: `logs/20260707-222314-3666363/`

## 남은 작업

없음: `SF-A1`, `SF-A2`, `SF-B1`, `SF-B2`, `SF-C1`, `SF-C2`, `SF-D1`, `SF-D2`, `SF-D3`, `SF-E1`과 `all`
runner는 닫힌 상태이며, 이 inventory에는 남은 `gap`/`partial` 행이 없다.
