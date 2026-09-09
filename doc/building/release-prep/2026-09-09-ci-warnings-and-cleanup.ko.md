# GitHub Actions 경고·CI 실패 정리와 저장소 정리 (2026-09-09)

## 결과

사용자 지시("git action warning 들 조사해서 수정", "e2e 는 language-cross e2e 만 포함",
"git 에 올라간 불필요한 파일들도 모두 정리", "root 에 있는 불필요하고 사용하지 않는 파일들은
다 정리")로 2026-09-08 이후 모든 run의 annotation 253건을 모아 종류별로 처리했다.

| 종류 | 건수 | 처리 |
| --- | ---: | --- |
| Node.js 20 deprecation (checkout/setup-node/setup-dotnet/setup-java/upload-artifact/deploy-pages/setup-python/action-gh-release/msvc-dev-cmd) | 130+ | 모든 action을 Node 24 릴리스로 상향. `ilammy/msvc-dev-cmd`는 Node 24 판이 없어 저장소 내 composite action `.github/actions/msvc-env`로 대체 |
| `setup-java v4 is deprecated` | 15 | v5 |
| `windows-11-arm` VS 2026 기본 전환 notice | 11 | 정보성. framework-node는 이미 VS 18 프롬프트를 VS 2022로 보이게 하는 단계가 있음 |
| C# nullability 경고 (`ZLinkSpotNodeCatalog` tuple) | 1 | 세 return의 tuple 요소 nullability 통일 |
| snupkg 미수락 경고 | 1 | nuget.org 검증 대기 재시도 15→30회(성공 run에서는 수락됨) |
| framework-dotnet.yml 상시 실패 (NU1301) | 79/79 run | 아래 "framework CI" |
| framework-node.yml 상시 실패 | 성공 0회 | 아래 "framework CI" |
| core-conan-release.yml 실패 | dispatch 전용 | 비공개 Conan remote용 legacy 워크플로우. secrets가 없어 실패. ConanCenter PR로 대체됐으므로 삭제 후보(사용자 결정) |

## framework CI

두 워크플로우는 존재하지 않는 `bindings/node/prebuilds/<target>/` 경로, `ZLinkLocalPackageRoot`
(워크플로우)와 `%ZLINK_LOCAL_PACKAGE_ROOT%`(nuget.config) 이름 불일치, sln에 포함된 컴파일 불가
e2e 프로젝트(8bae89dc0f 이후 `ConfigureCoreHwm()` 제거) 때문에 한 번도 통과한 적이 없었다.

- 공개된 binding 패키지(npm tarball, nuget flatcontainer)를 전파 대기와 함께 사용한다.
- 플랫폼별 Core 런타임은 `scripts/local-package/core/fetch-release.sh`의 릴리스 아카이브다.
  Node는 prebuild가 없는 플랫폼에서 `ZLINK_CORE_SOURCE=release`·`ZLINK_CORE_PACKAGE_PREFIX`로
  addon을 소스 빌드하고 런타임을 addon 옆에 둔다. .NET은 `ZLINK_LIBRARY_PATH`.
- `fetch-release.sh`는 macOS(`realpath -m`·`sha256sum` 없음)와 Windows(`unzip` 대체)에서도 돈다.
- e2e 범위: .NET sln은 `cross-language/Zlink.Framework.TestHost`만 유지(45개 제거). C++는
  `ZLINK_FRAMEWORK_CPP_BUILD_E2E` 기본 OFF, `run_e2e.sh`가 ON을 넘기고 cross-language host는
  `ZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE`(기본 ON). .NET e2e 34개 파일은
  `ConfigureInboundDispatch()`로 이관해 64개 e2e 프로젝트 모두 로컬 빌드 통과.

## 저장소 정리

- 런타임 출력물 363개 제거(e2e evidence/config 로그, bench `verify-*`, `framework/bench/tools/log`,
  CTest 데이터)와 `.gitignore` 규칙 추가. 보고서가 측정 원본으로 지정한
  `bench/with-grpc/log/<stamp>` 4개는 유지. e2e feature-map·porting-inventory 문서가 인용하던
  로그 경로는 더 이상 저장소에 없다.
- root: 빈 파일 `3`, Windows 로컬 tgz를 가리키던 `package.json`/`package-lock.json`, legacy
  `Dockerfile`·`Doxygen.cfg`·`README.doxygen.md`·`CXX_BUILD_EXAMPLES.md`·`zlink.sln`·`.vscode`
  제거. `installer.ico`는 `core/packaging/`으로 이동(CPack 경로도 `core/` 기준으로 수정).
  root `CHANGELOG.md`(5.0.x·0.13.0 시대)는 `core/CHANGELOG-history.md`로 이관하고 spec·nuget·
  SECURITY 참조를 `core/CHANGELOG.md`로 갱신. `README.md`는 2026-08-27에 .NET 패키지 README로
  덮어써진 것을 저장소 개요로 복원.
- 디스크에만 있는 `scratchpad/`(3.4 GB)와 `zlink-work/`(1.5 GB, gate 로그 위치)는 git 대상이
  아니며 삭제하지 않았다.

## 확인

