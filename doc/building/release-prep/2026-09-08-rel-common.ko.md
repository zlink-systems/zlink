# rel-common 공개 배포 준비 결과

## 변경 파일 목록

### 배포 workflow와 문서

- `.github/workflows/framework-release.yml`
- `doc/building/release-accounts.ko.md`, `doc/building/release-accounts.md`
- `doc/building/packaging.ko.md`, `doc/building/packaging.md`
- `doc/building/release-notes/bindings-0.17.3.ko.md`, `bindings-0.17.3.md`
- `doc/building/release-notes/framework-0.10.0.ko.md`, `framework-0.10.0.md`
- `doc/building/release-prep/2026-09-08-rel-common.ko.md`
- Java Central publication: `framework/languages/java/build.gradle.kts`,
  `framework/languages/java/scripts/upload-central-bundle.sh`

### Framework binding 0.17.3 의존

- C++: `framework/languages/cpp/CMakeLists.txt`, `build-windows.ps1`,
  `samples/run_samples.ps1`, `samples/sample-build-common.sh`,
  `tests/Zlink.Framework.PackageTests/stream_connector_consumer.cmake`
- Java: `framework/languages/java/gradle/libs.versions.toml`,
  `e2e/SubmitAdmission/Role/build.gradle.kts`, `e2e/SubmitAdmission/run_e2e.sh`,
  `bench/with-grpc/run_local.sh`, `run_local_kotlin.sh`, Java/Kotlin bench client 2개
- Node.js: `framework/languages/node/package.json`, `package-lock.json`,
  `packages/framework/package.json`, `packages/framework-locations-redis/package.json`,
  `test/contract/fixtures/node-public-contract.json`, `scripts/verify_packaged_contract.sh`
- .NET: `framework/languages/dotnet/Directory.Packages.props`,
  `perf/scripts/dotnet-env.sh`, `perf/scripts/environment.py`, package contract snapshot 2개

C++ vcpkg manifest 3개에는 binding package 의존 선언이 없고 framework 자체 버전
`0.10.0`만 있으므로 변경하지 않았다. Framework 자체 버전도 0.10.0으로 유지했다.

### .NET framework package ID와 메타데이터

- package project:
  - `src/Zlink.HttpClient/Zlink.HttpClient.csproj`
  - `src/Zlink.Framework.AspNetCore/Zlink.Framework.AspNetCore.csproj`
  - `src/Systems.Zlink.Stream.Connector/Systems.Zlink.Stream.Connector.csproj`
- friend assembly 정합:
  - `src/Zlink.Framework.Contracts/Properties/AssemblyInfo.cs`
  - `src/Zlink.Framework/Properties/AssemblyInfo.cs`
- local package/assembly 검사 정합:
  - `nuget.config`, `build-windows.ps1`
  - `tests/Zlink.Framework.UnitTests/Runtime/MaintenanceRuntimeTests.cs`
- package 소비 참조: `samples/{DeliveryDispatch,GameQuest,ShoppingMall,TicTacToe}/**/*.csproj`,
  `e2e/{LocationMessaging,ObservabilityOps,PubSub,RegistrationCodec,ResilienceLifecycle,RuntimeMonitoring,SpotActorTransfer,SpotService,StoreFailure,SubmitAdmission,ToActorMessaging}/**/*.csproj`
- 검증기: `scripts/verify_packaged_contract.sh`
- README: `framework/languages/dotnet/README.md`
- contract rename:
  - `contract/api/Zlink.HttpClient.api.txt` → `contract/api/Systems.Zlink.HttpClient.api.txt`
  - `contract/api/Zlink.Framework.AspNetCore.api.txt` → `contract/api/Systems.Zlink.Framework.AspNetCore.api.txt`
  - `contract/packages/Zlink.HttpClient.package.txt` → `contract/packages/Systems.Zlink.HttpClient.package.txt`
  - `contract/packages/Zlink.Framework.AspNetCore.package.txt` → `contract/packages/Systems.Zlink.Framework.AspNetCore.package.txt`
  - `contract/packages/Systems.Zlink.Stream.Connector.package.txt`,
    `Zlink.Framework.package.txt`, `Zlink.Framework.Contracts.package.txt` 갱신
- package 이름 문서: `framework/doc/framework/**`의 getting-started, HTTP client,
  .NET interface, E2E package 표 한·영 문서와 `framework/doc/contract-inventory/` JSON 2개

## 핵심 diff 요지

- C++·Java·Node.js·.NET의 실행 가능한 manifest, fallback, fixture와 성능 메타데이터에서
  binding/Core pin을 0.17.3으로 맞췄다. 생성된 bench log는 건드리지 않았다.
- Node workspace의 `@zlink-systems/zlink`는 local tgz 대신 registry exact version `0.17.3`을
  사용한다. package verifier도 exact registry version을 pack할 수 있게 했다.
- NuGet ID/assembly를 `Systems.Zlink.HttpClient`, `Systems.Zlink.Framework.AspNetCore`로 바꿨다.
  `RootNamespace`와 C# namespace, project 경로는 유지했다. 변경된 AssemblyName에 맞춰
  `InternalsVisibleTo`만 함께 고쳤다.
- 위 두 package와 `Systems.Zlink.Stream.Connector`에 Authors `zlink`, MPL-2.0,
  RepositoryUrl/ProjectUrl, README, SourceLink, nupkg+snupkg metadata를 넣었다.
- Java framework publication에 sources/javadoc, MPL-2.0 POM, in-memory GPG signing과 Central
  staging bundle upload를 추가했다. 공개 secret은 확정된 네 이름만 참조한다.
- framework release 초안은 `framework/v*`/dispatch를 받고 Node provenance publish, Java
  Central Portal bundle, C++ source archive/sha256/GitHub Release를 만든다. .NET push는 넣지 않았다.
- bindings 0.17.3 및 framework 0.10.0 릴리스 노트를 한·영으로 추가했고, 계정 문서를 확정
  채널 기준으로 전면 개정했다. packaging checklist에는 bindings 4언어 → framework 순서를
  추가했다.

## 검증

| 티켓/명령 | rc | 결과·산출물 |
| --- | ---: | --- |
| `0-1788856815-...-framework_Node_binding_0.17.3_package-lo` — `npm install --package-lock-only` | 1 | npm registry에 `@zlink-systems/zlink@0.17.3`이 아직 없어 404. [log](../../../.artifacts/perf-queue/log/0-1788856815-94354-codex-rel-common-framework_Node_binding_0.17.3_package-lo.log) |
| `0-1788857465-...-framework_.NET_restore_after_SourceLink_` — exact 0.17.3 restore | 1 | local source에는 0.17.0만 있고 nuget.org에는 0.17.3이 없어 `NU1102`. 공개 전 예상 결과. [log](../../../.artifacts/perf-queue/log/0-1788857465-13848-codex-rel-common-framework_.NET_restore_after_SourceLink_.log) |
| `0-1788857758-...-framework_.NET_build_after_friend_assemb` — focused .NET build, local 0.17.0 compatibility override | 0 | 새 AssemblyName과 friend assembly 참조 build 통과. SourceLink local warning과 기존 nullability warning만 남음. [log](../../../.artifacts/perf-queue/log/0-1788857758-27888-codex-rel-common-framework_.NET_build_after_friend_assemb.log) |
| `0-1788857802-...-framework_.NET_package_ID_and_metadata_c` — package contract snapshot/clean consumer | 0 | 9개 package 생성, 새 ID 3개 assembly와 clean consumer 통과. snapshot은 `/tmp/zlink-codex-rel-common/dotnet-contract/`에서 검토 후 정리. [log](../../../.artifacts/perf-queue/log/0-1788857802-30674-codex-rel-common-framework_.NET_package_ID_and_metadata_c.log) |
| `0-1788858144-...-framework_.NET_three-package_nupkg_and_s` — 공개 대상 3개 pack | 0 | 각 ID의 nupkg+snupkg, Authors/MPL/README/repository와 PDB 확인. `/tmp/zlink-codex-rel-common/dotnet-pack/` 산출물은 검토 후 정리. [log](../../../.artifacts/perf-queue/log/0-1788858144-44711-codex-rel-common-framework_.NET_three-package_nupkg_and_s.log) |
| `0-1788858052-...-framework_Java_Gradle_release_configurat` — Gradle `help` | 0 | Kotlin DSL과 signing/publication 설정 구성 통과. [log](../../../.artifacts/perf-queue/log/0-1788858052-40798-codex-rel-common-framework_Java_Gradle_release_configurat.log) |
| `0-1788858272-...-release_workflow_and_manifest_syntax_che` | 0 | workflow YAML, shell 2개, inventory JSON 2개 문법 통과. [log](../../../.artifacts/perf-queue/log/0-1788858272-47698-codex-rel-common-release_workflow_and_manifest_syntax_che.log) |
| `0-1788858736-...-corrected_final_release_draft_syntax_val` | 0 | 최종 workflow YAML, shell 2개, 변경 inventory JSON 2개 문법 재검증 통과. [log](../../../.artifacts/perf-queue/log/0-1788858736-55066-codex-rel-common-corrected_final_release_draft_syntax_val.log) |
| `0-1788859133-...-framework_.NET_assembly_identity_focused` | 0 | 새 AspNetCore AssemblyName을 사용하는 `MaintenanceRuntimeTests` 2개 통과. [log](../../../.artifacts/perf-queue/log/0-1788859133-62433-codex-rel-common-framework_.NET_assembly_identity_focused.log) |

조사 중 입력 오타·경로 착오로 무효 처리한 티켓은 `0-1788857263-...`(존재하지 않는 `.s` 파일,
rc=1), `0-1788858035-...`(`bashpersal`, rc=127), `0-1788858708-...`(존재하지 않는 inventory
경로, rc=2)이다. 첫 .NET build 티켓
`0-1788857662-...`는 새 AssemblyName과 기존 `InternalsVisibleTo` 불일치를 검출했고(rc=1), 위
friend assembly 수정 후 `0-1788857758-...`에서 통과했다. `git diff --check`도 통과했다.

## `release-dotnet.yml` 변경 제안(diff만)

이 파일은 rel-dotnet 담당이므로 수정하지 않았다. 같은 workflow 파일명에 묶인 OIDC 정책을
사용하려면 다음 형태로 별도 framework job을 추가해야 한다.

```diff
diff --git a/.github/workflows/release-dotnet.yml b/.github/workflows/release-dotnet.yml
@@
   push:
     tags:
       - 'dotnet/v*'
+      - 'framework/v*'
   workflow_dispatch:
     inputs:
+      target:
+        description: 'Release target: binding or framework'
+        required: true
+        default: 'binding'
+        type: choice
+        options: [binding, framework]
@@
   release-dotnet:
+    if: startsWith(github.ref_name, 'dotnet/v') || inputs.target == 'binding'
     name: Build and publish .NET binding
@@
+  release-framework-dotnet:
+    if: startsWith(github.ref_name, 'framework/v') || inputs.target == 'framework'
+    name: Build and publish .NET framework packages
+    runs-on: ubuntu-22.04
+    permissions:
+      id-token: write
+      contents: read
+    steps:
+      - uses: actions/checkout@v4
+      - uses: actions/setup-dotnet@v4
+        with:
+          dotnet-version: '8.0.x'
+      - name: Resolve framework version
+        id: version
+        shell: bash
+        run: |
+          if [ "${GITHUB_EVENT_NAME}" = push ]; then
+            version="${GITHUB_REF_NAME#framework/v}"
+          else
+            version="${{ inputs.version }}"
+          fi
+          [ "${version}" = "0.10.0" ] || exit 1
+          echo "version=${version}" >> "${GITHUB_OUTPUT}"
+      - name: Build and pack three framework packages
+        shell: bash
+        run: |
+          mkdir -p dist/framework-dotnet
+          for project in \
+            framework/languages/dotnet/src/Zlink.HttpClient/Zlink.HttpClient.csproj \
+            framework/languages/dotnet/src/Zlink.Framework.AspNetCore/Zlink.Framework.AspNetCore.csproj \
+            framework/languages/dotnet/src/Systems.Zlink.Stream.Connector/Systems.Zlink.Stream.Connector.csproj; do
+            dotnet pack "${project}" -c Release \
+              -p:PackageVersion="${{ steps.version.outputs.version }}" \
+              -p:PackageOutputPath="${GITHUB_WORKSPACE}/dist/framework-dotnet"
+          done
+      - uses: actions/upload-artifact@v4
+        with:
+          name: framework-dotnet-${{ steps.version.outputs.version }}
+          path: |
+            dist/framework-dotnet/*.nupkg
+            dist/framework-dotnet/*.snupkg
+      - uses: NuGet/login@v1
+        id: login
+        with:
+          user: zlink
+      - name: Push framework packages
+        shell: bash
+        run: |
+          dotnet nuget push 'dist/framework-dotnet/*.nupkg' \
+            --api-key "${{ steps.login.outputs.NUGET_API_KEY }}" \
+            --source https://api.nuget.org/v3/index.json --skip-duplicate
+          dotnet nuget push 'dist/framework-dotnet/*.snupkg' \
+            --api-key "${{ steps.login.outputs.NUGET_API_KEY }}" \
+            --source https://api.nuget.org/v3/index.json --skip-duplicate
```

## 사용자가 직접 해야 할 일

1. GitHub repository secret에 다음 네 이름이 설정돼 있는지만 확인한다. 새 token/key는 만들지
   않는다: `MAVEN_CENTRAL_USERNAME`, `MAVEN_CENTRAL_PASSWORD`, `SIGNING_KEY`,
   `SIGNING_PASSPHRASE`.
2. C++·Java·.NET binding 0.17.3 공개를 완료한다.
3. npm은 `zlink-systems` 계정으로 `@zlink-systems/zlink@0.17.3`을 로컬에서 처음 수동 게시한 뒤
   Trusted Publishing/provenance를 연결한다. `NPM_TOKEN`은 만들지 않는다.
4. 공개 binding 설치를 확인하고 Node lockfile을 `npm install --package-lock-only`로 다시 생성해
   registry가 제공한 integrity를 확정한다. .NET exact restore도 다시 실행한다.
5. Sonatype Central Portal의 USER_MANAGED deployment를 검토하고 공개한다.
6. rel-dotnet 담당자가 위 diff를 검토·적용한 뒤 framework .NET package를
   `release-dotnet.yml`에서 게시한다.
7. ConanCenter와 vcpkg 변경은 `zlink-systems` 계정으로 draft PR까지만 준비하고, 승인 후 별도로
   제출한다.

## 미해결 사항

- npm과 nuget.org에 binding 0.17.3이 아직 없으므로 registry 기반 lock/restore의 최종 성공과
  npm integrity는 binding 공개 뒤에만 확정할 수 있다.
- `scripts/local-package/sync-version.py`는 삭제된 옛 .NET package snapshot 파일명과 Node의
  local `resolved: file:...tgz` 형태를 아직 요구한다. rel-common 담당 범위 밖이라 수정하지
  않았으며, 감독자가 새 snapshot 이름과 registry URL 형태를 반영해야 `--verify-versions`가
  다시 통과한다.
- 요청된 .NET 3개 package 가운데 `Systems.Zlink.HttpClient`는 `Zlink.Framework.Contracts`에,
  `Systems.Zlink.Framework.AspNetCore`는 `Zlink.Framework`에 의존한다. 현재 3개만 게시하면 이
  transitive package가 nuget.org에 없을 수 있으므로, 기존 packable dependency package도 함께
  공개할지 아니면 세 package에 병합할지 감독자 결정이 필요하다.
- Node `@zlink-systems/nestjs`는 repository에서 `private`인
  `@zlink-systems/http-client@0.10.0`에 의존한다. 또한 Node package의 `LICENSE`와
  `framework/LICENSE`는 현재 FSL 문구지만 이번 공개 배포 기준은 MPL-2.0이다. 법적·package
  범위를 넓히지 않고 보고만 남겼다. 이 둘을 정리하기 전에는 framework Node publish를 실행하면
  안 된다.
- 담당 범위 밖 `doc/license/README*.md`도 framework .NET package를 FSL/Apache-2.0으로 설명해
  현재 MPL-2.0 metadata와 어긋난다. 라이선스 파일 정리와 함께 감독자가 갱신 범위를 정해야 한다.
- C++ vcpkg manifest에는 C++ binding package를 표현할 port가 없고 Core `zlink` 의존도 직접
  선언하지 않는다. 이번 변경은 기존 `zlink_cpp` local package 경로의 정확한 0.17.3 pin만
  유지한다.
