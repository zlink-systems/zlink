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
| **#98** framework-dotnet G5 | 같음 | #92(PR #136)가 머지되며 깨졌다 | **차단** — Issue #140 결정 필요. 적응 47곳은 worktree에 준비됨 |
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
| **#140** | `TrySubmit` 제거로 `SendFlags.DontWait` 대응 수단 없음 | framework .NET 컴파일 차단. 상위 보상은 AGENTS §3 위반 | **B 결정 대기** |
| — | tooling contract smoke가 Core prefix를 전달하지 않는다 | #100 검증 중 발견 | 미보고(#117에 합칠 것) |
| — | 로컬 패키지 post-G4 재빌드 | 공유 0.18.0 C++ 패키지가 G4 이전 헤더였다 | **완료** |

**G4 성능 활용**(`OK`면 admission 대기 없이 연속 제출)은 아직 **아무에게도 할당되지 않았다.** 언어별 G5가 끝난 뒤 A가 맡는다.

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
