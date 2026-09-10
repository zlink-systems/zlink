# Handoff — 머신 B: 바인딩 submit 종결자 결과 객체 캠페인 (#88~#101, milestone 0.18.0)

> 작성: 2026-09-10 저녁, 머신 A 감독자. 사용자 결정: 이 캠페인은 **머신 B**가, gRPC 벤치·framework 성능은 **머신 A**가 맡는다.
> 설계 [`../draft/bindings-submit-result-terminal.ko.md`](../draft/bindings-submit-result-terminal.ko.md),
> 적용 plan [`bindings-submit-result-terminal-plan.ko.md`](bindings-submit-result-terminal-plan.ko.md) — **두 문서가 계약이다.** 이 문서는 "무엇을, 어떤 순서로, 어디까지"만 적는다.
> 작업 방식: [`../principal/dev/development-workflow.ko.md`](../principal/dev/development-workflow.ko.md)(Issue → worktree → PR), 코드는 codex job, 스펙·정책·문서는 감독자.

## 1. 30초 요약

- **무엇**: 7언어 binding의 비동기 submit 종결자가 Core 제출 결과를 돌려준다 — `SendSubmission{result, admitted}`,
  `RequestSubmission{result, admitted, reply}`. `result`는 `OK`|`BACKPRESSURED`(제출 시점 스냅샷), 즉시 실패는 예외/에러 유지,
  payload 보관·재제출은 지금처럼 binding 소유("try" API 없음). 종결자 **이름**은 `bindings/doc/spec/async-coroutine-policy.ko.md` §6
  그대로(반환형만 바뀜). 호환성 유지 안 함. binding 버전은 이미 0.18.0(#103)이며 **이 캠페인이 bindings 0.18.0 릴리스 내용**이다.
- **왜**: request `submit()`이 admission과 reply를 stage 하나로 합쳐 producer가 backpressure를 볼 수 없다. perf는 turn당 1건으로
  우회했고(socket 1개면 깊이 1), gRPC 벤치 raw 행이 8k/s에 머물렀다. 근거·측정은 draft §1, `fw-bench-worklog/decisions.ko.md` FB-065.
- **결정된 것(G0, 바꾸지 않는다)**: plan §2 표. .NET `TrySubmit()` 제거, Go는 `Submit(ctx)` 즉시 반환 + `Result()`/`Admitted(ctx)`/`Reply(ctx)`,
  Rust는 boxed future, framework는 F1(공개 terminal에 backpressure 미노출, 내부만)·F2(동기 blocking 종결자 추가)·F2-a(runtime 문맥에서 호출 시
  `InvalidOperation`).
- **Core**: `core/v0.18.0` 태그·릴리스 빌드는 A가 냈다(2026-09-10 21:5x, `build.yml` run 34479303293). Core는 건드리지 않는다.

## 2. 순서와 병렬

| 단계 | Issue | 담당 | 병렬 | 게이트 |
|---|---|---|---|---|
| G1 스펙·정책 문안 | #88 | **감독자(B)** 직접 작성. plan §3 표 전부(ko+en) | G2와 동시 시작 가능(코드와 독립) | 문서 검사 PASS, PR |
| G2 Java 파일럿 | #89 | codex job | G1과 동시 | draft §8-1 contract 2 시나리오, Core unit·contract 통과. **이 커밋이 G4 참조 구현** |
| G3 Java perf·벤치 | #90 | codex job | G2 머지 뒤 | perf clients=1이 깊이 1 탈출(기준 2026-09-10: clients=100 248k/s, clients=1 8.5k/s); gRPC 벤치 raw request-backpressure 3-run 포기·오류 0. **Issue #12 브랜치 `bindings/12-bench-grpc-java-raw-bindings-perf-perfmu`의 `e1272851bd`(perf 구조 1차) 위에서** |
| G4 6언어 | #91 cpp · #92 dotnet · #93 node · #94 go · #95 rust · #96 python | codex job 6개 | G2 머지 뒤 **동시**(codex ≤5 권장, 큐로 6번째) | 언어별 contract·perf 통과. perf 클라이언트 수정은 같은 Issue·PR(plan §5.1 파일 목록) |
| G5 framework 4언어 | #97 java · #98 dotnet · #99 node · #100 cpp | codex job 4개 | 해당 언어 binding 머지 뒤 동시 | framework 테스트·cross-language e2e·F2-a 회귀 테스트 |
| G6·G7 | #101 | 감독자 ticket + job | 전부 머지 뒤 | draft §8 네 항목, 릴리스 노트(breaking), decisions FB 기록 |
| 릴리스 | #87 나머지 | 감독자 | G7 뒤 | `<lang>/v0.18.0` 태그 4언어(`bindings-release.yml`, .NET `release-dotnet.yml`), framework-dotnet·node CI 초록 |

G2 결과가 설계를 바꾸면 G4 브리프 전에 draft를 고치고 A에 알린다(같은 draft를 A의 bench 문서가 인용한다).

## 3. job 브리프에 반드시 넣을 것 (언어별 공통)

1. 계약: draft §3(공통), §4의 그 언어 절, §5(perf 규칙). "가능한 한 7언어 같은 모양"(plan §2 #9).
2. 파일: plan §4의 그 언어 행(공개·내부·테스트), §5.1의 perf 파일 목록.
3. 참조 구현: #89 커밋 해시(G4부터).
4. 검증: contract test 두 시나리오(즉시 admission→`result==OK`·`admitted` 완료 상태 / HWM→`BACKPRESSURED`·WRITABLE 뒤 `admitted`·request는 그 뒤 `reply`),
   동기 종결자·publish·reply 불변, 그 언어 perf multi 전 패턴 × tcp × {64,1024,4096,65536} runs=3 전후 비교(사이즈별 −5% 이내).
   측정은 `scripts/perf/perf-ticket.sh submit -p 2 -- …`로만.
5. 금지: `doc/**`·`bindings/doc/**`·`.github/**`·`scripts/local-package/**` 수정, `pkill -f`/`pgrep -f`, 실행 중 스크립트 편집,
   측정 조건 완화(HWM·warmup·timeout), 벤치에 application 상한 추가.
6. 보고: `.artifacts/codex/<job>/summary.md`(변경 파일:줄, contract 결과, perf 전후 표, 남은 제한) + `pr-body.md`(`Closes #N`, `## 검증` 절 필수 — `work.sh pr`가 검사한다).

## 4. A와의 경계 (충돌 방지)

| | B가 바꾸는 곳 | A가 바꾸는 곳 |
|---|---|---|
| bindings | `bindings/**` 전부(공개 종결자·owner·contract·perf) | 없음 |
| framework | **binding 호출 줄**(`.submit()`→`.reply()`/`.admitted()`), messaging call의 **동기 종결자 추가**, F2-a 검사, 3단계 backpressure가 `result`를 쓰는 부분 | runtime 내부(lane·pump·registry·envelope·dispatch·worker), 벤치 gRPC·framework 행 |
| bench | `framework/bench/grpc/*` **raw 드라이버**(#90·#91~#93) | gRPC·framework 드라이버, 러너, 비교 도구, 규격 문서 |
| 문서 | `bindings/doc/**`, `doc/perf/PERF_*`, framework 01장 §2·§4·§5·§15·§16, 언어별 interface, 08장 E2 문구 | `framework/bench/grpc/README.*`, 08장 나머지, `fw-bench-worklog` |

- 겹치는 파일(예 `framework/languages/java/.../ZLinkJavaRawServicePort.java`)은 B의 변경이 호출 줄 치환이라 rebase로 풀린다. A의 #85(ToNode lane 1회,
  `ZLinkChannelSocketRegistry`·`ZLinkChannelRouteCalls`)는 A가 먼저 PR을 내니 #97은 그 위에서 rebase한다.
- 같은 언어의 framework 파일을 두 머신이 같은 시간에 열지 않도록, G5(#97~#100) 착수 전에 A의 열린 PR을 확인한다(`gh pr list`).

## 5. 환경

- JDK 25: `JAVA_HOME=/home/hep7/.cache/zlink/jdk/temurin-25`(B에도 같은 위치로 준비). 기본 JDK 22면 Java perf가 거부한다(#46).
- Core 0.18.0 prefix: 릴리스 자산이 올라오면 `scripts/local-package/core/fetch-release.sh`로 `~/.cache/zlink/core/0.18.0/<플랫폼>`. 자산 전이면
  `bash scripts/build-core.sh release`(core/build)에서 prefix를 만든다(`env-local-core-prefix-no-release` 메모리와 같은 방법).
- 공유 Maven·npm 캐시(`.artifacts/wsl`)에 0.18.0 binding이 없으면 framework 테스트가 의존성 해석에서 멈춘다 → 언어별 `scripts/local-package/<lang>/build-wsl.sh`로
  로컬 패키지를 먼저 만든다(G5 전).
- codex 모델 id `gpt-6-astra`(최상위)·`gpt-5.6-sol/terra/luna`. job은 `scripts/dev/job.sh start <이름> --worktree <경로> --brief <파일> --model gpt-6-astra --effort high`.
  3분 감시자(`job.sh watch` 또는 Monitor)로 완료·실패만 받는다. worktree는 `work.sh start --issue N --no-packages` 뒤 `.artifacts/wsl` 심볼릭 링크.
- 벤치 포트 예약: 세션마다 `sysctl net.ipv4.ip_local_reserved_ports=5200-5299,6200-6219`(`env-bench-port-reservation`).

## 6. 완료 보고

- `doc/plan/bindings-submit-result-terminal-plan.ko.md` §9 진행 로그에 단계별 커밋·PR·측정값.
- `doc/plan/fw-bench-worklog/decisions.ko.md`에 FB 항목(캠페인 결과: perf clients=1 전후, gRPC 벤치 raw 전후, 회귀 없음 판정).
- binding 4언어 태그 뒤 A에 알린다(A의 framework 릴리스와 #85 재검증이 그 패키지를 쓴다).

## 7. 세션 진행 현황 (머신 B, 2026-09-11 갱신)

신규 이슈·재할당은 이 절과 A handoff(§4.5)에 함께 적는다(사용자 규칙 2026-09-11).

| 단계 | 상태 |
|---|---|
| G1 #88 스펙·정책 문안 | **완료·머지** PR #124, 보완 #128(per-lang en 쌍)·#129(#126 산문 정합). 옛 종결자 시그니처·산문 0건. |
| G2 #89 Java 파일럿 | **완료·머지** PR #119 (참조 구현). |
| G4 바인딩 | cpp #91(#122)·node #93(#123)·python #96(#121)·**go #94(#130)** 머지. **dotnet #92**·rust #95 codex 진행 중. |
| G3 #90 Java perf·gRPC Java raw | codex 진행 중. **main 기준**(handoff의 Issue #12 `e1272851bd`는 origin에 없음→소멸). 코드만, 측정 G6. |
| G5 #97~#100 framework | **→ 머신 A** (사용자 재할당 2026-09-10). B는 착수 안 함. |
| G3 #90 Java perf·gRPC Java raw | **완료·머지** PR #141. §5 루프 + gRPC raw 드라이버. |
| G6 perf 판정 | **criterion 2·4 PASS (2026-09-11, FB-071).** cpp ccu=1 8.5k→**300k**(깊이 탈출, C ref 275k 동급), ccu=100 회귀 없음(옛 150k→새 157k; 과거 0.17.5 97k·64B 137k 대비 유지·상승). ccu=1>ccu=100 역전은 harness 단일스레드 특성(C control·3언어·Core 버전·과거결과 4중 확증, §5/Core 회귀 아님). criterion 3(gRPC Java 3-run)은 `zlink:0.18.0` 공유 Maven 로컬 패키지(§5 전제) 발행 후속 — 판정 blocker 아님. |
| G7 #101 | 릴리스 노트(breaking)·바인딩 0.18.0 태그 남음. decisions FB-071 기록 완료. |

**신규/재할당 이슈 기록**:
- #126 (신규, area:docs kind:bug) — 바인딩 스펙 산문이 옛 계약 자기모순. B 감독 직접 처리, PR #128·#129로 완료·CLOSED.
- dotnet #92 — G4에서 누락됐다가 발견해 추가 투입(cpp/node/python/go만 먼저 돌림).

**결정·정정**:
- 바인딩 perf 측정은 전부 G6로 분리, perf **코드**는 G3/G4에서 완료(사용자 2026-09-11: 코드는 미리 병렬, 측정은 나중).
- Python 결과 객체는 Java 참조처럼 method 접근(`result()/admitted()/reply()`)으로 통일(draft §4 Python의 field 표기 대신 7언어 일관).
- JDK 25 위치 `/home/hep7hep7/.jdks/jdk-25.0.4.1+1` (gradle 자동 탐지). handoff §5의 `/home/hep7/.cache/zlink/jdk/temurin-25`는 틀림 — "JDK 25 부재"는 오판.
- G4·G5 병행: 바인딩(G4)과 framework(G5) 디렉터리 분리라 동시 진행 가능(계약 확정).
