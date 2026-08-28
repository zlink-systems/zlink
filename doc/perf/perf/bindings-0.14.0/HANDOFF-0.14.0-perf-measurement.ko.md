# Handoff — 0.14.0 bindings 최적화 & perf 재측정

> 작성: 2026-08-28 (세션 인계용). 이 문서 하나로 다른 에이전트가 이어받을 수 있게 정리했다.
> **branch**: `core-0.14.0-send-sync-terminals` (main에서 분기). 모든 작업은 이 브랜치에서.

---

## 0. 원래 목표 (변하지 않음)

- **bindings 라이브러리 최적화** — 불필요한 allocation/copy/synchronization 제거 + POSDDD 구조 개선. **성능 수치를 올리는 게 목적이 아니다.** perf는 **회귀 검증**용.
- Core는 thread-safe이므로 bindings에서 **Core가 보장하는 동기화를 중복**하는 부분만 제거.
- **public interface 불변** 원칙이되, 0.14.0에서 send/request sync 종결자 신설은 **감독자(사용자)가 명시 승인**한 계약 확장이다.
- 측정 언어: **cpp/dotnet/java/node** (+ C reference 기준). 계약 변경은 전 7언어(cpp/dotnet/java/node/python/rust/go)에 적용.
- **측정 격리 최우선**: perf 측정은 절대 병렬 금지. 스모크(크래시 확인)는 격리 불필요.

---

## 1. 완료된 작업 (커밋됨, branch에 9개)

| 커밋 | 내용 |
|---|---|
| `054b99ace7` | **send sync(+flags) 종결자** 7언어 신설. async terminal은 `zlink_send_async`(flags 없음), sync terminal은 `zlink_send_part(_rid)`+flag(NONE=blocking, DONTWAIT=즉시 backpressure). |
| `e476a438e3` | **request 3-terminal + `submit_sync` 통일**. request도 send처럼 HWM admission을 지남. 종결자: sync 반환 / sync callback(+flags) / async, 전 바인딩. async가 `submit()`인 언어(Java/Node/Python/Rust)는 sync를 **`submit_sync`**로 통일(과거 submit_blocking/submit(SendFlags) overload에서 개명). C++ `submit()`/`submit(callback)`/`async()`, .NET `Submit(SendFlags)`/`Async()`, Go `Flags().Submit(ctx)`+channel. |
| `da4dbd6022` | **perf runner 정합** — inflight-1 직렬화 제거, 코드 상한(maxInFlight/window) 제거, 연속 제출, POLLCOMPLETION 단독. **⚠️ 이 커밋이 아래 §2의 starvation 회귀를 심었다.** |
| `9b2bca784c` | Go multi-reqrep 패턴 2개 추가(다른 언어엔 있었음). |
| `310f6bf67e` | **스모크 수정 — starvation 회귀** (da4dbd6022가 심음: 연속 제출 inner-loop가 completion poller를 굶겨 "no active replies"). round-robin+매 라운드 drain으로 수정. **C + 7언어 전부** 있었음. STREAM 2차 이슈도 수정. |
| `225498f841` | **스모크 — ROUTER_ROUTER 전 transport + multi IPC**. 이전 스모크가 tcp/inproc만 봐서 ws/wss/tls/ipc를 놓침. C(PERF_TRANSPORTS 무시), C++/.NET/Rust/Go/Python(multi ipc 하드코딩 제외), Java(TLS/WSS teardown), Node(TLS multipart 누적) 수정. |
| `45733736a1` | **정책 문서**: 스모크 정의 + bindings↔C 비교=multi만 (PERF_POLICY.md §3.2, §1.1). |
| `ea9107ab4c` | Node multi IPC 활성화 — 손상은 ipc가 아니라 `Received` 재사용 버그였고 이미 수정됨. |
| `16c9012572` | Python multi IPC runner 테스트. |

**이중동기화 검토(별도 커밋 없음 — 변경 없었음):** 6언어(.NET/C++/Python/Node/Rust/Java) 전부 **Core 중복 락 없음**. 남은 락은 전부 Core가 관리 안 하는 바인딩 로컬 상태(completion registry, callback lifecycle, thread-local pool 등). 근거: `core/doc/guide/11-thread-safety.ko.md`.

---

## 2. 확정된 방법론 결정 (사용자 지시)

1. **스모크 테스트 정의** (PERF_POLICY.md §3.2에 반영):
   - 패턴: **ROUTER_ROUTER**(single) / **MULTI_ROUTER_ROUTER**(multi, **100 CCU**)
   - 크기: **1024B**만
   - transport: **전부** (single: tcp·ws·wss·tls·inproc·ipc / multi: tcp·ws·wss·tls·ipc, inproc은 multi 대상 아님)
   - 목적: transport 축 커버리지(각 runner가 전 transport에서 crash/fail 없이 도는지). 전 패턴 도는 게 아님.
2. **bindings↔C 성능 비교 = multi suite만**. single은 sender/receiver를 한 프로세스 스레드로 돌려 async 런타임 바인딩(coroutine/event loop)에 아티팩트 유발 → 비교에서 제외. **단 single·multi runner는 둘 다 유지**. C reference는 single+multi 둘 다 측정.
3. **Core는 수정 금지**(승인 필요). 계약 spec 문서(async-coroutine-policy, async-execution-model)는 감독자 소유.

---

## 3. 미해결 — 다음 에이전트가 이어서 할 일

### 3-1. ⚠️ 최우선 미해결: SENDSEND latency 측정 불일치 (검증 필요, 결론 없음)

**증상** (MULTI_ROUTER_ROUTER_SENDSEND, tcp, 1024B, 100 CCU, core 0.14.0, 깨끗한 측정):
- C reference: throughput **42,199**, latency **1518 ms**
- C++ binding:  throughput **179,973**, latency **2.004 ms**

**상태**: 원인 **미확정**. throughput은 C++가 높은데 latency는 C++가 1/750 — 보통 throughput↑면 in-flight↑라 latency↑여야 하는데 반대다. → C와 C++가 **SENDSEND latency를 서로 다른 지점/의미로 측정**할 가능성이 크다.

**주의(이전 에이전트의 잘못):** Little's Law(outstanding=latency×throughput)로 "C 큐가 64K로 깊다"고 **추정했으나 검증 안 함.** 이 추정을 사실로 받아들이지 말 것. "전 runner를 round-robin으로 통일하자"는 제안도 이 미검증 추정에 근거한 것이라 **보류**.

**해야 할 일**: 추정 말고 **코드로 검증**:
- C SENDSEND client/server의 latency **측정 지점**: `bindings/c/perf/multi/src/perf_multi_router_router_client.cpp`, `perf_multi_router_router_server.cpp`, `perf_multi_router_router_matched_client.cpp`(latency 계산이 여기 있음), `bindings/c/perf/multi/common/perf_multi_weighted_latency.hpp`, `perf_multi_metrics.hpp`. client send-ts 기준인지 server recv-ts 기준인지, 무엇을 sample로 기록하는지.
- C++ 대응: `bindings/cpp/perf/multi/src/perf_router_router_client.cpp`, `perf_router_router_server.cpp`.
- 두 값이 왜 다른지 사실을 확인한 뒤, **측정 의미가 C와 바인딩에서 동일해지도록** 정합(그게 latency든 outstanding depth든). SENDSEND latency 의미 자체(one-way flood에서 latency가 무엇인지)는 사용자 perf 정책 설계 사항이므로 필요하면 사용자에게 확인.

**참고 측정 로그**: `/tmp/claude-1000/.../scratchpad/rem_c_rr2.log`(C 깨끗), `rem_cpp_rr.log`(C++), `rem_c_rr.log`(C 오염본 — 무시).

### 3-2. 통제 환경 재측정 (pass/hold 매트릭스)

- runner는 이제 스모크 통과하므로 측정 가능. **bindings는 multi만** vs C(§2 방법론).
- §3-1 latency 측정 불일치를 먼저 해결해야 의미 있는 비교가 됨.
- plan: 미달 항목 대상 개선 pass. row 평균 통과면 통과, 미달이면 최대 3 pass, 개선 포인트 없으면 보류.

### 3-3. 잔여 관찰 (필요시)
- **Node single inproc**: 미지원(단일 스레드 이벤트루프 모델 제약). 확인만.
- **MULTI_STREAM 고 CCU**: 스모크 정의가 ROUTER_ROUTER로 바뀌어 STREAM은 스모크 대상 아님. Go STREAM 기본 10K clients는 메모리 guard로 skip됨.

---

## 4. 환경/도구 주의사항 (반드시 읽을 것)

### 측정 오염 — codex 에이전트가 **stray perf 프로세스**를 남긴다
- 측정 **직전 반드시** stray 프로세스 확인·정리: `ps -eo pid,args | grep -iE "perf_multi|perf_single|BindingBench|perf_.*server|perf_.*client"` 후 **PID로 kill**.
- **`pkill -f "perf..."` 쓰지 말 것** — 내 shell 명령줄에 "perf" 문자열이 있어 자기 자신을 죽인다(exit 144). 반드시 PID 지정.
- idle gradle/kotlin daemon(`java ... KotlinCompileDaemon`, GradleDaemon)은 벤치 아니고 idle이라 측정 영향 미미 — 유지 가능.
- 이번 세션에서 오염 사례: stray `perf_multi_router_router_server.js --transport wss` 때문에 C 측정이 42K→32K로 24% 낮아졌었다.

### codex 실행법 (이 세션에서 검증됨)
- 모델 ID는 **전체 이름**: `gpt-5.6-terra`, `gpt-5.6-sol`. **맨이름 `terra`/`sol`은 400 거부**됨(ChatGPT 계정).
- 파일 수정/테스트: `codex exec -m gpt-5.6-sol -s danger-full-access --skip-git-repo-check < prompt.md`
  - socket bind/TCP listen 필요(테스트·측정)하면 `-s danger-full-access`. 문서만이면 `-s workspace-write`.
- **AGENTS.md 브랜치 정책**: codex는 non-main 브랜치 + dirty worktree에서 승인 없이 수정 거부. 프롬프트에 **"사용자가 현재 브랜치 `core-0.14.0-send-sync-terminals`에서 작업하도록 명시 지시. main 전환 금지. bindings/<lang>/** 만 수정. commit·push 금지"** 승인 블록을 반드시 넣을 것.
- perf 하네스: `PERF_TRANSPORTS=...`(콤마), `PERF_MULTI_DURATION_SECONDS`, `PERF_SINGLE_DURATION_SECONDS`, `--pattern`, `--msg-sizes`, `--clients`, `--core-version 0.14.0`(캐시 `~/.cache/zlink/core/0.14.0`).
  - single: `bindings/<lang>/perf/run_benchmarks.sh` / `bindings/<lang>/perf/run_binding_single.sh`
  - multi: `bindings/<lang>/perf/run_benchmarks_multi.sh` / `run_binding_multi.sh`
  - C reference: `bindings/c/perf/run_benchmarks.sh`(single), `run_benchmarks_multi.sh`(multi)
  - 결과 라인: `RESULT,current,<pattern>,<transport>,<size>,<metric>,<value>` (metric: throughput/bandwidth/latency/latency_p95/latency_p99). harness가 binding vs C 비교(run_comparison.py)도 함.

### transport별 지원 (정정 스모크로 확인됨)
- **multi ipc**: C·C++·.NET·Java·Node·Python·Rust·Go **전부 지원**(이번에 활성화 완료). 고유 bind 경로(pid+timestamp) 필수(`ipc://*` 공유는 프로세스 간 충돌).
- **multi inproc**: 원리상 대상 아님(별도 프로세스).
- **single inproc**: Node만 미지원(이벤트루프 모델).

---

## 5. 미커밋/아티팩트 (커밋 대상 아님)
- repo 루트 NuGet 잔해(`Systems.Zlink.nuspec`, `[Content_Types].xml`, `_rels/`, `package/`, `runtimes/`, `provenance/`) — 압축 이전 세션 부산물, 무시.
- `doc/perf/perf/bindings-0.14.0/` 내 `*.xlsx`, `bindings-library-performance-improvement-plan-core-0.14.0.ko.md`(대용량), `log/` — 기존 아티팩트.
- `doc/perf/perf/bindings-0.13.2/spec-and-interface-change-proposals.ko.md`(M) — 기존 세션 변경.

## 6. 핵심 참고 문서
- 계약(감독자 소유, 읽기만): `bindings/doc/spec/async-coroutine-policy.{ko,en}.md`, `bindings/doc/spec/async-execution-model.{ko,en}.md`
- perf 정책: `doc/perf/PERF_POLICY.md`(§3.2 스모크, §1.1 비교 방침), `PERF_SINGLE_TEST_POLICY.md`, `PERF_MULTI_TEST_POLICY.md`
- Core thread-safety 근거: `core/doc/guide/11-thread-safety.ko.md`
- 이 작업 계획: `doc/perf/perf/bindings-0.14.0/routed-send-nonblocking-plan.ko.md`, `perf-runner-parity-issues.ko.md`
