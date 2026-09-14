# 네 언어 메시징 최적화 handoff와 진행 계획

이 문서는 새 세션의 감독 에이전트가 현재 worktree에서 미완료 최적화를 이어가기 위한 기록이다.
공개 계약이나 전체 완료 보고가 아니다. 사용자의 새 세션 전환 요청으로 신규 작업을 중지했다.

## 0. 중단 (2026-09-14)

사용자 결정으로 이 캠페인은 중단한다. 아래 §1~§8의 남은 계획은 실행하지 않는다.
중단 시점 기준 남은 작업은 약 45~55 단위(언어 × 기능 단계)로, 집중 작업 2~3주 규모였다.
§6-7의 "Core 대비 10–15%" 목표는 달성하지 못했고 달성 시도도 하지 않는다.

이 커밋은 중단 시점의 worktree 스냅샷이다. 검증된 변경과 진단용 잔재가 섞여 있으며
파일별 채택·폐기 판정을 하지 않았다. 이 브랜치를 main에 머지하려면 그 판정을 먼저 해야 한다.
머지 계획이 없다면 판정도 필요 없다.

진단 worker 단계에서 관측한 backpressure 90% 하락은 제품 결함이 아니다.
`MessagingFixedRuntimeLevel`·`MessagingNoHostPermit`은 `tests/Zlink.Framework.UnitTests`에만
존재하고 production `src`에는 없다. 실제 Framework dispatch 경로에는 그 worker 전달이 없으므로
재현 대상이 아니며 후속 이슈로 남기지 않는다.

이미 main에 반영된 이 캠페인의 성과는 그대로 유효하다:
Core HWM 언더플로 수정, Java peer 분류 stream 제거, C++ codec owner move, .NET completion owner.

## 1. 작업 위치와 시작 절차

- 저장소: `/home/hep7/project/zlink`
- 승인된 브랜치: `bench/319-unified-runner-contract`. Main 전환·merge·force push 금지.
- 측정과 공통 적용 지침의 기준 문서:
  `framework/bench/grpc/dotnet/Diagnostics/measurements.ko.md`
- 원래 goal 전문:
  `/home/hep7/.local/share/orca/codex-accounts/77d59aab-48ea-48cb-894e-e9430b6ce742/home/attachments/378b1ae7-700d-47c8-9f5c-63c04f245bc7/pasted-text-1.txt`

먼저 root와 대상 디렉터리의 `AGENTS.md`, POSDDD·system design 원칙을 읽는다.
`git branch --show-current`, `git status --short`, 실제 process와 artifact를 확인한다.
아래 과거 agent 이름이나 session ID가 새 세션에서도 유효하다고 가정하지 않는다.
Worktree에 미커밋 runtime·test·benchmark 변경이 많고 사용자의 문서 archive 이동도 있다.
`git add -A`, reset·restore·branch 전환으로 정리하지 않는다.

## 2. 유지할 전체 목표와 운영 규칙

C++, .NET, Java, Kotlin 모두 각각 해당 언어 Core/binding benchmark와 비교한다.
불필요한 할당·복사·경합·thread 전환·I/O 대기를 제거하고 POSDDD 구조 개선을 적용한다.
Core/binding을 변경해 Framework 비용을 보상하지 않는다.

단계는 Core-like → Codec → Envelope → Wire → Mailbox → 나머지 필수 기능 순서로 누적한다.
네 언어의 같은 단계 구현·측정·최적화·계약 검토가 끝난 뒤 다음 기능을 추가한다.
이미 검증된 .NET 단계와 원자료는 재사용한다. 현재는 다른 언어의 Core-like/Codec 정렬과
.NET worker 손실 원인 분리를 진행하며 다음 기능 추가를 보류했다.

- 감독은 설계·진단 승인·리뷰·spec 대조·교차언어 비교·문서를 담당한다.
- Sub-agent는 구현·관련 test·측정을 담당하며 root AGENTS의 모델 선택 규칙을 따른다.
- 독립 구현·작은 빌드/test는 자원 확인 후 병행한다. Perf는 다른 빌드/test/perf와 겹치지 않는다.
- Request는 실제 64 B 요청 / 4,096 B 응답, send는 실제 4,096 B 단방향이다.
- 새 측정은 warmup **2초**, active **5초**, runs **1회**. 저장된 3초/10초 결과는 재측정하지 않는다.
- 표는 실제 message/s와 `100 × (1 - 현재 / 직전)` 감소율을 사용한다. 감소율을 합산하지 않는다.
- 진단용 누적 경로와 실제 public Framework 전체 경로를 구분한다. 아직 전체 경로 검증은 미완료다.
- 오류·누락·abandoned·drain 실패는 정상 비교에서 제외한다. 관측된 증가는 증가로 보고하되
  단일 실행만으로 원인·음수 비용·임의의 noise 허용치를 확정하지 않는다.
- Runtime 수정 전 file:line·소유 계층·spec·교차언어·A/B/C/D를 보고하고 감독의 A/B 승인 뒤 구현한다.
- 수신 수집 지연, retry·timeout/budget 증가, 두 번째 poller, quota 중복, API·wire 변경으로 우회하지 않는다.
- 보호된 spec/internals/sample/e2e 문서는 이번 승인 범위가 아니다.

## 3. 완료된 측정과 해석

세부 이전 누적 단계와 원자료는 measurements 문서를 참조한다. 아래 값은 모두 진단 경로다.

### .NET worker 원인 분리

| 단계 | Serial | Backpressure | Send |
| --- | ---: | ---: | ---: |
| Worker 추가 전, permit static callback | 6,752.2 | 164,184.7 | 417,300.3 |
| Worker handoff | 5,898.4 | 16,299.4 | 314,568.9 |
| 전용 수신 작업 | 5,717.6 | 17,444.2 | 266,625.7 |
| Global lane drain | 5,640.2 | 18,525.0 | 269,106.0 |
| Channel 전달 진단 | 5,782.2 | 19,217.2 | 187,388.4 |

단위는 message/s다. 첫 세 행은 3초/10초, 마지막 두 행은 2초/5초이며 각각 runs=1이다.
전용 수신 작업에서 BP +7.0%, Global → Channel에서 BP +3.7%가 관측됐다.
큰 손실은 회복되지 않았고 Channel send는 직전 대비 30.4% 감소했다.
Mailbox 전달만 제거하면 회복된다는 가설은 지지되지 않는다. 유지한 permit·cross-thread reply나
binding 중 어느 하나가 주원인이라고 확정하지도 않았다. Channel은 Runtime 대체안으로 미채택이다.

`ZLinkStateLane`의 기존 `preferLocal: true → false`는 감독 승인된 B 후보로 측정했지만
최종 채택·Runtime 커밋은 미완료다. FIFO·단일 처리권은 검사했으며 경합 중 close/cancel 전체나
scheduler stall 원인을 입증한 test로 확대 해석하지 않는다.

### C++ Core-like/Codec

| 단계 | Serial | Backpressure | Send |
| --- | ---: | ---: | ---: |
| Core-like | 8,925.8 | 371,608.8 | 541,708.4 |
| + Codec | 8,631.8 (3.3%) | 343,616.4 (7.5%) | 511,722.6 (5.5%) |

2초/5초/runs=1이며 괄호는 직전 대비 감소율이다. 양쪽 모두 실제 BenchPayload protobuf를
매 메시지 직렬화·역직렬화한다. Codec만 실제 serializer registry와 extension을 사용한다.
Finite warmup·active·drain·graceful close 6/6 및 perf 6개 항목 오류·누락 0을 확인했다.
기존 completion 대기와 settle 규칙은 공통 helper로 이동했고 50 ms/200 ms/30초 값은 유지했다.
다음 단계 target은 아직 등록하지 않고 현재 CMake/runner는 Core-like·Codec만 허용한다.

## 4. 언어별 현재 변경과 검증 상태

### .NET

현재 작업 source는 `framework/bench/grpc/dotnet/Diagnostics/MessagingFeatureRamp.cs`,
test `.csproj`, `Diagnostics/run_optimization.sh`다. 이들은 이전 단계 설정도 포함한다.
기존 .NET runtime/test 변경 전체를 이번 새 control만의 변경으로 간주해 일괄 커밋하지 않는다.

Channel control은 실제 cross-thread worker·fresh Received·reply owner·permit을 유지한다.
이미 queued인 Count snapshot 최대64개만 기존 CanAdd/batch로 이동한다. Byte credit을
publication 전에 늘리고 dequeue·거부·join 후 잔여 owner 정리에서 한 번 반환한다.
83개 관련 test와 실제 socket lifecycle 3개, fidelity와 동일 DLL cohort 검증을 통과했다.

다음 no-host-permit Channel control은 **구현·빌드·83개 test·lifecycle 3개·fidelity 완료**,
**perf 미실행·미승인**이다. Compile-fixed `MessagingNoHostPermit=true`를 기존 Channel/dedicated
level3에만 적용한다. 실제 property와 guard는 `.csproj`에서 다시 확인한다.
Info는 `noPermitExplicit=true`, `permitBatchSize=0`,
`capacityComparison=host-permit-removed-no-replacement-bound`를 표시한다.
Host permit을 제거한 unbounded Channel은 production 계약 준수 수정안이 아니다.
새 quota/bounded Channel로 조건을 맞추지 않았다. Send의 queued 메모리 증가 가능성이 있으므로
다음 감독이 수용량 차이와 외부 RSS 안전 중단 계획을 검토한 뒤에만 측정을 승인한다.
안전 중단한 실행은 invalid이며 정상 처리량으로 쓰지 않는다. 관측된 source peak16,385를
hard bound로 가정하지 않는다.

Driver의 Framework DLL은 저장된 global baseline과 정확히 같고 Core/native/binding은 미변경이다.
문서 SHA만 다른 Shared·Contracts·Protobuf codec DLL은 public signature·assembly identity·PDB
source hash 일치 확인 뒤 artifact에만 저장된 cohort DLL을 사용했다. Framework DLL 교체는 없다.
빌드는 baseline `SourceRevisionId=cf735abbe07ac9f50e8b8e805628d6b5ce0f5709`로 metadata를 맞췄다.

과거 permit-off worker에도 약89% 손실이 있지만 현재 cohort·조건과 같지 않아 정량 baseline으로
재사용하지 않는다. 현재 no-permit control은 같은 cohort에서 host permit 기여를 분리하기 위한 것이다.
Managed receive/reply lock을 같은 장기 lock이라고 단정할 증거는 없다. Native 대기 귀속도 미확정이다.

### C++

세션 종료 시 커밋한 B 변경(`fcada59778`)은 `framework/include/zlink/framework/contracts/codecs/serializer.hpp` +9줄과
`tests/Zlink.Framework.UnitTests/test_cpp_framework_serializer_registry.cpp` +72줄뿐이다
(두 경로 모두 `framework/languages/cpp/` 기준).
Private rvalue bridge가 owned 임시 native message를 clone 대신 이동한다. Borrowed·const 동작은 유지한다.
4 KB memcpy 제거가 아니라 native owner/refcount 복제 제거다.
소유 계층은 Framework codec bridge, spec은 payload ownership §1·§2·§5이며 .NET의 직접 native owner
반환과 대조했다. Java/Kotlin byte-array mapping에 그대로 적용하지 않는다. 분류는 B다.
소유 규칙은 borrowed materialization·const clone 유지 / owned 임시 move이며 새 상태·quota는 없다.

기존 unit build `framework/languages/cpp/build/linux-ninja-release`에서 focused target 빌드82/82가 통과했다.
최종 focused CTest 결과는 아래 세션 종료 검증 항목에 기록한다. 새 Codec 빌드·finite·perf는 미실행이다.
Core/binding package0.18.0은 변경하지 않았다. 저장된 이전 diagnostic 바이너리를 덮어쓰지 말고
새 Codec-only build를 별도로 준비하여 동일 cohort와 fidelity를 검증한다. Core QPS는 재사용한다.

### Java/Kotlin

Owner-hosted test-only `ZLinkBenchCodecBridge.Session`을 구성했다. 실제 frozen
`ZLinkCodecRegistration`의 Composite declared-type send와 strict content-type receive를
Java source·target과 Kotlin source에서 사용한다. Kotlin source의 target은 공유 Java fixture다.
Dummy SerializerCapture는 제거했다.
Bridge는 production jar/module exports/API에 포함하지 않는다.
Generated BenchPayload는 bench Shared jar 하나를 재사용한다. Build 순서는 shared jar →
core diagnostic jar → bench installDist이며 reciprocal composite include나 proto 재생성은 없다.
Core diagnostic input은 절대 경로를 먼저 검사하고 diagnostic compile만 classpath 모드로 사용한다.

RawStack의 asymmetric/codec adapter는 기존 RawOperation·admission·completion·poller를 유지하고
실제 4,096 B reply body와 metric header를 검사한다. Generic operation은 기존 completion progress를
우회했으므로 폐기했다. 새 poller·retry·budget은 없다.
Shared·owner jar와 installDist 빌드는 통과했지만 Java/Kotlin×Core/Codec×3패턴 finite 전체는 미완료다.
초기 finite 시도는 30초 wrapper가 준비 빌드 단계에서 종료한 것으로 functional/perf 실패가 아니다.
기존 runner의 build와 실행을 분리하여 새 세션은 준비된 distribution을 재사용해야 한다.
유효한 JVM 처리량은 아직 없다. Kotlin 전체 public facade 완료로 해석하지 않는다.

## 5. 재사용할 원자료

아래 경로는 저장소 root 기준이고 artifact는 Git에 포함하지 않는다.

| 자료 | 경로 |
| --- | --- |
| .NET global 기준 | `.artifacts/mailbox-staged-optimization/worker-global-drain/summary.json` |
| .NET Channel 측정 | `.artifacts/mailbox-staged-optimization/worker-channel-handoff/summary.json` |
| .NET 패턴 raw | 위 출력 아래 `fixed-worker-{global-drain,channel-handoff}/zlink-dotnet-{패턴}-4096/results.json` |
| .NET no-permit 준비 driver | `.artifacts/mailbox-staged-optimization/worker-channel-no-permit-driver/` |
| .NET no-permit 검증 | 같은 parent의 `worker-channel-no-permit-{build,tests,lifecycle,info,fidelity,overlay}.log`, fidelity JSON·cohort SHA·lifecycle command |
| C++ 측정 | `.artifacts/cpp-diag-stage-core-codec-w2-a5-r1-20260914/{core,codec}/{패턴}/{results,summary,target-stats}.json` |
| C++ finite | `.artifacts/cpp-diag-lifecycle-selfcheck-aligned-20260914/` |
| C++ owner 빌드 | `.artifacts/cpp-codec-owner-build.log` |
| JVM 준비 | `/var/tmp/zlink-jvm-owner/{shared,core}/`, 중단 run `/var/tmp/zlink-jvm-finite/core-java/` |

.NET Channel ticket `2-1789365808-2212191-codex-worker-channel-handoff-isolation`과 C++ ticket
`1-1789365372-2186505-cpp-stage-CPP_aligned_core_codec_protobuf_w2_a5_r1`은 terminal rc0이다.
Ticket log는 `.artifacts/perf-queue/log/`에 있다. Lock 파일만으로 실행 중이라고 판단하지 않는다.

## 6. 다음 작업 순서와 완료 조건

1. Branch·dirty 범위·process·원자료를 확인하고 언어별 agent를 새로 배정한다. 과거 agent 이름은
   cpp_stage_alignment, dotnet_worker_diagnosis, measurement_doc_review(JVM 담당)였다.
2. C++ focused CTest 종료 검증을 확인한다. 별도 Codec-only diagnostic 준비 → fidelity·finite3패턴
   → 감독 리뷰 → 독점 perf2/5/1 → 실제 QPS·CPU·RSS·bandwidth와 owner clone 감소 근거를 기록한다.
3. JVM prepare/skip-build 구분을 확인하고 Core/Codec distribution을 한 번 준비한다.
   Java/Kotlin×두 단계×세 패턴 finite·warmup·active·drain·close·fidelity를 끝낸 뒤 독점 perf를 진행한다.
4. .NET no-permit 수용량·RSS 안전성을 검토한 뒤 기존 Channel 결과와 새 control만 비교한다.
   결과에 따라 public binding-only cross-thread reply repro나 실제 대기 trace를 선택한다.
   Native 버그를 단정하거나 Core 수정으로 보상하지 않는다.
5. Codec 단계 최적화·계약 검토를 네 언어에서 닫은 뒤 Envelope → Wire → Mailbox를 진행한다.
   .NET 기존 값은 보존하고 효과 있는 구조 개선의 교차 적용만 검토한다.
6. 남은 수신/reply 소유권·host permit·worker·handler/DI·cancellation/deadline·route/lifecycle을
   하나씩 누적해 측정·최적화·spec/POSDDD 리뷰한다. Public JSON 기본 serializer 계약은 유지한다.
7. 네 언어 실제 public Framework 전체 경로를 세 패턴에서 검증하고 관련 test → 최종 gate를
   완료한다. Core 대비 오버헤드10–15% 이내 목표를 달성하지 못하면 실제 남은 비용·한계를
   명시한다. 최종 전체 검증은 아직 미완료다.
8. 기존 measurements 문서를 공통 지침·언어별 차이·재현 절차 중심으로 정리한다.
   좁은 검증 범위별 커밋·push와 원자료 확인 후에만 전체 완료를 선언한다.

## 7. 새 세션 시작 프롬프트

> 현재 branch/worktree를 보존하고 이 handoff와 원래 goal, root/하위 AGENTS를 읽어라.
> 감독은 리뷰·문서·진단 승인을, sub-agent는 실제 구현·test·perf를 담당한다.
> 네 언어 모두 같은 기능 단계로 맞추고 기존 유효한 측정은 재사용하라.
> 새 측정은 request64 B/response4 KB, send4 KB, warmup2초/active5초/runs1이다.
> Perf를 다른 실행과 겹치지 말고, 먼저 미완료 finite·cohort·수용량 검증부터 진행하라.
> Runtime 변경은 소유 spec·교차언어·A/B 승인 뒤에만 구현한다.
> 진단 prototype 완료를 실제 Framework 전체 완료로 바꾸지 말고 전체 목표를 유지하라.

## 8. 세션 종료 검증

- .NET: 모든 build/test/lifecycle/perf handle terminal. No-permit perf 미실행.
- C++: owner build82/82·focused CTest1/1 모두 terminal rc0. CTest log는
  `.artifacts/cpp-codec-owner-ctest.log`이며 새 Codec diagnostic/perf는 미실행이다.
- JVM: 종료 직전 실제 process와 agent 보고 모두 실행 handle 없음.
  Runner의 `--prepare/--skip-build` 분리는 아직 미적용이다. Core installDist만 준비됐고
  Codec installDist·finite12개·perf 전체가 남아 있다.
- 주요 커밋: `225a5aa877`(Channel 측정 문서), `01e0a375db`(C++ 진단 코드),
  `fcada59778`(C++ codec owner move와 관련 테스트). 이 handoff를 마지막 별도 커밋으로 저장한다.
- 사용자 archive 이동과 검증되지 않은 기존 변경은 그대로 보존한다. 전체 최적화 goal은 미완료다.
