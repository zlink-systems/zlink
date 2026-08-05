[English](./packaging.md) | [한국어](./packaging.ko.md)

# 패키징 및 릴리즈 (Core / Bindings 분리)

## 1. 목표

- Core(`libzlink`)와 바인딩(Node/Python/Java/.NET/C++)을 **별도 프로세스**로 릴리즈한다.
- Core 릴리즈 완료 후, 바인딩은 언어별 수정/검증을 거쳐 **개별 태그**로 배포한다.
- Conan + vcpkg(overlay port)를 함께 유지한다.

## 2. 태그 규칙

- Core: `core/vX.Y.Z`
- Node: `node/vA.B.C`
- Python: `python/vA.B.C`
- Java: `java/vA.B.C`
- .NET: `dotnet/vA.B.C`
- C++ binding: `cpp/vA.B.C`

Core 버전(`VERSION`)과 바인딩 버전은 독립적으로 관리한다.

## 3. GitHub Actions

### Core 릴리즈

- 워크플로우: `.github/workflows/build.yml`
- 트리거: `core/v*` 태그, `workflow_dispatch`
- 산출물:
  - 플랫폼별 core native archive
  - source tarball
  - checksums
- Conan 배포 워크플로우: `.github/workflows/core-conan-release.yml`

### 바인딩 릴리즈

- 워크플로우: `.github/workflows/bindings-release.yml`
- 트리거:
  - 언어별 태그(`node/v*`, `python/v*`, `java/v*`, `dotnet/v*`, `cpp/v*`)
  - `workflow_dispatch` (target/version 지정)
- 동작:
  - 언어별 버전 파일 검증
  - 언어별 테스트/패키징
  - 필요 시 GitHub Release 생성
  - 필요 시 npm/PyPI/NuGet/Maven publish

필요 계정/시크릿: `doc/building/release-accounts.ko.md`

## 4. Conan

Core 소스 tarball 기준으로 관리한다.

공식 배포(ConanCenter)는 레시피 PR 기반이며, 사내 remote 업로드는 선택 사항이다.

### 관련 파일

| 파일 | 경로 |
|------|------|
| conanfile | `core/packaging/conan/conanfile.py` |
| conandata | `core/packaging/conan/conandata.yml` |
| README | `core/packaging/conan/README.md` |

### 업데이트 절차

1. `core/vX.Y.Z` 태그 릴리즈
2. tarball URL/sha256 확인
3. `conandata.yml` 업데이트
4. `conan create . --version X.Y.Z` 실행

## 5. vcpkg (overlay port)

공식 배포(vcpkg ports)는 PR 기반이며, overlay port는 개발/검증용으로 유지한다.

### 관련 파일

| 파일 | 경로 |
|------|------|
| portfile | `vcpkg/ports/zlink/portfile.cmake` |
| vcpkg.json | `vcpkg/ports/zlink/vcpkg.json` |
| README | `vcpkg/README.md` |

### 업데이트 절차

1. `core/vX.Y.Z` 태그 릴리즈
2. `vcpkg/ports/zlink/vcpkg.json`의 `version-string` 업데이트
3. overlay 설치 검증:

```bash
vcpkg install zlink --overlay-ports=./vcpkg/ports
```

## 6. 바인딩 릴리즈 운영 예시

1. Core `core/v0.10.0` 릴리즈
2. Node 바인딩 수정/검증
3. `node/v1.3.0` 태그로 Node만 배포
4. Python은 준비 후 `python/v0.10.2`로 별도 배포

## 7. 체크리스트

- [ ] Core `VERSION` 갱신
- [ ] Core 태그(`core/vX.Y.Z`) 릴리즈
- [ ] Conan metadata 갱신
- [ ] vcpkg overlay 버전 갱신
- [ ] 바인딩별 버전 파일 갱신
- [ ] 바인딩별 태그로 개별 릴리즈

## 8. 관련 문서

- [Framework와 Bindings 의존 경계 정리](./framework-bindings-dependency-boundary.ko.md)
- [Local Package 스크립트](../../scripts/local-package/README.ko.md)
