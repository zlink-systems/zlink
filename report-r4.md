# R4 Location 계약 정렬 보고

작업 branch `framework/1083-a3-gap-loc`, 시작 커밋 `75e495defd`. 비교 기준 main은 작업 시작에 고정한 `2dc1ba0bfe`이다. 확정 스펙 PR #1313 head `37d879a5a2102edd7b0eea134995f32435cb02fa`를 읽기 전용으로 대조했다. 보호된 스펙·다른 worktree·공용 local package를 수정하지 않았다. 빌드·테스트는 이 job의 WSL 사본에서 실행했다. 로그 경로는 `r4-logs/`이다.

세부 재현·명령·로그 대조: [C++](r4-logs/r4-cpp.md), [Java](r4-logs/r4-java.md), [Node·.NET](r4-logs/r4-node-dotnet.md). 최종 unit·contract의 로그 요약과 Java XML의 tests/failures/errors를 감독자가 직접 확인했다.

변경 파일은 네 언어의 repository·LocationRuntime·기존 validator와 관련 테스트, C++ 내부 Session resolver, .NET contract scanner, schema·validator·fixture generator 및 생성물이다. 개별 파일 목록은 [changed-files.txt](r4-logs/changed-files.txt)에 기록했다. 보고서와 양방향 경계 테스트를 추가했다.

**전체 정렬 미완료.** Java startup·encoder 검사는 판정 위치 증가 예외 승인 대기다. Java pending provider 종료와 startup 취소 정리 기한도 미완료다. Java 실제 샘플 각30회 비교에서 Redis cleanup은 main0회·branch2회 실패했으며 미해결이다. C++·Node·.NET·Java 최종 unit·contract는 통과했다. C++ 최종 실제 샘플34건과 .NET TicTacToe도 통과했다. 기존 실패를 재실행 통과로 지우지 않았다.

## 결정 소유·교차언어 대조

표의 파일은 `framework/languages/<언어>/` 아래다. Java Kotlin wrapper는 Java 계약을 사용하며 별도 repository·lease runtime을 구현하지 않는다.

| 결정·소유 계층 | 확정 조항 | C++ | Java | .NET | Node |
|---|---|---|---|---|---|
| Relocation 재확인, Framework repository | Location §10:1334–1337 | provider_relocation_repository.hpp:58–109 | ZLinkProviderRelocationRepository.java:38–108 | ZLinkProviderRelocationRepository.cs:61–113 | relocation-blob.ts:23–48 |
| 갱신 시작 간격·진행 중 대기, LocationRuntime | §5:684 | location_runtime.hpp:737–805 | ZLinkLocationRuntime.java:332,715,796 | ZLinkLocationRuntime.cs:764,792 | runtime/locations/runtime.ts:1160,1172 |
| startup 관계식, 등록 validator | §5:674 | framework_options_validation.hpp:36 | ZLinkLocationOptions.java:32,41,64,72 양수 검사만 있음 | ZLinkLocationRegistrationValidator.cs:43 | RegistrationValidators.ts:138 |
| StoreNow 진단, LocationRuntime 관찰 | §5:687–690 | location_runtime.hpp:411 | ZLinkLocationRuntime.java:840 | ZLinkLocationRuntime.cs:684 | runtime.ts:484 |
| weight 설정 상한 | glossary:1567,1582 | service_descriptor_registry.cpp:151 | MeshNodeRegistration.java:385,933 | ZLinkSocketConfigs.cs:14,84 | RegistrationBuilderPolicy.ts:18 |
| 비동기 Session route 조회, Framework resolver | 기존 Location 조회·Session seal 계약 | app.cpp:1463,1485; mesh_node_runtime.cpp:1136,1218 | ZLinkActorRuntime.java:3069,3154 | ZLinkActorRemoteJoiner.cs:662,1575 | actor-transfer-runtime.ts:979 |

최초 claim 완료 뒤 interval은 네 언어의 기존 동작을 유지했다. 갱신 시작을 최초 claim 시작으로 확장한 앞선 테스트 판정은 철회했으며 `java-schedule-before.log`를 결함 증거에서 제외했다.

소유 계층: Framework repository·LocationRuntime·등록 validator·schema 생성기.
스펙 조항: 확정 Location §5:674,684,687–690 및 §10:1334–1337; weight glossary0..10000.
교차언어 대조 결과: missing·충돌·갱신 시작·StoreNow 정렬, Java startup·encoder 미완료.
변경 분류: 구현·schema B, public 계약 fixture·진단 assertion A, cleanup 기한 정책 D.

## 발견별 원인·수정·재현

| 원인 위치(수정 전) | 수정 위치·내용 | 수정 전 실패 로그 | 수정 후 로그·요약 |
|---|---|---|---|
| Node relocation-blob.ts:25–48 상한4·missing 실패 | :23–48 상한 삭제·same reference 재저장·typed Conflict 공통 경로 | node-before.log:4실패/5 | node-after.log:5/5 |
| Java repository:37–83 missing·conflict 실패 | :38–108 재저장·새 reference, retry만 async continuation | java-before.log:3실패/5 | java-initial-completion-after.log focused 통과 |
| .NET repository:64–116 missing·conflict 실패 | :61–113 기존 PutAtCoreAsync로 통합 | dotnet-before.log:3실패/5 | dotnet-after.log:5/5 |
| C++ repository:100–104 오류kind별 missing 예외 | :58–109 예외 삭제·기존 loop | cpp-before-relocation.log 실패 | cpp-final-relocation.log:39/39 |
| pending provider가 caller 종료를 막음 | C++ :65,92 result_for 및 :69 기존 payload observer 이동; Node :28,31 awaitWithAbort; .NET Put WaitAsync(token) | cpp-before-deadline.log:2실패; node-pending-before.log:2cancelled; dotnet-pending-before.log timeout | C++ 각200ms 통과; node-abort-after.log:13/13; dotnet-pending-after.log:9/9 |
| Node abort.ts 이미 취소된 분기가 provider rejection 관찰 생략 | 기존 observer 등록 뒤 기존 취소 경계 사용 | node-pending-before.log unhandled responseLost | node-abort-after.log:13/13 |
| Node 즉시 Conflict/Missing의 microtask 반복이 deadline timer를 굶김 | relocation-blob.ts:24의 for 갱신부에서 기존 macrotaskBoundary 재사용; serial-execution-queue.ts의 helper를 abort.ts:41로 이동 | node-conflict-deadline-branch.log:20ms deadline이 2초 뒤에도 미종료; node-timer-deadline-before.log:두 shipping 경계실패 | node-timer-focused-final.log:16/16, node-timer-execution-focused.log:104/104; node-unit-contract-timer-final.log:1893/1893 |
| C++ location_runtime.hpp:748–749 즉시 재시작·pending polling | :737–805 시작+interval·completion wake | cpp-before-cadence.log, cpp-before-pending.log 실패 | cpp-final-runtime.log:17/17 |
| Java runtime:688–693,761 fixed delay·중복 수락 | :332,715,796 기존 heartbeatTask 예약, AtomicBoolean 삭제 | java-time-before.log 및 focused | java-recovery-lifecycle-after.log 통과 |
| .NET :801 / Node :1172 예정시각 누적 | .NET :792 / Node :1172 실제 시작+interval | dotnet-cadence-before.log 기대50ms/실제100ms | dotnet-cadence-after.log:1/1, node-after.log |
| Java :791–792 StoreNow+1ns | :840 원래 StoreNow, resolver assertion 원래 입력 exact equality(A) | java-time-before.log:1실패 | java-recovery-lifecycle-after.log 통과 |
| Java :357–397 이전 renew가 새 owner 덮음 | :357 token 승인, :435 lifecycle identity 통합 | java-lifecycle-before.log 실패 | java-recovery-lifecycle-after.log 통과 |
| Java heartbeat 두 lane 사이 lifecycle 재조회 | :332 snapshot·due 함께 capture, 후속 hop 삭제 | java-heartbeat-hop-before.log token1 기대/2 실제 | java-heartbeat-hop-after.log 통과 |
| Java 늦은 recovery 실패가 새 admission 닫음 | :483 설치 lane에 previous owner, :573,592 기존 lifecycle 승인 | java-recovery-lifecycle-before.log true 기대/false 실제 | java-recovery-lifecycle-after.log 통과 |
| C++ app.cpp:1486 Store.result() worker self-wait | :1485 co_await, 기존 resolver interface·두 caller async | cpp-branch-zoneworld.log, cpp-b8-trace-branch.log, cpp-b8-gdb-node-68478.log:333–355 | cpp-b8-fix-focused.log:1/1; 최종 cpp-validator-final-unit-contract.log:101/101; cpp-validator-stock-branch.log:34/34 PASS |
| .NET ContractSurfaceCoverage.cs:1656–1679 const default를 타입으로 치환 | 기존 Roslyn numeric canonicalization | dotnet-default-before.log, branch contract76/77 | dotnet-contract-final.log:78/78; const100 동치·const101 거부 |
| C++·.NET startup max+timeout 합산 overflow | 기존 validator의 max>=fencedLifetime 또는 timeout>=fencedLifetime-max 단락 비교; 같은 판정 위치·예외 | cpp-validator-boundary-branch.log UBSan overflow·잘못허용; cpp-validator-before-shipping.log 실패; dotnet-startup-overflow-before.log 예상ConfigurationException/실제OverflowException | C++ 수정후UBSan없음·거부; .NET dotnet-startup-overflow-after.log:60/60 |
| C++ host lifecycle fixture timeout1s가 strict 관계식 위반 | test_cpp_framework_host_lifecycle.cpp:886 timeout500ms(A), assertion 유지 | cpp-unit-contract.log:100/101 | cpp-final-unit-contract.log:101/101 |
| C++ provider fixture std::async 준비 경쟁 | public renew 반환→pending 확인→provider 완료 | cpp-main-unit-contract.log:102/103 | 최종101/101, 양쪽격리각30회 통과 |
| Node location-host 필수 drain mock 누락 | location-host.test.js:995 drain()=0(A), 기존 recv=null·assertion 유지 | 초기 whole gate mock 오류 | node-location-host-final.log:17/17 |

C++ self-wait는 Redis worker가 Location 완료 continuation을 실행한 뒤 같은 worker로 다시 제출한 조회를 기다린 것이다. 실제 stack으로 원인을 확정했다. 별도 pool 재스케줄 대안은 실행 정책을 추가하므로 채택하지 않았다. m6c 테스트는 resolver 첫 답을 pending으로 유지하여 public relocation 반환 후 완료하며 기존10ms sleep을 삭제했다. 구형 optional interface의 compile 실패를 기능 재현으로 주장하지 않는다.

Node 재시도 fairness는 기존 helper 한 구현을 이동하여 재사용했다. 별도 timer·cap·deadline·상태를 추가하지 않았다. 처음 제출 및 성공/오류 반환은 기존 시각을 유지하고, 모든 재시도만 공통 갱신부에서 양보한다. execution queue module을 repository에 직접 import하는 대안은 ASL map·structural wrapping 초기화 비용이 있어 채택하지 않았다. 공통 abort module은 import·초기화 부작용이 없다. 다른 언어는 C++의 직접 monotonic deadline 검사(:41–46,59–60), .NET의 독립 timer/token(:67–77), Java의 retry async executor(:74)로 같은 기한을 관찰한다.

Duration 경계는 timeout=표준최대값/2+1, interval=최소양수, 기본TTL/margin으로 재현했다. 원본 main은 C++·.NET 모두 정상 거부했다(cpp-validator-boundary-main.log, dotnet-startup-overflow-main.log). Node safe number도 거부했다(node-startup-number-boundary.log). checked addition helper 대안은 새 helper·overflow 예외 정책을 추가하므로 채택하지 않았다. 양수검사 뒤 TTL-margin은 표현 범위 안이며, max<fencedLifetime일 때만 차감하여 overflow 없는 동치 관계식을 판정한다. 사용자 budget·timeout을 늘린 것이 아니라 부적합 입력의 거부 검사다. Java 관계 판정은 기존 승인 대기로 유지한다.

## Schema·생성물

`service-wire-v1.schema.json:105,1029` weightMax를10000, startupRelation을 max 관계식으로 정렬했다. `validate-service-wire-schema.mjs:1636,1743`, `validate-runtime-conformance-fixtures.mjs:3488,3491`의 기존 판정도 정렬했다. `generate-service-wire-fixtures.mjs:1527`의 오류값101은 정상 범위가 되어 schema field bound+1을 생성하도록 고쳤다. generator로 재생성했다.

| 검사 | main·수정 전 | branch |
|---|---|---|
| 양방향 경계5건 | schema-before.log:4실패; schema-fixture-before.log:1실패 | schema-after.log:5/5 |
| schema self-test | main-schema-self-test.log:267 negative | schema-self-test.log:269 negative |
| generator drift | main-schema-generation-check.log 통과 | schema-generation-check.log:23 files 통과 |
| 전체 conformance | main-conformance.log 같은 실패 | conformance.log:3276 stream 테스트 identifier 누락 |
| 전체 decoder fixtures | main-schema-decoder.log 같은 실패 | schema-decoder.log:237,322 barrier invalid-field |

기본값·weight10000은 통과, interval1s/timeout8s·old weightMax100·weight10001은 실제 거부했다. 전체 conformance·decoder의 첫 실패는 lease/weight 수정 범위 밖이며 전체 통과로 보고하지 않는다.

## main 대비 전체·샘플·format

| 범위 | main 2dc1ba0bfe | branch |
|---|---|---|
| C++ unit·contract | cpp-main-unit-contract.log:102/103, fixture 준비 경쟁 | cpp-validator-final-unit-contract.log:101/101, 실패0, 104.92초, exit0 |
| C++ 실제 ZoneWorld | cpp-stock-final-main.log:34/34 PASS | cpp-validator-stock-branch.log:34/34 PASS, 최초 B8 정지 수정 |
| Java unit | java-main-full.log:총1686·2실패 | java-branch-gate-recovery-final.log:1700/1700, 기존 승인4건·R4 신규10건 증가 |
| Java contract·Kotlin·ZoneWorld unit | 25/25·91/91·4/4 | 동일 전체 gate25/25·91/91·4/4 |
| Java 실제 ZW-B2/B3/B7 각30회 | java-sample-repeat30.tsv:29회통과·timer1회실패 | 같은 TSV:26회통과·timer2회실패·Redis cleanup2회실패 |
| Node unit·contract | node-unit-contract-main.log:1875완료, idle worker 실패 | node-unit-contract-timer-final.log:1893/1893, exit0 |
| .NET unit 동일 bounded runner | dotnet-unit-main-bounded-final.log:2385/2385 | dotnet-unit-startup-final.log:2390/2390, 실패0, 3분36초 |
| .NET contract | dotnet-contract-main.log:77/77 | dotnet-contract-startup-final.log:78/78 |
| .NET TicTacToe | dotnet-sample-main.log placement completed | dotnet-sample-startup-final.log placement completed |
| Node TicTacToe | node-sample-main.log Ready authority fence 실패 | node-sample-branch.log LeaveGameMsg dispatch_error |
| format --check 언어별 | 네 언어 main 통과 | cpp-validator-final-format.log·node-timer-format-final.log·dotnet-format-startup-final.log·java-recovery-final-format.log 모두 exit0 |

.NET bounded runner의 기존 --blame-hang30s는 제품 timeout을 바꾸지 않는다. 이전 전체 hang은 target cutover test:782 대기로 확인했고 main/branch 격리각30회 통과했다. weight-zero request:true는 main30/30실패·branch23/30실패이며 Location을 등록하지 않는 fixture(:3044–3061)다.

Node idle worker·C++ provider 준비 경쟁·.NET drain/cutover·Java dispatcher의 격리각30회 통과는 최초 전체 실패를 무효화하지 않는다. 기록한 부하만 대조하며 순간 부하 동치를 주장하지 않는다.

원본 main의 Java Fanout timeout은 ZLinkFanoutNoDropTest.java:101에 기록됐다. java-fanout-repeat30.tsv 및 java-fanout-repeat30-summary.json의 동일 testcase 대조는 main/branch 각30회 모두 통과했으며, 최초 전체 실패는 미해결 관찰로 보존한다. 부하시작 평균은 main7.728·branch7.774, 종료평균은 main7.774·branch7.800이다. 임시 신규 testcase를 이식한 focused 실행은 전체 baseline으로 사용하지 않는다.

## 미완료·범위 밖 발견

| 항목·file:line | 분류·판정 |
|---|---|
| Java ZLinkLocationOptions.java:32,41,64,72, startup 관계 판정 없음 | B, 0→1 예외 승인 요청 중, 미수정 |
| render-service-wire-java.mjs:367–395 → generated/jvm/ServiceWireCodec.java:1399–1402,1520 | B encoder upper bound 누락. main/branch10001 허용(java-weight-encoder-{main,branch}.log); 0→1 예외 승인 요청 중 |
| Java repository:52,96; ZLinkStoreCancellation.java:3–4 | B pending 종료 제약. put SPI에 deadline 없고 cancellation은 boolean 조회만 가능. 새 timer/polling/API 금지 아래 미수정 |
| startup 취소 정리 기한: C++:194–197,647–682; .NET:113,165–173,603–610,728; Node:417,469–474,544,564; Java cleanup | D. §5:701 별도 기한 허용, 시작·예산 미정. Java main6/30·branch5/30실패, 해당 항목만 중단 |
| ZLinkSerialExecutionQueue.java:460–461 | 범위 밖 B. handler 실패 future를 수락 여부로 읽어 BACKPRESSURED 오분류. 최초 실패 유지, 양쪽격리각30회 통과 |
| ZLinkSpotTimerRegistry.java:688–690 | 범위 밖 기존 B. 실제 sample main1/30·branch2/30 같은 TimerSnapshot 종료 오류 |
| Program.java:96; FrameworkRuntime.java:2087; ObjectServerDescriptorPublisher.java:103–119 | 미해결 발견. branch Redis EVAL500ms cleanup2/30실패, main0/30. timer 오류와 구분, 회귀 여부 배제 불가 |
| Node location-store-resolvers.ts:895–906 → actor-remote-joiner.ts:134–152 → native join:172–179 | 범위 밖 B. Entry route의 Spot generation 누락, public ActorJoin main/branch 동일 repro(node-entry-repro-main.log, node-entry-repro-branch.log); 변경 함수 밖 미수정 |
| .NET ZLinkClientServerRuntimeService.cs:296–301; test:1103 | 범위 밖 기존 weight-zero 관측 누락. main30/30·branch23/30 같은 실패 |

Java encoder 같은 경계는 C++ generated codec:6258–6263, .NET:3452–3458, Node:4274–4278이 이미 검사한다. 공개 설정 validator의10000 검사를 encoder 완료 근거로 쓰지 않는다. 예외 승인이 필요한 근거는 사용자 지시의 “판정 위치가 늘면 반려”이며 응답 전 미구현을 유지한다.

## 판정 위치 전/후

| 결정 | 전 | 후 |
|---|---:|---:|
| Relocation 정책·reference 생성, 언어별 | 각1 | 각1 |
| Node 횟수 제한 / C++ missing 오류kind 예외 | 1 / 1 | 0 / 0 |
| startup 관계식 C++·.NET·Node / Java | 각1 / 0 | 각1 / 0 |
| scheduling C++·.NET·Node | 각1 | 각1 |
| Java heartbeat 수락 / lifecycle identity | 2 / 2 | 1 / 1 |
| Java snapshot / previous owner 쓰기 | 1 / 1 | 1 / 1 |
| Java next renewal due 쓰기 | 3 | 3 |
| Java renew·republish 결과 적용 | 각1 | 각1 |
| C++ deadline·취소 소유 | 1 | 1 |
| C++ resolver / blocking Store 대기 | 1 / 1 | 1 / 0 |
| Node abort 판정 | 2 | 1 |
| Node fairness helper 소유 / 재시도 취소 판정 | 1 / 1 | 1 / 1 |
| .NET default canonicalization | 1 | 1 |
| Java encoder bounds | 0 | 0 |
| Schema 관계식·weight·invalid fixture 소유 | 각1 | 각1 |

Java 결과 승인 predicate 자체는0→1이며 기존 결과 적용 판정 위치는1→1이다. 독립된 startup·encoder 검사0→1과 구분한다. 새 runtime 상태·queue·lock·timer·retry option은 없고 heartbeatInFlight·Node 상한·중복 오류 보존·unused helper를 삭제했다.

## supervisor-guide §2 여덟 기준

| 기준 | 판정 |
|---|---|
| 스펙 gap | B 계약 정렬, cleanup 기한 D 중단; 보호 문서 수정 없음 |
| 불필요한 규칙 | 상한·오류kind 예외·중복 수락 삭제, 새 policy 상태 없음 |
| 제어 분산 | 기존 repository·runtime·schema 소유 유지, 판정 위치 대조 |
| 동기화 | C++ worker self-wait 제거, Java lane 안 Store·다른 lane 대기 없음 |
| hot path | 메시지별 trace·할당·lock 추가 없음. polling/self-wait 제거는 정적 근거, 처리량 수치 주장 없음 |
| 리팩토링 잔여 | 승인 대기 B·pending 제약·cleanup D·범위 밖 발견 명시, 발견0 판정 불가 |
| 매직 값 | Node 상한 삭제, invalid weight schema bound+1, 기존 옵션·상수 재사용 |
| 계층 소유 | Framework Store·lease·resolver 소유, Core/binding 결정 보상 없음 |

리팩토링 점검: 성능2건·POSDDD12건·불필요 코드5건 발견, C++ polling·worker self-wait·deadline waiter, Java lifecycle/snapshot/recovery 소유, Node conflict/abort/fairness 소유 및 상한·unused helper·timestamp 가공·생성 fixture의 고정 오류값·C++/.NET duration overflow를 정리했다. 넘긴 항목: Java pending provider(repository:52,96), cleanup 기한 D(§5:701), dispatcher(:460–461), TimerSnapshot(:688–690), descriptor cleanup(FrameworkRuntime:2087), Node Entry route(:895–906), .NET 관측(:296–301). Java 두 검사 누락은 승인 대기다.
