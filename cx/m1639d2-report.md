# m1639d2 — .NET 메시지 경로

작업 브랜치는 `framework-dotnet/1639-per-message-cost`이며, 수정 범위는 .NET Framework와 .NET benchmark이다. 다른 agent 실행·스펙 수정·push·PR·merge는 하지 않았다.

## Rebase

`git fetch origin` 뒤 `origin/main`의 `fb9ff91d31f3286d319f82408b198b5b5a95e7f7` 위로 14개 커밋을 rebase했다. 충돌은 없었고 rebase 완료 HEAD는 `d4e36fc302`다. 충돌 해결 커밋은 필요하지 않았다. #1638의 ReceiveLoop·DrainRawSocket·Core timer permit 재개를 유지했다. rebase 직후 main Unit 2867/2867·Contract 96/96, branch Unit 2885/2885·Contract 96/96으로 새 실패는 0이다.

## Router target 분류 갱신

경로 접두사는 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/`다. peer ObjectRole은 별도 mutable 필드가 아니라 Admission.ObjectRole에서 읽는다. local SetObjectRole은 `ZLinkManagedMeshNode.cs:321`에서 시작 전에만 허용하며 remote target 분류를 바꾸지 않는다.

| 변경 | 위치 | 준비가 이어지는 경로 | 판정 |
|---|---|---|---|
| 새 peer 생성 | ZLinkManagedMeshNode.cs:8910 | index에 넣기 전에는 target이 아니다. 승인된 descriptor가 아래 두 index 경로로 들어간다. | 누락 없음 |
| NotRequired Admission·State·RID index | ZLinkManagedMeshNode.cs:8983, :8985, :8989 | 최초 전이 :9003 Rebuild, 반복 전이 :9007 PrepareRouterTargets | 누락 없음 |
| Accept Admission·ObjectRole·RID index | ZLinkManagedMeshNode.cs:9043, :9045 | Hello는 :9052 Connecting 뒤 :9053 Rebuild. Admit·admitted Update는 :9065 CompletePeerAdmission→:9105 Rebuild | 누락 없음 |
| admitted 전 Accept Update | ZLinkManagedMeshNode.cs:9043 | 직접 rebuild하지 않는다. guard :104–117이 immutable ObjectRole 변경을 거부한다. State는 Connecting을 유지하며 분류는 Unknown 또는 ObjectClientTarget으로 동일하다. | 분류가 바뀌는 전이 없음 |
| rejection·index 제거 | ZLinkManagedMeshNode.cs:9080, :9089 | :9090 Rebuild | 누락 없음 |
| admitted·draining | ZLinkManagedMeshNode.cs:9097 | :9105 Rebuild. control-send 수락 경로 :11972도 같은 CompletePeerAdmission을 사용한다. | 누락 없음 |
| route disconnect | ZLinkManagedMeshNode.cs:9382, :9383 | :9386 Rebuild. intent 없는 peer는 :9359 RemovePeer 경로 | 누락 없음 |
| peer 제거·Closed | ZLinkManagedMeshNode.cs:12295, :12297 | :12298 Rebuild | 누락 없음 |
| 공통 rebuild owner | ZLinkManagedMeshNode.cs:11672 | :11674 PrepareRouterTargets→ZLinkMeshChannelSelection.cs:25 | 분류 판정 owner 1개 |

mutation 위치를 전수 검색했으며 production 수정은 하지 않았다. 추가·admitted·draining·disconnect·client role·잘못된 role Update 거부·replacement·제거를 production 경로로 검사하는 두 theory case를 추가했다. 수정 전 두 case 모두 통과했다. 누락이 없으므로 실패를 만들기 위해 fixture를 바꾸지 않았다. 분류 판단은 1→1, 준비 호출 위치는 기존 2→2다.

## 완료 구간

수정 전 `TableRequestContinuationRunsInTheThreadPoolDispatcherTurn`이 production DestroyActorAsync의 미완료 Task에서 dispatcher scope를 관찰해 실패했다(`focused-before.log`: dispatcher=false). 전용 dispatcher thread→RCA ThreadPool 게시의 Framework 전환은 2회다. 수정 후 동일 request continuation이 dispatcher scope 안의 ThreadPool thread에서 실행되어 1회다.

`ZLinkCompletionDispatcher`가 `IThreadPoolWorkItem`을 구현한다. 기존 `_tail`이 비어 있을 때만 process 공유 drain을 ThreadPool에 게시하고, 실행 중에는 tail을 예약으로 유지한다. 기존 intrusive chain을 FIFO batch로 분리해 실행한다. 실행 문맥은 UnsafeQueueUserWorkItem으로 상속하지 않으며 thread-static dispatcher scope를 실행 후 해제한다. WorkItem wrapper·새 queue·running flag를 추가하지 않았다. callback exception 격리와 table의 callback-return 이후 drain 회계를 유지한다.

`ZLinkBackendSpotNodeWrapper.cs`의 table-owned reply TaskCompletionSource 7곳(Instance activation·user Spot create·remote Actor create/destroy·user Spot close·local Actor destroy·Actor request)에서 RCA를 제거했다. result·exception은 등록 callback, cancellation은 기존 table의 PendingReply.Execute에서만 완료된다. 일반 `ZLinkRequestCompletion.cs:15`는 timeout·cancellation stack에서도 완료할 수 있으므로 RCA를 유지했다. `_drained`의 RCA도 유지했다.

완료 자원 선택 위치는 dispatcher 1+reply TCS 7의 8→1이다. 표면 하나의 경로로는 2→1이다. runtime 새 상태·flag·queue·lock·timer·retry는 0이며 전용 thread 1개와 Monitor wait/pulse를 제거했다. 기존 head·tail·gate·dispatcher scope는 재사용했다.

## 검증

| 검사 | 결과 | 로그 |
|---|---|---|
| rebase main Unit / Contract | 2867/2867 / 96/96 | rebase-base-Unit.log / rebase-base-Contract.log |
| rebase branch Unit / Contract | 2885/2885 / 96/96 | rebase-branch-Unit.log / rebase-branch-Contract.log |
| 수정 전 분류 전이 / 완료 turn | 2 통과 / 1 실패 | focused-before.log |
| 관련 검사 | 129/129 | related-after.log |
| 최종 branch Unit / Contract | 2888/2888 / 96/96 | final-branch-Unit.log / final-branch-Contract.log |
| benchmark 할당 관찰 수정 전 | AllocatedBytes JSON 필드 부재로 실패 | bench-allocation-before.log |
| base·branch benchmark Boundary / Wire | 모두 exit 0 | {base,branch}-bench-{Boundary,Wire}.log |

benchmark는 resource interval 시작·종료와 target snapshot에서 GC.GetTotalAllocatedBytes(precise:true)를 읽는다. per-message 호출은 없으며 process의 관리 할당 byte를 관찰한다. target reset 전후 1 MiB 할당의 포함·제외를 검사했다. 두 runtime에 같은 benchmark 계측 patch를 적용했다. CPU와 할당은 active deadline snapshot 기준이며 native 할당은 포함하지 않는다. 로컬 Framework package를 만들어 revision별 NuGet cache로 복원했으며 binding/Core 1.19.0은 동일하다. native SHA256은 `b3e67363ef9b2577e8a273b89d7399525e5e550928f174b318eb466ac8b97054`다.

최초 준비 명령의 인자 인용 오류로 실행되지 않은 경로 검사 로그와 Windows `.dotnet-home`이 생성되었다. 이후 모든 실행은 사본 안의 shell script로 고정했고, 해당 Windows 산출물은 `m1639d2/accidental-dotnet-home`으로 이동해 저장소에서 분리했다. benchmark 첫 빌드는 이전 remote Framework package로 ZLinkRequest 타입을 찾지 못했으며 local package 복원 뒤 해결됐다. 이 두 준비 실패를 runtime 결함 재현으로 세지 않았다.

## 소유와 분류

소유 계층: Framework completion table이 terminal 권한을 꺼내고 process 공유 completion dispatcher가 새 turn을 실행한다. peer classification은 기존 node state lane과 immutable selection view가 소유한다.

spec 조항: `08-messaging-hot-path.ko.md:98,105,307`의 E5 완료 전환 하나·.NET ThreadPool 자원, `01-submit-and-completion.ko.md:389–405,425–430`의 atomic take→unlock→게시→새 turn·process 공유 lane·shutdown drain·예외 격리, `02-handler-turn-and-execution-gate.ko.md:137–149`의 infrastructure/application 분리다.

교차언어 대조: C++ `foundation/operation_registry.cpp:190–220`은 공유 dispatcher worker에서 callback을 실행하며 task.hpp:127–156은 held turn continuation을 이어 실행한다. Java `ZLinkServiceCompletionDispatcher.java:23,93–108`은 공유 platform worker이며 Kotlin도 이 owner를 사용한다. Node `mesh-completion-table.ts:121–141`은 Promise resolve 후 event-loop reaction으로 전달한다. 자원 종류의 차이는 hot-path §6 표가 정하며 이번 변경은 .NET 자원을 그 조항에 맞춘다. Java 일반 await의 추가 게시와 .NET direct binding/Framework dispatcher 경계는 q1645e의 별도 항목이며 이 job에서 변경하지 않았다.

변경 분류: B — 승인된 .NET table 완료 전달의 중복 실행 자원 선택을 제거했다. 분류 갱신 누락은 0건으로 검증만 추가했다. 새 public API·spec 결정은 없다.

전체 format은 base·branch 각각 1184파일 exit 0이다. Windows/WSL 입력 40개는 SHA256이 일치한다(`source-parity.json`). source와 benchmark output DLL hash는 `final-bench-assemblies.sha256`에 보존한다.

## 변경 파일과 커밋

- `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/MeshChannelSelectionHotPathTests.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/Wrappers/ZLinkBackendSpotNodeWrapper.cs`
- `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/MeshCompletionTableConvergenceTests.cs`
- `framework/bench/grpc/dotnet/Shared/BenchServerMetrics.cs`
- `framework/bench/grpc/dotnet/Client/Program.cs`
- `framework/bench/grpc/dotnet/Client/BenchMeasurement.cs`
- `framework/bench/grpc/dotnet/BoundaryTests/Program.cs`

| 커밋 | 범위 |
|---|---|
| 44388a5cc69b8996876258c153edad5d30a03c35 | 분류 전이 검증 |
| 9928c49a68e4b56e71483c8cf0545669a0cbf8ca | dispatcher 완료 전환 |
| 52ca712ece6e2daf975e1a39769f8242dba4b163 | benchmark allocation snapshot |

모든 추가 커밋에 Refs #1674·#1639가 있다. benchmark는 source ResourceSample과 target BenchServerMetrics에 allocation 시작값을 하나씩 추가했다. 두 값은 측정용이며 runtime 수락·완료 권한 상태가 아니다.

## 남은 비용 순위

아래 순위는 branch 중앙값에서 같은 pattern의 binding을 뺀 **process CPU·관리 byte 비용**이다. 함수별 profiler를 이번에 실행하지 않았으므로 함수 하나의 원인 비용으로 해석하지 않는다. `source/target`은 같은 active 구간의 자원 snapshot이다.

| 우선순위 | 영역 | binding 초과 CPU µs/메시지 | binding 초과 관리 byte/메시지 | 관련 경로 |
|---|---|---:|---:|---|
| 1 | request-serial target | 150.001 | 17721.8 | receive owner→ZLinkMeshDispatchPump.cs:620 DispatchRecord→application gate→typed reply |
| 2 | request-serial source | 75.839 | 8517.9 | ZLinkManagedMeshNode.cs:9583 RequestToNodeDirectAsync와 codec·binding reply 소비 |
| 3 | request-backpressure target / source | 29.199 / 26.460 | 11419.5 / 8056.0 | 같은 request 경로의 동시 submit·reply 진행 |
| 4 | send-saturation target | 13.692 | 6322.2 | receive dispatch·application job 처리 |

send source는 binding보다 CPU가 5.493µs 적지만 관리 할당은 5830.4byte 많다. base 대비 branch send source CPU 중앙값은 20.257→7.389µs(-63.5%)이고 target은 18.820→16.914µs다. serial source/target은 202.271/202.271→194.400/193.872µs, backpressure는 49.566/44.360→41.225/36.593µs다. 할당 중앙값도 세 pattern의 source·target 모두 줄었다. 이 값은 rebase 위에 누적된 메시지 경로 변경 전체의 차이이며, table reply RCA 삭제 하나의 효과로 주장하지 않는다. 일반 request benchmark는 direct binding Task 경로이며, registered send와 table request의 자원 변경을 구분한다.

## 남은 항목과 판정 범위

- 기능 검사의 남은 실패와 main 대비 새 실패는 0이다.
- original 36셀은 모두 유효하며 errors·abandoned·warmup abandoned·drain bound hit는 0이다. 오류 1을 넣은 사본은 집계기가 거부했다(`checker-selftest.txt`).
- branch/base Framework 처리량 중앙값은 send +14.77%, serial +6.96%, backpressure +21.72%다. branch/binding은 각각 0.5492·0.5650·0.4713이다. send·serial의 hot-path §7(c) 0.90과 backpressure의 benchmark 0.80을 충족하지 못했다. request-window·4096B는 이번 요청의 측정 범위 밖이므로 전체 hot-path 예산 충족을 선언하지 않는다.
- 완료 turn 테스트는 table atomic take 이후 dispatcher→caller 구간을 관찰한다. `ZLinkManagedMeshNode.cs:4979`의 managed terminal owner 왕복(q1645e B3)은 별도 잔여 비용이며 이번 dispatcher 판정 위치 수에 합산하지 않았다. owner가 경합하는 모든 terminal 경로의 전체 전환이 1임을 주장하지 않는다.
- `ZLinkRequestCompletion.cs:15`의 초기 BACKPRESSURED reply 합성과 `ZLinkManagedMeshNode.cs:9583` direct binding Task/Framework dispatcher 경계(q1645e D)는 변경하지 않았다. 스펙 경계를 정하지 않고 일반 RCA를 삭제하거나 게시를 더하지 않았다.

리팩토링 점검 범위의 잔여 발견은 0이다. 새 hot-path 할당·lock·frame 복사·대기·flow 문자열 생성은 없다. reusable IThreadPoolWorkItem과 기존 예약 node를 사용해 wrapper work item을 할당하지 않으며, enqueue allocation 검사를 포함한 관련 검사와 전체 label이 통과했다. off trace의 기존 interpolated string handler를 유지했다. 분류 판정과 execution gate·terminal 권한을 다른 곳으로 복제하지 않았다. 소유 계층과 정책을 바꾸는 새 옵션·helper·DTO·매핑표도 추가하지 않았다.

## 1024B 교대 측정

warmup 2초·active 5초·base→branch 3회다. CPU는 µs/메시지, 관리 할당은 byte/메시지이며 source/target 순서다. 분모는 셀의 active 처리량×5초로 CPU·할당의 구간과 맞췄다. process 전체 비용이며 HTTP 관리 계측도 포함한다.

| 회차 | revision | pattern | binding ops/s | Framework ops/s | Fx/binding | binding CPU s/t | Fx CPU s/t | binding byte s/t | Fx byte s/t |
|---|---|---|---:|---:|---:|---|---|---|---|
| 1 | base | send-saturation | 405070.0 | 199044.6 | 0.4914 | 12.077/3.397 | 20.257/18.820 | 2152.4/1216.6 | 8745.9/10251.0 |
| 1 | base | request-serial | 6561.8 | 3539.8 | 0.5395 | 125.880/43.890 | 202.271/202.271 | 3528.2/1482.2 | 13399.3/22500.8 |
| 1 | base | request-backpressure | 172115.4 | 71541.6 | 0.4157 | 15.408/7.751 | 49.566/44.394 | 3329.2/1480.1 | 12741.6/16329.0 |
| 1 | branch | send-saturation | 392948.0 | 217916.4 | 0.5546 | 12.719/3.481 | 7.737/17.566 | 2152.3/1217.1 | 7978.5/7488.6 |
| 1 | branch | request-serial | 5641.0 | 3247.4 | 0.5757 | 153.873/46.446 | 208.782/227.259 | 3530.5/1482.5 | 12050.4/19206.4 |
| 1 | branch | request-backpressure | 184755.6 | 87083.0 | 0.4713 | 14.765/7.394 | 40.995/37.091 | 3329.1/1480.1 | 11385.2/12899.6 |
| 2 | base | send-saturation | 423804.8 | 198722.2 | 0.4689 | 12.902/3.223 | 20.390/19.011 | 2152.3/1231.7 | 8801.9/10324.0 |
| 2 | base | request-serial | 6643.4 | 3502.2 | 0.5272 | 120.721/42.749 | 203.872/202.730 | 3527.4/1482.1 | 13387.2/22497.0 |
| 2 | base | request-backpressure | 178840.6 | 66275.4 | 0.3706 | 15.075/7.336 | 51.482/44.360 | 3329.4/1480.1 | 12746.1/16140.6 |
| 2 | branch | send-saturation | 415943.2 | 228448.6 | 0.5492 | 12.882/3.222 | 7.389/16.914 | 2152.3/1216.2 | 7984.5/7581.0 |
| 2 | branch | request-serial | 6701.4 | 4102.6 | 0.6122 | 117.886/43.871 | 183.786/181.836 | 3526.5/1482.2 | 12041.2/19203.1 |
| 2 | branch | request-backpressure | 196724.6 | 86245.8 | 0.4384 | 14.091/6.873 | 42.877/36.593 | 3329.2/1480.1 | 11393.1/12882.6 |
| 3 | base | send-saturation | 425516.0 | 213869.4 | 0.5026 | 12.958/2.844 | 17.946/17.216 | 2152.3/1231.6 | 8682.1/10173.3 |
| 3 | base | request-serial | 6187.4 | 4115.8 | 0.6652 | 121.214/43.314 | 183.196/181.739 | 3528.7/1482.3 | 13383.2/22496.4 |
| 3 | base | request-backpressure | 182045.8 | 80127.6 | 0.4402 | 14.963/7.427 | 45.477/38.164 | 3329.0/1480.1 | 12741.8/16171.3 |
| 3 | branch | send-saturation | 426279.2 | 231328.8 | 0.5427 | 12.921/3.026 | 7.202/16.600 | 2152.3/1231.5 | 7982.7/7539.3 |
| 3 | branch | request-serial | 7338.0 | 3786.0 | 0.5159 | 118.561/39.248 | 194.400/193.872 | 3525.5/1481.9 | 12044.4/19204.0 |
| 3 | branch | request-backpressure | 177949.4 | 92032.2 | 0.5172 | 15.184/7.418 | 41.225/35.075 | 3329.2/1480.1 | 11381.7/12918.2 |

### 중앙값

| revision | pattern | binding ops/s | Framework ops/s | 중앙값의 비 | binding CPU s/t | Fx CPU s/t | binding byte s/t | Fx byte s/t |
|---|---|---:|---:|---:|---|---|---|---|
| base | send-saturation | 423804.8 | 199044.6 | 0.4697 | 12.902/3.223 | 20.257/18.820 | 2152.3/1231.6 | 8745.9/10251.0 |
| base | request-serial | 6561.8 | 3539.8 | 0.5395 | 121.214/43.314 | 202.271/202.271 | 3528.2/1482.2 | 13387.2/22497.0 |
| base | request-backpressure | 178840.6 | 71541.6 | 0.4000 | 15.075/7.427 | 49.566/44.360 | 3329.2/1480.1 | 12741.8/16171.3 |
| branch | send-saturation | 415943.2 | 228448.6 | 0.5492 | 12.882/3.222 | 7.389/16.914 | 2152.3/1217.1 | 7982.7/7539.3 |
| branch | request-serial | 6701.4 | 3786.0 | 0.5650 | 118.561/43.871 | 194.400/193.872 | 3526.5/1482.2 | 12044.4/19204.0 |
| branch | request-backpressure | 184755.6 | 87083.0 | 0.4713 | 14.765/7.394 | 41.225/36.593 | 3329.2/1480.1 | 11385.2/12899.6 |

### 회차 분산

| revision | 구현 | pattern | min/max ops/s | max/min |
|---|---|---|---|---:|
| base | zlink-dotnet | send-saturation | 405070.0/425516.0 | 1.050 |
| base | zlink-dotnet | request-serial | 6187.4/6643.4 | 1.074 |
| base | zlink-dotnet | request-backpressure | 172115.4/182045.8 | 1.058 |
| base | zlink-framework-dotnet | send-saturation | 198722.2/213869.4 | 1.076 |
| base | zlink-framework-dotnet | request-serial | 3502.2/4115.8 | 1.175 |
| base | zlink-framework-dotnet | request-backpressure | 66275.4/80127.6 | 1.209 |
| branch | zlink-dotnet | send-saturation | 392948.0/426279.2 | 1.085 |
| branch | zlink-dotnet | request-serial | 5641.0/7338.0 | 1.301 |
| branch | zlink-dotnet | request-backpressure | 177949.4/196724.6 | 1.106 |
| branch | zlink-framework-dotnet | send-saturation | 217916.4/231328.8 | 1.062 |
| branch | zlink-framework-dotnet | request-serial | 3247.4/4102.6 | 1.263 |
| branch | zlink-framework-dotnet | request-backpressure | 86245.8/92032.2 | 1.067 |

## serial 분산 추가 진단

원래 교대 36셀을 바꾸지 않고, 주변 부하가 낮아진 뒤 같은 1024B·2초/5초·3회 교대로 serial만 다시 관찰했다. 동일한 binary·native package를 사용했다.

| 회차 | revision | binding ops/s | Framework ops/s | 비율 | source CPU raw/Fx µs | target CPU raw/Fx µs |
|---|---|---:|---:|---:|---|---|
| 1 | base | 7217.2 | 3924.6 | 0.5438 | 111.955/188.045 | 38.796/180.910 |
| 1 | branch | 7059.6 | 3785.4 | 0.5362 | 115.021/192.846 | 39.662/184.393 |
| 2 | base | 7064.2 | 4233.4 | 0.5993 | 114.096/179.525 | 41.901/179.052 |
| 2 | branch | 7550.2 | 3627.2 | 0.4804 | 110.990/197.949 | 37.085/190.229 |
| 3 | base | 7129.6 | 3968.4 | 0.5566 | 117.258/186.473 | 42.078/184.457 |
| 3 | branch | 6809.0 | 4090.0 | 0.6007 | 118.079/177.017 | 43.472/183.863 |

| revision | 구현 | 중앙값 ops/s | min/max | max/min |
|---|---|---:|---|---:|
| base | zlink-dotnet | 7129.6 | 7064.2/7217.2 | 1.022 |
| base | zlink-framework-dotnet | 3968.4 | 3924.6/4233.4 | 1.079 |
| branch | zlink-dotnet | 7059.6 | 6809.0/7550.2 | 1.109 |
| branch | zlink-framework-dotnet | 3785.4 | 3627.2/4090.0 | 1.128 |

## 분산 판정과 마무리

original send의 max/min은 main Framework 1.076, branch Framework 1.062로, 이전 43k~97k의 분리는 재현되지 않았다. original serial의 branch max/min은 binding 1.301, Framework 1.263이었다. 할당은 Framework source 12041.2~12050.4byte·target 19203.1~19206.4byte로 거의 같았다. framing·payload·in-flight·오류·송수신 수가 바뀐 두 workload로 갈린 증거는 없다.

주변 검증 프로세스가 실행되던 original 구간의 vmstat 평균 runnable은 회차별 3.25~5.15, CPU idle은 75.2~82.7%였다. I/O wait·steal은 모든 snapshot에서 0이다. 낮은 부하의 serial 추가 진단에서 branch max/min은 binding 1.109, Framework 1.128로 줄었다. 같은 binary에서 두 구현이 함께 좁아진 점은 host 실행 환경이 분산에 기여했다는 근거다. 다만 특정 background process·CPU 배치·주파수 중 하나를 원인으로 확정할 계측은 없으며 환경 간섭을 유일한 원인이라고 주장하지 않는다.

추가 진단의 serial branch/base Framework 중앙값은 3785.4/3968.4 = 0.9539(-4.61%)로 original의 +6.96%와 방향이 다르다. **serial 개선·회귀 판정은 보류한다.** original과 추가 진단 모두 binding 대비 0.90을 충족하지 못하므로 예산 미달 판정은 변하지 않는다. 측정값을 선택해 통과시키지 않았으며 warmup·duration·HWM·timeout을 바꾸지 않았다. 추가 진단 12/12셀도 오류·abandoned·drain 초과 0이다.

테스트 Framework DLL과 benchmark가 로드한 DLL은 revision별 SHA256이 같다: branch `3d88e6cdd9e8d8cbcdbeaa8d59632fe873d74553eeb6c3dec8d2789ce421c98e`, main `95b7bf09a36f96076a521853a8e20fbf56be236d980b31710f63f286db46dafb`. local package의 갱신 여부를 추정하지 않고 실제 입력을 대조했다.

로그·flow·script 438개를 `D:/t/jobs/fx/cx/m1639d2/`에 복사하고 SHA256을 대조했다(`export-manifest.json`). 작업 프로세스 0을 확인했으며 WSL 사본 제거는 아래 최종 확인에 기록한다. GitHub #1674 Project 상태는 In Progress다. push·PR·merge는 하지 않았다.

리팩토링 점검: 성능 1건·POSDDD 1건·불필요 코드 1건 발견, table 완료의 중복 자원 선택과 전용 worker·Thread 생성/문맥 억제 코드를 기존 FIFO dispatcher owner로 통합, 넘긴 항목 ZLinkManagedMeshNode.cs:4979의 managed terminal owner 왕복과 ZLinkRequestCompletion.cs:15의 direct BACKPRESSURED 완료 합성·ZLinkManagedMeshNode.cs:9583의 binding/Framework dispatcher 경계(이번 변경 함수 밖). 수정 범위의 잔여 발견 0건; 실행 자원 선택 8→1·runtime 새 상태/flag/queue/lock/timer/retry 0·전용 thread 1개 삭제. 예산은 send·serial·backpressure 모두 미충족이다.

최종 확인: WSL 사본 /home/hep7/worktree/zlink-1674-dotnet 제거 완료, 로그 SHA256 검증 완료, 작업 프로세스 0, Windows 지정 브랜치 clean, 마지막 커밋 52ca712ece6e2daf975e1a39769f8242dba4b163, push 없음.

## 완료 실행 자원 — 감독자 2차 리뷰 (#1674)

이 절의 변경은 앞 절의 단일 FIFO drain 설계를 대체한다. `9928c49a68`의 전환 1회는 확인됐지만, caller continuation까지 직렬 drain에 포함해 서로 다른 완료를 막는 결함이 있었다. 처리량은 이번 리뷰에서 측정하지 않았으며, 앞 절의 처리량 수치를 새 runtime의 결과로 사용하지 않는다.

### 재현

`CompletionDispatcherIsolationTests`는 실제 .NET backend로 host를 시작하고 공개 `IZLinkActorManager.GetOrCreate(...).Async()`와 `IZLinkActorClient.RequestToActor(...).Async<T>().AsTask()`를 사용한다. 두 Actor의 typed JSON handler가 각각 reply gate를 기다리게 해 request를 미완료 상태로 만든다. caller는 handler 밖의 일반 application caller이며, 서로 다른 Actor를 사용하므로 application gate의 self-request와 구분된다.

`CallerCanSynchronouslyWaitForAnotherTableRequest`는 `await a`가 미완료 Task에 먼저 등록된 뒤 a의 reply를 허용한다. 그 continuation에서 `b.WaitAsync(testGuard).GetAwaiter().GetResult()`로 b를 동기 대기하고, 별도 driver가 b의 reply를 허용한다. 5초 guard는 테스트의 교착을 해제하는 용도이며 request의 timeout·budget을 바꾸지 않는다. guard가 없으면 a가 점유한 drain이 b의 terminal callback을 실행할 수 없다.

`OneSecondBlockedCallerDoesNotDelayAnotherCaller`는 a의 continuation을 event로 막고 별도 task가 1초 뒤 해제한다. b의 첫 continuation instruction에서 경과 시간과 a의 release 상태를 읽는다. 합격 조건은 임의의 ms 경계가 아니라 **a를 해제하기 전에 b가 완료되는가**다. 모든 대기에 cleanup guard가 있어 실패 시 테스트가 무기한 멈추지 않는다.

| runtime | 동기 대기 | 다른 caller 지연 | 전환 검사 |
|---|---|---|---|
| `9928c49a68^` = `44388a5cc6` | 진행, 테스트 93ms | 1.209ms, a 미해제 | dispatcher scope 밖의 pool continuation `(false,true)`; 2회 |
| `9928c49a68` runtime | 5초 guard까지 진행 불가 → 실패 | 998.173ms, a 해제 후 완료 → 실패 | `(true,true)`; 1회 |
| 이번 수정 | 진행, 관련 검사에서 약 95ms | 최종 관련 검사 1.219ms, a 미해제 → 통과 | 공개 request의 continuation `(true,true)`; 1회 유지 |

before branch archive는 작업 시작 HEAD `52ca712ece`에서 만들었지만 `git diff 9928c49a68 HEAD -- framework/languages/dotnet`은 비어 있어 runtime과 언어 입력은 정확히 해당 커밋과 같다. benchmark 차이는 이번 검사에서 실행하지 않았다. 두 before revision에 같은 새 테스트를 적용했다. 로그는 `r2/logs/r2-before-parent.log`, `r2-before-branch.log`, 수정 후는 `r2-related.log`다.

초기 테스트의 `Task.Run(async ...)`는 reply가 먼저 완료되는 경쟁을 허용해 현재 runtime에서도 거짓 통과했다. 이 결과는 `r2-before-*-late-await.log`에 별도로 보존하고 판정에서 제외했다. caller async 함수를 직접 호출해 미완료 await 등록이 끝난 뒤 reply를 허용하도록 고친 결과 위 두 결함이 재현됐다. 초기 lifecycle interface 구현 누락으로 난 컴파일 오류도 `*-compile.log`로 구분했다.

### 선택한 형태와 소유

`ZLinkCompletionDispatcher`는 상태 없는 process 공통 게시 지점이다. table이 submit 전에 만든 `PendingCompletion` 자체가 `IThreadPoolWorkItem`이다. terminal 권한을 table에서 atomic take한 뒤 잠금을 놓고 기존 node를 `ThreadPool.UnsafeQueueUserWorkItem(work, preferLocal:false)`에 게시한다. 새 work item 안에서 terminal 결과를 TCS에 넣고 caller continuation을 그 turn에서 이어 실행한다. Framework가 만드는 전환은 1회이며, 별도 Task·delegate wrapper나 완료 queue를 만들지 않는다.

reply·send admission·cancellation·shutdown은 모두 같은 `Post`를 사용한다. callback exception은 해당 work item 안에서 잡고 `Completed(failure)`를 실행한다. callback이 반환된 뒤 기존 table의 `ReleaseReservation`이 outstanding을 회수하며 shutdown은 `CompletionDrained`를 기다린다. 다른 callback은 독립된 pool work item에서 진행한다. 새 global outstanding 계수는 없다. `UnsafeQueueUserWorkItem`은 게시자의 execution context를 상속하지 않으며 기존 thread-static dispatcher scope는 work item의 try/finally에서만 켜고 끈다.

resource 선택 판정 위치는 기존 `Post` 1→1이다. process drain의 예약·진행 판정 위치는 `Post`의 tail/head 조건 2곳과 drain의 empty 조건 1곳, 합계 3→0이다. dispatcher의 `_gate` lock 1개·`_head`/`_tail` 2개·node의 `Next` 1개·table의 dispatcher instance reference와 주입 constructor를 제거했다. 새 runtime flag·queue·lock·timer·retry·완료 계수는 0이다. ThreadPool의 queue와 worker 관리 정책을 Framework에서 복제하거나 변경하지 않았다.

소유 계층: Framework table의 terminal take·callback-return accounting과 .NET completion dispatcher의 ThreadPool 게시 지점이다. Core·binding의 완료 권한과 application execution gate는 바꾸지 않았다.

spec 조항: `01-submit-and-completion.ko.md:389–405`의 atomic take→unlock→게시→새 turn, :425–430의 process 공통 자원·shutdown drain·callback exception 격리, `08-messaging-hot-path.ko.md:98,105,307`의 E5 전환 하나와 .NET ThreadPool continuation이다. §3:106의 FIFO는 **binding 제출 순서**이며 서로 다른 operation의 완료 FIFO 요구가 아니다. §4 W4의 application worker drain은 이번 completion dispatcher와 다른 경계다.

교차언어 대조: hot-path §6은 C++·Java의 worker와 .NET의 pool continuation을 구분한다. 이번 수정은 .NET 자원 표와 감독자가 지정한 형태를 따른다. 다른 언어의 실행 자원을 변경하거나 네 언어에 동일한 thread 종류를 강제하지 않았다.

변경 분류: B — 단일 drain에서 inline caller가 다른 완료를 막는 재현된 결함을 수정했다. spec 수정과 새로운 public API는 없다.

### FIFO 의존 목록

아래 test 위치는 수정 전 HEAD(`52ca712ece`)의 `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/MeshCompletionTableConvergenceTests.cs`다. 원본을 `r2/fifo-before.cs`로 보존했다.

| file:line | 의존 내용 | spec 요구 여부 / 조치 |
|---|---|---|
| MeshCompletionTableConvergenceTests.cs:12 | 다른 operation의 blocker로 public request Task를 미완료로 유지 | 완료 FIFO 요구 없음. 공개 API의 handler reply gate로 이전했다. |
| MeshCompletionTableConvergenceTests.cs:68,105 | blocker가 살아 있는 동안 immediate send continuation은 실행될 수 없음 | 완료 FIFO 요구 없음. await 등록 뒤 immediate admission을 전달하고 새 dispatcher turn을 확인한다. |
| MeshCompletionTableConvergenceTests.cs:421,462,466 | 다른 table의 callback이 선행 callback 뒤에서 기다림, 같은 managed thread ID | spec에 같은 물리 worker 요구 없음. `BlockedCallbackDoesNotBlockOtherTables`로 교체했다. |
| MeshCompletionTableConvergenceTests.cs:565,575,622 | enqueue allocation 검사에서 process-wide blocker를 사용 | FIFO는 검사의 전제가 아니지만 serial 구현을 고정한 fixture였다. blocker·별도 dispatcher를 제거하고 기존 0-byte enqueue assertion은 유지했다. |
| MeshCompletionTableConvergenceTests.cs:635,692 | 서로 다른 operation을 `[1,2,3]` 완료 순서로 고정, reentrant는 이미 수락된 work 뒤 | spec 요구 없음. reentrant child가 parent callback의 반환을 기다리지 않고 완료되는지, 세 identity가 각각 한 번 완료되는지 검사한다. |
| ZLinkCompletionDispatcher.cs:25–62,71–100 | head/tail FIFO chain·단일 drain·batch 순서 | 기존 구현의 completion 직렬화였다. queue·drain·Next를 제거했다. |

runtime의 dispatcher 사용처와 table의 callback을 검색·대조했다. wrapper의 request callback은 각 correlation의 TCS/parts를 소유하며 operation 간 순서 판정을 하지 않는다. `FailAll`의 dictionary 순회는 terminal take를 일괄 수행하는 과정이며 callback의 실행 순서에 대한 계약이 아니다. 추가 FIFO 의존 runtime 호출자는 발견하지 못했다. 제출 owner와 application gate의 FIFO는 수정하지 않았다.

### 최종 검증과 파일

| 검사 | 결과 |
|---|---|
| 수정 후 관련 검사 | 33/33, 교착·1초 블로킹·전환 1회·shutdown drain·예외 격리·reentrant progress 포함 |
| .NET unit 전체 | 2891/2891, 실패 0 (`r2-final-Unit.log`) |
| .NET contract 전체 | 96/96, 실패 0 (`r2-final-Contract.log`) |
| .NET 전체 format | 1185파일 exit 0 (`r2-final-format.log`) |
| owner lane 게시와 test cleanup guard 보강 후 관련 검사 | 33/33 (`r2-related.log`), runtime은 전체 검사 뒤 변경 없음 |

owner-scope 테스트는 생성뿐 아니라 실제 `Complete` 게시도 owner lane 안에서 호출하도록 강화했다. callback의 `ZLinkStateLane.Current`가 null인지 확인한다. 단일 drain 제거 뒤에도 shutdown은 막힌 callback이 반환할 때까지 끝나지 않고, 다른 callback과 exception 처리만 독립적으로 진행하는 새 검사도 통과했다. 새 실패와 남은 기능 실패는 0이다. 처리량 측정 0회다.

변경 파일은 `Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs`, `ZLinkMeshCompletionTable.cs`, `tests/.../Runtime/MeshCompletionTableConvergenceTests.cs`, 새 `CompletionDispatcherIsolationTests.cs`, `Zlink.Framework.UnitTests.csproj`의 다섯 개다. `RunContinuationsAsynchronously`를 다시 추가하지 않았으며, 앞 커밋의 다른 메시지 경로 변경과 benchmark 코드는 건드리지 않았다.

리팩토링 점검: 성능 1건·POSDDD 1건·불필요 코드 1건 발견, 완료의 process-wide head-of-line blocking·drain 예약 판정·직렬 queue/lock/Next와 dispatcher 주입을 제거, 넘긴 항목 없음. resource 선택 1→1·drain 판정 3→0, 새 runtime 상태/flag/queue/lock/timer/retry/완료 계수 0. 표준 pool queue 관리 비용의 처리량 판정은 이번 요청에 따라 하지 않았다.

새 커밋: a5f7a5b5febace3629e24ccda0db6bc8c3cfccde (Refs #1674). 이전 커밋은 수정하지 않았다. 지정 브랜치 clean·push 없음.

2차 최종 확인: 변경 5개 파일의 Windows/WSL SHA256 일치, r2 로그·trace·script 57개 SHA256 보존, 작업 프로세스 0, /home/hep7/worktree/zlink-1674-dotnet 사본 제거 완료. 처리량 측정·push 없음. 마지막 새 커밋 a5f7a5b5fe.

