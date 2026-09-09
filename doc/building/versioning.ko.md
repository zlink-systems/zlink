[English](./versioning.md) | [한국어](./versioning.ko.md)

# 버전 정책

이 문서가 zlink 구성 요소의 버전 번호 규칙과 호환 관계를 소유한다. 다른 문서는 여기를 링크만
한다. 확정일 2026-09-09(사용자 결정).

## 1. 구성 요소별 번호

| 구성 요소 | 번호 형식 | 원본 파일 | 게시 표기 |
| --- | --- | --- | --- |
| Core (`core/`) | `MAJOR.MINOR` | `VERSION` (`LIBZLINK_VERSION`) | `MAJOR.MINOR.0` |
| Bindings (C++·Node·Java·.NET, 그리고 Python·Go·Rust) | `CORE_MAJOR.CORE_MINOR.N` | `BINDINGS_VERSION` | 그대로 |
| Framework (C++·Node·JVM·.NET) | `MAJOR.MINOR.HOTFIX` | `FRAMEWORK_VERSION` (`ZLINK_FRAMEWORK_VERSION`) | 그대로 |

- **Core는 두 자리다.** 세 번째 자리를 쓰지 않는다. Core의 어떤 수정이든(버그 수정 포함) `MINOR`를
  올린다. npm·NuGet·Maven은 세 자리 semver를 요구하므로 게시할 때는 `.0`을 붙인다.
- **Binding 번호는 어느 Core와 맞는지 말해 준다.** 앞 두 자리는 그 binding이 포함하거나 정확히
  의존하는 Core의 `MAJOR.MINOR`이고, `N`은 Core 변경 없이 binding만 고친 횟수다. Core가
  `0.18`로 오르면 binding은 `0.18.0`부터 다시 시작한다. 네 언어 binding은 하나의
  `BINDINGS_VERSION`을 공유하므로 한 언어만 고쳐도 네 언어를 같은 번호로 다시 게시한다.
- **Framework는 세 자리다.** 기능 추가·계약 변경은 `MINOR`, 기능 없는 수정과 binding pin만 올리는
  재게시는 `HOTFIX`를 올린다. `HOTFIX`는 `MINOR`가 오를 때 0으로 돌아간다.
- 1.0 이전에는 `MINOR`가 올라도 호환을 약속하지 않는다.

## 2. 호환 규칙

- Binding `X.Y.*`는 Core `X.Y`에만 맞는다. 다른 Core와 조합하지 않는다.
- Framework `A.B`는 릴리스 노트에 적힌 binding `X.Y.N`을 정확히 pin한다. 사용자가 framework를
  설치하면 그 binding이 함께 설치된다.
- 1.0 이후: 같은 `MAJOR` 안에서 Core의 공개 C API·ABI(`core/include/**`, `core/src/libzlink.vers`)와
  binding·framework의 공개 API를 깨지 않는다. 깨는 변경은 `MAJOR`를 올린다.

## 3. 릴리스 순서와 태그

| 순서 | 태그 | 워크플로우 | 조건 |
| --- | --- | --- | --- |
| 1 | `core/vX.Y.0` | `build.yml` (태그 ref로 dispatch) | `VERSION` 갱신, `core/CHANGELOG.md` 절 |
| 2 | `cpp/`·`node/`·`java/`·`dotnet/vX.Y.N` | `bindings-release.yml`, `release-dotnet.yml` | checkout의 `core/` 소스가 Core 태그와 정확히 같아야 한다 |
| 3 | `framework/vA.B.C` | `framework-release.yml`, `release-dotnet.yml` | binding 패키지가 레지스트리에 실제로 있어야 한다 |

태그는 한 push에 세 개 이하로 올린다. GitHub는 태그가 네 개 이상인 push에는 워크플로우 이벤트를
만들지 않는다(2026-09-09 binding 0.17.6에서 확인). 네 언어 binding은 `bindings-release.yml`을
언어별로 dispatch하는 편이 안전하다. 절차 전체는 [빌드·배포 파이프라인](./release-pipeline.ko.md).

## 4. 번호를 바꾸는 방법

- Core: `VERSION`만 수정한다.
- Binding: `BINDINGS_VERSION`만 수정한다.
- Framework: `FRAMEWORK_VERSION`만 수정한다. `sync-version.py --write`가 .NET props, Node 패키지·샘플·lockfile,
  Java·Kotlin build 파일, C++ vcpkg·Conan 매니페스트의 107개 파일을 맞춘다(registry는 스크립트 안에 명시).
- 그 뒤 `python3 scripts/local-package/sync-version.py --write`가 매니페스트·pin·헤더·스냅샷을
  한 번에 맞추고, `scripts/local-package/build-wsl.sh --verify-versions`로 누락을 확인한다.
  개별 파일을 손으로 찾아 고치지 않는다.

## 5. 1.0 계획

Core와 bindings는 B 머신에서 진행 중인 binding 성능 개선 캠페인이 끝나면 `1.0`으로 올린다.
조건은 다음과 같다.

- 공개 C API·ABI 동결(`libzlink.vers`), 지원 플랫폼 5종(linux-x64·linux-arm64·macos-arm64·
  windows-x64·windows-arm64) 릴리스 아카이브와 CI green.
- 네 언어 binding의 공개 API 동결과 성능 캠페인 판정 완료.
- Framework는 별도 일정으로 `1.0`을 정한다.

## 6. 이력

| 날짜 | 결정 |
| --- | --- |
| 2026-09-09 | Core `MAJOR.MINOR`, binding `CORE_MAJOR.CORE_MINOR.N`, framework `MAJOR.MINOR.HOTFIX` 확정. 기존 Core 0.17.5는 정책 이전 번호이며 1.0에서 정리한다. binding 0.17.6은 Node 수정만으로 네 언어를 재게시한 첫 사례. 다음 framework는 0.11.0(.NET admission 수정과 binding 0.17.6 pin). |
