# R5 Location 계약 정렬 보고

작업 branch는 `framework/1083-a3-gap-loc`이다. R5 시작점은 `8219bd53de`이고, `25e8a09cd1d1`을 병합한 `9e165eae37` 뒤에 작업했다. main 대조는 그 병합 대상 커밋의 동결 사본을 사용한다. 보호된 스펙 문서는 변경하지 않았다. 빌드·테스트는 `/home/hep7/worktree/zlink-1083-a3-gap-loc` 아래 WSL 사본에서 실행했다.

## 결정 소유·교차언어 대조

Relocation Store의 동일 bytes 확인·missing 재저장·충돌 시 새 reference는 Framework repository가 소유한다(Location Runtime §10:1334–1337). 갱신 시작 간격과 startup 관계식은 Framework LocationRuntime·등록 validator가 소유한다(§5:674,684). startup 취소와 pending provider 정리는 기존 host 종료 deadline이 소유한다(§5:701). Weight 범위는 glossary의 0..10000이다. 변경 분류는 계약에 테스트 입력을 맞춘 A와 기존 결함 B이며, Java startup 관계식과 generated encoder의 판정 위치 0→1은 감독자가 승인했다.

| 결정 | C++ | Java | .NET | Node |
|---|---|---|---|---|
| Relocation 재확인 | `provider_relocation_repository.hpp:58–109` | `ZLinkProviderRelocationRepository.java:38–108` | `ZLinkProviderRelocationRepository.cs:61–113` | `relocation-blob.ts:23–48` |
| 갱신 시작 간격 | `location_runtime.hpp:737–805` | `ZLinkLocationRuntime.java:332,715,796` | `ZLinkLocationRuntime.cs:764,792` | `runtime/locations/runtime.ts:1160,1172` |
| startup 관계식 | `framework_options_validation.hpp:36` | `ZLinkFrameworkRegistration.java:233` | `ZLinkLocationRegistrationValidator.cs:43` | `RegistrationValidators.ts:138` |
| weight 범위 | `service_descriptor_registry.cpp:151` | `MeshNodeRegistration.java:385,933`; generated `ServiceWireCodec.java` | `ZLinkSocketConfigs.cs:14,84` | `RegistrationBuilderPolicy.ts:18` |
| Authority 조회 완료 뒤 lane 전환 | `store_location_resolvers.hpp:331–354`; Redis 별도 worker `redis.cpp:159–164` | `ZLinkStatefulAuthorityRouteRuntime.java:99–108,483` | `ZLinkStoreLocationResolvers.cs:155–184,349–390` | `host/stateful-authority-route-runtime.ts:150–193` |
| provider 완료 뒤 수신 응답의 application 실행 경계 | `raw_mesh_node_owner.cpp:3284–3293`의 `enqueue_received(application)` | `ZLinkJavaRawMeshNode.java:5175–5184,6119–6129`의 inline `whenComplete`, 기존 `applicationDispatch:171` | `ZLinkManagedMeshNode.cs:7817`의 `EnqueueOwned(Application)` | `service-stateful-runtime.ts:4542–4556,3731`의 `enqueueApplicationFrame` |
| direct 정상 stop의 늦은 claim 정리 기한 | `location_runtime.hpp:215–231`의 fresh renew 또는 전달 deadline | `ZLinkLocationRuntime.java:293–319,399–400,733–738`의 최초 supplier 재사용 결함 | `ZLinkLocationRuntime.cs:204–260`의 stop caller token | `runtime/locations/runtime.ts:357–398`의 stop signal |
| host 기한 만료 뒤 resource close 시작 | `runtime/host/app.cpp:2874–2906`의 forced stop 뒤 hosted/channel/stream/provider close | `ZLinkFrameworkRuntime.java:2122–2141`의 네 async close가 공통 `deferStage`의 선행 gate에 막힘 | `ZLinkFrameworkHostRuntimeCoordinator.cs:82–90`의 `CloseResourcesAsync(CancellationToken.None)` | `host/runtime-shutdown.ts:49–61`의 stream/spot/channel/relocation dispose |

언어별 경로는 `framework/languages/<언어>/` 아래다. Authority의 Java 수정은 기존 lane의 비동기 완료 연결이다. C++은 별도 Redis worker, .NET·Node는 provider 완료 뒤 비동기 lane 진입을 사용한다. Java의 동기 `join()`은 Lettuce NIO callback에서 실행될 수 있어 같은 Redis connection의 다음 명령을 막았다. 이 결정의 소유 계층은 Framework Authority runtime과 execution gate다(Execution Gate §1, §12–13).

| 언어 | host 종료 기한의 생성·전달 | owner·pending provider 정리 적용 |
|---|---|---|
| C++ | `runtime/host/app.cpp:3741,3932` | `runtime/locations/location_runtime.hpp:227–245` |
| Java | `runtime/host/ZLinkFrameworkRuntime.java:1453,2094,2116` | `runtime/locations/ZLinkLocationRuntime.java:297–331`; 단계 대기 `runtime/host/ZLinkFrameworkShutdown.java:17–42` |
| .NET | `Runtime/Host/ZLinkFrameworkDrainExecutor.cs:98–104,295–304` | 같은 파일 `:571–577`에서 `Runtime/Locations/ZLinkLocationRuntime.cs:291–346`에 token 전달 |
| Node | `runtime/host/index.ts:1189–1203` | 같은 파일 `:1955–1959`에서 `runtime/locations/runtime.ts:368`에 signal 전달 |

네 언어 모두 host 종료 중에는 기존 host 기한을 사용한다. 직접 runtime 호출의 fallback은 기존 owner lease renew timeout이다. 새 timer·옵션·재시도·별도 deadline은 없다.

Java `StartupLifecycle`은 기존 `startupCompletion` 보관 필드 하나를 대체하고 기존 startup completion·local cancellation cleanup future 두 개를 같은 record에 둔다(`ZLinkLocationRuntime.java:41,272–276,998–1001`). 보관 필드 1→1, startup future 2→2다. `cleanupDeadline`은 host가 소유한 종료 시각 또는 직접 호출의 기존 renew 시각을 참조하며 새 시각을 판정하지 않는다(`:257–262,399–400`). cleanup 제출·소유 판정은 3→1로 줄었다.

C++ Store 성공 뒤 `_lane.run_checked(...).get()`은 provider callback을 기다리는 경로가 아니다. `location_runtime.hpp:818–819`의 private lane에서 실행하는 작업은 local 상태 읽기·쓰기이고 Store 작업은 lane 밖에서 수행한다. `state_lane.hpp:210`, `state_lane.cpp:88–100`은 idle lane을 호출자에서 바로 drain하며, `state_lane.hpp:249–254`의 완료 게시도 lane 밖이다. `01-execution/06-state-ownership-and-lanes.ko.md:211–223`의 완료 신호·블로킹 호환 조건에 맞으므로 provider 무응답 기한 판정을 이 local reset에 다시 복제하지 않았다. 일반 OS 스케줄링 지연에 대한 실시간 보장은 주장하지 않는다.

## 수정 전 재현과 수정 후 검증

| 발견·원인 | 수정 전 실패 | 수정 후 확인 |
|---|---|---|
| Java startup validator와 generated encoder의 경계 누락: `ZLinkFrameworkRegistration.java:233`, generated `ServiceWireCodec.java` | `r5-logs/java-bounds-before.log`: 경계 4/4 실패 | `r5-logs/java-bounds-after.log`: 12/12 통과; `java-bounds-conditional.log`: 유효 3·무효 4 구분 |
| Node host startup 취소·owner/pending cleanup: `host/index.ts:1132–1203,1955–1959`, `locations/runtime.ts:340,368` | `r5-logs/node-cleanup-public-before.log`: 1/1 실패; `node-cleanup-additional-before.log`: 3/3 실패; `node-cleanup-public-failure-before.log`: 2/2 실패 | `r5-logs/node-cleanup-focused-after.log`: 11/11 통과; `node-cleanup-related-fixed-after.log`: 관련 96/96 |
| .NET cancellation callback·owner cleanup: `ZLinkLocationRuntime.cs:121,187–190,291`; host `ZLinkFrameworkDrainExecutor.cs:300,571–577` | `r5-logs/cleanup-dotnet-cancellation-callback-red.log`: 2/2 실패 | `r5-logs/cleanup-dotnet-green-callback-final.log`: 집중 8/8 통과 |
| C++ pending Store 무기한 대기: `location_runtime.hpp:215–245`; host `app.cpp:3741,3932` | `r5-logs/cpp-cleanup-before.log`: 1/1 실패 | `r5-logs/cpp-cleanup-focused-after.log`: 22/22 통과 |
| Java startup cancellation/pending provider와 shutdown stage 독립 2초 성공 변환: `ZLinkLocationRuntime.java:254–331,687–780`, `ZLinkFrameworkShutdown.java:17–47` | `r5-logs/java-cleanup-before.log`: direct 경계 1/1 실패 | `r5-logs/java-cleanup-error-after-final.log`: 관련 5개 class 79/79 통과 |
| Java descriptor Store 종료 기한의 중복 판정: `ZLinkFrameworkRuntime.java:2118–2122`의 Location wrapper와 `ZLinkFrameworkShutdown.java:38–42`의 host stage | `r5-logs/java-cleanup-host-deadline-before.log`: host 기한이 지난 뒤에도 Store 작업이 제출되어 경계 1/2 실패 | `ZLinkFrameworkRuntime.java:2118`, `ZLinkFrameworkShutdown.java:38–56`에서 host stage 하나가 기한을 판정; `r5-logs/java-cleanup-host-deadline-after.log`: 기한 만료 전 제출·완료 및 만료 뒤 미제출을 포함한 집중 31/31 통과 |
| Java 기한 만료 뒤 비동기 resource close 미호출: `ZLinkFrameworkShutdown.java:38–47`, `ZLinkFrameworkRuntime.java:2122–2141` | `r5-logs/java-cleanup-resource-close-before.log`: 네 async resource close 호출 0/4, 경계 1/1 실패 | 기존 sync resource 경로를 `deferCloseStage`에 모으고 네 호출을 연결; `r5-logs/java-cleanup-resource-close-after.log`: expired resource 4/4 시작·Store 0/2 제출·host Timeout 및 정상 완료를 포함한 관련 36/36 통과 |
| Java direct 정상 stop의 늦은 raw claim이 최초 startup 기한을 재사용: `ZLinkLocationRuntime.java:293–319,399–400,733–738` | `r5-logs/java-cleanup-direct-stop-before.log`: 최초 claim timeout 뒤 renew로 startup 완료, 정상 stop에서 늦은 raw claim 정리가 TimeoutException, 1/1 실패 | 기존 lifecycle record의 stop 기한과 completion identity를 사용; `r5-logs/java-cleanup-direct-stop-after.log`: Recovery24·HostStartup3·Shutdown5·Authority callback2, 합계34/34 통과 |
| Java queue의 `Error` 뒤 terminal gate 미완료: `ZLinkSerialExecutionQueue.java:1153`; precommit abort fixture의 source 보존 기대 오류: `ZLinkStandaloneActorRelocationSourceBuilderTest.java:143–210` | `r5-logs/java-cleanup-error-before-final.log`: Error operation future 미완료 1/1 실패; 기존 전체 gate에서 Actor 종료 30초 만료 | `r5-logs/java-cleanup-error-after-final.log`: 동일 Error를 exceptional cause로 전달하고 다음 작업·queue.close 완료, abort 전 0회/pending·후 1회/완료 확인 |
| Java Redis Authority callback의 NIO lane 동기 대기: `ZLinkStatefulAuthorityRouteRuntime.java:99–108,481–483` | `r5-logs/java-cleanup-authority-before.log`: 공식 unit 1/1 실패; `redis-authority-harness/red-public.log`: 공개 Lettuce connection의 후속 EVAL이 509ms 뒤 500ms 기한 초과 | `r5-logs/java-cleanup-authority-after.log`: 1/1 통과; `java-cleanup-authority-class.log`: 3/3 통과; `redis-authority-harness/green-public.log`: 같은 조건에서 후속 EVAL 4ms 성공 |
| Java RawMesh actor·spot Authority provider 완료 callback의 inline 응답 제출: `ZLinkJavaRawMeshNode.java:5175–5184,6119–6129`; 실제 main Redis NIO stack `r5-logs/java-redis-info-1-main-watch-zone-node-2-90981-virtual-threads.txt:93–125` | `r5-logs/java-cleanup-provider-dispatch-before-final.log`: 기존 ingress에서 provider 완료 스레드와 callback 스레드가 같은 actor·spot 2/2 실패; 원본 INFO main 첫 샘플 descriptor EVAL timeout | `ZLinkJavaRawMeshNode.java:5175,6119`에서 기존 `applicationDispatch:171`로 연결; `r5-logs/java-cleanup-provider-dispatch-after.log`: 2/2 통과, 관련 binding 174/174, 최종 Java 전체 1891/1891 통과 |
| .NET 종료 deadline token 덮어쓰기: 수정 전 `ZLinkFrameworkDrainExecutor.cs:104` | `r5-logs/branch-unit-final.log`: `DrainCoordinatorTests.cs:741–765` 기대 `DeadlineExceeded`, 실제 `TeardownFailed` 1/2415 실패 | 덮어쓰기 삭제 후 `r5-logs/branch-drain-deadline-fixed.log`: 1/1, `branch-unit-fixed-full.log`: 2415/2415 통과 |

Redis 샘플의 정적 연결은 ZoneWorld `Program.java:86–87,144–149`의 한 Location Store에서 시작한다. `ZLinkFrameworkRuntime.java:294–304,352–358`, `ZLinkRegisteredLocationStores.java:24–25,48–49`, `ZLinkProviderLocationRepository.java:20–24`가 descriptor 제거와 Authority 조회에 같은 provider를 전달한다. `ZLinkRedisLocationStore.java:17–25`와 `ZLinkRedisLocationConnection.java:63–66,154–165`는 캐시된 Lettuce connection을 사용한다. 공개 Redis harness에서 Authority provider 완료 callback의 StateLane `join()`이 같은 connection의 뒤 EVAL을 509ms 막는 충분원인을 확인했고, 기존 `stateLane.runAsync()` 완료 연결로 고쳤다. 그 수정 뒤 stock branch 8회차에서 EVAL timeout이 발생했으므로 단독 해소 근거로 쓰지 않는다. 이어 원본 INFO main 첫 회차에서 같은 EVAL 실패와 Redis NIO callback의 `ZLinkJavaRawMeshNode.java:6207→6399` 동기 reply 제출 stack을 확보했다(`r5-logs/redis-info-main1-stack-boundary.json`). actor·spot 두 callback을 기존 application 실행 경로로 옮겼다. branch 8회차의 개별 EVAL 접수·encode 시각과 실제 native reply token은 기존 로그에 없어 정확한 인과는 추론하지 않는다.

Schema 자체 검사와 269개 나쁜 입력 self-test는 main·branch 모두 통과했다(`r5-logs/schema-valid-selftest-main.log`, `schema-valid-selftest.log`). Java codec 생성물 일치 검사도 통과했다(`r5-logs/java-codec-generator-check.log`). Runtime conformance fixture 검사는 양쪽에서 같은 테스트 식별자 누락으로 실패했다(`r5-logs/schema-runtime-fixtures-main.log`, `schema-runtime-fixtures.log`).

| 발견 | 소유 계층과 확정 스펙 | 분류 |
|---|---|---|
| Java startup 관계식·weight encoder | Framework 등록 validator·generated codec; Location §5:670–684, glossary:1567–1582 | B, 승인된 언어별 0→1 |
| 네 언어 startup 취소·pending owner/provider cleanup | Framework host lifecycle·LocationRuntime; Location §5:701,704–711 | B |
| Java queue `Error` terminal | Framework execution gate; `01-execution/02-handler-turn-and-execution-gate.ko.md:410–417,467–475` | B |
| Java precommit abort source 보존 fixture | Framework relocation source; `05-location-relocation/04-relocation-flow.ko.md:216–222,576,702` | A |
| Java Redis Authority NIO callback | Framework Authority runtime·state lane; Execution Gate:25–26,410–417,467–475 | B |
| Java RawMesh provider 완료 뒤 동기 응답 제출 | Framework application 실행 경계; Execution Gate:469–475 | B |
| Java direct stop의 늦은 claim cleanup 기한 | Framework LocationRuntime; Location §5:699–711 | B |
| Java host 기한 만료 뒤 비동기 resource close | Framework host lifecycle; Host relocation §14:749–758,788–802 및 Location §5:699–701 | B |
| .NET host deadline token 덮어쓰기 | Framework host drain; Location §5:701 | B |
| Java Ready 선행조건 fixture 다섯 곳 | Framework host startup·monitoring; `languages/java/interfaces/monitoring.ko.md:100–108,193–194,232–243`, `configuration-host.ko.md:729–730` | A |

우회 C나 미정 스펙 D를 구현하지 않았다. Core·binding이 소유하는 선택·재시도·오류 분류를 Framework에서 다시 판정하지 않았다.

Redis 진단은 원본 샘플에서 native Lettuce DEBUG를 켠 main·branch 유효 30쌍을 비교했다(`r5-logs/java-redis-read-repeat.log`, 2–32회 중 observer 설정 오류가 난 21회 제외). 양쪽 모두 B2/B3/B7와 종료가 통과하고 Redis 실패·스레드 포착은 0건이었다. DEBUG 배선 자체가 원본 로깅 조건을 바꾸므로 이 통과를 회귀 해소 근거로 사용하지 않는다. 최종 제품은 원본 INFO 조건에서 별도 30쌍으로 판정한다.

## main 대비 전체 gate와 간헐 경계

| 범위 | main `25e8a09cd1d1` | branch | 판정 |
|---|---|---|---|
| Node unit·contract | `r5-logs/node-main-full.log`: 1902건 중 3파일 7실패 | `r5-logs/node-branch-full.log`: 1928건 중 같은 3파일 7실패 | branch 추가 26건 통과, 동일 기준선 실패 |
| Node format | `r5-logs/node-main-format.log`: 통과 | `r5-logs/node-branch-format.log`: 통과 | 일치 |
| .NET unit·contract·format | 최초 `r5-logs/dotnet-main-unit.log`: 2405건 중 2실패; 올바른 local package 재검증 `dotnet-unit-main-correct-package.log`: 2405/2405, `dotnet-contract-main-provenance-valid.log`: 77/77; format 통과 | provenance 확인 frozen branch `r5-logs/dotnet-unit-branch-provenance-valid.log`: 2415/2415, `dotnet-contract-branch-provenance-valid.log`: 78/78; format 전체 통과·최종 수정 파일 CSharpier 통과 | 제품 소스가 일치하는 최종 unit·contract는 양쪽 모두 통과. 중간 WSL top-level 불일치 결과는 제외 |
| Java unit·contract·format | 최종 강제 실제 실행 `r5-logs/java-cleanup-resource-final-full-main.log`: 36/36 task 실행, core1738·contract25·Kotlin91·Server4, 합계1858/1858; `java-cleanup-resource-final-format-main.log`: 공식 format 통과 | 같은 명령 `r5-logs/java-cleanup-resource-final-full-branch.log`: 36/36 task 실행, core1773·contract25·Kotlin91·Server4, 합계1893/1893; `java-cleanup-resource-final-format-branch.log`: 공식 format 통과 | 최종 main·branch 전체 통과. 앞선 main FanoutNoDrop 1회 실패와 branch Ready fixture 5건은 각각 기준선 간헐·수정 완료로 별도 기록 |
| C++ unit·contract·format | WSL 복구 뒤 같은 profile의 full build 완료; `r5-logs/cpp-full-ctest-main.log`: 103/103 통과 | `r5-logs/cpp-full-ctest-branch.log`: 103/103 통과; 양쪽 format exit0 | 일치 |
| Java 실제 ZW-B2/B3/B7 각30회 | 최종 원본 INFO runner `r5-logs/java-redis-final-v2-info-30.tsv`: 29/30 통과, 28회차 기존 TimerSnapshot guard, Redis EVAL 실패 0 | 같은 명령·부하 조건: 30/30 통과, Redis EVAL 실패 0 | 최종 branch 신규 실패 0. main load1 평균 7.259(4.21–11.36), branch 7.471(4.92–12.54); 60회 core=installed 일치, branch Authority·source manifest 고정 |
| .NET 실제 TicTacToe | `r5-logs/redis-diagnosis-summary.md`: 원본 runner 통과 | 같은 runner 통과 | 양쪽 동일; 기존 Docker proxy socket을 `DOCKER_HOST`로 지정 |
| Node 실제 TicTacToe | `r5-logs/redis-diagnosis-summary.md`: LeaveGame 처리 TypeError 뒤 runner timeout | 같은 TypeError·timeout | 양쪽 동일 기준선 실패. flow에는 stack이 없어 정확한 최초 실패 줄은 미확정 |
| C++ 실제 ZoneWorld | `r5-logs/cpp-stock-main.log`: 원본 runner `PASS ZoneWorld.Cpp` | `r5-logs/cpp-stock-branch.log`: 같은 명령 `PASS ZoneWorld.Cpp` | 양쪽 통과, 포트 풀은 Java와 분리됨 |

Node 전체 실패 파일은 양쪽 모두 `node-test-gate.test.js`, `same-node-actor-join.test.js`, `stream-runtime.test.js`이다. Bingo.Ts package-mode source 대조, same-node Entry Join 다섯 건, actor packet spot mesh target의 같은 실패 집합이다. branch만의 새 Node 실패는 없다.

최종 Java branch 검증 source 20파일의 manifest는 `795247509913aed5dacd63d36019200cef386567589f375dd51037258853ff05`다(`r5-logs/java-cleanup-resource-final-source-sha256.txt`). Windows와 WSL의 Java source·script 2193개는 LF 내용 불일치·누락 0이며 기존 줄바꿈 차이 3개는 내용 비교에서 분리했다(`r5-logs/java-cleanup-resource-final-comparison.json`). 최종 실제 샘플 60회는 core JAR과 설치 JAR의 SHA가 매회 일치했고 branch Authority·source manifest가 고정됐다(`r5-logs/java-redis-final-v2-info-30-summary.json`).

Node 실제 TicTacToe의 양쪽 flow는 LeaveGameMsg 수신·admit·dispatch 뒤 같은 TypeError와 `leave-completed` 부재를 기록했다(`r5-logs/tictactoe-root2-node-main-logs/flow/flow-play.log:159–160`, 대응 branch 같은 줄). sample `play-actor-leave-game-handler.ts:27–28`의 `leaveActor`, Framework `spot-actor-membership.ts:196–198`의 leave 후 context 접근은 후보 호출 경로다. 기존 flow에 stack이 없으므로 최초 예외 줄을 확정하지 않는다. 이 sample 코드는 변경하지 않았다.

| .NET 간헐 경계 | main | branch | 로그·판정 |
|---|---:|---:|---|
| Owner claim conflict 확인 read 25ms | 1/30 실패, 평균 load1 34.393 | 3/30 실패, 평균 load1 34.283 | `r5-logs/dotnet-claim-repeat30.tsv`, 양쪽 `ReadCalls=0` |
| held target rollback force stop | 3/30 hang 중단, 평균 load1 45.380 | 2/30 hang 중단, 평균 load1 45.782 | `r5-logs/dotnet-force-repeat30.tsv`, 양쪽 testhost inactivity 30초 |
| main 전체 gate의 StreamSession terminal callback·Maintenance shutdown | 0/30, 평균 load1 13.693 | 0/30, 평균 load1 13.673 | `r5-logs/main-failures-repeat30.tsv`; 첫 main 실패는 `dotnet-main-unit.log:258,364`에 보존 |

단독 재실행 통과로 첫 실패를 지우지 않았다. .NET claim·held rollback은 양쪽에서 같은 실패 형태로 재현됐다. main 전체 gate의 별도 두 첫 실패는 교대 30회에서 양쪽 0/30이었으며 최초 main 관찰을 기준선으로 남긴다.

.NET Spot retire 테스트의 순수 한 줄 AB는 `9e165eae37` frozen branch 소스에서 `deadlineToken` 재대입을 유지한 사본 0/30 실패(평균 load1 11.786), Windows shipping과 같은 한 줄 삭제 사본 0/30 실패(11.714)였다. 지정 case TRX 60개가 각 1건 통과했다(`r5-logs/dotnet-spot-retire-ab-repeat30.tsv`, `dotnet-spot-retire-ab-provenance.json`).

중간 WSL top-level에서 Spot retire main 0/30·branch 23/30이 기록됐지만, `r5-logs/check-wsl-dotnet-source.log`에서 Windows branch 대비 .NET 소스·테스트 873개 중 67개가 달랐다. 특히 WSL `ZLinkFrameworkDrainExecutor.cs`에는 실제로 소비되는 `ShutdownCancellationToken` getter가 빠져 있었다. 이 결과와 top-level 전체 gate는 검증 자산 오류로 제품 판정에서 제외했다. Windows shipping과 일치하는 frozen branch `r5-final-branch`는 .NET 873개와 Node 1211개 모두 불일치 0(`r5-logs/check-wsl-dotnet-final-branch.log`, `check-wsl-node-final-branch.log`)이고, 순수 한 줄 AB도 양쪽 각30/30 통과했다. Java stock의 실제 frozen branch `r5-java-cleanup`은 Java 소스·build script 2371개 중 Windows와 불일치 0(`r5-logs/check-wsl-java-cleanup.log`)이다.

Java `ZLinkFanoutLocationRuntimeTest.monitorAndStopJoinTheAdmittedSubscriberReceive`는 main 1/30·branch 2/30에서 같은 `NOT_CONNECTED` 기대·`CONNECTING` 실제값으로 실패했다(`ZLinkFanoutLocationRuntimeTest.java:291`). 입력·assertion·관련 제품 소스가 같아 기존 monitor/discovery 수렴 간헐로 분리했다. Authority 완료 변경 뒤 setup 테스트 두 건은 main NodesAndServices 0/30·Placement 0/30, branch 각각 26/30·30/30으로 갈렸다. 반복 진입 load1 평균은 main 20.71·19.92, branch 19.34·19.29다. branch의 부하가 더 높지 않았다. `8219bd53de`에서 각각 5/5 통과했고 R5에서 Authority 제품 파일만 8219판으로 바꾼 진단도 각각 5/5 통과했다. Ready 전 상태를 읽는 fixture 선행조건을 이 branch에서 수정해 각30/30·전체 gate 통과를 확인했다.

수정 전 Java ZoneWorld 원본 runner의 owner cleanup 소요는 정상 main 29건 min 123·median 135·max 337ms, 정상 branch 28건 min 53·median 134·max 316ms였다(`r5-logs/redis-stock-final-zone1-owner-distribution.tsv`). branch 8회차만 약 29,955ms 뒤 descriptor EVAL timeout으로 종료됐다. main 8회차 STOPPING→owner cleanup은 44ms, owner cleanup→STOPPED는 142ms이고 branch 8회차는 각각 47ms·29,955ms다. 이 시간은 오류 표시까지의 전체 정리 시간이며 EVAL 명령 시작 시각을 뜻하지 않는다. 같은 회차의 +20초 thread dump에서 Lettuce NIO는 idle이었다. 원본 INFO의 추가 main 재현에서 Redis callback의 inline native reply stack을 확보했지만 branch 8회차의 개별 EVAL 시각·stack은 원본 로그에 없어 인과를 동일시하지 않는다. 최종 수정 뒤 원본 INFO 양쪽30회에서는 EVAL 실패가 0건이었다.

WSL 복구 뒤 정확한 수정 전 branch fixture를 같은 조건에서 다시 실행한 결과는 `r5-logs/java-cleanup-fixture-ready-before30-branch.log`: NodesAndServices 27/30, Placement 30/30 실패였다. 수정은 `NodesAndServicesTest.java:389`, `ZLinkRouteMeshPlacementConformanceTest.java:128`에 기존 startup 완료 단계 대기를 한 줄씩 추가했다. 기대값·fixture 입력·제품 Authority 비동기 완료는 변경하지 않았다. `r5-logs/java-cleanup-fixture-ready30-branch.log`에서 두 fixture 각30/30 통과했고 전체 gate도 1886/1886 통과했다.

두 fixture의 setup은 Framework host startup 완료가 선행조건이다. Java `ZLinkFrameworkRuntime.java:591–613,524`는 Authority 시작, SERVING descriptor 게시, `markServiceReady` 뒤 startup 완료를 알리고, `ZLinkFrameworkRuntimeTestAccess.java:24–25`가 그 완료 단계를 제공한다. Java 인터페이스 `languages/java/interfaces/monitoring.ko.md:100–108,193–194,232–243`과 `languages/java/interfaces/configuration-host.ko.md:729–730`은 SERVING/Ready 상태에서 placement를 판정한다. 다른 언어의 같은 테스트는 C++ placement conformance:383–385에서 host Ready, .NET `RuntimeConformanceFixtureTests.cs:721`에서 `StartAsync`, Node `route-mesh-placement-conformance.test.js:156–159`에서 application context와 `providers.ts:357–359`의 runtime start를 기다린다. Java 두 fixture에서 기존 startup 완료 단계만 기다리고 `NodesAndServicesTest.java:394–407`, `ZLinkRouteMeshPlacementConformanceTest.java:222–234`의 assertion은 유지한다. 변경 분류는 A(테스트 선행조건의 계약 적응)다.

RouteMesh RuntimeView의 추가 세 fixture도 같은 선행조건을 누락했다. `r5-logs/java-cleanup-runtime-view-before30-summary.json`의 동일 fixture·assertion 각 90회 비교에서 main 0/90 실패, branch 53/90 실패였고, 실패는 모두 예상 READY 또는 DEGRADED 대신 STARTING이었다. 세 setup에서 기존 startup completion만 기다린 뒤 `r5-logs/java-cleanup-runtime-view-after30-branch.log`에서 90/90 통과했다. 수정 전 load1 평균은 main 135.149·branch 144.192, 수정 후 branch 77.056으로 달랐으므로 속도 차이는 판정 근거로 쓰지 않는다. 기대값·제품의 Ready 판정은 유지했다.

## 판정 위치 전/후

전은 R5 시작 커밋 `8219bd53de`의 실제 판정 위치 수다. 독립된 결정마다 세었고 Java 두 경계의 0→1은 승인된 누락 보충이다.

| 결정 | 전 | 후 |
|---|---:|---:|
| Relocation 재확인·새 reference, 언어별 | 각 1 | 각 1 |
| startup 관계식 C++·.NET·Node / Java | 각 1 / 0 | 각 1 / 1 |
| weight encoder 상한 C++·.NET·Node / Java | 각 1 / 0 | 각 1 / 1 |
| Node host 종료 기한 소유 / pending owner cleanup | 3 / 2 | 1 / 1 |
| .NET 취소 callback·owner cleanup 기한 / 종료 deadline 분류 | 각 1 / 1 | 각 1 / 1 |
| C++ 정상 owner cleanup / startup 포함 cleanup | 2 / 3 | 1 / 2 |
| Java startup 취소 cleanup 시작 / lifecycle identity | 3 / 1 | 1 / 1 |
| Java shutdown 단계 대기 / pending provider 정리 | 각 1 | 각 1 |
| Java descriptor Store host 종료 기한 판정 | 2 | 1 |
| Java async resource close 제출 및 host 완료 기한 | 1(기존 sync close 경로), 1(공통 기한) | 1(기존 경로를 async까지 재사용), 1(공통 기한) |
| Java direct 정상 stop의 cleanup 기한 선택 | 1(최초 startup supplier 재사용) | 1(기존 stop deadline) |
| Java RawMesh actor·spot provider 완료 실행 경계 | 2(inline callback) | 2(기존 applicationDispatch) |
| Java queue terminal 오류 처리 / Authority reconcile lane 전환 | 각 1 | 각 1 |
| 위 결정의 판정 위치 합계 | 37 | 31 |

Java fixture 다섯 곳은 테스트 입력의 기존 startup 완료 단계를 관찰하도록 변경했다. 이는 제품 판정 위치가 아니다. 새 runtime 상태·플래그·timer·재시도·옵션은 추가하지 않았다. Node host 기한과 Java cleanup 시작 등 제품 판정 위치는 줄었다.

## 범위 밖과 supervisor-guide §2 대조

`ZLinkSerialExecutionQueue.java:460–461`의 handler 실패 수락 오분류는 #1295 PR #1328로 `origin/main`에 병합되어 이번 merge에 포함됐다. 남은 범위 밖 항목은 `ZLinkSpotTimerRegistry.java:688–690`의 TimerSnapshot, Node `locations/resolvers.ts:895–906`의 Entry route, .NET `ZLinkClientServerRuntimeService.cs:296–301`의 weight-zero 관측이다. 이번 수정 경로에서 드러난 branch 전용 결함은 같은 branch에서 수정한다.

| 기준 | 정적 판정 |
|---|---|
| 스펙 gap | 확정 Location §5·§10, glossary, execution gate, precommit abort 및 언어별 monitoring/configuration-host 조항이 변경 결정을 소유한다. 보호 스펙 변경 없음 |
| 불필요한 규칙 | 종료 stage의 독립 2초 성공 변환, Java descriptor의 두 번째 기한 wrapper와 중복 owner cleanup을 제거하고 기존 host 기한을 전달했다 |
| 제어 분산 | Node host 기한 소유 3→1, Java cleanup 시작 3→1·descriptor Store 기한 2→1, C++ 정상 owner cleanup 2→1; Java 경계 0→1 두 건은 승인된 누락 보충 |
| 동기화 | Java queue의 `Error` 뒤 gate 미완료를 기존 terminal 경로에서 수정했다. Authority NIO callback의 lane `join()`을 기존 async lane 완료 연결로, RawMesh provider callback의 inline application 처리를 기존 application 실행 경계로 옮겼다 |
| hot path | 정상 renew·trace-off에 새 lock·timer·trace 생성 없음. Node Promise race 임시 배열 제거; Java encoder는 기존 schema bound 비교 |
| 리팩토링 잔여 | 건드린 경로의 Redis NIO 대기와 .NET deadline token 덮어쓰기를 수정했다. 범위 밖 세 항목은 위에 기록 |
| 매직 값 | weight 상한은 schema 소유 값에서 생성한다. 새 의미 있는 숫자 literal·문자열 분기 없음 |
| 계층 소유 | Relocation repository·LocationRuntime·host lifecycle·execution gate는 Framework가 소유한다. Java의 Store 제출과 비동기 resource close는 host의 서로 다른 기존 종료 단계가 판정한다. Core/binding 결정을 재구현하지 않았다 |

리팩토링 점검: 성능 3건·POSDDD 13건·불필요 코드 5건 발견, Node 임시 배열·기한 분산·중복 abort·stop-race release, C++ 중복 owner cleanup, Java 중복 cleanup trigger·종료 stage 2초·descriptor 기한 중복·resource close 생략·direct stop 기한·Authority와 RawMesh의 NIO callback 실행, .NET deadline token 덮어쓰기를 고침; 넘긴 항목 `ZLinkSpotTimerRegistry.java:688–690`, `locations/resolvers.ts:895–906`, `ZLinkClientServerRuntimeService.cs:296–301`.

| 건드린 경로 | 성능 | POSDDD | 불필요 코드 | 근거 |
|---|---:|---:|---:|---|
| Node host·LocationRuntime | 1 | 4 | 2 | Promise race 배열 제거, host 기한·pending 소유 통합, 중복 abort·stop-race release 삭제; `r5-node-cleanup.md` |
| C++ LocationRuntime | 0 | 1 | 1 | `stop()`과 `cleanup_owner()`의 중복 owner Store 정리를 합침; `r5-logs/cpp-cleanup-summary.md` |
| Java LocationRuntime·host shutdown | 0 | 5 | 2 | cleanup 시작 분산·stage 독립 2초 성공 변환·descriptor 기한 중복·resource close 생략·direct stop 기한 재사용 제거; `r5-logs/java-cleanup-summary.md`, `java-cleanup-host-deadline-after.log`, `java-cleanup-resource-close-after.log`, `java-cleanup-direct-stop-after.log` |
| Java Authority callback | 1 | 1 | 0 | NIO의 lane 동기 대기 제거, 기존 async gate 재사용; `r5-logs/redis-diagnosis-summary.md` |
| Java RawMesh provider 완료 callback | 1 | 1 | 0 | NIO에서 inline application reply·staging 처리 제거, 기존 `applicationDispatch` 재사용; `java-cleanup-provider-dispatch-after.log` |
| .NET host deadline | 0 | 1 | 0 | 호출자 deadline token 덮어쓰기 제거, 기존 coordinator 기한 유지 |

같은 Java Authority·RawMesh 결함은 NIO 진행 비용과 gate 소유 위반 두 기준에 각각 한 번 기록했다. 성능 측정은 안정적인 load1<5 환경을 확보하지 못해 수행하지 않았다. 공개 Redis harness는 후속 EVAL 509ms timeout→4ms 완료를 보였고, RawMesh는 기존 application queue에 작업을 한 번 제출해 Redis NIO callback의 동기 reply 실행을 없앤다. 새 executor·lock·trace는 없으며 queue 제출 비용은 추가되므로 무비용이라고 주장하지 않는다.

## 검증 환경 중단 기록

최종 Java fixture 두 건의 Ready 선행조건을 추가한 직후 D: 공간이 0이 되어 WSL Ubuntu-24.04가 `Wsl/Service/CreateInstance/E_FAIL`로 중지됐다. 이 job의 중복 source archive를 제거하여 D: 여유를 확보했고 변경 source와 기존 로그는 보존했다. Ubuntu는 서비스 재시작 없이 정상 진입으로 회복됐다. 진단 사본의 중복 build만 realpath 확인 뒤 정리했고(`r5-logs/java-cleanup-diagnostic-build-prune.log`), WSL에서 runner 두 개의 `bash -n`을 통과했다. Java 수정 전/후 반복과 전체 gate, C++ main 동일 build/ctest를 이어서 완료했다. 이 환경 중단은 제품 테스트 실패로 해석하지 않는다.

WSL 복구 직후 .NET·Node TicTacToe 원본 runner는 `/var/run/docker.sock` 부재로 scoped Redis container를 만들지 못했다. 기존 Docker Desktop proxy socket을 `DOCKER_HOST`로 지정하고 동일한 root 접근을 양쪽에 적용하여 실제 sample을 비교했다(`r5-logs/redis-diagnosis-summary.md`). socket 권한·서비스·runner·fixture·제품 timeout은 바꾸지 않았다. 다음 Ubuntu 인스턴스 재시작으로 Java stock 5-branch가 Gradle 시작 후 중단됐고, 이 미완료 실행은 결과에 넣지 않고 own run ID의 exited Redis container만 정리한 뒤 재개했다(`r5-logs/java-stock-final-5-branch.interrupted.log`).
