[English](./release-accounts.md) | [한국어](./release-accounts.ko.md)

# 공식 배포 계정/시크릿 체크리스트

이 문서는 공식 배포에 필요한 계정과 GitHub Actions Secrets를 정리한다.

## 1. GitHub Repository 권한

- `Contents: write` 권한 (Release 생성)
- 태그 푸시 권한 (`core/v*`, `node/v*`, `python/v*`, `java/v*`, `dotnet/v*`, `cpp/v*`)

## 2. Node (npm)

- 필요 계정: npm 조직/패키지 배포 권한 계정
- 필요 시크릿:
  - `NPM_TOKEN`: npm publish 가능한 automation token

## 3. Python (PyPI)

- 필요 계정: PyPI 프로젝트 소유자/maintainer
- 필요 시크릿:
  - `PYPI_API_TOKEN`: PyPI API token (`__token__` 업로드용)

## 4. Java (Maven Repository)

현재 workflow는 기본값으로 GitHub Packages Maven registry에 배포하도록 구성되어 있다.

- 필요 계정: GitHub 계정 + repository `packages: write` 권한
- 필요 시크릿: 없음 (기본 `GITHUB_TOKEN` 사용)

외부 Maven 저장소(Nexus/Artifactory/Sonatype 등)에 배포하려면 아래 시크릿을 추가로 설정한다.

- `MAVEN_REPOSITORY_URL`
- `MAVEN_REPOSITORY_USERNAME`
- `MAVEN_REPOSITORY_PASSWORD`

## 5. .NET (NuGet)

- 필요 계정: NuGet.org(또는 사내 NuGet 서버) 배포 권한 계정
- 필요 시크릿:
  - `NUGET_SOURCE_URL` (예: `https://api.nuget.org/v3/index.json`)
  - `NUGET_API_KEY`

## 6. Conan (Core)

오픈소스 공식 배포를 ConanCenter로 할 경우는 업로드 토큰이 아니라 레시피 PR 기반으로 진행한다.

사내/개인 Conan remote에 업로드할 경우에만 아래 시크릿이 필요하다.

- `CONAN_REMOTE_URL`
- `CONAN_LOGIN_USERNAME`
- `CONAN_PASSWORD`

## 7. vcpkg

- overlay port 배포(현재 구성): 별도 계정 불필요
- 공식 vcpkg ports 반영: GitHub 계정 + PR 승인 필요

## 8. 워크플로우 파일

- Core 릴리즈: `.github/workflows/build.yml`
- Core Conan 배포: `.github/workflows/core-conan-release.yml`
- 바인딩 릴리즈/레지스트리 배포: `.github/workflows/bindings-release.yml`
