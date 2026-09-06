# F-R7-4 — .NET shutdown publication terminal 처리

## 결과와 계약

`MarkDraining`, `QuiesceServingChannels`, `CleanupOwner`가 첫 terminal을 한 번 소비한다.
게시의 `false`와 예외는 `DrainingStatePublishFailed`, owner cleanup 예외는
`OwnerCleanupFailed`로 끝난다. 공개 shutdown 결과는 기존 매핑대로
`ForceStopped/TeardownFailed`다. 성공은 `Stopped/None`이다.

- 소유 계층: Framework host lifecycle의 `ZLinkFrameworkDrainExecutor`.
- Spec 조항: [Host relocation flow §14](../../../framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md) step 2의 게시 terminal 소비·전파 시간 대기 금지, step 5의 정리 순서, step 6의 bounded termination 결과. [.NET exact interface](../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md)의 termination enum을 유지한다.
- 교차언어 대조: Java/Kotlin과 첫 terminal 처리 의미를 맞춘다. 현재 Node 소스에는 같은 재시도가 남아 있다. 아래 대조 근거 참조.
- 변경 분류: **B — 기존 결함**, 감독자가 확인한 D-106/F-R7-4 구현 지시 범위.
- 수정 전/후 규칙 수: 대상 단계의 terminal 뒤 처리 정책 **2 → 1**. 성공/실패별로 다음 단계 또는 시간 대기·재제출을 결정하던 정책을 첫 terminal 결과 처리로 통일한다. 대상 재시도 루프 **3 → 0**, 새 runtime 상태·timer·public API **0**.

## 원인과 수정 파일

기준 revision: `020c4ea99cbd9df7cd70c26b137889f0962cd468`.
다음 원인 위치는 수정 전
`framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkDrainExecutor.cs` 기준이다.

| 원인 위치 | 확인한 결함 |
|---|---|
| `:512` (`PublishDrainingMarkerAsync`), `:532` | `false` 또는 예외 뒤 `PollingInterval`만큼 기다리고 다시 게시한다. |
| `:569` (`PublishServingWeightAsync`), `:577` | `false` 뒤 기다리고 재제출한다. 예외는 coordinator의 일반 실패 경로로 나가 내부 reason도 `TeardownFailed`가 된다. |
| `:541` (`CleanupOwnerAsync`), `:559` | owner cleanup 예외를 삼키고 시간 대기 뒤 다시 호출한다. |
| `:480` (`ForceStopAsync`) | 정상 drain에서 이미 실패한 owner cleanup을 강제 teardown에서도 다시 호출한다. |

변경 파일:

- [ZLinkFrameworkDrainExecutor.cs](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkDrainExecutor.cs): 세 작업에서 시간 대기와 재제출을 제거한다. 첫 실패를 기존 `ZLinkDrainForceException`의 `failures` 인자에 담고, `ExecuteWithProgressAsync`에서 원인 예외를 로그에 남긴 뒤 기존 force reason을 반환한다. `false`는 작업을 명시한 `InvalidOperationException`으로 기록한다. 강제 teardown은 기존 `OwnerCleanupFailed` reason으로 이미 소비한 cleanup terminal을 판별하고 남은 resource 정리를 계속한다.
- [DrainCoordinatorTests.cs](../../../framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/DrainCoordinatorTests.cs): 첫 terminal 회귀 7건을 추가하고, 재시도를 요구하던 기존 2건을 호출 1회·시간 상한·정확한 순서 단언으로 교체한다.
- 이 보고서.

현재 실패 소비 위치는 executor `:290`, 강제 teardown의 cleanup terminal 판별은 `:478`,
세 작업은 각각 `:518`, `:532`, `:545`다. 공개 결과 매핑은
`ZLinkFrameworkMaintenanceRuntime.cs:505`를 그대로 사용한다.

실패 전달 대안은 executor에 새 누적 상태를 두는 방법과 기존 예외의 failure 목록을 사용하는
방법을 비교했다. 기존 `ForceStopAsync`의 목록은 지역 변수이므로 정상 drain과 공유되지 않는다.
새 field를 추가하지 않고 기존 `ZLinkDrainForceException`과 executor logger에서 실패를
소비하는 방법을 택했다. 원인 예외의 identity까지 회귀 테스트로 확인한다.

## 교차언어 및 sample 대조

- Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:2095`: `runDrain`은 `markDraining`의 `whenComplete`에서 첫 실패를 `DRAINING_STATE_PUBLISH_FAILED`와 원인 예외로 넘긴다. `:2248`의 cleanup도 `closeAsync`의 첫 실패를 `OWNER_CLEANUP_FAILED`로 끝낸다.
- Kotlin `framework/languages/java/zlink-framework-kotlin/build.gradle.kts:14`: `zlink-framework-core`를 사용하므로 같은 lifecycle 구현을 공유한다.
- Node `framework/languages/node/packages/framework/src/runtime/host/index.ts:1883`, `:1898`: 현재 작업 공간에는 `publishHostDraining`의 `while`과 `cleanupOwnerForDrain`의 `for (;;)`가 남아 있다. 작업 지시의 Node parity 설명과 현재 소스가 다르다. 언어 구조상 필요한 차이가 아니라 같은 종류의 기존 결함이며, 이번 .NET 변경 범위 밖이다.
- `DrainingStatePublishFailed`를 .NET 테스트와 ZoneWorld·ShoppingMall sample에서 검색했다. 발견한 테스트는 `DrainCoordinatorTests.cs`뿐이다. sample과 SampleRegression 테스트에는 해당 reason 기대치가 없다. sample 수정 및 runner 실행은 하지 않았다.

## 회귀 테스트

테스트 파일은 `Runtime/DrainCoordinatorTests.cs`다.

| 테스트 | 검증 |
|---|---|
| `Shutdown_Publication_Failure_Consumes_First_Terminal` (`:792`, 4건) | MarkDraining/serving weight × `false`/예외. 첫 호출만 실패하고 이후에는 성공하는 store operation 대역을 사용한다. 내부 reason, 호출 1회, accepted work 단계 미진입, 강제 resource 정리와 실패 원인 보존을 확인한다. |
| `Shutdown_Success_Consumes_First_Terminals_Without_Polling` (`:838`) | 공개 `Stopped/None`, 내부 `Drained`, 게시·cleanup 각 1회와 기존 순서를 확인한다. |
| `Shutdown_Owner_Cleanup_Failure_Consumes_First_Terminal` (`:873`, 2건) | 첫 cleanup의 동기/비동기 예외. 공개 `ForceStopped/TeardownFailed`, 내부 `OwnerCleanupFailed`, 강제 teardown을 포함한 cleanup 1회와 원인 예외 보존을 확인한다. |
| 기존 실패 테스트 (`:750`, `:768`) | 영구 `false`도 첫 결과로 끝나며 호출 1회와 seal 이후 단계 순서를 정확히 단언한다. |
| 기존 성공·미완료 publication 테스트 (`:20`, `:702`) | shutdown 정리 순서와 publication이 미완료인 동안 다음 단계에 진입하지 않는 계약을 유지한다. |

첫 terminal 회귀 9건은 `Stopwatch.GetTimestamp/GetElapsedTime`으로 경과 시간을 측정하고
shutdown이 `PollingInterval`(2초)보다 빨리 끝나는지 단언한다. 5초 shutdown deadline은
polling보다 길어서 재시도가 deadline 취소로 숨지 않는다. production timeout과 budget은
변경하지 않았다.

## Gate 결과

로그와 TRX: `/tmp/zlink-dotnet-r7.MLmOsW/`.

- 수정 전 게시 회귀: **4/4 실패** (`red.log`, `red.trx`). `false`는 약 2초 대기하고, marker 예외는 재시도로 `Drained`가 되며, serving-weight 예외는 내부 reason이 `TeardownFailed`가 된다.
- 관련 `DrainCoordinatorTests`: **55 passed, 0 failed** (`focused.log`, `focused.trx`).
- 최종 첫 terminal 회귀: **9 passed × 5회, 0 failed** (`regression-1`부터 `regression-5`의 `.log`/`.trx`). 각 TRX의 가장 긴 테스트 전체 시간은 23.6–26.2ms다. shutdown 경과 시간 자체의 기준은 테스트 안의 monotonic 단언이다.
- 전체 unit gate: **2004 passed, 0 failed, 0 skipped**, 2분 56초 (`unit-gate.log`, `unit-gate.trx`). 한 번 실행했다. 요청에 적힌 baseline은 1981건이고 이번 추가는 7건이며, 현재 checkout의 전체 inventory로 검증했다.
- `git diff --check`: 통과.

SDK는 `8.0.130`, package는 `Systems.Zlink.0.17.0.nupkg`, SHA-256은
`350b8b789a1b31328bd477d895283efc6b986a1b242c310eadda914cf79c98c3`이다.
모든 테스트에 다음 환경과 lock을 사용했다. 전체 gate에는 로그 보존용
`--logger 'trx;LogFileName=unit-gate.trx' --results-directory /tmp/zlink-dotnet-r7.MLmOsW`
옵션만 추가했다.

```bash
export TMPDIR=/dev/shm/zlink-tmp-dotnet
export ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib
export UseSharedCompilation=false MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
pkg_hash=$(sha256sum .artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg | awk '{print $1}')
export NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-${pkg_hash:0:16}
cd framework/languages/dotnet
flock -w7200 /tmp/zlink-dotnet-gate.lock dotnet test tests/Zlink.Framework.UnitTests --blame-hang --blame-hang-timeout 10m
```

반복 회귀의 filter는 `FullyQualifiedName~Consumes_First_Terminal`이다.

## BLOCKERS

.NET 작업의 blocker와 남은 테스트 실패는 없다. 범위 밖 Node의 남은 재시도는 위 위치와 함께 감독자에게 보고한다.
기존 다른 언어·binding 변경은 보존했으며, commit은 하지 않았다.
