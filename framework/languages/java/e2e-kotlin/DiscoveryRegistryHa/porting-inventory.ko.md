# Kotlin DiscoveryRegistryHa StoreFailure 포팅 인벤토리

기준 구현: `framework/languages/java/e2e/DiscoveryRegistryHa`

공통 문서: `framework/doc/framework/common/e2e/config-6-store-failure.ko.md`

현재 Kotlin DiscoveryRegistryHa E2E는 Config 6 StoreFailure 기준으로 전환했다. 실행 그래프는
`Shared`, `Client`, `Server/Provider`, `Server/Consumer`만 남고, legacy Discovery/Registry role과
`DR-*` scenario source는 제거했다.

상태 값:

- `done`: 현재 파일이 목표 위치와 의미를 만족한다.
- `not-needed`: Kotlin 구조에서 같은 파일 단위가 필요 없으며 비고에 근거를 적었다.

| 기준 파일 | Kotlin 대응 파일 | 분류 | 상태 | 비고 |
|-----------|------------------|------|------|------|
| `.gitignore` | `.gitignore` | config-root | done | Gradle 산출물과 logs 제외는 유지한다. |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | `SF-A1`~`SF-D3` 검증 결과와 legacy 제거 상태를 반영했다. |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | 전용 Redis container, 실행별 key prefix, Provider/Consumer/Client role만 사용해 `SF-A1`~`SF-D3`와 `all`을 실행한다. |
| `Shared/build.gradle.kts` | `Shared/build.gradle.kts` | build | done | Shared Gradle project를 유지한다. |
| `Shared/Messages.*` | `Shared/src/main/kotlin/.../Messages.kt` | shared | done | channel request/reply 타입을 Kotlin shared source로 유지한다. |
| `Client/build.gradle.kts` | `Client/build.gradle.kts` | build | done | Client application project를 유지한다. |
| `Client/Program.*` | `Client/src/main/kotlin/.../Program.kt` | client-entry | done | Client binary entry point가 client application만 실행한다. |
| `Client/Support/ClientOptions.*` | `Client/src/main/java/.../ClientOptions.java` | support | done | scenario, consumer endpoint, expected/dead rid, location timing 입력만 CLI option으로 파싱한다. |
| `Client/Support/ClientContext.*` | `Client/src/main/kotlin/.../ClientScenarioContext.kt` | support | done | `SF-A1`~`SF-D3` StoreFailure oracle을 Kotlin으로 구현했다. |
| `Client/Support/ScenarioAssert.*` | `Client/src/main/kotlin/.../ScenarioAssert.kt` | support | done | scenario assertion helper를 유지한다. |
| `Client/Scenarios/Sf*.java` | `ClientScenario.kt`, `ClientScenarioContext.kt` | scenario | done | Kotlin은 scenario별 파일 대신 dispatcher와 context method로 `SF-*`를 매핑한다. |
| `Server/Provider/build.gradle.kts` | `Server/Provider/build.gradle.kts` | build | done | Provider role이 Redis location extension에 의존한다. |
| `Server/Provider/Program.*` | `Server/Provider/src/main/kotlin/.../Program.kt` | server-entry | done | Provider binary entry point가 provider application만 실행한다. |
| `Server/Provider/ProviderApplication.*` | `Server/Provider/src/main/kotlin/.../ProviderApplication.kt` | server-role | done | provider API channel, flow logging, location timing, `ZLinkRedisLocationStore` bean을 구성한다. |
| `Server/Provider/ProviderEndpoints.*` | `Server/Provider/src/main/kotlin/.../ProviderHttpServer.kt` | server-role | done | health/evidence/shutdown HTTP endpoint를 제공한다. |
| `Server/Provider/ProviderOptions.*` | `Server/Provider/src/main/java/.../ProviderOptions.java` | configuration | done | provider rid/http/api endpoint, Redis location endpoint/key prefix, timing, log dir를 CLI option으로 파싱한다. |
| `Server/Provider/ProviderHandlers.*` | `Server/Provider/src/main/kotlin/.../WorkRequestHandler.kt` | handlers | done | provider request handler를 Kotlin으로 유지한다. |
| `Server/Provider/ProviderEvidenceStore.*` | `Server/Provider/src/main/kotlin/.../ProviderEvidenceStore.kt` | support | done | provider rid state를 Kotlin support로 유지한다. |
| `Server/Consumer/build.gradle.kts` | `Server/Consumer/build.gradle.kts` | build | done | Consumer role이 Redis location extension에 의존한다. |
| `Server/Consumer/Program.*` | `Server/Consumer/src/main/kotlin/.../Program.kt` | server-entry | done | Consumer binary entry point가 consumer application만 실행한다. |
| `Server/Consumer/ConsumerApplication.*` | `Server/Consumer/src/main/kotlin/.../ConsumerApplication.kt` | server-role | done | consumer client channel, flow logging, location timing, Redis store bean을 구성한다. |
| `Server/Consumer/ConsumerEndpoints.*` | `Server/Consumer/src/main/kotlin/.../ConsumerHttpServer.kt` | server-role | done | request endpoint와 public location status/peer query endpoint를 제공한다. |
| `Server/Consumer/ConsumerOptions.*` | `Server/Consumer/src/main/kotlin/.../ConsumerOptions.kt` | configuration | done | consumer rid/http endpoint, Redis location endpoint/key prefix, timing, store mode, log dir를 CLI option으로 파싱한다. |
| `Server/Consumer/PollingOnlyLocationStore.*` | `Server/Consumer/src/main/java/.../PollingOnlyLocationStore.java` | support | done | `SF-A2` polling-only 검증을 위해 watch interface를 노출하지 않는 wrapper를 둔다. |
| `Server/Registry/*` | 없음 | legacy | not-needed | Config 6 StoreFailure에서는 Registry role을 실행하지 않으므로 제거했다. |
| `Server/Probe/*` | 없음 | legacy | not-needed | Registry query probe는 location store 전환 후 필요 없으므로 제거했다. |
| `Server/Embedded/*` | 없음 | legacy | not-needed | Embedded registry/provider 배포 scenario는 StoreFailure 기준에서 제외되므로 제거했다. |

## Scenario ID 매핑

| Scenario ID | Kotlin 구현 | 상태 |
|-------------|-------------|------|
| `SF-A1` | `ClientScenarioContext.runStoreFailureBaseline()` | done |
| `SF-A2` | `ClientScenarioContext.runStoreFailurePollingFallback()` | done |
| `SF-B1` | `ClientScenarioContext.runStoreFailureFailStaticOutage()` + recovered check | done |
| `SF-B2` | `ClientScenarioContext.runStoreFailureGraceExceeded()` + recovered-with-peers check | done |
| `SF-C1` | `ClientScenarioContext.runStoreFailureCrashLeaseExpiry()` | done |
| `SF-C2` | `ClientScenarioContext.runStoreFailureGracefulRemoval()` | done |
| `SF-D1` | `ClientScenarioContext.runStoreFailureShortOutageTraffic()` + recovered-with-peers check | done |
| `SF-D2` | `ClientScenarioContext.runStoreFailureLongOutageRecovery()` | done |
| `SF-D3` | healthy/outage/recovered status methods | done |

## 검증

- `../../gradlew --project-cache-dir /tmp/zlink-kotlin-dr-gradle-cache --no-daemon --no-parallel --max-workers=1 :Client:installDist :Server:Provider:installDist :Server:Consumer:installDist --console=plain`
  통과.
- `timeout 420s ./run_e2e.sh SF-A1` 통과: `logs/20260704-051543-91770`.
- `timeout 1200s ./run_e2e.sh all` 통과: `logs/20260704-051605-92888`.
