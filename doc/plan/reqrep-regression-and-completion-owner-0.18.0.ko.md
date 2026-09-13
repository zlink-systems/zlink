# reqrep 회귀 수정 + completion owner 단일화 캠페인 (core 0.18.0)

> 시작: 2026-09-12 · 갱신: 2026-09-13
>
> 발단: 0.18.0 bindings 성능 계획서(`doc/perf/perf/bindings-0.18.0/...`)의 java·.NET reqrep
> 항목에 "말도 안 되게 낮은 수치"(소형 2~7%)가 있다는 사용자 지적. 원인 규명 결과 두 갈래로
> 확장됐다: (1) perf 하네스의 completion-drain 회귀, (2) 그 회귀가 드러낸 바인딩 completion
> ownership 모델의 문제 → 스펙 단일화. 이 문서가 두 단계와 부수 작업의 계획·상태를 소유한다.

## 1. 원인 (확정)

reqrep 소형 저조 = perf 하네스가 요청/응답 completion을 **요청 문맥(제출 스레드/goroutine/
coroutine)이 아니라 별도 백그라운드 drain**에 맡긴 회귀. C·cpp·Rust는 요청 문맥에서 poller로
직접 drain(정상). 백그라운드 drain 경로는 payload 무관 flat ceiling·latency 폭발·(Python) 측정
실패를 유발했다.

- 회귀 도입 커밋: .NET = `9c8187872b`(G4), Java = `810983b674`(G3, `4d458e429a`가 부분 복원).
- 문서의 기존 판정("send builder P/Invoke 계약 비용으로 보류")은 **오진**이었음(실측 반증).

## 2. Phase 1 — perf 하네스 reqrep C-parity 복원 (완료)

요청 문맥이 공개 `PollCompletion` poller로 completion을 직접 drain + HWM admission window로
연속 제출(C/cpp 방식). perf 하네스만 수정, 바인딩·Core 불변, 분류 B.

| 바인딩 | 조치 | 결과(C 대비) | PR |
|--------|------|--------------|----|
| .NET single | 하네스 복원 | 소형 2~7%→40~44%, aggregate 34~128% | #284(머지) |
| .NET multi | 코드 기복원, 재측정 | 5~10%→47~79% | #284 |
| Java single | 하네스 복원 | 18~66%→41~127% | #285(머지) |
| Go single | 요청 goroutine drain 복원 | inproc latency 701×→2×, 소형 1.58~3.70× | #286(머지) |
| Python single | 요청 스레드 drain 복원 | 소형 12셀 1/11실패→12/0, latency 1392×→6.6× | #290(머지) |

- C++·Rust·Node는 원래 정상(회귀 아님). Node·Rust·Python 소형은 각 런타임 per-op 바닥.

## 3. Phase 2 — completion owner를 public poller로 단일화 (바인딩 7/7 완료)

Phase 1에서 드러난 사실: 바인딩은 poller 미등록 시 **runtime(백그라운드) owner**로 completion을
drain하는데(스펙 §4 구 조항), 이 경로에 락 결함·Python 간헐 deadlock(#293)이 있었고, 스펙 §4가
"runtime owner 기본"(69~70행)과 "poller wait() 필수·별도 drain thread 없음"(77~81행)을 **동시에**
규정하는 모순이 있었다. 사용자 결정: **runtime owner 제거, public poller 단일 owner, 오사용은
submit 시점 fail-fast**.

### 3.1 스펙 (완료)
- `bindings/doc/spec/async-execution-model.ko.md` §4·§7 개정 — completion owner = PollCompletion
  poller의 `wait()` 구동 문맥 하나뿐; 비동기 completion terminal은 owner 없이 제출 시 `InvalidState`
  즉시 거부; blocking terminal은 호출 문맥 in-line drain. **PR #299(머지)**.

### 3.2 바인딩 코드 (분류 A, 각: runtime pump 제거 + async fail-fast + blocking in-line drain + 테스트/샘플 개정 + 게이트 5회 + perf 회귀 체크)

| 바인딩 | 상태 | 커밋 / PR | 게이트 | perf 회귀 |
|--------|------|-----------|--------|-----------|
| .NET | ✅ 머지 | #316 | 252/252 + 동시성 91/91 ×5 | 없음(42/42 complete) |
| Java | ✅ 머지 | #321 | unit128+integ35 + 동시성 ×5 | 없음(24/24 complete) |
| Go | ✅ 머지 | #322 (커밋 9ee3d41f9d + one-way hang 회귀 수정 dca30bc5bd) | go test ×5 green | 없음(ALL/tcp/65536 complete) |
| Python | ✅ 머지 | #324 (#293 CLOSED) | pytest 248 + 동시성 73×5 green | 없음(42/42 complete) |
| C++ | ✅ 머지 | #323 | contract 20/20 + 동시성 9파일×5 green | 없음(42/42 complete) |
| Rust | ✅ 머지 | #325 | cargo test 188 + 동시성 83×5 green | 없음(42/42 complete) |
| Node | ✅ 머지 | #327 (커밋 32250cbf31) | binding 182/182 + samples 7/7 + 동시성 서브셋 ×5 green | 없음(single reqrep+one-way tcp: complete, fail_fast 0, InvalidState/hang 없음) |

- Go 특이: 공개 API가 async-only(`Submit(ctx)`) 표면이라 completion-backed blocking terminal 없음
  → runtime goroutine 제거 + async fail-fast만. 규칙 2→1.
- **Node 특이(2026-09-13 전환 완료 #327)**: Node는 blocking terminal 없이 async(Promise)만이고
  `wait()` 구동 thread가 없다. libuv readable 콜백은 event-loop wakeup만 전달하고, 그 안에서
  socket-lifetime public `Poller.wait(events, 0)`(nonblocking)가 completion을 drain한다(**새 thread
  없음**). binding `runtimeWatch` background drain 제거→readable-only watch, async request/
  backpressured send는 owner 없으면 즉시 `InvalidState`. framework는 신규
  `node-event-loop-poller.ts`가 socket-lifetime public poller를 libuv 콜백에서 구동(completion-capable
  socket만 `PollCompletion` 등록). 규칙 3→2. binding/framework 동종 소유 구조.
- Python 특이: async 경로 데몬 스레드(`zlink-python-completion`)가 #293 deadlock 원인 영역 →
  제거로 #293 해소 여부를 함께 확인.
- **C++·Rust도 전환 대상(2026-09-13 확인)**: 둘 다 바인딩 본체에 runtime(백그라운드) owner 스레드가
  있다 — C++ `completion_owner.cpp:709`의 `_runtime_thread`, Rust `completion_owner.rs`의
  `runtime_loop` reactor thread(REACTOR_WAIT_MS=25). perf 하네스가 poller 구동이라 회귀는
  없었지만, 스펙 #299(single public owner)에는 미준수 → .NET/Java/Go/Python과 동일 전환 필요.

## 4. 부수 작업

- **§9.0 평균 지표 보정(완료 #298)**: 비율 데이터 산술평균이 대형 outlier(PUBSUB·대형, 한 셀
  1128%)로 왜곡돼 Rust single이 128.7%(">C")로 보이던 문제 → **기하평균(+중앙값)**으로 교체
  (Rust 91.9%=near-C). 판정(통과/보류) 불변.
- **Go/Python single one-way 측정(완료 #297)**: 미측정이던 5 one-way 패턴 30셀씩 측정. Go
  geomean 59.9%, Python 28.7%(소형 per-op 바닥). §9.5.1/§9.7.1·§9.0 반영.
- **Go/Python multi 측정(남음)**: multi suite 전 패턴 미측정 → 측정해 §9.5.2/§9.7.2 채우기.
  결과는 geomean/median으로 보고. (코드 캠페인 사이 자원 경합 피해 실행.)
- **#293 Python 간헐 deadlock: 해소됨(2026-09-13)**. Phase 2 Python 전환이 원인이던
  백그라운드 daemon 완결 스레드를 제거하면서, 그 deadlock 테스트가 5×5 green·hang 없음으로
  확인됨(Python 전환 커밋 `c005e8b8c2`). 별도 gdb 규명 불필요.

## 5. 남은 작업 순서

1. **바인딩 Phase 2: ✅ 7/7 완료** — .NET#316·Java#321·Go#322·Python#324·C++#323·Rust#325·Node#327 전부 머지.
2. **framework: ✅ #326 머지** — .NET/Java receive poller에 PollCompletion 추가(위반 재현·수정), C++ 이미 준수(무변경), Go/Rust/Python framework 런타임 없음. .NET 2177/0·Java green·cpp cross-language smoke green.
   - **Node framework: ✅ #327에 포함 머지** — 신규 `node-event-loop-poller.ts`가 socket-lifetime public poller를 libuv 콜백에서 구동, completion-capable socket에 `PollCompletion` 등록. typecheck/build/lint green, 영향 contract 번들 214/215.
   - **기존 인프라 이슈(범위 밖, 본 전환 무관)**: (a) `channel-client.test.js`의 `AbortSignal` fixture가 Node 24.19 `AbortSignal.any()` strictness로 실패(이 커밋이 건드리지 않은 fixture), (b) `run_node_runtime_gate.js`의 TAP integrity 파서가 Node 24.19 파일별 TAP 출력과 비호환 → 공식 gate command 실패(개별 파일 실행은 회귀 없음). 둘 다 별도 수정 필요.
   - **관찰(범위 밖)**: 통합 build 중 Go binding test `TestPublicRequestRetriesExactPacketAfterWritable/run-4` 실패 관찰 — 별도 확인 필요.
3. **Go/Python multi 측정(진행 중)** → §9.5.2/§9.7.2 반영. single one-way·reqrep은 이미 측정·반영(§9.5.1/§9.7.1). multi는 fresh main 워크트리에서 별도 측정(codex terra, geomean/median 보고).
4. (별도 환경) **#293** 네이티브 규명(단, Phase 2 Python 전환으로 재현 테스트는 5×5 green·해소됨 — §4 참조).

## 6. 운영 메모 (재현·함정)

- **codex 실행은 반드시 `< /dev/null`** — stdin EOF 대기로 "Reading additional input from
  stdin..."에서 무한 wedge(2026-09-13 실측: 두 작업 ~7h 정지). scope "active"만 믿지 말고
  codex.out 증가·CPU·산출물로 실동작을 3분 간격 확인.
- 측정은 직렬(동시에 두 perf 프로세스 금지), OOM 방지 위해 `systemd-run --user --scope
  -p MemoryMax=9G`로 감싼다. `--reuse-build`는 stale jar/패키지 위험(수정 전 수치),
  `_JAVA_OPTIONS`는 러너 버전파싱을 깨뜨림 → 둘 다 금지.
- 각 바인딩 코드 작업은 감독자가 diff 리뷰 후 push/PR/merge. 정합성 게이트는 동시성 테스트
  5회 반복. perf 회귀 체크는 fail-fast 파손(InvalidState) 여부 + reqrep·one-way 수치 대조.
- 결정 원칙: 규칙 수가 줄어드는 근본 수정(runtime/public owner 이중 → public 단일).
