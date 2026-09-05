# D-095 — .NET 테스트 fixture 단조 시간원 수정 결과

테스트 유지보수자가 UTC 사용처의 변경 여부와 검증 결과를 확인하는 기록이다.
`framework/languages/dotnet/tests/**`의 반복 대기 31곳과 retention replay 대기 1곳을
`Stopwatch.GetTimestamp()`/`GetElapsedTime()`으로 계산한다. 기존 timeout, polling 간격,
retry 조건과 assertion은 유지한다. Runtime·Core·binding·다른 언어 코드는 수정하지 않는다.

- 소유 계층: 테스트 fixture의 대기 한도 측정. Transport/retry 정책의 소유권 변경 없음.
- 근거: [D-095](decisions.ko.md)의 경과 시간원 통일과 이번 fixture 감사·수정 지시.
- 교차언어 대조: 이번 범위는 .NET fixture뿐이며 runtime 동작 변경 없음. 기존 [runtime 감사](fix-dotnet-monotonic-durations-summary.md)의 Java nanoTime 대조 결과를 참조하며 다른 언어 코드는 조사·변경하지 않았다.
- 변경 분류: **B — fixture의 기존 시간원 결함**. 사용자 fix 지시 범위에서 구현했다.
- 수정 전/후 규칙 수: fixture 경과 시간 판정의 wall/monotonic 2개 → monotonic 1개. 새 helper·clock abstraction·보정 상태 없음.

대안은 모든 helper에 TimeProvider를 주입하는 방법과 기존 Stopwatch 패턴을 사용하는 방법이다.
테스트에 시간 주입 인터페이스를 추가할 필요가 없어 후자를 선택했다.
`3810baab93`에서 이미 바뀐 Canonical join `WaitUntilAsync`와 ClientServer
`WaitUntilAsync`는 그대로 유지한다. Canonical join `ReceiveAsync`도 2초 그대로다.
이 변경만으로 과거의 단발 timeout 원인이 UTC 점프였다고 확정하지는 않는다.

## 사용처 감사

수정 전 `ff9d6eadbc` 기준 UTC 호출 **192개 / 49파일**을 조사했다.
아래 경로 기준은 `framework/languages/dotnet/tests/`이며 행 번호는 **수정 전**이다.
동일 행의 두 호출은 `×2`로 표시한다. 대기 한도 생성·비교·remaining에 쓰던 64개 호출을
제거하고, timestamp 127개와 절대 시각의 최초 상대 변환 1개를 유지한다.
`DateTime.Now`/`DateTimeOffset.Now`는 없었다. `GetUtcNow()`는 fake clock의 wire deadline·
관측 값·store 데이터로 분류했다. 기존 Stopwatch/TimeProvider, 상대 Task.Delay/WaitAsync/CTS,
WorkerPoolTests의 monotonic `Environment.TickCount64` 대기는 UTC 차감이 없어 유지한다.

### 변경한 대기 — 32곳 / 18파일

각 대기는 시작 timestamp 하나로 제한 시간을 비교한다. Poller remaining도 같은 시작값을 쓴다.

| file:line | 대기 owner | 유지한 한도 |
|---|---|---|
| `Systems.Zlink.Stream.Connector.Tests/Support/StreamConnectorTestSupport.cs:91,95` | `DispatchUntilAsync` | timeout |
| `Systems.Zlink.Stream.Connector.Tests/Support/StreamConnectorTestSupport.cs:105,108` | `WaitUntilAsync` | timeout |
| `Zlink.Framework.UnitTests/Runtime/ActorHandlerActivationTests.cs:435,438` | `WaitUntilAsync` | 5초 |
| `Zlink.Framework.UnitTests/Runtime/BackendAdapterFactoryTests.cs:189,193,195` | `PublicPoller_SubPollIn_AndDealerCompletion_ProgressTogether` | 2초 |
| `Zlink.Framework.UnitTests/Runtime/BackendAdapterFactoryTests.cs:317,318` | `ReceiveAsync` | timeout |
| `Zlink.Framework.UnitTests/Runtime/BoundSessionReplacementLifecycleTests.cs:420,423` | `WaitUntilAsync` | 2초 |
| `Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinIngressReplyTests.cs:1038,1039` | `ReceiveActorJoinAsync` | timeout ?? 2초 |
| `Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinIngressReplyTests.cs:1074,1075` | `ReceiveCompletionAsync` | 2초 |
| `Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinIngressReplyTests.cs:1118,1119` | `ReceiveAsync` | 2초 |
| `Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinIngressReplyTests.cs:1134,1135` | `ReceiveMonitorEventAsync` | 2초 |
| `Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinIngressReplyTests.cs:1149,1166` | `SendHelloAsync` | 2초 |
| `Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinWireAdmissionNegativeTests.cs:452,469` | `SendHelloAsync` | 2초 |
| `Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinWireAdmissionNegativeTests.cs:478,479` | `ReceiveAsync` | 2초 |
| `Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinWireAdmissionNegativeTests.cs:492,495` | `WaitUntilAsync` | 2초 |
| `Zlink.Framework.UnitTests/Runtime/ChannelOutboundTerminalTests.cs:211,227` | `MalformedClientServerEnvelopeRecordsInvalidFrameDispatchError` | 5초 |
| `Zlink.Framework.UnitTests/Runtime/ChannelOutboundTerminalTests.cs:412,415` | `WaitUntilAsync` | timeout |
| `Zlink.Framework.UnitTests/Runtime/ClientServerChannelRuntimeTests.cs:883,884` | `AutomaticClient_SelectsAcrossPositiveWeightReadyServers` | 5초 |
| `Zlink.Framework.UnitTests/Runtime/ClientServerChannelRuntimeTests.cs:1683,1684` | `PollReceivedAsync` | timeout |
| `Zlink.Framework.UnitTests/Runtime/EntrySpotActorDispatchTests.cs:11141,11142` | `WaitAsync (sent)` | timeout |
| `Zlink.Framework.UnitTests/Runtime/EntrySpotActorDispatchTests.cs:11156,11157` | `WaitAsync (outcome)` | timeout |
| `Zlink.Framework.UnitTests/Runtime/EntrySpotActorExactRequestMessageFlowTests.cs:140,141` | `Flowless_Actor_Stream_Ingress_Creates_One_Inbound_Flow_For_Join_And_Reply` | 5초 |
| `Zlink.Framework.UnitTests/Runtime/MeshNodeShutdownSealTests.cs:376,385` | `SendAsync` | 2초 |
| `Zlink.Framework.UnitTests/Runtime/MeshNodeShutdownSealTests.cs:395,396` | `ReceiveAdmitAsync` | 2초 |
| `Zlink.Framework.UnitTests/Runtime/MeshNodeShutdownSealTests.cs:436,439` | `WaitUntilAsync` | timeout ?? 5초 |
| `Zlink.Framework.UnitTests/Runtime/RelocationBehaviorConformanceTests.cs:1350,1351` | `WaitUntilAsync` | 15초 |
| `Zlink.Framework.UnitTests/Runtime/RouteCodecTests.cs:236,238` | `ReceiveAsync` | timeout |
| `Zlink.Framework.UnitTests/Runtime/RouteCodecTests.cs:284,286` | `SendUntilReceivedAsync` | timeout |
| `Zlink.Framework.UnitTests/Runtime/RouteMeshRuntimeServiceTests.cs:612,613` | `WaitForStatusAsync` | timeout ?? 30초 |
| `Zlink.Framework.UnitTests/Runtime/StreamSessionForcedCleanupTests.cs:1386,1389` | `WaitUntilAsync` | 2초 |
| `Zlink.Framework.UnitTests/Runtime/TimerLifecycleTests.cs:571,574` | `WaitUntilAsync` | 5초 |
| `Zlink.Framework.UnitTests/Runtime/UnhandledDispatchPolicyTests.cs:650,651` | `ReceiveAsync` | timeout |
| `Zlink.Framework.UnitTests/Runtime/StatefulServiceRuntimeTests.cs:4023` | original deadline 뒤 replay 대기 | 기존 wire 500ms + 25ms, Unix ms 절삭 유지 |

`StatefulServiceRuntimeTests`는 wire deadline 생성 시의 UTC와 Stopwatch 시작점을 함께 기록한다.
기존 wire Unix ms 값으로부터 `afterDeadline - deadlineNow`를 한 번 확정하고,
대기 직전에는 Stopwatch 경과 시간만 차감한다. 500ms deadline·25ms offset과 ms 절삭은 유지한다.

### 유지한 UTC — 128개

| file:line | 유지 근거 |
|---|---|
| `Common/FrameworkTestEnvironment.cs:103` | 임시 실행 디렉터리 이름에 기록하는 시각. |
| `Systems.Zlink.Stream.Connector.Tests/Support/StreamConnectorTestSupport.cs:132,133` | 인증서 NotBefore/NotAfter 데이터. |
| `Zlink.Framework.ContractTests/Configuration/FrameworkRuntimeContracts.cs:127` | runtime status 보고 시각. |
| `Zlink.Framework.ContractTests/Configuration/RouteMeshRuntimeContracts.cs:91` | RouteMesh status 보고 시각. |
| `Zlink.Framework.ContractTests/Spots/InstanceSpotContracts.cs:58` | closing context에 전달하는 절대 deadline. |
| `Zlink.Framework.ContractTests/Spots/SpotContracts.cs:84,357,358` | closing context deadline과 timer tick 보고 timestamp. |
| `Zlink.Framework.Locations.Redis.Tests/ActorCreateCommandRuntimeTests.cs:27,101` | descriptor timestamp와 wire deadline. |
| `Zlink.Framework.Locations.Redis.Tests/ActorManagerProductionTests.cs:632` | store conflict 결과의 StoreNow. |
| `Zlink.Framework.Locations.Redis.Tests/RedisLocationStoreLifecycleTests.cs:519,584,592` | Redis script 응답의 store timestamp. |
| `Zlink.Framework.Locations.Redis.Tests/RedisOpaqueProviderTests.cs:338` | 경계 변환: Redis cleanupDue를 상대 Task.Delay로 한 번 변환. 아래 설명 참조. |
| `Zlink.Framework.UnitTests/Runtime/ActorRelocationProtocolTests.cs:439,747,761,891` | relocation context deadline, authority snapshot/결과의 StoreNow. |
| `Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinIngressReplyTests.cs:199,810` | admission/request wire deadline. |
| `Zlink.Framework.UnitTests/Runtime/CapacityMonitoringProjectionTests.cs:101` | 관측 event timestamp. |
| `Zlink.Framework.UnitTests/Runtime/DeferredActorJoinDurabilityTests.cs:562,600` | authority snapshot과 StoreNow. |
| `Zlink.Framework.UnitTests/Runtime/DrainCoordinatorTests.cs:1574` | runtime status 보고 시각. |
| `Zlink.Framework.UnitTests/Runtime/DurableSenderRuntimeTests.cs:54,160` | 요청에 넣는 Unix ms wire deadline. |
| `Zlink.Framework.UnitTests/Runtime/EntrySpotActorDispatchTests.cs:4184,4257,4332,4399,4653,6548,7618,7651,7697,7781,7847,7885,7934,7979,9163,9182,9198` | actor/closing context·wire deadline과 blob store의 ExpiresAt/StoreNow. |
| `Zlink.Framework.UnitTests/Runtime/FanoutAutomaticDiscoveryTests.cs:153,307` | location runtime snapshot의 관측 시각. |
| `Zlink.Framework.UnitTests/Runtime/GeneratedNodeIdentityTests.cs:114` | descriptor UpdatedAt. |
| `Zlink.Framework.UnitTests/Runtime/GeneratedRelocationCodecConformanceTests.cs:143` | relocation store ExpiresAt/StoreNow. |
| `Zlink.Framework.UnitTests/Runtime/InstanceSpotActivationJournalTests.cs:287` | descriptor UpdatedAt. |
| `Zlink.Framework.UnitTests/Runtime/LocationResolverTests.cs:697,731` | location change event timestamp. |
| `Zlink.Framework.UnitTests/Runtime/LocationRuntimeQueryTests.cs:469` | authority snapshot StoreNow. |
| `Zlink.Framework.UnitTests/Runtime/ProviderLocationRepositoryAuthorityTests.cs:1127,2833,3142,3190` | store lease/만료·stagedAt 데이터와 conflict StoreNow. |
| `Zlink.Framework.UnitTests/Runtime/ProviderRelocationRepositoryTests.cs:123,171` | blob store ExpiresAt/StoreNow. |
| `Zlink.Framework.UnitTests/Runtime/RelocationRuntimeTests.cs:61,111,1067,2304,3824,3825,4164,4231,4325,4346,4362,4393,4414,4439,4479,4500,4527,4628,4648,4663,4695,4749,4772,5021` | wire deadline, descriptor UpdatedAt, authority/blob/relocation store 데이터. |
| `Zlink.Framework.UnitTests/Runtime/RelocationTreeParallelIoTests.cs:618,638,697` | relocation store ExpiresAt/StoreNow. |
| `Zlink.Framework.UnitTests/Runtime/ServiceRuntimeFoundationTests.cs:2669` | descriptor UpdatedAt. |
| `Zlink.Framework.UnitTests/Runtime/StandaloneActorRelocationPrecommitTests.cs:491` | descriptor UpdatedAt. |
| `Zlink.Framework.UnitTests/Runtime/StandaloneActorRelocationRuntimeTests.cs:54,943,1095,1128,1156,1187,1590,1624,1636,1661,1782,1855,1903,1923,1939,1963,1980,2000` | lease 유효성 입력, descriptor/authority/blob/relocation store 데이터. |
| `Zlink.Framework.UnitTests/Runtime/StatefulServiceRuntimeTests.cs:2727,2814,2888,2949,3199,3323,3405,3694,3768,3854,3990,4046` | descriptor·wire/context deadline, 공개 closing deadline assertion, fake UTC 초기값. 3990의 UTC는 wire 생성과 상대 한도 확정에 한 번 사용. |
| `Zlink.Framework.UnitTests/Runtime/TimerLifecycleTests.cs:170` | timer tick 생성 입력 timestamp. |
| `Zlink.Framework.UnitTests/Runtime/WeightContractTests.cs:450` | descriptor UpdatedAt. |
| `Zlink.Framework.UnitTests/Runtime/ZLinkObservationQueueTests.cs:99` | 관측 event timestamp. |
| `Zlink.Framework.UnitTests/Support/Relocation/InMemoryRelocationStore.cs:37,59,75,132,156` | authoritative test store의 ExpiresAt/StoreNow 데이터. |
| `Zlink.HttpClient.UnitTests/TlsTestServer.cs:25×2` | 인증서 NotBefore/NotAfter 데이터. |

`RedisOpaqueProviderTests:338`은 테스트에서 시작한 경과 시간을 재는 코드가 아니다.
Redis가 반환한 절대 `cleanupDue`를 읽은 경계에서 `max(0, ceil(cleanupDue - UTC) + 100ms)`로
한 번 변환하고, 이후 상대 `Task.Delay`가 대기한다. Store timestamp는 monotonic epoch와
직접 비교할 수 없으므로 이 경계 변환과 기존 100ms offset을 유지한다.

## 검증

모든 .NET 실행은 아래 환경과 `flock -w7200 /tmp/zlink-dotnet-gate.lock`을 사용한다.
Solution 전체 빌드 없이 test project를 지정하며, 로그와 TRX를 보존한다.

```bash
source /tmp/zlink-d095/env.sh
# TMPDIR=/dev/shm/zlink-tmp-dotnet
# ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib
# NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-b9a964ffb25a3bf8
# UseSharedCompilation=false MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
cd /home/hep7/project/zlink/framework/languages/dotnet
```

| 검증 | 결과 (passed / failed / skipped) | 로그 |
|---|---|---|
| 변경된 unit 클래스 16개 (partial 파일 포함), build + focused | 447 / 0 / 0 | `/tmp/zlink-fixture-monotonic/touched-unit.log` |
| 변경된 StreamConnectorTests 클래스, build + focused | 156 / 0 / 0 | `/tmp/zlink-fixture-monotonic/touched-connector.log` |
| unit half 1회 | 1978 / 0 / 0 | `/tmp/zlink-fixture-monotonic/unit-half.log` |
| join half 1회 | 16 / 0 / 0 | `/tmp/zlink-fixture-monotonic/join-half.log` |
| SampleRegressionTests 1회, build + test | 157 / 0 / 0 | `/tmp/zlink-fixture-monotonic/sample-regression.log` |
| 감사 표 대조 | 192개 호출 누락·중복 없음, 수정 후 UTC 128개 | 수정 전 목록 `/tmp/zlink-fixture-sites-before.json` |
| `git diff --check` | 통과 | 테스트와 요약 문서 범위 |

TRX는 `/tmp/zlink-fixture-monotonic/results/`에 각 로그와 같은 이름으로 보존했다.
문제 사례 `Canonical_actor_join_malformed_body_returns_protocol_error`는
`CanonicalActorJoinWireAdmissionNegativeTests` 소속이며 focused와 unit half에서 모두 통과했다.
Join half의 16개는 `CanonicalActorJoinIngressReplyTests`다.

실행 명령의 test 인자는 다음과 같다. 각 명령 앞에 위 환경에서
`flock -w7200 /tmp/zlink-dotnet-gate.lock dotnet test`를 사용했다.
각 실행에 `--logger 'trx;LogFileName=<로그명>.trx'`와
`--results-directory /tmp/zlink-fixture-monotonic/results`를 추가했다.

```bash
# 변경된 클래스 filter는 /tmp/zlink-fixture-monotonic/focused-filter.txt에 보존
 tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj -f net8.0 --filter '<변경된 16개 클래스의 FullyQualifiedName OR filter>'
 tests/Systems.Zlink.Stream.Connector.Tests/Systems.Zlink.Stream.Connector.Tests.csproj -f net8.0 --filter 'FullyQualifiedName~StreamConnectorTests'
 tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj -f net8.0 --no-build --filter 'FullyQualifiedName!~CanonicalActorJoinIngressReplyTests'
 tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj -f net8.0 --no-build --filter 'FullyQualifiedName~CanonicalActorJoinIngressReplyTests'
 tests/Zlink.Framework.SampleRegressionTests/Zlink.Framework.SampleRegressionTests.csproj -f net8.0
```

Unit half와 join half는 앞선 focused에서 빌드한 동일 assembly를 사용했다.
테스트 통과를 위해 timeout·budget·retry·fixture 조건·assertion을 완화하지 않았다.

## BLOCKERS

- **없음.** 지정된 검증은 모두 실패·skip 0이다.
- 기존 runtime `ZLinkSpotNodeCatalog.cs:768`의 CS8619 경고는 유지했다. Runtime은 요청 범위 밖이다.
- Redis 절대 cleanup 시각의 최초 상대 변환은 위 감사 표의 의도된 유지 항목이다. Host UTC 점프 자체를 수정하거나 테스트로 재현한 것은 아니다.
- 변경 파일은 위 표의 테스트 18개와 이 요약 문서뿐이다. 기존 Java·Node 변경과 untracked 파일은 수정하지 않았다. Commit하지 않았다.

문서의 지침 준수와 코드·TRX 결과 부합 여부를 검토했다. 감사 표를 원본 UTC 호출 목록과
기계적으로 대조해 누락·중복이 없음을 확인했다.
