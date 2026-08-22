# Server-side 성능 회복 작업 계획

> 이 문서는 성능 회복 작업의 조사 절차와 남은 실험을 정하는 작업 계획이다. 현재 공개
> 계약을 정의하지 않는다.

이 작업은 [Core byte HWM과 흐름 제어 작업 계획](./core-byte-hwm-flow-control-plan.ko.md)에서
분리됐다. 그 문서의 §8.2가 성능 gate의 측정 규칙과 확장 순서를 소유하며, 이 문서는 그
규칙을 그대로 참조하고 남은 조사 절차만 서술한다. 기능 구현(byte HWM, completion-lane
흐름 상태, 언어 binding parity)은 그 문서의 §12에서 이미 완료됐고 이 문서의 범위가
아니다.

## 1. 새 작업자의 시작 절차

이 문서와 아래 worklog만 현재 작업의 입력으로 사용한다. 이전 대화의 중간 결론을 완료
근거로 사용하지 않는다.

1. Repository root(`/home/hep7hep7/project/zlink`)에서 `git branch --show-current`와
   `git status --short`를 확인한다. 현재 branch는
   `codex/bindings-0.11.1-performance`다. 이 branch에서는 사용자 승인 없이 commit,
   stash, reset, restore와 origin push를 사용할 수 있다. Branch 전환은 여전히 사용자
   승인이 필요하다.
2. `gmon.out`과 이미 존재하는 dirty worktree 변경은 사용자 작업이다. 이 문서 범위와
   겹치지 않으면 수정하지 않는다.
3. 아래 필수 문서를 읽는다.

| 구분 | 읽을 문서 | 확인할 내용 |
|---|---|---|
| 기능 완료 근거와 §8.2 측정 규칙 | `doc/plan/autohwm/core-byte-hwm-flow-control-plan.ko.md` | §8.2.0~§8.2.3 노이즈 확인·판정 규칙·확장 순서, §12 진행 checklist |
| Client 쪽 무죄 판정과 방법론 교정 | `doc/plan/autohwm/worklog/stage1-removal-baseline.md` §8.11~§8.19 | 측정 방법 교정, poll 폭풍 발견과 상한 측정, mixed-runtime split |
| Server 쪽 조사 시작점과 미완 ledger | `/home/hep7hep7/project/zlink-perf-track`의
  `doc/plan/autohwm/worklog/perf-track-server-side.md` | Cell harness, profiling 도구, 상한 측정 5종, server-only ledger 미완 상태(§9.5) |
| Plan 작성 규칙 | `doc/AGENTS.md` | Plan과 정식 spec의 역할 분리 |
| 한국어 문서 원칙 | `doc/principal/documentation/documentation-principles.ko.md` | 현재 상태, 문장 구조 |
| Perf 디렉터리 규칙 | `bindings/c/perf/AGENTS.md` | `core/build` runtime과 provenance 확인 |
| 공통 perf 정책 | `doc/perf/PERF_POLICY.md`, `PERF_MULTI_TEST_POLICY.md` | Paired 측정, workload·metric 계약 |

4. 기존 perf-track worktree(`/home/hep7hep7/project/zlink-perf-track`, branch
   `perf/server-side-track`)를 재사용한다. 이 worktree는 main branch에서 갈라진
   시점에 머물러 있으므로, 작업을 시작하기 전에 main worktree의 최신
   `codex/bindings-0.11.1-performance` HEAD로 rebase해 재측정 기준을 현재로
   맞춘다. Worktree의 `scratchpad/`에 이미 committed된 도구
   (`zprof.c`, `zresolve.py`, `cell.sh`, `abba.sh`, `build_ledger.sh`,
   `ledger_measure.sh`)를 재사용하고, 새 diagnostic 도구가 필요하면 같은 방식으로
   `scratchpad/`에 committed한다.
5. 이 계획 실행 중에는 host를 다른 작업과 공유하지 않는다.
   `perf-track-server-side.md` §9.3은 다른 track의 build가 동시에 돌아가는 동안
   측정한 첫 sweep 전체가 host 분산으로 무효화됐다는 사실을 기록하고 있다. 측정
   window 동안 `uptime` load를 관찰해 다른 프로세스가 CPU를 점유하지 않는지 확인한다.

## 2. 목표

Multi `ROUTER_ROUTER_SENDSEND / tcp / 256 B`에서 local runtime이 `0.10.1` release
대비 [core 계획 §8.2.3](./core-byte-hwm-flow-control-plan.ko.md#823-판정-규칙)의 판정
규칙(throughput·bandwidth·mean·p95·p99 median 전부, 완화 없이)을 통과하는 것이 이번
작업의 gate다. 이 case를 통과한 뒤에는 [core 계획
§8.2.2](./core-byte-hwm-flow-control-plan.ko.md#822-확장-순서)가 정한 확장 순서(Multi
`DEALER_ROUTER_SENDSEND` → `DEALER_ROUTER_REQREP` → `ROUTER_ROUTER_REQREP` →
`PUBSUB` → Single `ROUTER_ROUTER` → Single `DEALER_ROUTER`)를 그대로 따른다.

## 3. 현재 상태

### 3.1 도달점과 회귀 회복 이력

- Stage 1 시작 시점 처리량은 `0.10.1` 대비 69.9%였다(`stage1-removal-baseline.md`
  §6-7, median local 129.677 Kops/s vs 0.10.1 187.935 Kops/s). 여러 단계의 profile-guided
  수정을 거쳐 현재 도달점은 89.5%다(commit `2578672247` 시점의 `final4` gate,
  `stage1-removal-baseline.md` §8.14: throughput·bandwidth median 158.482/81.143 vs
  177.004/90.626, latency mean·p95·p99도 115~117%로 전부 미달).
- Byte-HWM 작업이 도입한 회귀 commit `3ef4d09a37`이 만든 손실은 완전히 회복됐다.
  회복은 poll 경로 재작업 5개 commit(`d58a179033` restore POSIX descriptor pollset →
  `b818c03f71` → `5ad65a4627` → `2855521baa` → `ce568e103c` → `078d4f6ac7` reserve poller
  item vector, `git log --oneline d58a179033..078d4f6ac7`로 5개 확인)과 codex review
  지적 4건을 반영한 수정 commit `2655d6af30`으로 이뤄졌다(`stage1-removal-baseline.md`
  §8.12-8.13).
- Client 쪽은 상한 측정으로 무죄 판정을 받았다. Client의 poll 경로 CPU를 진단 patch로
  30%에 가깝게 걷어내는 두 상한 실험 모두 noise floor(±3.5%) 안에서만 움직였다
  (`stage1-removal-baseline.md` §8.18: poller 재사용 +1.4%, poller 재사용 + admission
  제거 +0.4%).
- Mixed-runtime 4-cell 실험(A=local/local, B=0.10.1/0.10.1, C=local server/0.10.1
  client, D=0.10.1 server/local client, `stage1-removal-baseline.md` §8.19)이 server가
  파이프라인을 약 90%로 제한하고 client는 약 93%로 제한한다는 것을 보였다(C 90.5%,
  D 92.6%, A ≈ min(C, D) ≈ A의 89.9%와 일치). 이 실험은 runner에 process별 Core runtime
  override를 추가한 commit `bde05d5b18`으로 가능해졌다.

### 3.2 Server 쪽 첫 라운드에서 배제한 원인

`perf-track-server-side.md`(perf-track worktree)의 server-only 조사는 다음을 확인했다.

- Cell-C 격리(local server + 0.10.1 client)에서 server-owned 손실은 약 9%다(ABBA 3
  block, mean 91.1%, §3).
- Server의 `recv`(2.0010/msg), `send`(1.0006/msg), `epoll_wait`(약 0.33/msg) syscall
  횟수는 두 runtime에서 소수점 4자리까지 동일하고, 모두 같은 asio call site
  (`reactive_socket_{recv,send}_op_base::do_perform`, `epoll_reactor::run`)에서
  발생한다(§5.1, §5.3). Engine·transport의 read/write batching과 decoder framing은
  두 runtime에서 같게 동작하므로 무죄다.
- Server에서 유일하게 큰 구조적 차이는 `poll(2)` 폭풍이다. Local server는
  message당 4.7330회, 0.10.1 server는 0.0312회를 호출한다(약 152배, §5.1). 원인은
  ROUTER socket의 mailbox notification eventfd가 async executor 소유이고
  `process_commands()`가 `socket_base_lifecycle.cpp:103`에서
  `async_mailbox_owns_commands () && !async_executor`이면 조기 반환해
  `mailbox_t::recv()`(primary signaler를 읽는 유일한 경로)에 도달하지 못하기
  때문이다. 그 결과 primary signaler eventfd가 영구적으로 readable 상태로 남고
  `socket_poller_t::wait`이 매번 `poll()`을 호출해도 아무 descriptor도 소비하지 못한
  채 busy-spin에 가깝게 도는 구조가 된다(§5.4).
- 이 poll 폭풍을 실제로 제거하는 상한 실험(v3: 소비 안 된 descriptor를 fruitless
  check 뒤 소비, v4: drain 후 재확인하고 block하는 arm-then-block 구조)은 모두
  12~18% **악화**했다(§6.2, 5개 block 전부 같은 방향). Spin을 남겨 두는 변형(v1, v2)과
  poller의 lock-free `has_in()` probe(v5)는 noise floor 안에서 중립이었다(§6.1, §6.3).
  즉 poll 폭풍은 발견됐지만 병목이 아니다.
- v4는 poll 구조와 message-per-poll batching을 0.10.1과 거의 동일하게 맞췄는데도
  (36 대 32 messages per public poll) 여전히 0.10.1보다 28% 느렸다(§6.2). 이는 남은
  cost가 poll 구조 자체가 아니라 **io-thread executor에서 application thread로의
  수신·relay handoff**에 있다는 가장 강한 제약이다(§8).
- Server CPU는 +6.6%만 늘었는데 처리량은 -9%다(§5.2, §8). CPU 귀속과 처리량 상한이
  다르다는 같은 패턴이 client 쪽 §8.13.3, §8.18.3에서도 반복 관측됐다.

### 3.3 잔여 용의 구역과 미완 조사

- 잔여 용의 구역은 **executor→application 수신 handoff**다(위 v4 결과 근거).
- `stage1-removal-baseline.md` §8.17.2의 완전한 loss ledger(`0.10.1` →
  `425b9c2a82` → `81e98c1c0e` → `408f1d4a9d` → `e13e561ba9` → `7bee681ec5` →
  `ad5e936bed` → `cc122eaa40` → `f159a51a99` → `2728d70d44` → HEAD)는 **전부 cell-A
  측정**(server와 client가 같은 runtime, 결과가 `min(server, client)`)이다.
  Client가 약 93%에서 상한을 걸므로, 이 ledger로는 최대 약 7%까지의 server 단독
  회귀가 구조적으로 보이지 않는다(`perf-track-server-side.md` §8, §9).
- 이 구멍을 메우려는 server-only ledger(server를 매 commit으로, client를 항상
  `0.10.1`로 고정)가 `perf-track-server-side.md` §9에서 시작됐다. Build
  provenance(§9.2)까지는 확인했고 `.text` md5로 두 쌍의 byte-identical 빌드
  (`c4 == c5`, `c6 == cc`)를 자유 noise control로 확보했다. 그러나 첫 sweep은
  다른 track의 동시 build 때문에 host 분산으로 전부 무효화됐고(§9.3, 같은 binary가
  37%~491%로 요동), harness를 quiet-gate와 collapse-filter로 보강한 뒤(§9.4)의 본
  sweep은 **시작되지 않았다**(§9.5 "sweep in progress"로 중단). 이 ledger를
  완주하는 것이 다음 작업의 최우선 항목이다(§5(a)).

## 4. 방법론

다음 방법론은 이 host에서 검증됐다. 새 작업자는 그대로 따른다.

- **ABBA block이 필수다.** 이 host는 세션 안에서 벤치를 돌릴수록 단조 감소한다
  (`stage1-removal-baseline.md` §8.17.1: 한 세션 안에서 190 K → 133 K). 순차
  median 비교는 뒤에 실행되는 쪽에 불리하게 편향된다. 모든 비교는 A, B, B, A
  순서로 실행하고 각 쪽의 평균을 비교한다.
- **Noise floor는 ±3.5%다.** 같은 binary를 ABBA 4 block으로 대조 측정해 얻은
  값이다(`stage1-removal-baseline.md` §8.17.1). 이보다 작은 효과는 판정하지
  않는다.
- **동시 부하 아래서는 noise floor 자체가 무너진다.** `perf-track-server-side.md`
  §9.3은 다른 track이 동시에 build하는 동안 같은 binary를 측정해 37%~491%까지
  요동친 사례를 기록한다. Load가 높을 때는 ±3.5% 규칙을 적용하지 않고 측정을
  다시 한다. §9.4의 quiet-gate(연속 3회 1분 load average가 `QUIET` 미만이어야
  시작)와 collapse-filter(한 block의 4개 표본이 1.25배 넘게 벌어지면 버리고
  재시도)를 이어서 사용한다.
- **변형 binary는 `.text` md5 대조가 필수다.** `stage1-removal-baseline.md`
  §8.16.3/§8.17.1은 variant build script가 이전 revert를 정리하지 않아 서로 다른
  이름의 세 variant가 실제로는 동일 binary였던 사고를 기록한다. 모든 variant
  build 뒤 `.text` section의 md5가 서로 다름을 확인하고, 원본으로 되돌린 뒤에는
  원래 md5로 복귀했는지도 확인한다.
- **CPU 귀속은 처리량 상한이 아니다.** `stage1-removal-baseline.md` §8.18.3과
  `perf-track-server-side.md` §7-8이 반복 확인했듯, sampling profile이 어떤
  함수에 CPU 시간이 몰려 있다고 보여도 그 함수를 없앴을 때 처리량이 그만큼
  오른다는 보장이 없다. 모든 후보는 구현 전에 **상한(upper bound) 측정**을
  먼저 한다. 후보 code path를 통째로 제거·우회하는 진단용 throwaway build를
  만들어 그 상한이 noise floor를 넘는지 먼저 확인하고, 넘지 않으면 실제 구현에
  들어가지 않는다. 상한 측정 뒤 진단 patch는 되돌리고 `.text` md5로 원상태
  복귀를 확인한다.
- **측정은 load 1.5 미만에서만 한다.** Report에 측정 시점의 `uptime` load를
  기록한다.
- **이 host에는 `perf`, `gdb`, `strace`가 없다.** `kernel/yama/ptrace_scope=1`이
  tracer의 자식이 아닌 process에 대한 attach를 막아 `strace -c`도 실행할 수
  없다. `gprof`는 main thread만 측정하므로 멀티스레드 workload에 무용하다
  (`perf-track-server-side.md` §4). 대신 `LD_PRELOAD` 기반 계측을 사용한다.
  - **SIGPROF 전 thread sampler**: `ITIMER_PROF` 1 ms 간격, `SA_SIGINFO`
    handler로 신호를 받은 thread의 `REG_RIP`를 기록해 io thread도 샘플링한다.
  - **libc interposition을 통한 syscall count**와 caller histogram
    (`__builtin_return_address(0)`), 그리고 public `zlink_poll` 자체의
    interposition으로 public poll 호출 대비 실제 `poll(2)` syscall 수를 비교한다.
  - 이 도구(`zprof.c`, symbolizer `zresolve.py`, cell harness `cell.sh`, ABBA
    driver `abba.sh`)는 perf-track worktree의 branch `perf/server-side-track`,
    commit `69a24d0821`에 `scratchpad/`로 committed되어 있다
    (`perf-track-server-side.md` §4, §10). `PERF_MULTI_COMPONENT`와
    `ZPROF_OUT` 환경변수가 모두 설정된 경우에만 동작하므로 benchmark 두
    process 외에는 계측하지 않는다. Instrumentation 자체가 처리량을 왜곡하므로
    profiled run을 처리량 측정으로 사용하지 않는다.
- **Test와 perf runtime의 build 디렉터리를 구분한다.** `core/build`는
  `ZLINK_BUILD_TESTS=OFF`로 구성되어 test target이 없다. Test는 반드시
  `ZLINK_BUILD_TESTS=ON`인 `core/build-tests`에서 빌드·실행한다. 판정 전에는
  항상 두 디렉터리 모두 재빌드한다. `stage1-removal-baseline.md` §8.11은 이
  구분을 지키지 않아 여러 stage의 test 결과가 최대 2026-08-22 01:05~01:08에
  만들어진 낡은 binary를 실행한 것으로 드러난 사고를 기록한다.
- **Perf runtime provenance는 매번 report META로 확인한다.** Report가 출력한
  `libzlink.so` 경로가 `core/build` 밖의 local 경로거나 최신 source보다
  오래됐으면 측정을 중단한다.

## 5. 다음 실험

우선순위 순으로 진행한다. 각 실험 뒤 [core 계획
§7.2](./core-byte-hwm-flow-control-plan.ko.md#72-구현-중-기록할-증거)와 같은 형식으로
변경한 source, 실행한 focused test, provenance, report 경로를 기록한다.

### 5.1 (a) Cell-C 격리 loss ledger 완주 — 최우선

`stage1-removal-baseline.md` §8.17.2의 손실 원장은 전부 cell-A(server와 client가
같은 runtime) 측정이라 client가 약 93%에서 거는 상한 때문에 server 단독 회귀
최대 약 7%가 구조적으로 보이지 않았다. `perf-track-server-side.md` §9가 이미 시작한
server-only ledger(server를 매 commit으로 build하고 client는 항상 `0.10.1`로 고정)를
완주해 이 손실을 재측정한다.

- Ledger가 지나가는 commit 경로는 §8.17.2와 동일하게 `core/v0.10.1` → `425b9c2a82`
  → `81e98c1c0e` → `408f1d4a9d` → `e13e561ba9` → `7bee681ec5` → `ad5e936bed` →
  `cc122eaa40` → `f159a51a99` → `2728d70d44` → HEAD이며, 한 단계도 생략하지 않는다.
- Build는 `/tmp/wt_ledger`의 별도 git worktree에서 각 commit을 순서대로 checkout해
  단일 incremental Ninja 디렉터리로 빌드하고(§9.1), toolchain 설정을 `core/build`와
  동일하게 맞춘다(Release, `ENABLE_LTO=ON`, `ENABLE_CLANG=ON`, `WITH_TLS=ON`,
  `WITH_LIBBSD=ON`, `ZLINK_CXX_STANDARD=17`, `ZLINK_CV_IMPL=stl11`).
- 각 build의 `libzlink.so`를 `/tmp/led/<label>/`로 복사하고 `.text` md5를 즉시
  기록한다. §9.2가 이미 `c4 == c5`, `c6 == cc`(Linux에서 byte-identical) 두 쌍을
  발견했으므로 이 둘을 무료 noise control로 활용한다. `VERSION`이 `2728d70d44`에서
  0.10.1→0.11.0으로 바뀌면서 공유 build 디렉터리에 이전 `libzlink.so.0.10.1`이
  남아 잘못 복사되는 build script 결함이 있었으나 `par`·`head` 재빌드로 수정됐다
  (§9.2). 새 작업자도 매 build 전 `lib/libzlink.so*`를 지우고 `libzlink.so.0`
  symlink로 artifact를 확인한다.
- 측정은 `scratchpad/cell.sh L<label>`로 하고(ledger runtime을 server, `0.10.1`
  release runtime을 client로 고정), §9.4의 hardened harness(quiet-gate, load
  1분 평균이 연속 3회 `QUIET` 미만일 때만 시작, collapse-filter로 spread 1.25배
  넘는 block 폐기)를 그대로 사용한다. 매 step을 같은 cell B 참조에 ABBA로
  block한다.
- `core/v0.10.1`을 자기 toolchain으로 소스에서 빌드한 `c0`는 released `0.10.1`
  binary(cell B) 대비 약 102.8%로 측정되는 계통 편차가 있다(§9.4). 개별 commit의
  비율은 released `0.10.1`(B)가 아니라 `c0`를 기준으로 읽는다.
- §9.5는 이 sweep을 시작하지 못하고 중단한 상태("sweep in progress")다. 이
  실험의 산출물은 각 commit 구간의 server-only 손실·이익 비율 표이며, 이것으로
  server 단독 회귀를 특정 commit에 귀속시킨다.

### 5.2 (b) 주인 commit 특정 후 diff 후보 + 상한 측정

(a)의 결과로 server-only 손실을 특정 commit 구간에 좁힌 뒤, 그 구간의 diff에서
Linux에 실효가 있는 변경만 후보로 추린다(Windows 전용 `#ifdef` 블록은 §8.16.2·§9.2가
이미 여러 차례 배제 근거로 썼던 것과 같은 방식으로 제외한다). 각 후보는 §4의 상한
측정 규칙에 따라 구현 전에 진단용 throwaway build로 상한부터 재고, noise floor를
넘는 후보만 실제 구현으로 진행한다.

### 5.3 (c) Executor→application 수신 handoff 구조 분석

`perf-track-server-side.md` §6.2의 v4 결과(poll 구조와 message-per-poll batching을
0.10.1과 동일하게 맞춰도 28% 느림)가 가리키는 실제 용의 구역이다. Server가 CPU를
`0.10.1`보다 덜 쓰면서도 더 느린 패턴이 §5.2(§5.2 self-time 표), §8.13.3,
§8.18.3에서 반복 관측됐으므로, 원인은 CPU 비용이 아니라 io-thread async executor가
수신한 메시지를 application thread에 넘기는 경로의 blocking·batching 구조에 있다고
의심한다. 이 방향은 (a)·(b)로 commit 단위 귀속이 끝난 뒤, 또는 (a)·(b)가 결정적
음성으로 끝났을 때 시작한다. 조사 시작점은 `socket_base_lifecycle.cpp`의
`async_mailbox_owns_commands()`/`process_commands()` 경로와 async executor가
`xdispatch_io()` 둘레에서 잡는 receive mutex(`perf-track-server-side.md` §6.3이
언급하는 `receive_runtime().sync`, `socket_base_api.cpp:576`)다.

## 6. 주의사항

- **현재 HEAD가 새 기준이다.** Flow-state 기능(completion-lane PAUSE/RUNNING,
  Core C API, event, metric, 7개 언어 binding parity)이 이미 구현되어 있다. 측정
  시 flow state는 항상 `RUNNING`이어야 하며, 이 상태에서의 처리량이 이 계획의
  기준선이다.
- **Host를 다른 작업과 공유하지 않는다.** `perf-track-server-side.md` §9.3은
  동시 부하가 측정을 완전히 무효화한 사고를 기록하고 있다. 측정 window 동안은
  이 host에서 다른 build나 벤치를 돌리지 않는다.
- **기존 perf-track worktree와 도구를 재사용하되 rebase한다.** `perf-track-server-side.md`가
  기록된 시점의 branch `perf/server-side-track`은 main worktree의 `codex/bindings-0.11.1-performance`
  HEAD보다 오래됐다. 작업을 시작하기 전에 최신 HEAD로 rebase하고, §9.5의 ledger
  sweep은 rebase 이후 HEAD 기준으로 다시 잰다.
- **§8.17.2, §8.17.4의 cell-A 측정 결과를 server-only 결론으로 재사용하지
  않는다.** 두 결과 모두 client 상한에 가려 server 단독 효과를 볼 수 없는
  측정이다(§3.3).

## 7. 판정 규칙과 확장 순서

판정 규칙과 확장 순서는 [core 계획
§8.2.0~§8.2.3](./core-byte-hwm-flow-control-plan.ko.md#82-짧은-성능-비교)을 그대로
따르고 이 문서에서 다시 서술하지 않는다. 다음만 이 문서에서 보강한다.

- §8.2.0의 noise floor 확인은 이 작업을 다시 시작할 때도 매번 먼저 실행한다. §4가
  기록한 대로 noise floor는 host 부하 상태에 따라 ±3.5%보다 훨씬 커질 수 있으므로,
  이전 세션의 noise floor 값을 가정하지 않고 그 세션에서 다시 잰다.
  다음 대상 case는 §8.2.1의 첫 회귀 case(Multi `ROUTER_ROUTER_SENDSEND / tcp /
  256 B`)이며, 이 case를 통과한 뒤에만 §8.2.2의 확장 순서로 넘어간다.

## 8. 중단 조건

다음 조건에서는 범위를 임의로 넓히지 않고 사용자에게 근거와 선택지를 보고한다.
보고 형식은 [core 계획 §10](./core-byte-hwm-flow-control-plan.ko.md#10-중단-조건과-결과-인계)의
`Result:` 블록을 따른다.

- §5(a)·(b)·(c)의 상한 측정으로 모든 후보가 noise floor 안에 있고, 구조 재설계
  외에 다른 대안이 없다고 판정된 경우. 이 경우 보고에는 다음을 포함한다.
  - 시도한 각 후보의 상한 측정 결과(ABBA block별 비율)와 `.text` md5로 확인한
    variant 목록
  - Server-only ledger(§5.1)가 특정한 손실 구간, 또는 시도했으나 결정적 결론을
    내지 못한 이유
  - 구조 재설계가 필요하다면 그 구조가 무엇인지(예: executor→application handoff의
    batching 방식 변경)와 예상 영향 범위(다른 socket type·topology에 대한 파급)
- Local과 `0.10.1`의 workload, HWM, OS buffer 또는 runtime provenance를 맞출 수
  없는 경우
- 같은 원인의 crash·OOM·timeout이 세 번 재현되고 더 진행할 수 없는 경우
- Core 코드가 아닌 Framework 변경이나 새 public API가 성능 회복에 필요해진 경우

## 9. 완료 조건

- Multi `ROUTER_ROUTER_SENDSEND / tcp / 256 B`에서 throughput·bandwidth·mean·p95·p99
  median 전부가 [core 계획 §8.2.3](./core-byte-hwm-flow-control-plan.ko.md#823-판정-규칙)의
  규칙으로 `0.10.1`보다 나쁘지 않다.
- [core 계획 §8.2.2](./core-byte-hwm-flow-control-plan.ko.md#822-확장-순서)의 확장
  순서를 case 한 개씩 같은 방식으로 통과한다.
- 전체 perf matrix를 한 번에 실행하지 않는다.
- 판정에 사용한 report 경로와 median 계산이 노이즈 완화 없이 §8.2.3 규칙 그대로
  기록되어 있다.
- Framework source, public API, spec과 test에는 변경이 없다.
- 이 작업으로 만든 코드 변경이 [core 계획 §12.2, §12.5](./core-byte-hwm-flow-control-plan.ko.md#125-최종-core-gate)의
  해당 미완료 행을 완료로 갱신할 근거가 된다.

## 10. 진행 checklist

완료한 항목만 `[x]`로 바꾸고 Evidence 열에 test output, perf report 또는 변경 파일
경로를 기록한다. 실행하지 않은 항목을 추정으로 완료 처리하지 않는다. 진행할 수
없으면 `[ ]`를 유지하고 Evidence에 `BLOCKED:`와 첫 원인을 적는다.

| Done | 확인 항목 | Evidence |
|---|---|---|
| [ ] | Branch, dirty worktree와 host 단독 사용 조건을 확인했다. | |
| [ ] | perf-track worktree를 최신 `codex/bindings-0.11.1-performance` HEAD로 rebase했다. | |
| [ ] | Rebase 이후 코드에서 §8.2.0 noise floor를 다시 쟀다. | |
| [ ] | §5.1 server-only ledger를 `0.10.1`부터 HEAD까지 한 단계도 생략 없이 완주했다. | |
| [ ] | Ledger가 특정한 손실 구간에서 Linux 실효 diff 후보를 추렸다. | |
| [ ] | 각 후보의 상한을 구현 전에 측정했다. | |
| [ ] | Noise floor를 넘는 후보만 실제로 구현했다. | |
| [ ] | 구현한 변경 뒤 focused test(`core/build-tests`, 재빌드)가 통과했다. | |
| [ ] | Multi `ROUTER_ROUTER_SENDSEND / tcp / 256 B`가 §8.2.3 규칙으로 통과했다. | |
| [ ] | §8.2.2 확장 순서의 나머지 case를 한 개씩 통과했다. | |
| [ ] | 모든 후보가 noise 안이고 구조 재설계가 필요하다고 판정되면 §8 형식으로 보고했다. | |
