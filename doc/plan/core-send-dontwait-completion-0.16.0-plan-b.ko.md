# 0.16.0 캠페인 — 머신 B 실행 계획 (성능 판정·gate 도구·posddd 리팩토링)

> 작성일: 2026-09-03
> 상위 계획: [`core-send-dontwait-completion-0.16.0-plan.ko.md`](core-send-dontwait-completion-0.16.0-plan.ko.md) §0.3a
> 작업 기록: [`c016-worklog/`](c016-worklog/README.ko.md) (판정 D-021~D-050, 브리프, 드라이버)
> 역할: 감독관(Claude Fable) = 리뷰·판정·커밋, 구현 = codex sol(고난도 sol ultra), 문서 작성 = sonnet

이 문서는 머신 B의 새 세션이 상위 계획을 다 읽지 않아도 자기 몫을 끝낼 수 있게 쓴 실행 계획이다.
B는 세 가지를 맡는다: **(1) Core 2차 수정의 성능 판정, (2) hotpath gate 도구, (3) posddd 리팩토링 + wake
불변식 테스트**. 모두 branch에서 하고 PR로 `main`에 합친다. 머신 A는 그동안 Phase 7 smoke와 Phase 9 준비,
로컬 빌드 기반 Phase 10~12를 진행하며, release 태그는 B의 리팩토링 merge와 sweep2 PASS 뒤에만 찍는다(D-050).

## 0. 시작 조건

- `origin/main`에 Core 2차 수정 커밋(`core: ... hot path phase 2` 제목, 머신 A가 push)이 있어야 한다.
  `git log --oneline -5 origin/main`으로 확인한다. 없으면 §1 환경 구성만 해 두고 기다린다.
- B에서 성능을 재는 동안 B에서 다른 빌드·테스트를 돌리지 않는다(짝비교는 상대 비교라 머신은 무관하지만
  측정 중 조용해야 한다).

## 1. 환경 구성 (한 번)

```bash
git clone git@github.com:zlink-systems/zlink.git ~/project/zlink   # 또는 git pull
cd ~/project/zlink
export ZLINK_WORK=~/project/zlink-work
mkdir -p $ZLINK_WORK && cp -r doc/plan/c016-worklog $ZLINK_WORK/c016

# Core (테스트 포함)
cmake -S core -B core/build -DZLINK_BUILD_TESTS=ON -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build core/build -j

# 성능 baseline worktree: 직전 release core/v0.15.1를 같은 머신에서 자체 빌드 (--core-version 금지)
git worktree add --detach ~/project/zlink-perf-core-0.15.1 core/v0.15.1
( cd ~/project/zlink-perf-core-0.15.1 \
  && cmake -S core -B core/build -DZLINK_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release \
  && cmake --build core/build -j )
# 측정 방식 동일화: main의 벤치 3파일을 baseline worktree에 복사 (커밋 f5a62c4b3f)
for f in bindings/c/perf/single/common/perf_single_one_way.hpp \
         bindings/c/perf/single/src/perf_dealer_router.cpp \
         bindings/c/perf/single/src/perf_router_router.cpp; do
  cp $f ~/project/zlink-perf-core-0.15.1/$f; done
which valgrind || sudo apt-get install -y valgrind      # hotpath gate 도구용
```

codex job 기동 형식(모든 job 공통; 상태 파일은 `$ZLINK_WORK/c016/`):

```bash
systemd-run --user --scope --unit=<이름> -p MemoryMax=24G -p OOMPolicy=continue -q bash -c \
  'setsid nohup bash -c "cd ~/project/zlink; export ZLINK_WORK=~/project/zlink-work; \
   codex exec -m gpt-5.6-sol -c model_reasoning_effort=\"<high|ultra>\" -s danger-full-access --skip-git-repo-check \
   \"\$(cat $ZLINK_WORK/c016/briefs/<브리프>.prompt)\" < /dev/null > $ZLINK_WORK/c016/<이름>.log 2>&1; \
   echo EXIT:\$? >> $ZLINK_WORK/c016/<이름>.log" > /dev/null 2>&1 & disown'
```

감시는 `systemctl --user is-active <이름>.scope`(pgrep 자기매칭 금지)와 progress 파일 append로 한다.

## 2. 작업 1 — Core 2차 수정의 성능 판정 (branch `perf/phase2-judge`)

합격 기준(D-040, 스펙 [Core hot path §5.2](../../core/doc/spec/core/systems/10-hot-path.ko.md)):
cell(pattern·transport·size·metric) throughput·bandwidth ≥ 0.95, latency ≤ 1.05 **그리고** (pattern, transport)별
size `64,256,1024,65536` 기하평균 throughput·bandwidth ≥ 1.0, latency ≤ 1.0. 둘 다 PASS. gate 완화·이월 금지.

```bash
git checkout -b perf/phase2-judge origin/main
export ZLINK_WORK=~/project/zlink-work
bash $ZLINK_WORK/c016/tools/sweep2.sh --only single     # 42 cell × 4 size, ~60~90분
bash $ZLINK_WORK/c016/tools/sweep2.sh --only multi      # 28 cell × 4 size, ~40~60분
# 결과: $ZLINK_WORK/c016/sweep2-results.md (cell + agg 열), report는 runner-results/{baseline,candidate}/
```

- 전 cell PASS → 결과 표를 `doc/plan/c016-worklog/sweep2-results-phase2.md`로 복사해 커밋(판정 근거), A에 알린다.
- FAIL cell이 있으면 같은 조건으로 1회 재측정해 환경 변동과 회귀를 구분한다. 재현되면 sol ultra job으로 수정한다:
  브리프는 `briefs/hotpath-phase2-resume.prompt`를 복사해 "남은 작업" 절을 FAIL cell 목록·report 경로로 바꾼다.
  수정 후 감독관 기능 gate(§5) → sweep2 재판정 → 커밋.
- 이 branch의 커밋은 PR로 `main`에 합친다(리팩토링 전에 합쳐야 리팩토링 비회귀 비교의 기준이 된다).

## 3. 작업 2 — hotpath gate 도구 (branch `perf/hotpath-gate`, sol high)

브리프 `briefs/hotpath-gate.prompt`. 산출물: `core/tests/perf/hotpath_bench.cpp`, `hotpath_gate.py`,
`hotpath_reference.json`, CMake 등록(ctest label `hotpath`, valgrind 있을 때만). 기준값은 **작업 1을 통과한 트리**로
생성한다(리팩토링 전). 감독관 검수: 3회 반복 결정성(±0.5%), 인위 회귀 FAIL 확인, `ctest -R hotpath_gate` PASS.
커밋 메시지 예: `core/tests: add the callgrind hot-path instruction gate`. PR로 합친다.

## 4. 작업 3 — posddd 리팩토링 + wake 불변식 테스트 (branch `refactor/posddd`, sol ultra + sol)

- 리팩토링: 브리프 `briefs/posddd-refactor.prompt`. **모듈 경계로 worktree를 나눠 병렬 투입**(D-045 교훈):
  예) worktree 1 = `core/src/api/socket/*` + `part_helper_*`, worktree 2 = `core/src/runtime/sockets/common/*` +
  `dealer/router/internal`, worktree 3 = `core/src/runtime/core/pipe.*`. 각 job은 자기 worktree만 수정하고, 감독관이
  순서대로 branch에 합친다(충돌 시 감독관이 해소).
- wake 불변식 테스트: 브리프 `briefs/wake-invariant-tests.prompt`(sol high), 별도 worktree, `core/tests/**`만 수정.
- 규칙: 동작 보존(동작 변경이 필요하면 BLOCKERS로 보고), 공개 API·ABI 불변, 스펙
  [Core hot path §3](../../core/doc/spec/core/systems/10-hot-path.ko.md) 위반 잔존 시 §4 형태로 정리, 성능 이득이 없어도
  구조 개선이면 채택(D-044).
- Gate: §5 기능 gate + `ctest -R hotpath_gate`(기준값 ±5%) + `sweep2.sh` 비회귀(작업 1 결과와 비교해 나빠진 cell 없음).
- 커밋은 리팩토링 항목별로 분리(불필요 코드 제거 / 책임 분리 / 명명)해 리뷰 가능하게 하고, PR로 합친다.

## 5. 감독관 기능 gate (모든 커밋 전 공통)

```bash
ulimit -v 16777216
cmake --build core/build -j
ctest --test-dir core/build -j2                          # 전체 (기준 134 + 추가분)
ctest --test-dir core/build -R '^test_single_lane_' -j2  # ×2
for l in c cpp go rust; do for h in zlink_enum.h zlink/socket/api.h zlink/eventing/api.h; do
  cmp -s core/include/$h bindings/$l/include/$h || echo "MIRROR DIFF $l/$h"; done; done
git diff --check
bash bindings/cpp/tests/run_tests.sh
ZLINK_CORE_SOURCE=local bash bindings/python/tests/run_tests.sh
```

에이전트 산출물은 감독관이 diff를 직접 읽고(계약 불변·스펙 §3·중복/pass-through 제거 근거) 파일 명시 `git add`로
커밋한다. 알려진 load-flake: `test_single_lane_flow_snapshot_accounting`은 전체 suite 병렬에서 드물게 즉시 실패 →
단독 재실행 1회로 판정. C++ 테스트의 exit 86/134는 재링크 직후 일시 현상 → 1회 재실행.

## 6. 하지 말 것

- `doc/**`·`core/doc/spec/**` 수정(에이전트 금지; 스펙 변경은 감독관이 판정해 별도 커밋). framework/** 수정.
  `scripts/local-package/**` 실행(버전 파일을 0.15.x로 되돌린다). `--core-version` 사용. `hotpath_reference.json`
  에이전트 수정. `git add -A`·디렉터리 add. 측정 중 다른 빌드·테스트.
- 성능 미달 cell을 gate 완화·이월로 처리하는 것. 측정 결함이면 벤치를 고친다(예: one-way latency는 in-flight 1 구간).

## 7. 합류와 인계

- 순서: `perf/phase2-judge` → `perf/hotpath-gate` → `refactor/posddd` 순으로 PR·merge. 각 PR 본문에 gate 결과와
  sweep2 표(또는 비회귀 표) 요약을 넣는다.
- 세 PR이 모두 `main`에 들어가면 A에 알린다. A는 그 시점에 Phase 9 태그(release Core 빌드)로 넘어간다.
- 판정·발견은 `$ZLINK_WORK/c016/decisions.md`에 D-051부터 이어서 적고, 마지막 PR에 `doc/plan/c016-worklog/decisions.ko.md`로
  복사해 커밋한다. spec gap이 발견되면(특히 "completion poller owner의 blocking request" 계약) 감독관이 문안을 만들어
  사용자 승인 후 스펙에 반영한다.

## 8. 실행 결과 (2026-09-04, 머신 B)

- 작업 1(성능 판정): 원래 벤치의 결함 2건(REQREP latency를 포화 queue 깊이로 보고, latency 1µs 반올림)과 one-way ack 경계를
  정정한 뒤 판정. 정정 벤치가 드러낸 회귀 3건을 수정: PAIR 경로 원인 5개(8b6c2aa906), ROUTER 첫 activation 유실(90b58fd213,
  main의 phase-2 커밋에도 있던 결함), PUBSUB/inproc 64 KiB NODROP preflight(머신 A 커밋 이후). 30 cell 판정에서 throughput 집계
  전부 PASS; 남은 미달은 p95/p99 tail 집계 1~9%(WSL2 drift)로 사용자 결정(D-B70)에 따라 release 판정에서 제외. 전체 70 cell
  4-size sweep은 다음 release 전 재실행.
- 작업 2(hotpath gate 도구): `core/tests/perf/hotpath_bench.cpp`·`hotpath_gate.py`·`hotpath_reference.json`, ctest `hotpath_gate`
  (370717c0f0). 결정성 ±0.06%, 인위 회귀 3.8× FAIL.
- 작업 3(posddd 리팩토링 + wake 테스트): api/socket(d80ba60c9b), runtime/sockets(23bb5a968f), runtime/core(341974c4d6) 순감
  약 2,300줄, 공개 API/ABI 불변; `test_wake_invariants`(36d3174f63). 범위 밖 이동이 필요한 BLOCKERS는 각 rf 요약 참조.
- 합류: 단일 PR #1 → main 8d58b7f891 (사용자 지시로 3-PR 대신 1-PR, 브랜치 추가 없이 `perf/phase2-judge`에서 수행).
  판정 D-B54~B70은 `c016-worklog/decisions.ko.md`.
