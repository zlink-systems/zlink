# Handoff — 머신 A: gRPC 벤치와 framework 성능 0.90 (milestone 0.18.0)

> 최초 작성 2026-09-10 저녁, **2026-09-10 23:40 갱신**(세션 1 진행분 반영).
> 이전 기록은 [`handoff-2026-09-10-framework-perf.ko.md`](handoff-2026-09-10-framework-perf.ko.md), 판정은
> [`fw-bench-worklog/decisions.ko.md`](fw-bench-worklog/decisions.ko.md) FB-059~FB-070.
> 규범: `framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.{ko,en}.md`, 06(state lane),
> 벤치 규격 `framework/bench/grpc/README.{ko,en}.md`.

## 0. 지금 가장 중요한 것 — G4가 framework를 깼다

머신 B의 G4(바인딩 submit 결과 객체)가 **머지됐다**: #119 Java · #121 Python · #122 C++ · #123 Node.
`submit()`의 반환이 `CompletionStage`/`Promise`/`async_result_t`에서 `SendSubmission`/`RequestSubmission`으로 바뀌었다.
**framework 4언어는 적응되지 않았다.**

| 언어 | 바인딩 G4 | framework 상태 | 처리 |
|---|---|---|---|
| Java | #119 머지 | 컴파일 실패 | **복구 완료** — PR #125 |
| Node | #123 머지 | **조용히 깨짐** — 컴파일·`npm test` 전부 통과하는데 admission 대기가 사라졌다 | **복구 완료** — PR #135. `await-thenable` lint 게이트 포함 |
| C++ | #122 머지 | 컴파일 실패 (`.async().result()`; 새 `send_submission_t`는 `result`가 멤버다) | **복구 완료** — PR #134 |
| .NET | #92 머지(PR #136) | **컴파일 실패, 차단됨** | 47곳 적응 완료. **`TrySubmit` 2곳은 바인딩 결정 대기 — Issue #140** |

**Node가 가장 위험하다.** `SendSubmission`은 thenable이 아니라 `await op.submit()`이 즉시 그 객체를 돌려준다.
TypeScript도 런타임도 오류를 내지 않는다. 결과로 흐름 제어가 사라지고, `catch` 블록(NotConnected→peer 정리,
Backpressured 보고)이 통째로 죽은 코드가 됐다. 상세는 Issue #99 코멘트.

**소유권 변경(사용자 승인 2026-09-10)**: G5(#97~#100)는 원래 머신 B 몫이었으나 **머신 A가 가져간다.**
범위는 **컴파일·의미 복구까지**로 한정하고, 동기 blocking 종결자와 성능 활용은 별도로 남긴다.

**성능 활용은 아직 아무도 안 했다.** `result`가 `OK`면 admission을 기다리지 않고 다음 제출로 가고
`BACKPRESSURED`일 때만 `admitted`를 기다리는 구조 — 이것이 G4의 실제 이득이고 스펙 08 E4·E5다.
각 언어 perf 작업이 rebase한 뒤 별도 단계로 한다.

## 1. 목표와 지금 숫자

- **목표(사용자)**: framework send·request 처리량 ≥ 같은 언어 raw binding의 **0.90**.
  판정 패턴은 규격 §7.2(request-backpressure 기준, §5.2 깊이·지연 병기).
- **C++**: serial 회귀를 회복했다(1-run, `cpp-submit-r3`): serial **+207%**, window +246%/+10%,
  saturation **+698%**. 원인은 짧은 state turn마다 있던 강제 pool 왕복이었다. 0.90 판정은 아직 없다.
- **Java**: #85 머지로 request-serial/1024 **+40.3%**(2,308→3,238 req/s). raw 8,784 대비 0.37.
  #47(복사 제거)은 **보류** — 아래 §4.
- **.NET**: #48 1단계 진단 승인 완료, 2단계 구현 중.
- **Node**: 벤치 framework 행이 **열렸다**(`bench-pairing-13b`). 1 ms ingress 타이머는 **Issue #111** 대기.

## 2. 이 세션에서 끝난 것

| PR | Issue | 내용 |
|---|---|---|
| #105 | #46 | bench java 러너가 JDK 25를 탐색한다 |
| #106 | #87(A 몫) | Conan·vcpkg 0.18.0. `release-check.sh core 0.18.0` **PASS**, prefix 생성 |
| #107 | #85 | Java ToNode registry 3→1 turn. 감독 검증 1,518 테스트 통과 |
| #109 | #37 | 측정 비교표 도구. 오류·abandoned·drain 셀은 비율을 만들지 않고 근거를 드러낸다 |
| #113 | #112 | local-package staging 별칭이 CMakeCache에 박혀 두 번째 실행이 항상 실패하던 결함 |
| #114 | #10(일부) | Node codec protobuf bytes round-trip |
| #116 | #115 | `job.sh --effort xhigh` |
| #125 | #97(일부) | framework Java를 새 submit 종결자에 적응 — **main 복구** |

## 3. 새로 연 Issue (전부 감독 재검증에서 나왔다)

| # | 내용 |
|---|---|
| #108 | #85의 registry turn 1회를 고정하는 회귀 테스트가 없다. 3 turn으로 되돌아가도 통과한다 |
| #110 | Rust 바인딩 테스트가 고정 `sleep`으로 inproc 연결을 추정해 부하에서 `NotConnected`. 로컬 패키징을 3번 막았다 |
| #111 | **Node 수신 readiness 공개 API.** 메커니즘은 이미 있다(`uv_poll`, completion 소켓에서 사용 중) — 공개 표면만 없다. 이름 제안 `setReadableHandler`. **사용자 승인 완료, 스펙 문안 대기** |
| #112 | local-package 경로 별칭 (수정 완료) |
| #115 | job.sh xhigh (수정 완료) |
| #117 | framework C++ PR CI — hiredis·redis++ 부트스트랩과 cpp binding package가 선행 |
| #120 | stream multiclient ready gating 테스트가 GitHub 러너에서만 60초 hang. 로컬은 0.68초, `taskset -c 0,1`도 0.68초 |

## 4. 판정 대기 중인 것

### #47 (framework Java 복사 제거) — **머지하지 않았다**
구조는 타당하다(`List<byte[]>` → `List<Message>`, frame 표현 소유자 2→1). 그러나 감독 3-run 재측정에서
request-backpressure 4096의 **꼬리가 무너진다**.

| | run1 | run2 | run3 | job 1-run | ~130 ms 이상치 |
|---|---:|---:|---:|---:|---|
| before(main) | 0.513 | 0.561 | 0.445 | 0.451 | **0/4** |
| after(#47) | 0.510 | **129.5** | 0.436 | **147.4** | **2/4** |

처리량 중앙값 −3.1%(cell 허용치 5% 안), p99 중앙값은 동일한데 **절반의 확률로 ~130 ms 정지**한다.
유력한 기전: native `Message`를 제출 경로 내내 붙들어 HWM에 걸린다. **G4 적용(OK면 대기 없음) 뒤 재측정**해서
사라지는지 보는 것이 순서다.

### 리뷰 대기 job 산출물
- `cpp-submit-r3`(#49) — serial +207% 회복. 커밋 검토·테스트 재실행·PR 필요.
- `node-ingress-50b`(#50) — envelope header 직접 기록, persistent worker 재사용. 이미 post-G4 main 병합됨.
- `bench-pairing-13b`(#13) — .NET·C++·Node framework 행을 `ToNode`로, 패턴 3개. **G4가 같은 bench 파일
  (`framework/bench/grpc/{cpp,node}`)을 건드렸으므로 병합 충돌을 확인해야 한다.**
- PR #118(#16) — PR 검증 워크플로우. Python 스모크가 native extension을 빌드하지 않아 실패한다(내 실수, 수정 필요).

## 4.5 머신 A에 추가 할당된 항목 (원래 계획에 없던 것)

세션 중에 A가 새로 맡게 된 것들이다. 새로 생기면 여기에 계속 적는다.

| Issue | 무엇 | 왜 A가 맡았나 | 상태 |
|---|---|---|---|
| **#97** framework-java G5 | 새 submit 종결자 적응 | main이 깨졌고 B는 브랜치조차 없었다. 사용자 승인 2026-09-10 | **완료** (PR #125, 컴파일·의미 복구까지) |
| **#99** framework-node G5 | 같음 + `await-thenable` lint 게이트 | 같음. Node는 **조용히** 깨져 더 급했다 | **완료** (PR #135). 감독이 job의 hot-path wrapper를 제거하고 계약 중복도 합쳤다 |
| **#100** framework-cpp G5 | 같음 | 같음 | **완료** (PR #134). 공유 cpp 패키지가 G4 이전이라 막혀 있던 것을 감독이 재빌드 |
| **#98** framework-dotnet G5 | 같음 | #92(PR #136)가 머지되며 깨졌다 | **F1 완료** (PR #155). #140은 오판이라 닫았다 |
| **#108** | #85 registry turn 1회 회귀 고정 테스트 | 감독 리뷰에서 발견 | 미착수 |
| **#110** | Rust 테스트 sleep 의존 | 로컬 패키징을 3번 막았다 | 미착수 |
| **#111** | Node 수신 readiness 공개 API | job의 D 판정을 감독이 기각하고 재정의. **사용자 승인 완료** | 스펙 문안 대기(감독) |
| **#112** | local-package 경로 별칭 | 감독이 원인 규명 | **완료** (PR #113) |
| **#115** | job.sh xhigh | 사용자 결정(astra는 항상 xhigh) 반영 | **완료** (PR #116) |
| **#117** | framework C++ PR CI | #16 범위에서 분리 | 미착수 |
| **#120** | stream 테스트가 러너에서만 hang | 새 CI가 발견 | 미착수(비차단 격리 중) |
| **#126** | 바인딩 스펙 산문이 옛 Promise 계약을 말한다 | framework 파손 조사 중 발견. **잘못된 호출을 문서가 승인하고 있었다** | **완료 (머신 B, PR #128·#129)** — 7언어 per-lang README + policy·model 산문을 결과 객체 계약으로 정합 |
| **#133** | 벤치 클라이언트가 예외를 버리고 개수만 센다 | #48의 `client errors=2` 원인을 확정할 수 없었다 | job 진행 중 |
| **#137** | C++ 벤치가 `catch` 안에서 `co_await` 해 main에서 컴파일 안 됨 | G4(#122)가 컴파일 못 한 채 머지. `framework/bench/**`는 어떤 CI도 안 봄 | **완료** (PR #139) |
| **#140** | `TrySubmit` 제거로 `SendFlags.DontWait` 대응 수단 없음 | framework .NET 컴파일 차단이라고 봤다 | **감독 오판 — 닫음.** 제거는 설계였고(스펙 `01-submit-and-completion.ko.md:446`), framework 2곳이 순간 backpressure를 terminal 거부로 바꿔 메시지를 잃던 쪽이 결함이었다 |
| — | tooling contract smoke가 Core prefix를 전달하지 않는다 | #100 검증 중 발견 | 미보고(#117에 합칠 것) |
| — | 로컬 패키지 post-G4 재빌드 | 공유 0.18.0 C++ 패키지가 G4 이전 헤더였다 | **완료** |

### G5의 정확한 잔여 — F1은 끝났고 **F2·F2-a만 남았다** (2026-09-11 감독 확인)

#97~#100이 아직 OPEN인 이유를 오해하기 쉽다. 세 가지를 구분한다.

| 조각 | 무엇 | 상태 |
|---|---|---|
| **F1** | 내부 binding 호출을 결과 객체 소비로 (`.reply()`/`.admitted()`) | **4언어 완료** — PR #125·#155·#135·#134 |
| **G1(스펙)** | `01-submit-and-completion.{ko,en}.md` §2·§4·§5·§15·§16 + **5개 언어 interfaces 문서** | **완료.** 시그니처가 이미 문서에 확정돼 있다 |
| **F2·F2-a** | framework 공개 표면에 **동기 blocking 종결자** 추가, runtime 실행 문맥에서는 `InvalidOperation` | **미착수 — 이것이 #97~#100의 잔여 전부다** |

확정된 이름(스펙 §16, `async-coroutine-policy.ko.md` §6):

| 언어 | 비동기(현행) | 동기 blocking(추가) |
|---|---|---|
| Java·Node | `submit()` | `submit_sync()` |
| Kotlin | wrapper `await()` | 추가 없음 — Java 표면 그대로 |
| .NET | `Async()` | `Submit()` |
| C++ | `async()` | `submit()` |

F2-a 판정 술어는 **언어마다 한 곳에 모은다.** 이미 있는 표지를 합쳐 쓰고 새로 만들지 않는다 —
application job context(handler turn) · Spot activation(Spot turn) · state lane current.
실패는 **부작용 전에** 나야 한다. 제출한 뒤 던지면 메시지를 잃는다.

**Node 주의** — Node는 단일 스레드다. 문서가 정의한 blocking 의미를 event loop 위에서 정직하게
만족시킬 수 없으면 **억지로 구현하지 말고 D(스펙 공백)로 보고**하게 했다. busy-wait·event loop
thread의 `Atomics.wait`은 금지다.

**G4 성능 활용**(`OK`면 admission 대기 없이 연속 제출)은 **#151**로 열려 있다. 언어별 G5가 끝난 뒤 A가 맡는다.
**#47 재판정의 선행 조건이다.**

### 2026-09-11 추가 할당 (계속)

| Issue | 무엇 | 상태 |
|---|---|---|
| **#45** | envelope header를 message마다 새로 만들지 않는다 (4언어) | job `envelope-45` 진행 중 |
| **#60** | .NET oversized server reply가 `CapacityExceeded` 대신 deadline까지 남는다 | job `dotnet-oversized-60` 진행 중 |
| **#82** | Node application `Rejected`가 .NET spot-route client에서 `internal_failure`로 바뀐다 | job `node-rejected-82` 진행 중. **본문이 비어 있어 재현이 1단계** |
| **#111** | Node 수신 readiness 공개 API | 스펙 문안 **완료**(PR #163). job `node-readiness-111` 구현 진행 중 |
| **#158** | C++ send-saturation에서 owner FIFO가 53% 버린다 | phase 1 판정 완료(거부 자체는 스펙 허용, **drop 지표 부재와 벤치의 `completed == received` 가정이 결함**). job `cpp-ownercap-158b` 진행 중 |
| **#97~#100** | 위 F2·F2-a | 브리프 4개 작성 완료, 슬롯 대기 |

### 2026-09-11 오후 판정·환경 (계속)

**#164/#165 — main이 4일간 red였고 아무도 몰랐다.** `bindings/python/tests/test_perf_multi_runner.py`
단언 2건이 2026-09-07 러너 정책 정합(`b93b176061`, `d634417a37`) 이전 값을 들고 있었다.
**이 테스트를 돌리는 CI가 없었다.** #16의 PR 검증이 처음 찾아냈다. 구현은 옳고 테스트가 낡았다 —
C 레퍼런스(`bindings/c/perf/run_comparison.py:1255`, `:3830-3845`)가 근거다. PR #165 머지.

**#82 닫음 — 재현되지 않는다.** Node ↔ .NET 양방향 5회씩 10/10에서 `kind=rejected|origin=application`이
보존된다. 그 경로에는 이미 교차언어 단언이 있다(`node_dotnet_smoke.js:531`). 4언어가 모두
"framework exception이면 그 kind, 아니면 `internal_failure`"로 동일하며, `internal_failure`가
나오는 유일한 조건은 오류 모델 §5 마지막 항목이 정한 그대로다.

**#143 순서 결정** — 세 언어의 선행 조건이 다르다. C++(#49)만 CLOSED라 지금 가능하고,
.NET(#48)·Node(#50)은 그 이슈가 머지된 뒤다. 한 job으로 묶지 않는다.

**#15는 아직 선행 조건이 없다** — `ZLINK_CTX_OPT_BLOCKY`가 Core header에 살아 있고 binding이
`MaxMessageSize`를 아직 노출한다. binding 1.0.0에서 실제로 제거된 뒤의 정리 작업이므로
1.0.0 마일스톤으로 옮길 것을 제안했다. **0.18.0을 막지 않는다.**

**#87 Core 릴리스 사전 검사는 이미 PASS다.** `scripts/dev/release-check.sh core 0.18.0` →
버전·릴리스 노트·Conan/vcpkg SHA 전부 PASS. `hotpath_gate`도 release-gate 빌드에서 PASS.
남은 것은 태그·dispatch(외부 공개 행위)와 바인딩 4언어 릴리스(머신 B)다.

### G5 종료 (2026-09-11)

| 언어 | F1 | F2·F2-a | 결과 |
|---|---|---|---|
| Java #97 | PR #125 | **PR #169** | 닫음 |
| .NET #98 | PR #155 | **PR #170** | 닫음 |
| Node #99 | PR #135 | **제공하지 않기로 결정** | 닫음 |
| C++ #100 | PR #134 | job 진행 중 | — |

**Node 결정(F2-b, 사용자 2026-09-11).** Node는 단일 JS 스레드라 완료를 나르는 실행 문맥이
호출 thread와 같다. request 대상이 같은 process의 handler면 **호출자가 기다리는 응답을 호출자가
만들어야 해 교착한다.** 대상이 로컬인지 원격인지 제출 시점에 늘 알 수 없어 "로컬일 때만"이라는
규칙도 세울 수 없다. **지킬 수 없는 약속을 표면에 두지 않는다.**
스펙 `01-submit-and-completion.{ko,en}.md` **새 §4.1**이 이유를 소유한다.
binding Node의 `submit_sync()`는 그대로다 — binding은 framework runtime의 완료 배달에 의존하지 않는다.

**`submit_sync`는 interface의 `default` 메서드로 정했다.** job이 Java에서 abstract로 두어 같은
보일러플레이트가 구현 7곳에 복사됐고 Kotlin 테스트 더블 3개가 컴파일되지 않아 전체 빌드가
깨져 있었다. `default`로 올려 규칙을 한 곳에 뒀고 **스펙도 같은 이유로 고쳤다**(G1에서 쓴
`abstract`는 구현이 없던 시점의 추정이었다). .NET은 이미 default interface method였다.

F2-a 판정 술어는 **언어마다 하나**이며 기존 표지를 합쳐 쓴다 — application job context(handler turn)·
Spot activation(Spot turn)·state lane. **검사는 제출보다 먼저** 한다. call의 단발 gate를 먼저
claim 하면 거절된 호출이 뒤이은 정상 호출에 보인다.

### 2026-09-11 오후 판정 (계속)

| Issue | 판정 |
|---|---|
| **#45** | Java·C++ 병합(PR #167). Node는 main이 이미 최종 Buffer 직접 기록, .NET은 #48의 HeaderPlan이 같은 일을 한다. Java encode heap 1,560→96 B/op, C++ decode new 42→13회/op. **wire byte 동일** golden 테스트 추가 |
| **#111** | 병합(PR #168). serial 1024 **471→730 ops/s (+55%)**, 평균 2.11→1.36 ms, ELU 0.744→0.234, 오류·abandoned 0. 1 ms 타이머가 실제로 사라졌다 |
| **#82** | 닫음 — 재현 안 됨. 양방향 10/10에서 `kind=rejected|origin=application` 보존. 그 경로에 이미 교차언어 단언이 있다(`node_dotnet_smoke.js:531`) |
| **#154** | **내 이슈 본문이 오진이었다.** `503/errno 93`은 의도적으로 `invalid://startup-failure`를 넘기는 **다른(통과하는)** 테스트의 로그였다. 실제 원인은 테스트 결함 2건이며 `d6ec19765e`가 최초 불일치 커밋으로 **#98보다 앞선다.** Actor 기대값은 스펙 §8.1대로 `Unavailable`이 맞다 — 승인 후 구현 중 |
| **#60** | 9회(현재 6 + 실패 당시 커밋 3) 재현 안 됨. **관측을 먼저 붙였다** — `TestHostMessageFlowListener`가 `zlink.message_flow` 외를 버리고 있었고 버려지던 것이 원인 규명에 필요한 부분이었다 |
| **#164/#165** | main이 4일간 red. Python perf 러너 테스트 2건이 정책 정합 이전 값을 단언. **이 테스트를 돌리는 CI가 없었다** |
| **#166** | 새로 엶 — `ZLinkCompositeRelocationBarrierTest`가 CI 러너(2-core)에서만 실패. 로컬 20-core 12/12, `taskset -c 0,1` 10/10 통과. PR #118을 막는다 |
| **#171** | 새로 엶 — .NET Unit suite 간헐 실패 2종(LogicalMulticast admission `Ok`/`Backpressured`, StatefulService handover 0/101). **`Ok` 대 `Backpressured`는 타이밍이 아니라 흐름 제어 상태 단언이다** |
| **#143** | 순서 결정 — C++(#49)만 CLOSED라 지금 가능. .NET(#48)·Node(#50)은 그 뒤 |
| **#15** | 선행 조건 미도래(`ZLINK_CTX_OPT_BLOCKY` 잔존, binding이 `MaxMessageSize` 노출). 1.0.0으로 옮기자고 제안 |
| **#47** | **#151 Java 투입(2026-09-11).** #47 판정에서 드러난 것이 흐름 제어 이완이고 #151이 바로 그 구조를 바꾼다. #151 브리프에 **깊이 상한을 증명하는 회귀 테스트**를 완료 조건으로 넣었다 |

### 플랫폼 차단 — 사용자 결정 2026-09-11: **CI에 진단을 붙이고 0.19.0으로 이월**

마일스톤 **0.19.0을 새로 만들었고**(`#3`) #17·#23·#77을 옮겼다. #15는 1.0으로 옮겼다.
**#18은 재현되지 않아 닫았다.**

| Issue | 플랫폼 | 근거 |
|---|---|---|
| ~~#18~~ | Windows | **닫음.** win-x64/node22가 최근 4회 연속 success, 8~10분에 끝난다(한도 30분). Chromium 단계 자체가 green |
| #17 | macOS | **증상이 바뀌었다.** hang은 없다(12분 정상 종료). 대신 osx-arm64에서만 `MalformedPushedControl_ReconnectsAndReadmits`(net8)와 `DirectNoBindReply_BackpressuredSubmissionIsNotTerminalRefusal`(net10)이 깨진다. 최근 14 run 중 7 실패, 같은 run에서 linux·windows는 전부 통과 |
| #23 | Windows | framework C++ Windows CI가 아예 없다(#117이 그걸 만든다) |
| #77 | macOS ARM | 재현됨. 실패 테스트가 run마다 다르다 — `channel-client.test.js` 80/81(RouteMesh ready 10초 timeout), `backend-contract.test.js`의 MeshNode 채널 record dispatch(`2 !== 0`) |

**붙인 진단**: `framework-dotnet.yml`에 실패 시 TRX artifact 업로드(PR #177). TRX에는 테스트별
stdout과 stack이 있어 raw 로그보다 좁히기 쉽고, job 만료 뒤에도 남는다.
`framework-node.yml`에도 게이트 출력을 `tee`로 남기려 했으나 **되돌렸다** — 그 단계가 Windows에서
pwsh로 도는데 `shell: bash`로 바꾸는 것은 성공 경로의 실행 셸을 바꾸는 일이라 이득보다 위험이 크다.
#77에 필요한 것은 로그 보존이 아니라 **테스트 쪽 message-flow 추적**이며 0.19.0에서 한다.

### main이 red였다 — 내 머지 때문 (2026-09-11)

**`framework-node.yml`의 node20 job이 전 플랫폼에서 실패하고 있었다.**

```
error: 'Promise.withResolvers is not a function'
  framework/languages/node/test/contract/backend-contract.test.js:199,240,241
```

`Promise.withResolvers`는 **Node 22에서 들어왔는데 Node 20이 필수 런타임**이다
(`framework/languages/node/scripts/verify_node_abi_matrix.js:16` → `['20', '22']`).
`4a233e576d`(#99 G5 Node, **내가 머지한 PR #135**)에서 들어왔고 node22 job은 통과하므로 보이지 않았다.

PR #176으로 로컬 `deferred()` helper로 바꿨고 **node20이 5개 플랫폼 전부 green**이 됐다.
같은 종류(Node 22+ 전용 API: `Array.fromAsync`·`Object.groupBy`·`toSorted`·`toSpliced`·
`Array.prototype.with`)를 전체 훑었고 **다른 위반은 없다.**

**교훈: 언어 런타임 최소 버전은 테스트 코드에도 적용된다.** 상위 버전에서만 돌려보면 안 보인다.

### #16 완료 — 그리고 CI가 만들자마자 결함 4건을 찾았다

PR #118 머지. `pull_request`에서 Core ctest·binding smoke·framework Java를 검증한다.
**셋 다 main에 이미 있던 것이고 돌리는 CI가 없어서 보이지 않았다.**

| 찾은 것 | 처리 |
|---|---|
| stream 테스트가 러너에서만 hang — 완료 판정이 수신 개수였다 | #120 → 프레임 byte 기준 |
| Python perf 러너 단언 2건이 정책 정합 이전 값 — **4일간 red** | #164 → PR #165 |
| relocation barrier 테스트가 2-core에서만 실패 — future 완료를 turn 종료로 착각 | #166 → PR #175 |
| Python binding smoke가 Core prefix 미전달 | 워크플로에서 `sync-local-core-libs.sh` 배선 |

**남은 구멍: `integrationTest`를 CI가 돌리지 않는다.** G4 이후 `.submit().reply()`로 바뀌지 않은
호출부가 거기 남아 있다가 PR #169에서야 드러났다. 같은 종류다. 후속으로 추가한다.

### #151이 지금 캠페인의 중심이다 (2026-09-11)

**G4의 이득을 framework가 하나도 쓰지 않고 있다.** G5는 의미 보존이 목표였으므로
`send`는 `.admitted`, `request`는 `.reply`를 **항상** 기다리게 적응시켰다. `result`를 보지 않는다.

```
result == OK            → 이미 로컬 큐에 들어갔다. 기다릴 것이 없다.
result == BACKPRESSURED → 바인딩이 payload를 보관하고 WRITABLE에서 재제출한다. 이때만 기다린다.
```

바인딩 perf 루프는 이미 이 구조다(#96). framework만 뒤처져 있다.

**분담 확정 (사용자 결정 2026-09-11): Java·.NET은 A, Node·C++는 B.**

| 언어 | 담당 | 상태 |
|---|---|---|
| Java | **A** | 2차 job 진행 중. **36셀 before/after 측정 통과** — send-saturation before 73,084~77,556 / after 73,700~77,195, request-backpressure p99 양쪽 0.44~0.60 ms, `peak_in_flight`(bp-4096) before 25/58/63 · after 67/28/37. **1차의 −32%도 #47의 깊이 폭증(1,101)도 없다** |
| .NET | **A** | job 진행 중. 기준선 측정 완료 |
| Node | **B** | **완료·머지**(`af5df22136`). `node-raw-binding-port.ts` 두 send가 `result==Backpressured`일 때만 admitted await. 깊이-상한 테스트(OK 1024회 깊이≤1) 추가, framework 1692 테스트 통과. same-machine A/B(perf 큐): send-saturation 1024 +1.20%, 4096 −1.11% = **무회귀**(Java −24~32% 미재현, 불변 Promise). |
| C++ | **B** | **완료·머지**(`bb264d7517`). `raw_dealer_port.cpp`·`raw_route_port.cpp` OK면 co_return(공유 completion source 제거). 깊이-상한 테스트(깊이==0), ContractTests·-Wall 클린. A/B(raw 불변식 completed==received 검증): 1024 +1.58%, 4096 +1.56% = **무회귀**. grpc/protobuf 1.51.1 툴체인 non-sudo 구성으로 측정(bench rejection-counter gap #158은 우회, 미수정). |

B에게 넘긴 1차 기각 맥락 네 가지는 **Issue #151 코멘트**에 적었다 —
① 공유 객체가 가변이면 안 된다(Node `Promise.resolve()`는 불변이라 안전, C++은 확인 필요)
② send-saturation 처리량을 반드시 잰다
③ `peak_in_flight`를 p99와 함께 싣는다
④ Node의 수정 지점과 바인딩 쪽 구조는 이미 올바르다는 것

**Java 1차 기각 사유 두 가지 — 다른 언어도 같은 것을 본다.**

**① send-saturation 처리량이 떨어졌다.** 1024에서 80,223 → 54,390 (**−32%**), 4096에서
78,929 → 60,048 (**−24%**). 덜 기다리는데 느려졌다. 가장 유력한 가설은 **이미 완료된 stage를
돌려주면 `thenXxx` continuation이 호출 thread에서 인라인으로 돌아**, 전에 completion owner
thread가 받아주던 downstream 작업까지 송신 루프가 떠안는다는 것이다. 파이프라이닝이 사라진다.

**② 공유 *가변* future.**
```java
private static final CompletionStage<Void> ADMITTED = CompletableFuture.completedFuture(null);
```
`CompletableFuture`는 가변이고 `ZLinkActorBoundSessionSender.java:124`에
`submission.toCompletableFuture().cancel(true)`가 있다. **한 번만 취소되면 프로세스 전체의
이후 모든 `OK` send가 영구히 취소된 상태를 돌려받는다.**

Node 바인딩은 같은 자리에서 `Promise.resolve()`를 공유하는데 **Promise는 불변이라 안전하다**
(`bindings/node/src/zlink/runtime/messaging/completion_owner.ts:70,307`). Java·.NET은 그렇지 않다.
**.NET을 리뷰할 때 `ValueTask`/`Task` 공유 인스턴스를 먼저 본다.**

### Node send/backpressure 구조 (참고 — #151 Node 설계 전에 읽을 것)

바인딩이 전부 소유하고 framework는 `await`만 한다.

1. **제출은 항상 `DONTWAIT` 한 번.** `completion_owner.ts:268` `socketSubmitSend(..., DONTWAIT, token)`.
   절대 블로킹하지 않는다.
2. **`OK`** → `RESOLVED_SEND`(공유 `Promise.resolve()`). `completionId != 0`이면 `InternalError`로 던진다.
3. **`Backpressured`** → 그때서야 `snapshotRetryPayload(payload)`로 복사한다(Core는 SEND payload를
   보관하지 않는다). `EAGAIN`이 아니거나 `completionId == 0`이면 던진다. `CompletionEntry`를 만들고
   `ensureRuntimeWatch()`로 **이때 비로소** mailbox fd 감시를 켠다.
4. **재제출은 completion drain이 한다** — `captureWritable()`이 검증 후 `writableRetries`에 넣고,
   drain 루프가 **completion 큐를 `NO_DATA`까지 비운 뒤** 재제출한다. 재제출이 곧바로 새 WRITABLE을
   밀어넣기 때문이다.
5. **framework는 `result`를 보지 않는다** — `node-raw-binding-port.ts:247,300`이 항상
   `.submit().admitted`를 await 한다. 이것이 Node 몫의 #151이다.

### #158 2차 리뷰 — 계수는 맞고 측정 조건이 틀렸다

`171,243 completed = 118,070 received + 53,173 rejected` 대사와 107,374건 삼중 일치는 훌륭하다.
그런데 벤치 타깃이 `set_min_level(fw::log_level_t::debug)`로 돌게 됐다
(`bench_framework_cpp_server.cpp:62`, 변경 전에는 없던 줄). **이 벤치가 #7의 기준이다** —
그대로 머지하면 앞으로의 모든 C++ framework 숫자가 debug 로깅 비용을 안고 나온다.
`stats_http_server_t`/`snapshot("bench")` 경로로 읽도록 되돌려보냈다.

C++ metric이 debug에서만 발행되는 것(`host_capacity_runtime.hpp:150`이 같다)은 **기존 관례**이며
job 잘못이 아니다. **#173**으로 분리했다 — .NET은 `Meter` counter라 항상 발행한다.

### DI scope — 내 요약이 스펙보다 강했다 (사용자 지적 2026-09-11)

#48 제목과 항목 7에 **"DI scope를 제거한다"**고 썼는데 **틀린 표현이다.**
메시지마다 scope를 만드는 것은 ASP.NET·NestJS의 정상 패턴이고 **스펙도 그 의미를 유지하라고 한다.**

`01-execution/08-messaging-hot-path.ko.md` §4.3:
> **handler instance 조회와 DI scope는 W1~W3 안에서 상수 시간이다.**
> **scoped handler의 의미는 유지하되**, scope 생성과 조회가 record마다 컨테이너를 탐색하지 않도록
> **활성화 경로를 캐시한다.**

**요구는 "없애기"가 아니라 "상수 시간으로 만들기"다.** 제목을 고쳤다 →
*"...record별 task를 제거하고 **DI 활성화를 상수 시간으로 만든다**"*.

#### 4언어 현황 (감독 확인)

| 언어 | record마다 scope | 활성화 | lane 건너감 | 판정 |
|---|---|---|---|---|
| **.NET** | `CreateAsyncScope()` (`ZLinkHandlerDispatcher.cs:59`) | **reflection** | **예** | **문제** |
| **C++** | 없음 — 등록 시 lambda가 `services.get_required<TOwner>()`를 캡처(`handler_registry.hpp:99-103`) | 상수 시간 | 아니오 | 정상 |
| **Java** | core dispatch에 없음. Spring 통합은 `spring-boot-starter`로 분리 | — | — | 정상 |
| **Node** | core dispatch에 없음. NestJS 통합은 `packages/nestjs`로 분리 | — | — | 정상 |

**.NET 고유 문제다.** 그리고 비싼 것은 scope 생성이 아니다.

```csharp
// ZLinkScopedHandlerInstanceOwner.cs:21-35
public object Resolve(Type handlerType)
{
    return AwaitStateLane(_lane.RunAsync(() =>          // ← lane 왕복 + 호출 thread blocking
    {
        if (_fallbackInstances.TryGetValue(handlerType, out var existing)) return existing;
        var created = ActivatorUtilities.CreateInstance(Services, handlerType);   // ← reflection
        ...
```

세 가지가 겹친다 — **lane 왕복**(#48 항목 1의 "왕복 14회" 중 하나), **reflection**, 그리고
**캐시가 한 번도 적중하지 않는다**(`ZLinkHandlerDispatcher.cs:60`이 scope마다 새 owner를 만들어
record마다 빈 상태로 시작).

**C++이 참고다** — 등록 시점에 `service_provider_t&`를 받는 invoker lambda를 만들어 캡처해 두고
record마다는 실행만 한다. 스펙이 말하는 "활성화 경로를 캐시한다"가 그것이다.

**#5·#6·#7 진단 브리프에 쓸 문장:** "DI scope를 제거한다"가 아니라
**"활성화를 상수 시간으로 만들고 DI 해석이 state lane을 건너가지 않게 한다"**.
전자로 쓰면 job이 scoped 의미를 없애는 방향으로 간다.

### C++ 측정 경로가 열렸다 (2026-09-11) — 네 가지가 막고 있었다

**`ctest --preset linux-ninja-release -L 'framework-unit|framework-contract'` → 65/65, 실패 0.**
C++이 처음으로 완전히 깨끗하다. 오늘 이전에는 최대 55/65였다.

| 막고 있던 것 | 해결 |
|---|---|
| **#202** protobuf 33.x에서 codec이 컴파일 안 됨 → Release 8개 대상 "Not Run" | PR #214. `GetTypeName()` 반환형이 `const std::string&`→`absl::string_view`로 바뀐 것. 지원 범위 3.21.12~33.x를 `backend-dependency-policy` §8에 명시(사용자 승인) |
| **#182** m6b가 부하에서 깨져 판정을 **세 번** 오염 | PR #211. 실시간 timer 단언 → 이벤트 소비 관측. **2 CPU + 부하 3에서 20/20** |
| **#173-A** metric이 debug 로그에서만 발행 → 읽으려면 측정이 **3.14% 느려짐** | PR #215. log 게이트 3곳 제거. 기본 레벨에서 `246,312 = 246,312 + 0` rc=0 |
| **#158** 벤치가 53% 유실을 통과시킴 | PR #183 + #215로 닫힘 |

**#7(C++ 0.90) 판정을 이제 깨끗한 조건에서 할 수 있다.**

### #77 — macOS 전용이 아니었다. Node binding의 mailbox FD edge 유실

0.19.0으로 이월했다가 **이 리눅스 기계에서 재현**해 되돌렸다(전체 suite 안에서 실패, 단독 24회 중 1회).

**원인(Core 소스로 확정): `send`·`recv`도 mailbox FD edge를 소진한다.**
`setReadableHandler`의 watch가 그 edge에 의존하므로 **send 한 번이 대기 중인 수신 readiness를 지웠다.**

`102`의 정체도 밝혔다 — Framework `RequestResult.NotFound`이고 Channel 선택 실패가 즉시 `102/0`
completion을 만든다. "peer가 ready가 되지 않았다"와 일관된다.

**왜 지금 드러났나:** `setReadableHandler`는 PR #168(#111)에서 들어갔다. 그 전에는 이 watch가
send backpressure 재제출에만 쓰였고(`byToken.size !== 0` 조건), #111이 수신 readiness까지 같은
watch로 몰았다. **밑에 있던 mailbox edge 의미가 그때 표면화됐다.**

결과: PR #216. `npm test` **30/30회 통과, `102` 실패 0회**(고치기 전 12회 중 4회).

**후속 spec gap: `ZLINK_OPT_FD`의 edge 소진 의미가 Core 스펙에 없다**
(`core/doc/spec/core/socket/README.ko.md:353-354`에 이름과 타입만). 어떤 연산이 edge를 소진하는지
규정이 없어 아무도 몰랐다. 절차(문안 → codex 리뷰 → 사용자 승인 → 전 언어)를 밟을 대상이다.

### 스펙 변경 절차 (사용자 지시 2026-09-11)

> 스펙 상세화가 필요한건 스펙 상세화를 하고 codex 리뷰하고, 나에게 승인 받고,
> 모든 framework에 동일하게 적용해야해

**오늘 두 건을 이 절차로 처리했고, 두 번 다 리뷰가 감독 초안의 오류를 잡았다.**

| 스펙 | 리뷰가 잡은 것 |
|---|---|
| **제출 stage 격리**(PR #185, `async-coroutine-policy` `#submission-stage-isolation`) | "한 번의 `cancel()`이 오염시킨다"가 **틀렸다** — 성공 완료된 `CompletableFuture`의 `cancel(true)`는 `false`를 반환하고 상태를 안 바꾼다. 실제 수단은 `obtrudeException()`. Go는 공개 채널이 없고, Rust의 move는 내부 `Arc` 공유의 부재를 뜻하지 않으며, C++의 `shared_ptr`는 금지 대상이 아니다(제약할 것은 `_consumed` 공유). **현재 7언어에 위반 없음 — 예방 규범이다.** |
| **metric 기록의 log 독립성**(PR #208, `02-runtime-metrics` §2.1) | "구독자가 있으면"이 **4언어 어디도 그 개념이 아니다.** "구독 이전 이력 보존"은 .NET `Enabled`·Java NOOP sink·Node 위임과 충돌. "C++ 안에서도 술어가 둘"은 **틀렸다**(`enabled()`는 wrapper). tracing §4가 이미 일부를 정하고 있었다. **그리고 전수 조사로 C++ 8개·Java 6개 계기 누락을 찾았다.** |

**교훈: 감독 단독 문안은 사실 오류를 담는다.** 7언어를 다 열어보지 않고 표를 쓰면 추측이 섞인다.

### 측정 방법론 — 3-run으로 판정하지 마라 (2026-09-11, 두 번 당하고 배움)

**같은 실수를 Java #151과 .NET #151에서 두 번 했다.**

| | 1차 판정 (3-run) | 2차 재측정 | 실제 |
|---|---|---|---|
| Java #151 | send −24~32% → **기각** | +0.34% / +2.90% | **회귀 없음.** 재현 안 됨 |
| .NET #151 | 깊이 +9%, 처리량 −2% → **되돌려보냄** | 처리량 +8%·+7.8%·+4.2%, request p99 **−34%**, mean in-flight **−22%** | **전부 개선** |

**두 경우 모두 "회귀"가 측정 잡음이었다.** 그리고 두 경우 모두 **내 검증 빌드가 같은 기계에서 돌고 있었다** —
Java 1차 때 load가 21까지 갔다. 게이트 기준은 10이다.

**규칙:**
1. **판정 측정은 최소 5회, 교대(`start B then A`)로 한다.** 3회로는 request-backpressure 셀의 폭
   (p99 119~224 ms, peak 6,600~7,900)을 가를 수 없다.
2. **측정 중에는 내 검증 빌드를 돌리지 않는다.** codex job도 비운다.
3. **`peak_in_flight`만 보지 말고 `mean_in_flight`와 함께 본다.** .NET 2차에서 peak 최댓값은
   7,628→8,738로 늘었는데 **mean은 1,578→1,231로 줄고 p99는 −34%**였다. peak 하나로는 반대 결론이 난다.
4. 오류·abandoned가 0이 아닌 셀은 처리량 비교에서 제외한다(이건 지키고 있었다).

**#47 기각도 이 기준으로 다시 봐야 한다** — 당시 after 7 run / before 4 run이었고 교대가 아니었다.

### #12 — 0.90의 분모가 무너져 있었다 (2026-09-11 해결)

Java raw `request-backpressure`가 **0.2~0.4 ops/s**였다. 원인은 벤치 raw 클라이언트가 poller 없이
tight loop로 제출만 하고 완료를 binding runtime pump에 맡긴 것이다. `PerfMultiSocketReqRep` 구조로
바꾸니 **5,929~7,981 ops/s** — 약 2만 배다.

**이 숫자가 0.90 판정의 분모다.** 고치기 전에 쟀다면 framework가 raw보다 수천 배 빨라 보였다.

### 부하가 판정을 오염시킨다 — 내가 당했다

C++ `test_cpp_framework_m6b_runtime`이 #45 브랜치에서 1/5 실패했다. 회귀로 볼 뻔했는데
**같은 조건에서 main은 8회 중 3회 실패했다**(더 나쁘다). 두 실행 모두 **load average 약 21**이었고
게이트 기준은 10이다. 해당 테스트가 실시간 timer에 의존한다.

**규칙: 내 검증 빌드도 job과 같은 부하 예산을 쓴다.** 5개 job이 도는 중에 무거운 빌드를
동시에 돌리면 내가 만든 부하로 내가 판정을 그르친다.

### 환경 — 이 머신에 없던 것 두 가지 (2026-09-11에 찾아 고침)

**1. JDK 25가 없어서 framework Java가 아예 빌드되지 않았다.**
`zlink-jvm-baseline.settings.gradle.kts`가 `zlinkJavaLanguageVersion = 25`를 강제하는데
(`b6b5fcaa97`, 2026-09-09) toolchain auto-download repository가 설정돼 있지 않다. 그래서
컴파일 오류가 아니라 **의존성 해석 단계에서 죽는다.**

```
Cannot find a Java installation ... matching: {languageVersion=25, ...}
Toolchain download repositories have not been configured.
```

**이 증상을 코드 문제로 오진하기 쉽다** — `envelope-45` job이 "binding API에
`toCompletableFuture()`가 없다"고 보고했는데 실제 원인은 이것이었다(binding jar는 class file
version 69 = Java 25로 빌드돼 있고 API는 정상이다). Temurin 25를 `/usr/lib/jvm`에 풀어 해결.

**2. framework C++ 빌드에는 환경변수 두 개가 필요하다.**
```bash
export VCPKG_ROOT=/home/hep7/.cache/zlink/pr/vcpkg
export ZLINK_LOCAL_PACKAGE_ROOT=/home/hep7/project/zlink/.artifacts/wsl
```
`base` preset의 toolchain이 `$env{VCPKG_ROOT}`이고 `CMakeLists.txt:63`이
`$ENV{ZLINK_LOCAL_PACKAGE_ROOT}`로 zlink_cpp prefix를 만든다. worktree의 빈
`.artifacts/wsl`로 fallback 하면 `zlink_cpp 0.18.0`을 못 찾는다.
**설정을 바꿔 재configure 할 때는 `rm -rf build/<preset>` 부터 한다.**

### 이 머신에서 판정할 수 없는 것 (플랫폼 차단)

WSL x64에서는 재현도 검증도 불가능하다. 0.18.0 마감 때 **별도로 분류**한다.

| Issue | 플랫폼 |
|---|---|
| #17 .NET DrainCoordinator hang | macOS |
| #77 Node RouteMesh bootstrap 102 | macOS ARM |
| #18 Node Chromium Stream Connector E2E | Windows CI |
| #23 framework C++ sample preset | Windows |

**머신 B 바인딩 현황 알림 (2026-09-11, B 감독)** — A의 G5 계획용:
- G4 바인딩 머지: cpp #122 · node #123 · python #121 · **go #94(#130)**. → main에 0.18.0 바인딩 소스 반영.
- 진행 중(codex): **dotnet #92**(A의 #98 dotnet G5 전제 — 이제 OPEN 아님, 머지 임박 시 알림) · rust #95 · **G3 #90 Java perf·gRPC Java raw**(main 기준; handoff의 Issue #12 `e1272851bd` 브랜치는 origin에 없어 소멸 → main에서 진행).
- G5(#97~#100)는 A 소유 재확인. B는 G6(perf 재측정)·G7(릴리스 노트)만 남음. 바인딩 4언어 태그는 G4·G3 전부 머지 후.

## 4.7 2026-09-11 판정 요약

### 닫은 이슈 (근거와 함께)

| # | 판정 |
|---|---|
| **#69** Java ROUTE_NOT_CONNECTED 449,143건 | framework가 아니라 **구 Java binding의 multipart 수신 결함**이었다. native 호출 사이에서 virtual thread carrier가 바뀌어 `BUSY → 수신 정체 → liveness 만료` 연쇄. **물리 연결은 내내 READY.** `cde62f8300`의 whole-message 전환이 기제를 제거했고 현재 **413,595건 완료·오류 0**. #75와 같은 뿌리 |
| **#11** Node completion 100건 유실 | G4가 completion owner를 재작성하며 사라졌다. 5회 독립 실행 **각 400/400, 유실 0** |
| **#8** C++ send target 소실 | 재현 안 됨(20초 실행, probe/ACK 유지). 대신 **#158**을 분리 |
| **#140** TrySubmit 제거 | **내가 오독했다.** 스펙 `:446-447`이 그 경로를 "없앤다"고 이미 정했다. framework 쪽 문제였고 #98이 고쳤다 |

### 기각한 산출물

**#47 (Java 복사 제거)** — 처리량은 좋으나(serial +5.8%, saturation +8.4%) **흐름 제어가 느슨해졌다.**

| request-backpressure 4096 | run1 | run2 | run3 |
|---|---:|---:|---:|
| before peak in-flight | 25 | 31 | 25 |
| after peak in-flight | 25 | 23 | **1,101** |
| after p99 (ms) | 0.496 | 0.477 | **138.88** |

처리량은 세 run이 0.7% 안에서 같다 — Little의 법칙(1,101÷11,355≈97 ms)이 138 ms를 설명한다. **느려진 게 아니라 큐가 깊어졌다.** 누적 after 7 run 중 3회, before 4 run 중 0회. `#133` 덕에 `client_error_summary = []`로 오류가 아님을 확인했다.

**#151(G4 성능 활용)이 같은 구조를 통째로 바꾸므로 그것을 먼저 넣고 재측정한다.** 지금 고치면 곧 다시 바뀔 코드를 고치게 된다.

### 새로 연 이슈

| # | 내용 |
|---|---|
| **#159** | **hotpath_gate가 4개 cell에서 23~32% 초과 — 0.18.0 릴리스 차단.** 허용치는 ±5%. 4개가 비슷한 비율로 함께 올라 공통 경로 한 곳으로 보인다. **framework 캠페인의 분모도 흔들린다** |
| **#158** | C++ send-saturation에서 도착한 330,164건 중 **173,549건(53%)을 owner FIFO가 버린다.** 전송 실패가 아니라 도착 후 폐기 |
| **#151** | G4 결과 객체의 성능 활용 — `OK`면 admission 대기 없음. **아직 아무도 안 썼다** |
| **#153** | 같은 버전으로 다시 만들면 소비자 캐시가 갱신되지 않는다(4언어가 각각 다르게 깨짐) |
| **#154** | .NET Redis 테스트 2건 — A/B가 #153 때문에 불가능 |
| **#143** | .NET·C++·Node의 turn 수 고정 테스트(#108의 확장) |
| **#148**·**#137** | Java·C++ 벤치가 컴파일되지 않던 것(둘 다 머지) |

### build_all.sh 사용 시 주의
worktree에서 돌리면 `.artifacts/wsl`이 없어 C++가 `zlink_cpp`를 못 찾는다. **`ZLINK_LOCAL_PACKAGE_ROOT=/home/hep7/project/zlink/.artifacts/wsl`를 함께 준다.**

## 5. 착수 순서 권고

1. **G5 나머지** — Node(#99, 진행 중) → C++(#100, `cpp-submit-r3` 머지 뒤) → .NET(#98, #92 머지 뒤).
   이게 없으면 아무것도 머지되지 않는다.
2. 리뷰 대기 산출물 4건 머지.
3. **G4 성능 활용**을 언어별로 — `OK`면 연속 제출, `BACKPRESSURED`만 대기. **#47 재판정은 이 뒤에 한다.**
4. #111 스펙 문안(감독) → 바인딩 공개 API → framework ingress 전환. **Node는 이것 없이 0.90 불가.**
5. 0.90 판정 측정 — 조용한 기계에서 3-run.

## 6. B와의 경계 (갱신)

- **G5(#97~#100)는 이제 A가 한다**(사용자 승인). B는 G4 나머지(#92 .NET, #94 go, #95 rust)와 G6·G7.
- A는 여전히 `bindings/**` 런타임을 바꾸지 않는다. 예외: #111이 승인되면 Node 바인딩 공개 API 추가.
- `framework/bench/grpc/**`는 A가 소유하나 **G4가 raw 드라이버를 이미 바꿨다**. 병합 시 대조한다.

## 6.5 반복해서 물린 환경 함정 (같은 실수를 다시 하지 않기 위해)

G4 이후 **패키지 신선도** 문제가 연달아 작업을 막았다. 전부 코드 문제가 아니었다.

| 증상 | 진짜 원인 | 대응 |
|---|---|---|
| C++ G5 job이 "빌드 실패"로 종료 | 공유 `.artifacts/wsl/install/zlink-cpp/0.18.0`이 **G4 이전 헤더** | `build-wsl.sh` 재실행 |
| .NET에서 `'Task' does not contain 'Admitted'` 24건 | **`~/.nuget/packages/zlink/0.18.0`이 같은 버전이라 갱신되지 않음** | `rm -rf ~/.nuget/packages/zlink/0.18.0` |
| Node에서 `Property 'admitted' does not exist` | worktree가 자체 `.artifacts/wsl`을 갖고 옛 tgz 사용 | 공유로 symlink + tarball 재설치 |
| bench C++ `request_submission_t is not a member` | job-local 패키지(0.17.6)에 고정 | 캐시 지우고 `ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT`로 재설정 |
| `tooling_contract` 실패 | smoke가 **Core prefix를 전달하지 않음** | `CMAKE_PREFIX_PATH` 환경변수로 전달 |

**`hotpath_gate`는 반드시 `scripts/build-core.sh release-gate`(LTO ON)로 빌드한 트리에서 돌린다.**
기준값이 LTO ON에서 만들어졌다. LTO OFF로 돌리면 4~5개 cell이 23~32% 초과로 나오고, 그것은
회귀가 아니라 **조건 차이**다. 2026-09-11에 이 한 줄을 몰라 "0.18.0 릴리스 차단"으로 잘못
보고했다(#159, 정정 후 닫음).

**규칙: 바인딩이 바뀐 뒤에는 `build-wsl.sh`를 돌리고, NuGet은 캐시까지 지운다.** 버전이 같으면 아무것도 갱신되지 않는다.

### 벤치는 CI에 넣지 않는다 (사용자 결정 2026-09-11)

`framework/bench/**`는 **필요할 때만 구동한다.** CI paths 필터에 넣지 않는다.

대신 `framework/bench/grpc/build_all.sh`가 그 자리를 대신한다 — 스크립트 주석 그대로
*"the local framework gate calls this so a runtime/API change that breaks a bench is caught
before a measurement window"*. **이번 세션의 사고는 이걸 안 돌린 탓이다.**

**측정 전 절차 (반드시)**

```bash
# 1) 바인딩·Core가 바뀌었으면 로컬 패키지부터
bash scripts/local-package/build-wsl.sh
rm -rf ~/.nuget/packages/zlink/<VERSION>      # 같은 버전이면 캐시가 안 바뀐다

# 2) 벤치가 지금 빌드되는지 먼저 확인 (측정 창을 낭비하지 않는다)
BENCH_LANGS="java" bash framework/bench/grpc/build_all.sh

# 3) 그다음 측정 티켓
bash scripts/perf/perf-ticket.sh submit -p 1 -o supervisor -d '<설명>' -- <명령>
```

2026-09-11에 이 순서를 지키지 않아 #137(C++ 벤치)·#148(Java 벤치)을 **측정하려는 순간에** 발견했다.
둘 다 G4 바인딩 변경 뒤 벤치가 적응되지 않은 것이었다.

## 7. 환경 (이 세션에서 확인·수정)

- **framework Java 로컬 검증**: 소스 빌드 Core + `ZLINK_JAVA_BINDINGS_SOURCE` includeBuild. 1,518 테스트 1분 20초.
  공개 Maven에 없는 binding 버전에서도 된다. `.github/workflows/pr-verify.yml`의 `framework-java` job이 같은 절차다.
- **framework Node 로컬 검증**: `scripts/local-package/http-client/build-wsl.sh node` 뒤 `package.json`의
  `@zlink-systems/zlink`를 `file:` 타르볼로 임시 치환 → `npm install --package-lock-only` → `npm ci`.
  **끝나면 `git checkout -- package.json package-lock.json`.**
- `JAVA_HOME=/home/hep7/.cache/zlink/jdk/temurin-25` — 시스템 기본은 JDK 22라 Gradle이 거절한다.
- perf 티켓 큐는 고정 Core prefix(`.artifacts/perf-queue/core-prefix.env` = 0.17.5)를 쓴다. 스크립트가
  0.18.0으로 덮어쓰면 그쪽이 이긴다.
- `/tmp` 8 GB tmpfs가 job 산출물로 찬다. 세션 시작 시 오래된 `zlink-*` 디렉터리를 지운다.

## 8. worktree

유지: `zlink-47-*`(#47 판정 보류) · `zlink-48-*`(#48b 실행 중) · `zlink-49-*`(#49 리뷰 대기) ·
`zlink-50-*`(#50 리뷰 대기) · `zlink-13-bench-pairing`(#13 리뷰 대기) · `zlink-99-*`(#99 실행 중) ·
`zlink-16-*`(PR #118) · `zlink-10-*`·`zlink-97-*`(Refs PR이라 유지) · `zlink-12-*`(B의 #90 base).
