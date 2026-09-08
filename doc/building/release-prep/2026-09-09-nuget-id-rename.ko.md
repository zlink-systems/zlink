# 2026-09-09 NuGet package ID 변경 기록

## 결과

nuget.org에 게시하는 .NET package ID를 다음과 같이 확정했다. C# namespace,
`AssemblyName`, `RootNamespace`, project 경로는 변경하지 않았다.

| 기존 PackageId | 변경 PackageId |
| --- | --- |
| `Systems.Zlink` | `Zlink` |
| `Systems.Zlink.HttpClient` | `Zlink.HttpClient` |
| `Systems.Zlink.Framework.AspNetCore` | `Zlink.Framework.AspNetCore` |
| `Systems.Zlink.Stream.Connector` | `Zlink.Stream.Connector` |

기존 `Zlink.Framework`, `Zlink.Framework.Contracts`, `Zlink.Framework.Codecs.*`,
`Zlink.Framework.Locations.Redis`, `Zlink.Framework.Provider.Abstractions`는 유지했다.

## 변경 파일

- Package 선언·소비: `bindings/dotnet/src/Zlink/Zlink.csproj`,
  `framework/languages/dotnet/Directory.Packages.props`, framework의 packable project
  3개, framework bench·sample·e2e·test의 `PackageReference`가 있는 csproj 45개,
  `framework/languages/dotnet/nuget.config`.
- Contract: `framework/languages/dotnet/scripts/verify_packaged_contract.sh`,
  `contract/packages/Zlink.Framework.package.txt`, PackageId 기준으로 이름을 바꾼
  `contract/packages/{Zlink.Framework.AspNetCore,Zlink.HttpClient,Zlink.Stream.Connector}.package.txt`.
  `contract/api/Systems.Zlink.*.api.txt`는 AssemblyName 기준이므로 이름과 내용을 유지했다.
- Local package·gate: `scripts/local-package/{README.ko.md,build-windows.ps1,sync-version.py}`,
  `scripts/local-package/dotnet/{build-wsl.sh,fixtures/public-consumer/PublicConsumer.csproj}`,
  `scripts/gate/{common.sh,cross-language-e2e.sh,rebuild-dev.sh}`.
- Framework 실행 보조: `framework/languages/dotnet/{build-windows.ps1,README.md}`,
  `perf/scripts/{dotnet-env.sh,environment.py}`, `samples/run_samples.sh`,
  `e2e/SubmitAdmission/{run_e2e.sh,feature-map.ko.md}`.
- 문서: root·binding README와 binding guide/reference, `framework/doc/**`의 .NET
  package 표기와 contract inventory, `doc/building/`의 dependency boundary,
  release account, binding/framework release note, 이전 release-prep 기록.

## `Systems.Zlink`를 유지한 곳

- C#의 `namespace`, `using`, fully-qualified type 이름은 public source identity이므로 유지했다.
- `AssemblyName`, `RootNamespace`, `InternalsVisibleTo`는 assembly contract이므로 유지했다.
- `Systems.Zlink.Stream.Connector` source/test project 경로와 solution project 이름은
  project identity이므로 유지했다.
- `contract/api/Systems.Zlink.*.api.txt` 파일명·`assembly` 행과 package snapshot 안의
  `lib/net8.0/Systems.Zlink.*.{dll,xml}`은 AssemblyName 기준이므로 유지했다.
- `framework/languages/dotnet/perf/scripts/runner.py`의 `Systems.Zlink` token은 loaded
  assembly 이름을 분류하므로 유지했다.
- `bindings/doc/spec/**`, `doc/plan/**`, `doc/perf/**`와 원칙을 소유하는
  `doc/principal/**`의 표기는 변경 금지 범위이거나 과거 산출물 기록이므로 유지했다.
  `doc/bug/2026-09-07-windows-framework-sample-execution.ko.md`와
  2026-09-08 release-prep의 과거 ticket 산출물 경로도 당시 증적 그대로 유지했다.
- `.github/workflows/release-dotnet.yml`은 지시대로 확인만 했다. 실제 pack/push는
  `*.nupkg` glob을 사용해 PackageId를 하드코딩하지 않는다. 설명과 주석의 예전 이름은
  실행 계약이 아니므로 수정하지 않았다.

활성 csproj의 `PackageId`, `PackageReference`, `PackageVersion`, NuGet source mapping,
local-package·gate의 nupkg/nuspec 경로에는 기존 PackageId가 남아 있지 않다.

## 검증 티켓

| 티켓 | rc | 결과 |
| --- | ---: | --- |
| `0-1788888970-15975-codex-nugetid-sync-version_check` | 1 | consumer용 sync regex의 기존 ID를 발견했다. 규칙 수정 후 재실행했다. |
| `0-1788889003-17469-codex-nugetid-sync-version_check_after_PackageId_rule_` | 0 | Core 0.17.3, binding 0.17.3, 변경 필요 파일 0개. |
| `0-1788889016-18460-codex-nugetid-http-client_dotnet_local_packages_with_Z` | 0 | `Zlink.HttpClient.0.10.0.nupkg`, `Zlink.Stream.Connector.0.10.0.nupkg` 생성. |
| `0-1788889037-19639-codex-nugetid-binding_Release_pack_PackageId_Zlink` | 0 | `/tmp/zlink-codex-nugetid/pkg/Zlink.0.17.3.nupkg` 생성, nuspec ID `Zlink` 확인. |
| `0-1788889054-20723-codex-nugetid-regenerate_binding_local_package_Zlink_I` | 0 | `.artifacts/wsl/nuget/Zlink.0.17.3.nupkg` 재생성. |
| `0-1788889093-22211-codex-nugetid-framework_dotnet_build_with_Zlink_local_` | 1 | package restore/ID는 통과했다. 기존 e2e 26곳의 `ConfigureCoreHwm` CS1061로 전체 build 실패. |
| `0-1788889136-24330-codex-nugetid-verify_packaged_contract_with_split_pack` | 1 | 새 ID·기존 assembly 9개 검증 후 ID 변경에 따른 dependency snapshot 정렬 차이를 발견했다. |
| `0-1788889216-29092-codex-nugetid-generate_review_contract_snapshots_for_r` | 0 | review snapshot 모드에서 package 생성과 source/package API 동일성 통과. |
| `0-1788889353-36210-codex-nugetid-dotnet_sample_regression_tests_with_Zlin` | 0 | 157개 통과. |
| `0-1788889370-37566-codex-nugetid-dotnet_framework_unit_tests_with_Zlink_p` | 0 | 2,025개 통과. |
| `0-1788889597-51004-codex-nugetid-dotnet_all_samples_with_Zlink_package_ID` | 0 | 전체 sample gate 통과. |
| `0-1788890197-77256-codex-nugetid-verify_packaged_contract_after_PackageId` | 1 | 새 ID·기존 assembly 9개 검증 후 기존 license snapshot drift에서 실패. |

마지막 고정 snapshot 검증의 첫 차이는 `Zlink.Framework.AspNetCore`의 기존 snapshot이
MPL-2.0 expression을 기대하지만 현재 csproj 산출물은 `LICENSE` file metadata를 포함한다는
점이다. Review snapshot에는 이 차이 외에 이번 ID 변경과 무관한 HTTP client license와
Framework·Stream Connector public API drift도 있었다. PackageId-only 범위를 지키기 위해
이 metadata/API snapshot은 갱신하지 않았다.

## 정적 확인

- `git diff --check`: 통과.
- 변경한 shell script `bash -n`: 통과.
- 변경한 Python script `py_compile`: 통과.
- `release-dotnet.yml`: pack과 push가 `dist/**/*.nupkg` glob을 사용함을 확인했다.

## 작업 범위 밖 상태

작업 시작부터 있던 untracked benchmark log 디렉터리 3개는 건드리지 않았다. 검증 중
별도 작업이 수정한 `framework/languages/node/package-lock.json`도 이 작업의 변경 목록에서
제외하며 수정하거나 되돌리지 않았다.
