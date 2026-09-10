[English](./versioning.md) | [한국어](./versioning.ko.md)

# 버전 정책

이 문서가 zlink 구성 요소의 버전 번호 규칙과 호환 관계를 소유한다. 다른 문서는 여기를 링크만
한다. 확정일 2026-09-09, 언어별 버전 소유 전환일 2026-09-10(사용자 결정).

## 1. 구성 요소별 번호와 원본

| 구성 요소 | 번호 형식 | 원본 파일 | 게시 표기 |
| --- | --- | --- | --- |
| Core (`core/`) | `MAJOR.MINOR` | `VERSION` (`LIBZLINK_VERSION`) | `MAJOR.MINOR.0` |
| 각 Binding | `CORE_MAJOR.CORE_MINOR.N` | `bindings/<language>/VERSION` (`ZLINK_BINDING_VERSION`) | 그대로 |
| 각 Framework | `MAJOR.MINOR.HOTFIX` | `framework/languages/<language>/VERSION` (`ZLINK_FRAMEWORK_VERSION`) | 그대로 |

- **Core는 두 자리다.** 세 번째 자리를 쓰지 않는다. Core의 어떤 수정이든(버그 수정 포함) `MINOR`를
  올린다. npm·NuGet·Maven은 세 자리 semver를 요구하므로 게시할 때는 `.0`을 붙인다.
- **Binding 번호는 어느 Core와 맞는지 말해 준다.** 앞 두 자리는 해당 binding이 포함하거나 정확히
  의존하는 Core의 `MAJOR.MINOR`이고, `N`은 Core 변경 없이 그 언어 binding만 고친 횟수다. Core가
  `0.18`로 오르면 해당 binding은 `0.18.0`부터 다시 시작한다. 언어마다 VERSION을 소유하므로 한 언어의
  hotfix가 다른 언어의 번호나 게시를 바꾸지 않는다.
- **Framework는 세 자리다.** 기능 추가·계약 변경은 `MINOR`, 기능 없는 수정과 해당 언어 binding pin만
  올리는 재게시는 `HOTFIX`를 올린다. 언어마다 VERSION을 소유한다.
- 1.0 이전에는 `MINOR`가 올라도 호환을 약속하지 않는다.

## 2. 호환 규칙

- Binding `X.Y.*`는 Core `X.Y`에만 맞는다. 다른 Core와 조합하지 않는다.
- 각 언어의 Framework `A.B`는 그 언어 릴리스 노트에 적힌 binding `X.Y.N`을 정확히 pin한다.
  사용자가 framework를 설치하면 해당 binding이 함께 설치된다.
- 1.0 이후: 같은 `MAJOR` 안에서 Core의 공개 C API·ABI(`core/include/**`, `core/src/libzlink.vers`)와
  binding·framework의 공개 API를 깨지 않는다. 깨는 변경은 `MAJOR`를 올린다.

## 3. 릴리스 순서와 태그

| 순서 | 태그 | 워크플로우 | 조건 |
| --- | --- | --- | --- |
| 1 | `core/vX.Y.0` | `build.yml` (태그 ref로 dispatch) | `VERSION` 갱신, `core/CHANGELOG.md` 절 |
| 2 | `<language>/vX.Y.N` | `bindings-release.yml`, .NET은 `release-dotnet.yml` | 선택 언어 VERSION과 태그가 같고 checkout의 Core 소스가 Core 태그와 정확히 같아야 한다 |
| 3 | `framework-<language>/vA.B.C` | `framework-release.yml`, .NET은 `release-dotnet.yml` | 선택 언어 VERSION과 태그가 같고 해당 binding 패키지가 레지스트리에 실제로 있어야 한다 |

GitHub는 태그가 네 개 이상인 한 push에 워크플로우 이벤트를 만들지 않을 수 있다(2026-09-09
binding 0.17.6에서 확인). 태그는 언어별로 push하거나 workflow를 선택 언어로 dispatch한다. 절차 전체는
[빌드·배포 파이프라인](./release-pipeline.ko.md)이 소유한다.

## 4. 번호를 바꾸는 방법

- Core: root `VERSION`만 수정한다.
- Binding: 대상 `bindings/<language>/VERSION`만 원본으로 수정한다.
- Framework: 대상 `framework/languages/<language>/VERSION`만 원본으로 수정한다.
- 그 뒤 `python3 scripts/local-package/sync-version.py --write`가 선택한 원본에서 매니페스트·pin·헤더·
  스냅샷을 맞춘다. `scripts/local-package/build-wsl.sh --verify-versions`로 누락과 다른 언어의 의도치 않은
  변경이 없는지 확인한다. JSON·lockfile·vcpkg·Conan처럼 원본 VERSION을 직접 읽을 수 없는 파일만
  동기화 대상으로 남긴다.

## 5. 1.0 계획

1.0 조건은 다음과 같다.

- 공개 C API·ABI 동결(`libzlink.vers`), 지원 플랫폼 5종(linux-x64·linux-arm64·macos-arm64·
  windows-x64·windows-arm64) 릴리스 아카이브와 CI green.
- 각 언어 binding의 공개 API 동결과 B 머신의 binding 성능 개선 캠페인 판정 완료.
- Framework는 Core·bindings와 같은 1.0 계열에 맞춘다. 배포 순서는 Core → 해당 언어 binding →
  해당 언어 framework다.
- 1.0에서 C++ 공개 자산(소스 archive)과 vcpkg port·Conan recipe의 checksum을 갱신한다.

## 6. 이력

| 날짜 | 결정 |
| --- | --- |
| 2026-09-09 | Core `MAJOR.MINOR`, binding `CORE_MAJOR.CORE_MINOR.N`, framework `MAJOR.MINOR.HOTFIX` 확정. |
| 2026-09-10 | 공용 binding/framework 버전 파일을 언어별 VERSION으로 분리. 언어별 hotfix와 릴리스가 다른 언어 버전을 바꾸지 않게 함. |
