# rel-dotnet 공개 배포 준비 기록

## 변경 파일

- `bindings/dotnet/src/Zlink/Zlink.csproj`
- `bindings/dotnet/Directory.Build.targets`
- `.github/workflows/release-dotnet.yml`
- `doc/building/release-prep/2026-09-08-rel-dotnet.ko.md`

## 핵심 변경

- 패키지 ID `Systems.Zlink`와 버전 `0.17.3`은 유지했다.
- NuGet 작성자와 assembly 회사명을 `zlink`로 설정했다.
- MPL-2.0 license expression, 프로젝트·저장소 URL, git 저장소 형식, 패키지 README를 설정했다.
- `Microsoft.SourceLink.GitHub` 8.0.0을 build 전용 dependency로 추가하고 저장소 URL 공개 및 untracked source embedding을 설정했다.
- `IncludeSymbols=true`, `SymbolPackageFormat=snupkg`를 설정했다.
- `CI=true`일 때만 deterministic build와 continuous integration build를 활성화한다.
- 패키지 README는 기존 `bindings/dotnet/README.md`를 nupkg 루트에 포함한다.
- 저장소 안에서 .NET 패키지 아이콘 파일을 찾지 못해 `PackageIcon`은 추가하지 않았다. 새 이미지도 만들지 않았다.
- 새 `release-dotnet.yml`은 `dotnet/v*` tag push와 필수 `version` 입력의 수동 실행을 지원한다. 입력·tag 버전, `BINDINGS_VERSION`, csproj 버전과 Core 버전을 검증하고 Core release artifact를 검증한 뒤 build, test, nupkg·snupkg pack을 수행한다.
- 워크플로우 권한은 `id-token: write`, `contents: read`만 지정했다. `NuGet/login@v1`의 `user: zlink`로 단기 API key를 받은 뒤 nupkg와 snupkg를 nuget.org에 각각 `--skip-duplicate`로 전송한다. environment와 장기 NuGet secret은 참조하지 않는다.

## 검증

- 중간 pack 티켓 `0-1788855009-91538-codex-rel-dotnet-rel-dotnet_Release_pack_and_NuGet_metada`: rc=0. 조건부 Authors가 SDK 기본값을 덮지 못해 nuspec에 `Systems.Zlink`가 기록되는 것을 발견했고 설정을 수정했다. 로그: `.artifacts/perf-queue/log/0-1788855009-91538-codex-rel-dotnet-rel-dotnet_Release_pack_and_NuGet_metada.log`.
- 최종 pack 티켓 `0-1788855155-98826-codex-rel-dotnet-rel-dotnet_corrected_Release_pack_and_Nu`: rc=0. `/tmp/zlink-codex-rel-dotnet/packages/Systems.Zlink.0.17.3.nupkg`와 `.snupkg`를 생성했다. nuspec에서 ID `Systems.Zlink`, version `0.17.3`, authors `zlink`, MPL-2.0, README, project URL, git repository URL·commit을 확인했다. nupkg에서 README, Core provenance와 Linux x64 native runtime 3개를 확인했고 snupkg에서 PDB를 확인했다. 로그: `.artifacts/perf-queue/log/0-1788855155-98826-codex-rel-dotnet-rel-dotnet_corrected_Release_pack_and_Nu.log`.
- assembly 생성 파일에서 `AssemblyCompanyAttribute("zlink")`를 확인했다.
- `actionlint`는 머신에 설치되어 있지 않았다. 대체 YAML parse 티켓 `0-1788855329-12190-codex-rel-dotnet-rel-dotnet_workflow_YAML_parse_validatio`: rc=0. 로그: `.artifacts/perf-queue/log/0-1788855329-12190-codex-rel-dotnet-rel-dotnet_workflow_YAML_parse_validatio.log`.
- `git diff --check`: 통과.

## 사용자가 직접 해야 할 일

1. 감독자가 `.github/workflows/bindings-release.yml`에서 `dotnet/v*` tag trigger와 기존 `release-dotnet` job을 제거하고, 수동 target 안내·검증에서도 `dotnet`을 제거한다. 이 정리가 먼저 완료되어야 같은 tag에서 두 워크플로우가 동시에 publish하지 않는다.
2. 감독자가 이 job의 diff와 위 충돌 제거 diff를 검토하고 commit·push한다.
3. nuget.org Trusted Publishing 정책 `zlink-dotnet-release`가 owner `zlink-systems`, repository `zlink`, workflow 파일명 `release-dotnet.yml`, environment 없음으로 유지되는지 확인한다.
4. tag 배포는 사용자가 `dotnet/v0.17.3` tag를 push한다. 수동 배포는 `release-dotnet.yml`을 version `0.17.3`으로 실행한다. 어느 방식이든 실제 publish 전에 하나만 선택한다.

이 워크플로우에 필요한 repository secret은 없다. `NUGET_API_KEY`와 `NUGET_SOURCE_URL` secret을 만들거나 설정하지 않는다. `NUGET_API_KEY`라는 이름은 `NuGet/login@v1`이 실행 중에 내보내는 단기 output으로만 사용한다.

## 미해결 사항

- 패키지 아이콘 파일이 없으므로 이번 nupkg에는 icon이 없다. 아이콘을 제공할지는 감독자가 결정해야 한다.
- 기존 `bindings-release.yml` 정리 전에는 `dotnet/v*` tag가 기존 workflow와 새 workflow를 모두 시작한다.
- 로컬 저장소의 git remote는 GitHub 기본 host가 아닌 SSH alias `github.com-zlink`를 사용하며, 로컬 pack은 빈 SourceLink map 경고를 냈다. nuspec의 repository URL과 commit은 정상이다. GitHub Actions의 checkout remote에서는 기본 GitHub host를 사용하므로 실제 release workflow에서 SourceLink warning과 PDB source map을 다시 확인해야 한다.
