# 작업 환경 메모 — 여기 있는 것과, 여기서 밟는 것

> 이 문서는 "이 머신에서 무엇을 쓸 수 있고 무엇을 조심해야 하는가"를 소유한다. 빌드 방법은
> [`doc/building/build-guide.ko.md`](../../building/build-guide.ko.md), 검증의 판단 기준은
> [`verification-principles.ko.md`](./verification-principles.ko.md), 작업 절차는
> [`development-workflow.ko.md`](./development-workflow.ko.md)가 소유한다.
>
> **확인한 것만 적는다.** 추측한 함정은 다음 사람의 시간을 더 쓰게 한다.

## 1. 검증 자산 — CI로 가기 전에 여기를 본다

로컬에서 재현할 수 있는 것을 CI 왕복으로 확인하지 않는다. 이미 갖춰져 있는 것들이다.

| 무엇 | 어디 | 무엇을 재현할 수 있나 |
|---|---|---|
| `g++-11`, `g++-13` | `/usr/bin` | **교차 툴체인.** 릴리스 아카이브는 ubuntu-22.04(GCC 11·binutils 2.38)가 만들고 이 머신은 24.04(GCC 13·binutils 2.42)다. 정적 아카이브·ABI 문제는 이 차이에서만 드러난다 |
| vcpkg 트리 | `~/.cache/zlink/vcpkg/installed/x64-linux` | **다른 라이브러리 버전과의 충돌.** Boost 1.92가 들어 있고 Core는 vendored 1.85를 쓴다 |
| emsdk 3.1.38 | `~/.cache/zlink/emsdk` | **emscripten 링크.** Unity 6000.0~6000.4가 쓰는 버전에 맞췄다 |
| Playwright Chromium | `~/.cache/ms-playwright` | **브라우저 실행.** `scripts/browser-e2e/`가 쓴다 |

없는 것을 만드는 비용과 CI 왕복 비용을 비교한다. 대개 만드는 쪽이 싸다 — GCC 하나 더
까는 데 1분이고, 그것 하나로 CI 5회차를 아꼈다(#418).

## 2. 밟은 것들

**`pgrep -f`·`pkill -f`가 자기 셸을 죽인다.** 패턴이 명령줄 전체와 대조되므로 그 명령을
실행 중인 셸 자신이 매치된다. 셸이 죽고 종료값 144가 나온다. 프로세스 이름으로 좁히거나
(`pgrep -x`) PID를 먼저 얻어 `kill`한다.

**주 체크아웃이 main이 아닐 수 있다.** `/home/hep7/project/zlink`는 이전 작업 브랜치에
머물러 있곤 한다. 거기서 읽은 "현재 코드"로 판단하면 틀린다 — 이미 고쳐진 것을 결함으로
보고하거나, 제거된 API를 "빠뜨렸다"고 지적하게 된다. 사실 확인은
`git grep <패턴> origin/main -- <경로>`로 하거나, 파일:줄을 인용하기 전에
`git rev-parse --abbrev-ref HEAD`로 어디서 읽었는지 본다.

**worktree를 지우면 거기서 도는 작업이 죽는다.** 머지 뒤 정리할 때 그 worktree를 쓰는
job이 남아 있는지 먼저 본다.

**`/tmp`는 8 GB tmpfs다.** 테스트가 남긴 `/tmp/zlink-native-*`가 쌓여 차면 도구 출력이
ENOSPC로 깨진다. 긴 세션에서는 한 번씩 정리한다.

## 3. 릴리스 툴체인

릴리스 아카이브를 만드는 러너와 그 버전은 `.github/workflows/build.yml`이 소유한다.
**로컬 통과는 릴리스 툴체인 통과가 아니다.** 산출물의 바이너리 성질(심볼 가시성, LTO,
링크 가능성)을 다루는 변경이면 러너 버전을 확인하고 그 버전으로 재현한다.
