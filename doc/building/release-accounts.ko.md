[English](./release-accounts.md) | [한국어](./release-accounts.ko.md)

# 공개 배포 계정과 시크릿

이 문서는 zlink 공개 배포에 사용하는 확정 계정, namespace와 GitHub Actions secret 이름을
정리한다. Python·Go·Rust binding은 이번 공개 배포 범위 밖이다.

## 공개 채널

| 채널 | 계정·namespace | 배포 방식 |
| --- | --- | --- |
| GitHub | 조직 `zlink-systems`, 저장소 `zlink-systems/zlink` | 언어별 태그와 GitHub Release 자산 |
| GitHub (examples 미러) | `zlink-systems/zlink-{cpp,dotnet,java,node}-examples` — 읽기 전용, issue·wiki 비활성, PR을 받지 않는다 | 릴리스 워크플로가 언어별 게시·검증 뒤 `examples-mirror.yml`을 불러 push([release-pipeline](release-pipeline.ko.md) §1) |
| Maven Central | namespace `systems.zlink` 검증 완료 | Sonatype Central Portal bundle 업로드 |
| nuget.org | 개인 계정 `zlink` | Trusted Publishing(OIDC), API key 없음 |
| npm | 개인 계정 `zlink-systems`, scope `@zlink-systems`, 2FA | Trusted Publishing(OIDC)+provenance. binding은 `bindings-release.yml`, framework 8개는 `framework-release.yml`로 패키지별 등록(2026-09-09 완료) |
| ConanCenter | GitHub 계정 `zlink-systems` | recipe PR 제출 방식 |
| vcpkg | GitHub 계정 `zlink-systems` | ports PR 제출 방식 |

nuget.org Trusted Publishing 정책은 다음 값으로 고정돼 있다.

| 항목 | 값 |
| --- | --- |
| 정책 | `zlink-dotnet-release` |
| repository owner | `zlink-systems` |
| repository | `zlink` |
| workflow 파일명 | `release-dotnet.yml` |
| environment | 없음 |
| package glob | `Zlink`, `Zlink.*` |

따라서 binding과 framework의 .NET package push는 모두
`.github/workflows/release-dotnet.yml`에서 수행한다. 다른 workflow에서 받은 OIDC token은 이
정책과 일치하지 않는다.

## GitHub Actions secrets

공개 배포 workflow가 참조하는 repository secret은 다음 여덟 개뿐이다. 문서와 로그에는 값이 아니라
이름만 기록한다.

| Secret | 용도 |
| --- | --- |
| `MAVEN_CENTRAL_USERNAME` | Sonatype Central Portal token username |
| `MAVEN_CENTRAL_PASSWORD` | Sonatype Central Portal token password |
| `SIGNING_KEY` | armored GPG 개인키를 base64로 인코딩한 값 |
| `SIGNING_PASSPHRASE` | GPG 개인키 passphrase |
| `EXAMPLES_MIRROR_KEY_CPP` · `_DOTNET` · `_JAVA` · `_NODE` | 각 examples 미러 저장소에 등록한 write deploy key(ed25519)의 개인키. 미러 하나에만 push할 수 있고, deploy key는 저장소 하나에만 붙으므로 넷이다. 회전은 `ssh-keygen` → `gh repo deploy-key add --allow-write` → `gh secret set`으로 하며 개인키 파일은 등록 직후 지운다 |

기존 signing key를 사용하며 새 key나 token을 생성하지 않는다. `GITHUB_TOKEN`은 다른 저장소에
push할 수 없으므로 미러만 deploy key를 쓴다.

## 사용하지 않는 secret과 사내 remote

| 이름 | 처리 |
| --- | --- |
| `NPM_TOKEN` | 사용하지 않음. 첫 npm 배포는 사용자 로컬 수동 게시, 이후 OIDC 사용 |
| `NUGET_API_KEY` | 사용하지 않음. `NuGet/login@v1`이 발급한 단기 token만 사용 |
| `PYPI_API_TOKEN` | 이번 배포 범위 밖인 Python용이므로 사용하지 않음 |
| `MAVEN_REPOSITORY_URL` | 사내 Maven remote 전용. Maven Central 공개 배포에는 사용하지 않음 |
| `MAVEN_REPOSITORY_USERNAME` | 사내 Maven remote 전용 |
| `MAVEN_REPOSITORY_PASSWORD` | 사내 Maven remote 전용 |

ConanCenter와 vcpkg 공개 배포에는 registry login secret이 없다. 둘 다 GitHub PR로 제안하고,
이 준비 작업에서는 draft까지만 만든다.

## 공개 배포 순서

1. `core/v0.17.3` GitHub Release와 자산을 확인한다.
2. C++·Node.js·Java·.NET binding 0.17.3을 배포한다.
3. 공개 채널에서 binding 설치와 checksum을 확인한다.
4. Framework 0.10.0을 C++·Node.js·Java·.NET 순으로 배포한다. .NET push는
   `release-dotnet.yml`에서 수행한다.

관련 workflow 초안은 `.github/workflows/bindings-release.yml`,
`.github/workflows/release-dotnet.yml`, `.github/workflows/framework-release.yml`이다.
