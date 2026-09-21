# 워크트리 설정 — 머신은 한 번, 트리는 `rebuild-dev → gate`

> 이 문서는 "새 worktree가 gate까지 돌 수 있게 되는 절차와 그 원리"를 소유한다. Issue·PR
> 흐름은 [`development-workflow.ko.md`](./development-workflow.ko.md), 이 머신의 자산과 함정은
> [`workspace-notes.ko.md`](./workspace-notes.ko.md), 빌드 방법은
> [`doc/building/build-guide.ko.md`](../../building/build-guide.ko.md), 로컬 패키지 명령은
> [`scripts/local-package/README.ko.md`](../../../scripts/local-package/README.ko.md)가 소유한다.

## 1. 규칙 하나

**머신 설치는 한 번, worktree에서는 `work.sh start → rebuild-dev → gate`뿐이다.** 별도 준비
단계나 체크리스트는 없다. 무엇이 갖춰졌는지는 `scripts/gate/check-env.sh`(Windows
`check-env.ps1`)가 항목마다 한 줄로 알려 주고, `rebuild-dev`와 각 gate는 시작할 때 같은 검사를
호출한다. 검사가 `missing`을 내면 그 줄의 설치 명령을 실행하고 다시 시작한다.

2026-09-21 이전에는 이 규칙이 없었다. 캐시가 트리 안에 있었고 gate가 빌드 산출물을 "이미
있다"고 가정해, 새 clone의 첫 gate에서 실패 40여 건 중 절반이 환경이었다(#816).

## 2. 머신에 한 번

`check-env.sh`가 검사하는 항목이다. 항목 이름은 스크립트 출력과 같다. "왜"만 여기 적고
"어떻게 확인하는가"는 스크립트가 소유한다.

| 항목(`check-env` 출력 이름) | 왜 필요한가 |
|---|---|
| `JDK 25` (`JAVA_HOME` 또는 PATH의 `java`) | `bindings/java`가 JDK 25 미만을 거부한다. 없으면 `rebuild-dev`의 java 패키지 단계에서 멈춘다 |
| `VCPKG_ROOT` | C++ framework·샘플·cross-language 빌드가 opentelemetry-cpp 등을 vcpkg에서 찾는다. `VCPKG_OVERLAY_PORTS`는 스크립트가 `vcpkg/ports`로 export한다. WSL에서는 Linux 트리(`~/.cache/zlink/vcpkg`)를 가리켜야 하며 `/mnt/c`의 Windows 트리가 아니다 |
| `Playwright Chromium` | Node·shared ZoneWorld client의 브라우저 샘플. 머신 캐시(Playwright 기본 위치: Linux `~/.cache/ms-playwright`, Windows `%LOCALAPPDATA%\ms-playwright`) 하나를 모든 트리가 쓴다 |
| `docker` | 샘플의 Redis, cross-language e2e |
| `dotnet`, `node >= 20`, `cmake >= 3.20`, `ninja`(Linux) | 각 언어 빌드. Windows는 MSVC generator를 쓰므로 ninja가 없다 |

WSL 셸 profile에 `JAVA_HOME`·`VCPKG_ROOT`를 두면 새 셸마다 다시 export하지 않는다. Windows는
User 환경 변수로 같은 값을 둔다(Machine 값이 다른 버전을 가리키면 `JAVA_HOME`을 보는 도구는
User 값을 쓰지만 `java` 명령은 PATH 순서를 따른다).

## 3. worktree 만들기 — 로컬 패키지는 링크다

`scripts/dev/work.sh start --issue <N>`(Windows `work.ps1`)이 브랜치와
`~/project/zlink-<N>-<slug>` worktree를 만들고 로컬 패키지를 준비한다. 이때 binding 패키지는
**빌드하지 않고 링크한다**.

- binding 패키지(nuget `Zlink.*`, npm `@zlink-systems/zlink`, maven `systems.zlink:zlink*`, C++
  `install/zlink-cpp`)는 입력이 같으면 결과가 같으므로 `~/.cache/zlink/packages/<키>/`에 한 번만
  게시하고, worktree의 `.artifacts/wsl/` 안 파일은 그곳으로의 symlink다. 키는
  `sha256(bindings/ 트리 ‖ 언어별 VERSION ‖ Core 버전 ‖ scripts/local-package/ 트리 ‖ 플랫폼 ‖
  도구 버전 id)` 앞 16자리이며, 게시된 키는 불변이다. 동시 빌드는 `<키>.lock`으로 하나만
  쓴다.
- Core release prefix는 `~/.cache/zlink/core/<ver>/<플랫폼>`을 링크한다.
- `bindings/`·`scripts/local-package/`에 커밋되지 않은 변경이 있으면 공유하지 않고 worktree
  전용 `.artifacts/wsl-private/`에 빌드한다. dirty 산출물은 공유 키로 게시하지 않는다.
- 공유 범위는 binding 패키지뿐이다. framework가 만드는 패키지(`Zlink.HttpClient`,
  `@zlink-systems/http-client` 등)와 빌드 트리는 worktree마다 만든다.
- hit 경로도 miss와 같이 검증한다(`sync-version.py --check`, `build-wsl.sh --verify-versions`).
  .NET은 gate가 패키지 digest별 `NUGET_PACKAGES`를 써서 같은 버전·다른 내용이 섞이지 않는다.
- 문서 작업처럼 패키지가 필요 없으면 `--no-packages`.

확인: `ls -la .artifacts/wsl/nuget`이 `-> ~/.cache/zlink/packages/<키>/dotnet/nuget/…`를 보이면
링크다. `scripts/local-package/build-wsl.sh --cache-key`가 키를 낸다.

Windows(`build-windows.ps1`)는 아직 이 캐시를 쓰지 않고 worktree마다 `.artifacts/windows`에
빌드한다(#816 2단계 후보).

## 4. 빌드와 gate

```bash
scripts/gate/rebuild-dev.sh <tag>        # core/build-dev → Core prefix → cpp/dotnet/java/node 패키지 → node/java 재설치
scripts/gate/framework-gate.sh <tag>     # 7 샘플 × 4 언어, node/java/dotnet 테스트
scripts/gate/cross-language-e2e.sh <tag> # cpp all-stage, node smoke, java-cross
scripts/gate/bindings-gate.sh <tag>      # Core를 바꿨을 때
```

결과는 `zlink-work/gates/<tag>/results.txt`(step마다 append되므로 마지막 상태를 본다). gate는
한 번에 하나만 돌린다 — 샘플은 고정 포트를 쓰고 테스트는 시간을 단언한다. Windows는
`scripts/gate/rebuild-dev.ps1` 뒤 각 언어 `samples/<S>/run_sample.ps1`이다.

gate가 필요로 하는 빌드 산출물(Java cross-language `Host/installDist`, C++
`build/linux-ninja-c-e2e` 구성, `/dev/shm/zlink-tmp-*`)은 gate 자신이 만든다. 사람이 미리
만들어 두는 것은 없다.

## 5. 확인된 함정

- **저장소 안 Node 샘플은 repository 모드다.** `framework/languages/node`에서 `npm ci` 한 번이면
  되고, 샘플 디렉터리에서 `npm install`을 하면 registry 패키지와 workspace 패키지가 섞여
  7종이 전부 실패한다(`Nest can't resolve dependencies of the DiscoveryService`). 첫 줄
  `sample_dependency_mode=repository`로 확인한다. 샘플별 `npm install`은 examples 저장소 clone의
  절차다.
- **Java 샘플에 `ZLINK_LIBRARY_PATH`를 주지 않는다.** jar가 native를 동봉하며, 이 변수는 파일
  경로로 `System.load`되므로 디렉터리를 주면 `UnsatisfiedLinkError`가 난다.

## 6. 정리

`work.sh done --verified <sha>`가 merge 뒤 worktree와 링크를 지운다(캐시는 남는다).
`scripts/local-package/cache-prune.sh --keep 5`는 존재하는 worktree가 링크한 키를 지우지
않는다. 방치된 worktree는 `scripts/dev/worktree-sweep.sh`로 판정한다. worktree를 지우기 전에
거기서 도는 job이 없는지 본다.
