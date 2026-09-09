[English](./release-pipeline.md) | [한국어](./release-pipeline.ko.md)

# 빌드·배포 파이프라인 한눈에 보기

이 문서는 zlink의 각 구성 요소가 **어디서(GitHub Actions 워크플로우), 무엇으로(트리거·인증),
어디로(공개 채널)** 빌드되고 배포되는지를 한 장에 정리한다. 절차 세부는
[패키징 가이드](./packaging.ko.md), 계정·secret은 [배포 계정](./release-accounts.ko.md)을 본다.
모든 배포는 GitHub Actions에서 수행하며 로컬 PC에서 publish하지 않는다.

## 1. 구성 요소별 배포 경로

| 구성 요소 | 산출물 | 공개 채널 | 워크플로우 | 트리거 | 인증 |
| --- | --- | --- | --- | --- | --- |
| Core (`core/`) | 플랫폼별 native archive 5종, source tarball, checksums, provenance | GitHub Release `core/vX.Y.Z` | `build.yml` | `core/vX.Y.Z` 태그를 만든 뒤 그 ref로 `workflow_dispatch` | `GITHUB_TOKEN` |
| Core (Conan) | recipe | ConanCenter | 없음(PR) | `conan-io/conan-center-index`에 `recipes/zlink/` PR | GitHub 계정 |
| Core (vcpkg) | port | microsoft/vcpkg | 없음(PR) | `microsoft/vcpkg`에 `ports/zlink/` + `versions/` PR | GitHub 계정 |
| Binding C++ | source archive(`bindings/cpp` 전체 + root LICENSE; vcpkg port·Conan recipe가 설치된 Core package에 맞춰 빌드) | GitHub Release `cpp/vX.Y.Z` | `bindings-release.yml` | `cpp/v*` 태그 또는 dispatch | `GITHUB_TOKEN` |
| Binding Node | `@zlink-systems/zlink` (linux-x64 prebuild 포함) | npm | `bindings-release.yml` | `node/v*` 태그 또는 dispatch | npm Trusted Publishing(OIDC, provenance) |
| Binding Java | `systems.zlink:zlink`, `zlink-ext-netty` | Maven Central, GitHub Packages | `bindings-release.yml` | `java/v*` 태그 또는 dispatch | `MAVEN_CENTRAL_*`, `SIGNING_*`(GPG) |
| Binding .NET | `Zlink` nupkg (+snupkg) | nuget.org | `release-dotnet.yml` (target `binding`) | `dotnet/v*` 태그 또는 dispatch | nuget Trusted Publishing(`NuGet/login`, 정책 `zlink-dotnet-release`) |
| Framework C++ | source archive + sha256(`cmake -P framework/languages/cpp/cmake/prepare-source-archive.cmake`: `framework/languages/cpp` + `framework/runtime` + `framework/LICENSE`, generated protocol header를 `--check`로 검증) | GitHub Release `framework/vA.B.C` | `framework-release.yml` | `framework/v*` 태그 또는 dispatch | `GITHUB_TOKEN` |
| Framework Node | `@zlink-systems/*` 8개 | npm | `framework-release.yml` | 위와 동일 | npm Trusted Publishing(패키지별 등록) |
| Framework JVM | `systems.zlink:zlink-framework-*` 13개(Kotlin 포함) | Maven Central | `framework-release.yml` | 위와 동일 | `MAVEN_CENTRAL_*`, `SIGNING_*` |
| Framework .NET | `Zlink.Framework*`, `Zlink.HttpClient`, `Zlink.Stream.Connector` 등 9개 | nuget.org | `release-dotnet.yml` (target `framework`) | `framework/v*` 태그 또는 dispatch | nuget Trusted Publishing |
| 문서 사이트 | mkdocs 정적 사이트 | GitHub Pages | `docs.yml` | `main` push(문서 경로) | `GITHUB_TOKEN` |

Python·Go·Rust binding은 `bindings-release.yml`에 job이 있으나 공개 배포 범위 밖이다.
`core-conan-release.yml`은 사내 Conan remote용 legacy 워크플로우로, secret이 없어 동작하지 않는다.

## 2. 빌드 스크립트와 도구 위치

| 대상 | 로컬 빌드 진입점 | CI·릴리스에서의 빌드 | 산출물 |
| --- | --- | --- | --- |
| Core | `scripts/build-core.sh dev\|release\|release-gate` (트리 `core/build-dev`, `core/build-release`). CMake 직접 빌드는 [빌드 가이드](./build-guide.ko.md)·[CMake 옵션](./cmake-options.ko.md) | `build.yml`의 플랫폼 job이 `core/`를 CMake로 빌드해 `core/dist/<platform>/`를 아카이브 | GitHub Release `core/vX.Y.Z` 자산 |
| Core 로컬 prefix | `scripts/local-package/core/fetch-release.sh --version V --platform P` (릴리스 아카이브 → `~/.cache/zlink/core/<V>/<P>`), `scripts/gate/materialize-local-core-prefix.sh` (dev 빌드 → prefix) | 모든 binding·framework job이 같은 `fetch-release.sh`를 사용 | `~/.cache/zlink/core/`, CI는 `.artifacts/core-release/` |
| Bindings 7언어 | `scripts/local-package/build-wsl.sh [cpp\|node\|java\|dotnet\|python\|go\|rust\|c]` (Windows `build-windows.ps1`); 언어별 테스트 `bindings/<lang>/tests/run_tests.sh` | `bindings-release.yml`·`release-dotnet.yml` 각 job의 "Build and test" 단계 | `.artifacts/wsl/{npm,nuget,maven,install}/` |
| Framework C++ | `framework/languages/cpp/CMakePresets.json` preset, Windows `build-windows.ps1`; 샘플 `samples/run_samples.sh`; 시나리오 e2e `e2e/<name>/run_e2e.sh`(opt-in) | `framework-release.yml`은 source archive만 만든다 | GitHub Release `framework/vA.B.C` |
| Framework .NET | `dotnet build framework/languages/dotnet/Zlink.Framework.sln` (`ZLINK_LOCAL_PACKAGE_ROOT` 필요); 샘플 `samples/<name>/<name>.sln` | `framework-dotnet.yml`(검증), `release-dotnet.yml` target `framework`(pack·push) | nuget.org |
| Framework JVM | `framework/languages/java/gradlew assemble` (테스트는 `test`); Central bundle `scripts/upload-central-bundle.sh` | `framework-release.yml` `release-java` | Maven Central |
| Framework Node | `framework/languages/node`에서 `npm ci && npm run build`; http-client 로컬 tgz는 `scripts/local-package/http-client/build-wsl.sh node`; 게이트 `npm run verify:ci`, 릴리스 게이트 `verify:release` | `framework-node.yml`(검증), `framework-release.yml` `release-node`(pack·publish) | npm |
| 버전 동기화 | `scripts/local-package/sync-version.py --write` 또는 `build-wsl.sh --sync-versions` / `--verify-versions` | 각 릴리스 job이 태그·`VERSION`·manifest 일치를 검증 | — |
| 통합 게이트 | `scripts/gate/{rebuild-dev,framework-gate,bindings-gate,cross-language-e2e}.sh <tag>` ([설명](../../scripts/gate/README.md)) | CI 워크플로우가 같은 범위를 플랫폼 matrix로 수행 | `zlink-work/gates/<tag>/` |
| 성능 측정 | `scripts/perf/perf-ticket.sh submit …` (티켓 큐, `perf-queue-runner.sh`) | CI에서는 하지 않음 | `.artifacts/perf-queue/`, `doc/perf/perf/` |
| CI 보조 | `.github/actions/msvc-env`(Windows MSVC 환경), `scripts/ci/dotnet-test-retry.sh` | 워크플로우가 호출 | — |
| 워크플로우 | `.github/workflows/`: `build.yml`, `bindings-release.yml`, `release-dotnet.yml`, `framework-release.yml`, `framework-node.yml`, `framework-dotnet.yml`, `docs.yml`, (legacy) `core-conan-release.yml` | — | — |

## 3. 배포 순서

번호 규칙과 호환 관계는 [버전 정책](./versioning.ko.md)이 소유한다. 버전 파일은 `VERSION`(Core)과 `BINDINGS_VERSION`이며 `scripts/local-package/sync-version.py --write`가
저장소 전체의 pin을 맞춘다. 순서는 항상 **Core → bindings 4언어 → framework 4언어**다. framework
패키지는 공개된 binding 패키지를 pin으로 참조하므로 순서 역전은 허용하지 않는다.

1. Core: `VERSION` 갱신, `core/CHANGELOG.md` 절 추가, `core/vX.Y.Z` 태그 push, 그 ref로
   `build.yml` dispatch. Release 자산과 `checksums.txt`, `release-provenance.txt`를 확인한다.
2. Bindings: `sync-version.py --write` 후 `cpp/v`, `node/v`, `java/v` 태그(→ `bindings-release.yml`)와
   `dotnet/v` 태그(→ `release-dotnet.yml`). 각 job은 checkout의 `VERSION`·Core 소스가 태그와
   정확히 같은지 검증하고, Core 릴리스 아카이브(`scripts/local-package/core/fetch-release.sh`)를 받아
   빌드·테스트·패키징한다. CI에서 Core를 다시 빌드하지 않는다.
3. Framework: `framework/vA.B.C` 태그 하나로 `framework-release.yml`(C++·Node·JVM)과
   `release-dotnet.yml`(.NET)이 함께 돈다. 두 워크플로우는 binding 패키지가 레지스트리에서 실제로
   제공될 때까지(최대 45분) 기다린 뒤 진행한다.
4. Conan·vcpkg: Release의 source tarball 해시로 `core/packaging/conan/conandata.yml`과
   `vcpkg/ports/zlink/portfile.cmake`를 갱신하고 두 upstream 저장소에 PR을 낸다. 초안 본문은
   `doc/building/pr-drafts/`에 있다.

dispatch 예시:

```bash
gh workflow run build.yml --ref core/v0.17.5 -f libzlink_version=0.17.5
gh workflow run bindings-release.yml -f target=node -f version=0.17.5 -f create_release=true -f publish_registry=true
gh workflow run release-dotnet.yml -f target=binding -f version=0.17.5
gh workflow run framework-release.yml -f version=0.10.0 -f publish_registry=true
gh workflow run release-dotnet.yml -f target=framework -f version=0.10.0
```

## 4. 채널별 동작 방식

- **GitHub Release**: `softprops/action-gh-release`가 태그별 Release를 만들고 자산을 올린다.
  Core Release 노트는 `core/CHANGELOG.md`의 해당 절에서 자동 추출한다.
- **npm**: 토큰 없이 OIDC로 게시한다(`npm publish --provenance --access public`). npmjs.com에서
  패키지마다 Trusted Publisher(조직 `zlink-systems`, 저장소 `zlink`, 워크플로우 파일명)가 등록돼
  있어야 한다. binding은 `bindings-release.yml`, framework 8개는 `framework-release.yml`이다.
  `setup-node`의 `registry-url`은 쓰지 않는다(빈 `_authToken`이 생겨 404가 난다).
- **Maven Central**: Gradle이 서명(in-memory GPG)·sources·javadoc을 포함한 bundle을 만들고
  `*/scripts/upload-central-bundle.sh`가 Central Portal Publisher API에 `publishingType=AUTOMATIC`으로
  올린다. 검증을 통과하면 사람 클릭 없이 공개된다. 상태는
  `POST https://central.sonatype.com/api/v1/publisher/status?id=<deployment>`로 조회한다.
- **nuget.org**: `NuGet/login@v1`이 발급한 단기 토큰으로 push한다. 정책 `zlink-dotnet-release`는
  워크플로우 파일명 `release-dotnet.yml`에 묶여 있어 다른 워크플로우에서는 push할 수 없다.
  패키지 ID는 `Zlink`·`Zlink.*`다(`Systems.` 접두어는 nuget.org에서 타인에게 예약돼 있다).
  snupkg는 패키지 검증 뒤에야 수락되므로 최대 30회 재시도하고 실패해도 릴리스를 막지 않는다.
- **ConanCenter·vcpkg**: 바이너리가 아니라 레시피만 등록한다. 사용자가 설치할 때 GitHub Release의
  source tarball을 받아 해시를 검증하고 직접 빌드한다. 머지는 upstream 유지관리자가 한다.

## 5. 레지스트리 전파와 재시도

게시 직후 레지스트리는 metadata를 먼저 보이고 실제 파일은 몇 분 뒤에 제공한다(npm tarball 약 3분,
nuget flatcontainer는 validation 뒤). framework 워크플로우와 framework CI는 pin된 binding 버전의
tarball/nupkg가 200이 될 때까지 1분 간격으로 최대 45분 기다린다. 이미 게시된 버전은 건너뛰므로
일부 패키지만 실패했을 때 같은 버전으로 다시 dispatch하면 남은 것만 올라간다.

## 6. 배포 후 확인

```bash
curl -sI https://registry.npmjs.org/@zlink-systems/zlink/-/zlink-0.17.5.tgz | head -1
curl -sI https://api.nuget.org/v3-flatcontainer/zlink/0.17.5/zlink.0.17.5.nupkg | head -1
curl -sI https://repo1.maven.org/maven2/systems/zlink/zlink/0.17.5/zlink-0.17.5.pom | head -1
gh release view core/v0.17.5 --json assets -q '.assets[].name'
```

## 7. 지원 플랫폼과 런타임 요구

| 플랫폼 | Core 릴리스 | Node prebuild | .NET runtimes | 비고 |
| --- | --- | --- | --- | --- |
| linux-x64 | ✅ | ✅ | ✅ | 릴리스 러너 ubuntu-24.04, glibc ≥ 2.38 필요 |
| linux-arm64 | ✅ | 소스 빌드 | Core 아카이브 | |
| macos-arm64 | ✅ | 소스 빌드 | Core 아카이브 | |
| windows-x64 | ✅ | 소스 빌드 | Core 아카이브 | 아카이브에 OpenSSL DLL 포함 |
| windows-arm64 | ✅ | 소스 빌드 | Core 아카이브 | |
| macos-x64 (Intel) | ❌ | ❌ | ❌ | Core부터 미지원(2026-09-09 결정) |

"소스 빌드"는 패키지 설치 시 `ZLINK_CORE_SOURCE=release`와 `ZLINK_CORE_PACKAGE_PREFIX`로 가리킨 Core
릴리스 아카이브에 대해 addon을 컴파일한다는 뜻이다.

## 8. CI(검증) 워크플로우

배포와 별개로 `main` push·PR에서 도는 검증이다. framework CI는 공개된 binding 패키지와 Core 릴리스
아카이브만 사용하며, e2e는 `cross-language`만 포함한다(언어별 시나리오 e2e는 각 `run_e2e.sh`로 opt-in).
7개 샘플(Bingo·DeliveryDispatch·GameQuest·ShoppingMall·SupportChat·TicTacToe·ZoneWorld)도 framework
빌드·CI·배포에 포함하지 않는다. 샘플은 로컬 gate(`scripts/gate/framework-gate.sh`)와 각 언어의
`samples/run_samples.sh`, Node `npm run test:samples`로만 검증한다.

| 워크플로우 | 대상 | 매트릭스 |
| --- | --- | --- |
| `framework-node.yml` | Node framework gate, Chromium STREAM e2e, Node↔.NET cross-language smoke | 5 플랫폼 × Node 20/22 |
| `framework-dotnet.yml` | .NET framework unit·contract·stream connector 테스트 | 5 RID × net8.0 Debug / net10.0 Release |
| `build.yml` | Core 빌드·검증(릴리스 겸용) | 5 플랫폼 |
| `docs.yml` | 문서 사이트 빌드·배포 | ubuntu |

CI 워크플로우는 ref별 `concurrency`로 같은 브랜치의 새 push가 진행 중인 run을 취소한다(matrix job 누적과
릴리스 runner 기아 방지). 릴리스 워크플로우는 취소하지 않는다.
Windows job의 MSVC 환경은 `.github/actions/msvc-env`(composite, Node 런타임 없음)로 잡는다.
공유 러너의 스케줄러 노이즈로 실패한 .NET 단위 테스트는 `scripts/ci/dotnet-test-retry.sh`가 실패
테스트만 한 번 재실행한다(테스트 허용치는 바꾸지 않는다).

## 9. 로컬에서 하는 것과 하지 않는 것

- 로컬: 버전 갱신(`sync-version.py`), CHANGELOG·릴리스 노트 작성, 태그 push, dispatch, 결과 확인,
  Conan/vcpkg 레시피 갱신과 로컬 빌드 검증(`conan create`, `vcpkg install`).
- 하지 않음: `npm publish`, `dotnet nuget push`, Central 업로드, 서명. 모두 워크플로우에서만 한다.
  secret 값은 GitHub repository secret에만 있고 문서·로그에는 이름만 적는다.

## 10. 관련 기록

- [배포 계정과 secret](./release-accounts.ko.md)
- [패키징 절차](./packaging.ko.md)
- [릴리스 노트](./release-notes/)
- [릴리스 준비 작업 기록](./release-prep/) — 2026-09-08·09의 워크플로우 수정 이력
- [PR 초안](./pr-drafts/) — ConanCenter #30935, vcpkg #53846
