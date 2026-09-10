---
title: "Handoff 2026-09-10 — framework 메시징 성능 캠페인·작업 방식·whole-message API"
---

# Handoff 2026-09-10 — 새 세션이 이어받을 것

> 이 문서는 세션 fa34ae83(2026-09-10)의 감독자가 새 세션에 넘기는 인수인계다. 결정의 근거는
> `doc/plan/fw-bench-worklog/decisions.ko.md` FB-056~FB-064가 소유하고, 이 문서는 **지금 어디까지 왔고
> 무엇을 다음에 해야 하는지**만 적는다. 읽는 순서: §1 → §2 → §4 → 나머지.

## 1. 30초 요약

- **목표(사용자)**: framework send·request 처리량이 같은 언어 binding 직접 경로의 **0.90 이상**. 1.0 릴리스는
  내일~모레. 측정·수정을 릴리스 경로에 넣는다.
- **규범**: `framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.{ko,en}.md`
  (PR #26, codex 리뷰 4회). E1 문구는 PR #52로 정정됨(wire가 요구하는 한 번의 쓰기는 추가 복사가 아니다).
- **오늘 밝혀진 핵심**: 남은 격차의 정체는 "무엇을 하느냐"가 아니라 **"그 일을 어디서 하느냐"** 다.
  Java 제출 경로에서 맵 조회 3회(본문 0.57 µs)가 lane 왕복으로 **177 µs**를 쓴다(Issue #78, FB-063).
  같은 처방으로 send 지연 120 ms → 0.3 ms(PR #76), 수신 pump platform thread로 send 4.7배(PR #80).
- **벤치는 비교 짝이 어긋나 있었다**(FB-064). core는 DEALER→ROUTER, framework는 RouteMesh ToChannel,
  gRPC는 1:1. Issue #13을 "짝 맞추기"로 재정의했고 Java부터 작업 중.
- **Core API 방향 결정(사용자)**: part 단위 API 8개 제거 + whole-message send·recv 신설 + `zlink_xpub_recv_part`
  → `zlink_xpub_recv` 이름만 변경. 설계 `doc/draft/core-whole-message-recv-api.ko.md` §7, 계획
  `doc/plan/core-whole-message-recv-api-apply-plan.ko.md` §10, Issue #63. **다른 머신이 진행 중(약 2시간)**.

## 2. 지금 돌고 있는 것 (세션 종료 시점)

| job | worktree | 이슈 | 하는 일 | 끝나면 |
|---|---|---|---|---|
| `bench-pairing` | `~/project/zlink-13-bench-pairing` | #13 | Java 벤치 행을 core ROUTER↔ROUTER·framework ToNode로, 패턴 3개, 3-run | 보고서 읽고 → 문서(`framework/bench/grpc/README.*`, `doc/comparison.*`) 감독자가 반영 → PR(Refs #13) → 나머지 언어 확대 여부 결정 |
| `java-lane-lookup` (3차) | `~/project/zlink-78-lane-lookup` | #78 | 제출 경로 조회 3회 왕복을 turn 1회로. 검증·timeout 확정을 `submit()`으로 옮기는 것은 **감독자가 허용함**(#78 코멘트) | 테스트 직접 재실행 → 제출 구간 µs·turn 진입 횟수 전후 확인 → PR(Closes #78) |

### 결과를 어디서 받나

job 결과는 두 곳에 남는다.

**1. 보고서 파일** — job이 끝나면 여기에 쓴다. 같은 폴더에 PR 본문 초안 `pr-body.md`, 측정 원본, 로그가 있다.

| job | 보고서 |
|---|---|
| `bench-pairing` | `.artifacts/codex/bench-pairing/summary.md` |
| `java-lane-lookup` | `.artifacts/codex/java-lane-lookup/summary.md` |

**2. 코드** — 작업 worktree의 branch에 커밋으로 남는다. **push는 하지 않으므로** 감독자가 검증한 뒤 PR을 올려야 main에 들어간다.

| job | worktree | branch |
|---|---|---|
| `bench-pairing` | `~/project/zlink-13-bench-pairing` | `bench/13-pairing` |
| `java-lane-lookup` | `~/project/zlink-78-lane-lookup` | `framework-java/78-lane-lookup` |

**끝났는지 보는 법**: `bash scripts/dev/job.sh status` — 상태가 `완료`, summary 열이 `있음`이 되면 끝난 것이다.
`실패`면 `.artifacts/codex/<job>/job.log` 끝을 본다. `죽음`(summary 없이 종료)이면 로그 끝의 마지막 오류를 본다.

**받은 뒤 감독자가 하는 순서**(오늘 매번 한 절차):
1. `summary.md`를 읽고 BLOCKERS부터 본다. 작업자가 스펙 충돌로 멈췄으면 그 판단이 옳은지 스펙을 직접 읽고 판정한다.
2. worktree에서 `git fetch && git rebase origin/main` 뒤 **테스트를 직접 돌린다**(작업자 결과를 그대로 믿지 않는다).
   Java: `JAVA_HOME=/home/hep7/.cache/zlink/jdk/temurin-25 ./gradlew --no-daemon :zlink-framework-core:test contractTest --continue`.
3. diff를 읽는다. 특히 스펙 06(state lane)·08(hot path)과 충돌하는 형태가 없는지.
4. `pr-body.md`를 바탕으로 PR 본문을 쓰되, **감독자가 직접 확인한 것과 작업자 보고를 구분해 적는다.** 측정값은 표로.
5. 작업 worktree 안에서 `work.sh pr --body <파일> --closes|--refs` → `work.sh done --verified $(git rev-parse HEAD)`.
   `Refs`면 worktree가 남고 `Closes`면 지워진다. `done` 뒤에는 `cd ~/project/zlink`(셸이 지워진 디렉터리에 남는다).
6. 결과가 결정 기록에 남을 만하면 `doc/plan/fw-bench-worklog/decisions.ko.md`에 FB-nnn으로 적는다.

3분 주기 감시는 새 세션에서 다시 건다 — Monitor로 `scripts/dev/job.sh watch` 또는 scratchpad의 watch-jobs.sh 형태.

**다른 머신이 맡은 것(건드리지 않는다)**: #63(whole-message API), #60, #61, #65, #77, #79, 그리고 Windows #22·#23·#24.

## 3. 오늘 main에 들어간 것 (PR 번호)

| 영역 | PR | 내용 |
|---|---|---|
| 스펙 | #26, #52 | messaging hot path 08장 신설, E1 정정 |
| 작업 방식 | #28, #30, #39, #40, #41, #42, #43, #51, #55 | work.sh, 공유 캐시, job.sh, session-setup, ci-watch, worktree-sweep, release-check, job.sh 결함 2건 |
| 문서 | (직접) | 작업 방식 §2.0(세 종류 구분, 실험→PR, Issue는 PR 시점), §4.3(스크립트 목록) |
| 벤치 | #44, #70 | 드라이버 공정성(FB-060), raw도 protobuf 직렬화(#66) |
| C++ | #53, #59, #73 | 수신·완료 통합(serial -20% 회귀 알고 머지), 계약 테스트 60/60, 제출·완료 2차 |
| Java | #29, #62, #74, #76, #80 | 수신 폴링 제거, 복사·실행기·활성화, 클라이언트 정리(+7.4%뿐), send 지연 120→0.3 ms, platform pump |
| .NET | #71 | lane 왕복·지속 worker·batch (send 3배, window 2배). I0 대기는 미완 |
| Node | #72 | 64건 batch·복사 제거. 대기는 여전히 1 ms 타이머(#50 코멘트에 perf 참조 구현 있음) |
| 계획 | (직접) | FB-059~064, draft §7 / plan §10 |

## 4. 다음에 할 일 — 우선순위

1. **#78 결과 확인·머지.** 예상: 제출 구간 193 → ~16 µs, request-serial 1,989 → ~3,000 ops/s(binding 대비 0.31 → 0.48).
2. **완료 구간 프로파일.** 제출 구간만 쟀다. 응답 도착 → caller continuation 구간이 미측정이고, 요청당 스레드 전달
   12회 중 대부분이 여기로 추정. 방법은 `.artifacts/codex/java-client-profile/brief.md`와 같게, 구간만 바꾼다.
   **#78 머지 뒤에** 잰다(섞이면 안 된다).
3. **#13 결과로 새 기준 잡기.** 분모가 바뀌므로 이전 숫자와 비교하지 않는다. Java가 되면 나머지 4언어로 확대.
4. **Java에서 찾은 패턴을 다른 언어에 적용.** (a) 수신 pump가 가상/그린 스레드인지(FB-062), (b) 제출 경로에서
   읽기 전용 조회가 lane 왕복인지(FB-063). .NET은 감사에서 lane 왕복 14회가 나왔고(#48), C++은 7회(#49).
5. **warmup 20초 검증.** gRPC 벤치 셀당 34초 중 20초가 warmup. bindings perf는 2초다. 10초·5초로 줄여 결과가 같은지
   먼저 확인한 뒤 줄인다(측정 조건 변경이므로 근거 기록).
6. **모델 비교 벤치 별도 이슈.** DEALER→ROUTER, ToChannel vs ToNode(=채널 선택 비용), ClientServer. gRPC 표에 섞지 않는다.
7. 릴리스 판단: 4언어 3-run은 #13 반영 + 조용한 기계에서만. `request-window`는 gRPC 비교에서 제외.

## 4.5 따로 알아야 할 두 건

### 4.5.1 Java 수신 pump — platform thread로 바꿨고, 가상 thread 문제는 반만 확인됐다 (Issue #75, PR #80, FB-062)

**경위.** send 지연 수정(#68) 작업자가 "가상 thread에서 멀티파트 수신이 실패한다"고 보고했다. 3-part record 100건을 받는 재현에서
platform thread는 100건, 가상 thread는 7건 뒤 `ZlinkRecvException`(`NativeRouterReceiveSupport.recvRemainingMultipartParts`).
그래서 수신 pump(`ZLinkJavaRawMeshNode.startPump`)를 platform thread로 바꾸는 **임시 우회**로 시작했다.

**그런데 재현이 현재 main에서는 안 난다.** 감독자가 같은 재현을 직접 돌리니 네 조합(virtual/platform × forceYield on/off) 모두 100건 정상이었다.
그 실패는 당시 작업 branch 상태나 계측 에이전트(`carrier-probe.jar`)에 의존했던 것으로 보인다. **정확성 결함은 미확정이다.**

**성능 차이는 실재한다.** 같은 커밋을 pump 종류만 바꿔 재니(1-run, 1024 B, `.artifacts/vt-compare/`):

| 패턴 | 가상 thread | platform thread | 차이 |
|---|---:|---:|---:|
| send-saturation 처리량 | 14,356 msg/s | 67,712 msg/s | **4.7배** |
| send-saturation 지연 | 125.462 ms | 0.247 ms | **1/508** |
| request-serial 처리량 | 1,927.8 ops/s | 2,233.2 ops/s | +15.8% |

이 pump는 socket 하나를 blocking으로 기다리는 전용 실행 단위라 가상 thread의 이점(대기 중 carrier 반납)이 없고 비용만 남는다.
그래서 **우회가 아니라 성능 수정으로 채택**했다(PR #80, 주석에 측정값 기록).

**남은 것(#75 열어 둠):**
- 가상 thread에서 blocking 수신이 왜 이렇게 느린지 원인 미확정. 다른 언어의 수신 pump가 그린 스레드·task 기반이면 같은 확인이 필요하다.
- 멀티파트 실패 보고가 무엇이었는지. 재현 코드는 `.artifacts/codex/java-send-latency/binding-repro/`에 있다(`run.sh`가 에이전트와
  `-Dissue68.forceYield=true`를 쓴다 — 그 조건에서만 나는지 확인).
- 근본 해결은 #63이다. part 루프 자체가 없어지면 carrier 이동 여부와 무관해진다.

### 4.5.2 Core C API를 whole-message로 통일 (Issue #63, 다른 머신 진행 중)

**결정(사용자, 2026-09-10).** part 단위 공개 API를 없애고 send·recv 모두 parts 배열 + count를 받는 whole-message API로 통일한다.

**왜.** part 단위 표면이 "한 record의 첫 part부터 FINAL까지 같은 thread"라는 계약을 만들고, 그 계약이 미완성 record 상태·`BUSY`·
부분 재시도 규칙·thread 이동 민감성을 낳는다. #75가 그 사례다. 한 번의 호출로 record를 다루면 이 조건이 통째로 사라진다.
성능으로도 메시지당 native 경계 왕복(.NET 진단 14회)이 send·recv 각 1회로 줄어든다.

**조사 결과(저장소 전수, draft §7.2):** lazy send 없음, part 단위 수신 의존 없음(Core가 첫 part 공개 전에 record 전체를 버퍼링 —
`core/src/runtime/sockets/common/socket_base.hpp:816`), STREAM·XPUB은 이미 part 단위가 아님, `framework/languages`의 part API 호출 **0건**,
Core 내부 호출자 **0건**. 예외는 perf 하네스 한 곳(`bindings/c/perf/single/common/perf_single_reqrep.hpp:647-664`, 빈 FINAL만 재시도)뿐.

**공개 표면 변화:**

| 구분 | 개수 | 내용 |
|---|---:|---|
| 제거 후 대체 | 8 | send 5(`zlink_send_part`·`_rid`·`zlink_request_part`·`zlink_reply_part`·`zlink_publish_part`), recv 3(`zlink_recv_part`·`zlink_router_recv_part`·`zlink_subscribe_part`) |
| 이름만 변경 | 1 | `zlink_xpub_recv_part` → `zlink_xpub_recv` (계약·인자 불변) |
| 그대로 유지 | 1 | `zlink_stream_recv_packet` (header/body 고정 2슬롯) |
| 사라지는 타입 | 1 | `zlink_part_flag_t` (MORE/FINAL) |

결과적으로 공개 헤더에 `_part`로 끝나는 함수가 남지 않는다. **바인딩 공개 시그니처는 send·recv 모두 불변** — 내부 루프가 1회 호출로 바뀔 뿐.
선례: `zlink_completion_t`가 이미 `reply_parts` 배열 + `reply_part_count`를 공개한다(`api.h:64-65`). 새 관용이 아니라 기존 관용을 나머지에 맞추는 것.

**먼저 정할 계약 셋(plan §10.2):** `count > capacity` 처리, STREAM의 길이 0 part "peer 끊기" 의미 보존, 실패 시 whole-record 재시도.
**함께 삭제되는 스펙 규칙:** "한 record는 같은 thread", 진행 중 시퀀스에서 오는 `BUSY`, "실패한 FINAL은 staged prefix를 버린다".

**문서:** 설계 `doc/draft/core-whole-message-recv-api.ko.md` §7(제거 8개의 현재 시그니처 전문·대체 표·신설 시그니처 초안·바인딩 매핑),
계획 `doc/plan/core-whole-message-recv-api-apply-plan.ko.md` §10(제거·신설 대상, 계약 셋, 단계 게이트 추가분, `libzlink.vers`, 삭제할 스펙 규칙).

**이 세션이 할 일은 없다.** 다른 머신이 진행한다. 끝나면 #75 우회(platform pump)를 되돌릴지 다시 판단한다 — 다만 platform pump는
성능 근거로 채택한 것이라 #63과 무관하게 유지될 가능성이 크다.

## 5. 각 언어의 현재 위치와 남은 것

| 언어 | 마지막 1-run(1024) | 남은 큰 것 |
|---|---|---|
| Java | request-serial 1,989(binding 6,417·gRPC 5,929) / send 67,712(platform pump) | #78 제출 turn, 완료 구간 미측정, 요청별 timer→entry 값(일부 남음), W5 지속 worker |
| .NET | send 4096 62,694(3배) / window 4096 20,360(2배) | I0 대기: 깨우기 원인 4개(data·completion·permit·deadline) 배선 — **timer가 아니다**(#48 코멘트) |
| C++ | serial 0.132~0.137(**-20% 회귀 알고 머지**) / window 0.021 / send 0.010~0.014 | serial 회복 먼저, E1 본문 이중 생성, header JSON 2회 파싱, I4 큐 2단, W5 |
| Node | 측정 불가(framework 벤치 클라이언트 없음, #10) | 대기를 perf 구조로(`bindings/node/perf/multi/perf_multi_dealer_router_server.ts:102-112`, 상한은 다음 관리 deadline), #10 코덱 bytes + 하네스 전환 |

## 6. 감독자가 배운 규칙 (반복 금지)

- **"기능이 없다"는 보고는 `bindings/*/perf`에서 같은 패턴을 찾아 인용시킨 뒤에만 받는다.** Node "비동기 readiness 없음"이
  틀렸던 사례(FB-062 경위, #50 코멘트).
- **정적 감사로 고른 후보는 측정으로 확인한 뒤 고친다.** 클라이언트 항목 7개를 다 없앴는데 +7.4%였다(PR #74). 프로파일이
  진짜 원인(177 µs 왕복)을 찾았다.
- **"lane 밖에서 읽으라"는 스펙 06 §3 위반이다.** 답은 turn 안에서 다 끝내기(#78 정정 코멘트).
- **"새 timer 금지"는 요청별 timer 이야기다.** 회전 단위 공용 대기는 금지 대상이 아니다(#48 정정 코멘트).
- **재현 보고는 현재 main에서 직접 다시 돌린다.** 가상 스레드 멀티파트 실패(#75)가 main에서는 4조합 모두 정상이었다.
- **측정은 조용한 기계에서만.** 큐가 CPU 15% 이하를 기다리지만 600초 뒤엔 그냥 돈다. 병렬 job 8개일 때 .NET raw가 4분의 1로
  떨어진 오염 사례(FB-061).
- **`pkill -f`·`pgrep -f` 금지.** 오늘 두 번 자기 셸·빌드를 죽였다. `.pid`로만.
- **실행 중인 bash 스크립트를 편집하지 않는다.** 새 파일로 쓰고 교체.
- **버리는 실험은 Issue·PR·보드 없이.** 결과가 좋으면 그때 PR, Issue는 PR 시점에(작업 방식 §2.0).
- **worktree에 `.artifacts/wsl` 링크가 없으면 계약 테스트가 실패한다.** `work.sh start --no-packages` 뒤 수동 링크 필요
  (`ci/worksh-package-link` 브랜치에 수정 초안, 미완).

## 7. 정리 대상 worktree (다음 세션이 판단)

`bash scripts/dev/worktree-sweep.sh`로 표를 본다. 실행 중 job의 worktree는 자동 보호된다.
- 머지돼서 지워도 되는 것: `zlink-21-spec-*`, `zlink-33-*`, `zlink-47-*`, `zlink-48-*`, `zlink-49-*`, `zlink-50-*`, `zlink-75-*`,
  `zlink-6-mesh-ingress`, `zlink-fwperf-cpp`(모두 branch가 main에 포함).
- 실험(머지 안 함): `zlink-56-framework-java`(사다리·교차·L0 실험 커밋), `zlink-fwclient-profile`. 보고서는 `.artifacts/codex/java-ablation*/`, `java-client-profile/`에 있으니 worktree는 지워도 된다.
- 미착수 빈 worktree: `zlink-37-work`, `zlink-46-work`.
- 남길 것: `zlink-5-dispatch-batch`(.NET 1차 WIP 62d1cff790, 2차는 #71로 머지됨 — 확인 뒤 정리), `zlink-jdk25-unsafe`(#14), `zlink-worksh-pkglink`(위 §6 마지막 항목 초안).

## 8. 자주 쓰는 명령

```bash
bash scripts/dev/session-setup.sh --check            # 포트 예약·tmpfs·측정 큐·패키지 상태
bash scripts/dev/job.sh status                       # codex job 상태 (--all 전체)
bash scripts/dev/job.sh start <이름> --worktree <경로> --brief <파일> --model gpt-6-astra --effort high
bash scripts/dev/work.sh start --issue N --no-packages   # 그 뒤 ln -s ~/project/zlink/.artifacts/wsl <worktree>/.artifacts/wsl
bash scripts/dev/work.sh pr --body <파일> --refs|--closes   # 작업 worktree 안에서
bash scripts/dev/work.sh done --verified <sha>            # 작업 worktree 안에서, 끝나면 cd ~/project/zlink
bash scripts/perf/perf-ticket.sh submit -p 1 -o supervisor -d '<설명>' -- <측정 명령>   # 모든 측정
JAVA_HOME=/home/hep7/.cache/zlink/jdk/temurin-25   # Java 빌드·테스트·벤치 전부 (기본값은 JDK 22라 실패, #46)
```

codex 모델 id: `gpt-6-astra`(최상위), `gpt-5.6-sol`, `gpt-5.6-terra`, `gpt-5.6-luna`. 잘못된 id는 job이 즉시 죽고 로그에만 남는다 — job.sh가 시작 전에 걸러 준다.
