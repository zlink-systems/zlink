# Framework messaging bench 2차 — server-to-server 측정, HTTP trigger, 언어별 문서, gRPC 비교 보고서

작성 2026-09-09. 사용자 요청과 현재 상태 분석, 필요한 작업을 정리한 계획이다. 결정 기록은
`doc/plan/fw-bench-worklog/decisions.ko.md`에 `FB-` 번호로 이어서 쓴다(1차 캠페인 FB-001~FB-044
뒤).

## 0. 요청 정리 (사용자, 2026-09-09)

- `framework/doc/framework/common/bench/with-grpc-local.ko.md`가 어느 정도 작성돼 있으나
  **server to server 메시징 성능 측정이고 트리거는 HTTP call**이라는 측정 방식이 문서에 없다.
  이를 명확히 한다.
- 언어별로 **같은 형식**의 bench 문서를 둔다.
- 언어별 성능 테스트를 수행한다.
- **gRPC 성능 비교 문서**를 작성한다.

### 0.1 이 계획이 확정으로 두는 해석

"server to server, 트리거는 HTTP call"은 공통 perf 규격
(`framework/doc/framework/common/perf/README.ko.md` §4.2 server-driven 부하)의 모델과 같다.
즉 측정 대상은 **server process A가 server process B에 보내는 메시징**이고, HTTP 호출은
**부하를 시작하라는 신호(control)**이며 측정 operation이 아니다. A는 HTTP 요청을 받으면
자기 안의 logical stream으로 B에 요청을 반복하고, 완료·지연을 자기가 집계한다. 이 해석이
틀리면 §11의 결정 항목에서 바로잡는다.

## 1. 현재 상태

### 1.1 규격 문서

`with-grpc-local.ko.md`(481행)·`.en.md`(520행)는 1차 캠페인(`framework-bench-with-grpc-5lang-plan.ko.md`,
2026-09-06~07)이 다섯 언어 중립으로 정리한 규격이다. 비교 대상(`grpc-<lang>` /
`zlink-<lang>` / `zlink-framework-<lang>` + C 기준), 패턴 넷(`request-serial`, `request-window`,
`request-backpressure`, `send-saturation`), ROUTER↔ROUTER 계약, 출력 형식, 판정식(§7.2), 포트
대역까지 갖췄다.

규격이 정한 실행 모델은 **client process 1개가 server process 1개에 직접 부하를 건다**(§3).
사용자가 말한 server-driven·HTTP trigger 모델은 없다. 두 모델은 측정 위치가 다르다.

| 항목 | 현재 규격(client-driven) | 요청 모델(server-driven) |
| --- | --- | --- |
| 부하 생성 위치 | 독립 client process | server A 안의 logical stream |
| 시작 신호 | client가 직접 시작 | 외부 HTTP call → A의 trigger endpoint |
| 측정 구간 | client의 request 제출~reply | A의 outbound call 직전~reply 완료 |
| gRPC 대응 | client stub → server | A의 gRPC client stub → B |
| 결과 수집 | client 출력 + server stats | A·B의 stats endpoint(이미 §9 포트에 있음) |

### 1.2 언어별 구현 상태 (1차 캠페인 결과, `fw-bench-worklog/report-with-grpc-5lang.ko.md`)

| 언어 | grpc | zlink raw | zlink framework | 언어별 문서 | 상태 요약 |
| --- | --- | --- | --- | --- | --- |
| `.NET` | 측정 | 측정 | 측정 | `bench/with-grpc/README.ko.md` 있음 | raw window 깊이 8(단일 thread 제출 한계), framework window p99 174 ms |
| Node | 측정 | window에서 정지 | **미구현**(codec이 bytes 미지원) | 없음 | client socket 정지 최소 재현 `bench/with-grpc/repro/` |
| Java | 측정 | window에서 정지(reply 유실) | 측정(깊이 4.5) | 없음 | 정지 재현 `bench/with-grpc/repro/` |
| Kotlin | 측정 | 측정 | 측정 | 없음 | client만 Kotlin, server는 Java 바이너리 공유; send @1024 G5 미달 |
| C++ | 측정 | 측정(12셀 G5 통과) | **미구현** | 없음 | 6셀 미측정 |
| C 기준 | 측정 | 측정 | 해당 없음 | `bindings/c/bench/BENCH_POLICY.md` | request-window @4096 분모가 세 구간에서 G5 미달 |

90셀 중 78셀 측정, 판정 20개 중 게재 조건을 만족한 것은 2개(모두 기준 0.80 미달). 관리형
런타임 binding 셋(.NET·Node·Java)이 window 100에서 서로 다른 방식으로 무너지는 것이 중심
관측이었다.

### 1.3 이미 있는 자산 중 재사용할 것

- 공용 집계기 `framework/bench/tools/`(테스트 28개). 언어 client의 자체 표는 판정 근거가
  아니다(규격 §7.1). 새 모델에서도 판정은 집계기 출력으로 한다.
- 7축 perf 규격의 HTTP trigger·admin 계약(§4.2, §5.1 role config, §16)과 그것을 구현한 .NET
  canonical runner(`framework/languages/dotnet/perf/`, c016 phase 1). trigger listener, reset
  sequence, 중복 trigger 처리, stats endpoint가 이미 설계·검증돼 있다.
- 측정 티켓 큐(`scripts/perf/perf-ticket.sh`)와 고정 Core prefix(D-BP33·D-BP45). 측정 중
  Core를 다시 빌드하지 않는다.
- 측정 원본 위치: `framework/languages/<lang>/bench/with-grpc/log/<stamp>/`(보고서 부록).

### 1.4 알려진 결함과 차단 요인 (고치지 않으면 새 측정도 같은 자리에서 막힌다)

| 우선 | 항목 | 영향 |
| --- | --- | --- |
| 0 | `zlink-c` 기준선 불안정(request-window @4096 G5 미달 3회) | formula 1 분모. 이 행이 풀리기 전에는 어떤 언어도 판정을 게재하지 못한다 |
| 0 | 관리형 binding의 완료 전달(Node 정지, Java reply 유실, .NET 단일 thread 제출) | window·backpressure 패턴이 S2S 모델에서도 같은 곳에서 무너진다. B 머신의 binding 성능 캠페인이 다루는 영역과 겹친다 — 조정 필요 |
| 0 | framework handler 생성 실패를 수락·폐기하고 성공을 돌려줌 | 결과 정합성 |
| 1 | Node framework codec의 bytes 미지원 | Node framework 행이 비교에 참여 불가 |
| 1 | framework send 경로의 backpressure/drain(.NET drain 16.7 s) | send-saturation 셀 |
| 1 | framework request 깊이 상한(window 100에 실제 4.5~12) | window 셀 |
| 2 | `zlink-framework-cpp` 6셀 미구현 | C++ 3자 표 불완전 |
| 2 | bench가 gate에 없음(.NET bench가 빌드 불가 상태로 커밋된 적 있음) | 재파손 위험 |

## 2. 목표와 산출물

| 산출물 | 위치 | 소유 |
| --- | --- | --- |
| 규격 개정: server-driven 모델·HTTP trigger·S2S 측정 구간 정의 | `framework/doc/framework/common/bench/with-grpc-local.{ko,en}.md` | 감독자 |
| 언어별 bench 문서 5개(같은 절 구성) | `framework/languages/<lang>/bench/with-grpc/README.{ko,en}.md` | 감독자(초안은 job이 보고서로 제출) |
| 언어별 runner 개정(server A: trigger + logical stream, server B: echo) | `framework/languages/<lang>/bench/with-grpc/` | codex job |
| 공용 집계기 확장(S2S 셀 스키마, A/B stats 병합) | `framework/bench/tools/` | codex job |
| 언어별 측정 원본 | `framework/languages/<lang>/bench/with-grpc/log/<stamp>/` | 티켓 큐 |
| gRPC 비교 보고서(공개 문서) | `framework/doc/framework/common/bench/with-grpc-comparison.{ko,en}.md` | 감독자 |
| 결정 기록 | `doc/plan/fw-bench-worklog/decisions.ko.md` (FB-045~) | 감독자 |

비교 보고서는 1차의 `report-with-grpc-5lang.ko.md`(doc/plan, 비공개)와 달리 **공개 문서**다.
그래서 근거 수치·조건·한계를 규격 §7.1대로 남기되, 캠페인 진행 기록은 담지 않는다.

## 3. 측정 모델 (규격 개정안의 핵심)

### 3.1 역할

| 역할 | process | 구현 |
| --- | --- | --- |
| Trigger client | 1 (언어 무관, 공용 runner의 curl 또는 작은 script) | `POST http://127.0.0.1:<A trigger>/bench/start` 로 `{runId, cellId, pattern, payloadBytes, durationMs, warmup}` 전달 |
| Server A (source) | 1 per 구현 | HTTP trigger listener + stats endpoint + B로 향하는 client(gRPC stub / raw ROUTER / framework channel client) |
| Server B (target) | 1 per 구현 | echo(request) 또는 count(send) + stats endpoint |

구현 셋(`grpc-<lang>`, `zlink-<lang>`, `zlink-framework-<lang>`)마다 A·B 한 쌍이다. 같은
언어의 세 쌍은 §9 포트 대역 안에서 offset으로 구분한다(대역에 trigger 포트 1개를 추가하거나
예비 포트 `+8`을 trigger로 쓴다 — §11 결정).

### 3.2 측정 구간과 집계

- 측정 operation은 **A의 outbound call 직전부터 완료(reply 수신 또는 send 완료 통지)까지**다.
  HTTP trigger 왕복은 세지 않는다(7축 규격 §4.2와 동일).
- warmup → measured active → bounded settle 순서는 현재 규격 §3을 그대로 쓴다. settle의 30초
  상한과 오염 셀 규칙도 유지한다.
- A가 셀 결과 JSON을 자기 log 디렉터리에 쓰고 `RESULT` 라인을 stdout에 낸다. B의 stats는
  runner가 settle 뒤 읽어 같은 JSON에 합친다. 집계기는 A·B 두 원본을 셀 하나로 만든다.
- 패턴 넷과 payload 두 크기(1024·4096), window 100, send concurrency 8, ROUTER↔ROUTER 계약,
  29바이트 측정 header, 두 part wire 모양은 그대로다. 바뀌는 것은 부하가 시작되는 위치뿐이다.

### 3.3 gRPC 쪽의 대응

gRPC 구현의 A는 같은 HTTP trigger listener를 갖고, B로 향하는 unary stub을 logical stream
수만큼 돌린다. gRPC server 구성은 언어 기본값을 두고 결과에 기록한다(규격 §8.2). 이렇게
해야 "같은 업무(A→B 요청)를 두 스택으로 구현했을 때의 비용"이라는 규격의 질문이 유지된다.

## 4. 언어별 작업

| 언어 | runner 개정 | 추가로 필요한 것 | 예상 job |
| --- | --- | --- | --- |
| `.NET` | client → server A로 재구성. canonical perf runner의 `ServerSupport`(trigger·admin·stats)를 재사용 | `Zlink.Framework.Perf.ServerSupport`를 bench가 참조할 수 있게 공유 위치로 이동 또는 복제 없이 참조 | sol 1 |
| Node | 같음 | framework 행은 codec bytes 지원이 선행(제품 결함) — 그 전에는 `unsupported`로 기록 | sol 1 (+ codec 별도) |
| Java | 같음 | reply 유실 재현(`repro/`)이 S2S에서도 재현되는지 먼저 확인 | sol 1 |
| Kotlin | client(A) Kotlin, B는 Java 바이너리 공유(1차와 동일 판단) | 없음 | terra 1 |
| C++ | 같음 + `zlink-framework-cpp` 6셀 구현 | framework C++ HTTP hosting(trigger listener)은 framework 자체 기능으로 있음 | astra 1 |
| C 기준 | A/B 재구성 없이 유지(HTTP trigger 없음). formula 1 분모 안정화가 별도 항목 | 기준선 안정화 job | astra 1 |
| 집계기 | S2S 셀 스키마·A/B 병합·언어별 문서 표 생성 | 테스트 유지 | sol 1 |

문서·규격·보고서는 job이 쓰지 않는다. job은 보고서에 "언어별 문서에 들어갈 표와 값"을
제출하고 감독자가 같은 형식으로 옮긴다.

## 5. 언어별 bench 문서의 공통 형식

다섯 문서가 같은 절 번호와 제목을 갖는다. `.NET` README를 이 형식으로 고쳐 기준으로 삼는다.

1. 비교 대상 — 그 언어의 세 구현 이름과 사용하는 API(표)
2. 실행 방법 — runner 명령, 환경 변수 표, 한 패턴만 실행하는 방법
3. 프로세스 구성 — A·B·trigger, 포트(§9 대역), 사용 모듈(규격 §8.1의 그 언어 행)
4. 언어별로 다르게 둔 값 — warmup, gRPC server 구성, 런타임·라이브러리 버전(규격 §8.2)
5. 결과 위치 — `log/<stamp>/`의 파일 구성, 집계기 호출 명령
6. 알려진 제약 — 그 언어에서 `unsupported`인 셀과 이유(제품 결함 링크)

작성 규칙은 `doc/principal/documentation/documentation-principles.ko.md` 원칙 7과 §7.8(검증
가능한 내용)을 따른다. 한/영 쌍을 둔다.

## 6. gRPC 비교 보고서의 구성 (공개 문서)

1. 무엇을 비교하는가 — 규격 §0의 질문, 측정 모델(§3), 언어와 버전
2. 결과 요약 — 언어 × 패턴 × payload의 처리량·p99 표(집계기 출력 그대로), formula 1·2 판정
3. 언어별 3자 표 — 규격 §7.1이 요구하는 동반 정보(warmup, 구성, 깊이, drain)
4. 읽는 방법 — §7.3 언어 간 비교 금지 규칙, `unsupported`의 의미
5. 한계 — 로컬 loopback, 단일 머신, 기준선 불안정 등
6. 원본 위치

1차 보고서의 §8 "계측이 바로잡은 것"·§9 "한계" 중 공개 가치가 있는 것만 옮긴다.

## 7. 측정 절차

- 모든 측정은 티켓 큐로 낸다(`scripts/perf/perf-ticket.sh submit -p 1 -o <owner> -d "<설명>" -- <명령>`).
  한 번에 한 언어. load average 10 미만에서 시작. Core는 `.artifacts/perf-queue/core-prefix.env`의
  고정 prefix(현재 0.17.5)이며 측정 중 재빌드하지 않는다.
- 셀당 3-run, 재현성 조건 G5(스프레드 ≤10%)는 1차 게이트(계획 §6)를 그대로 쓴다.
- 조건(timeout·sleep·window·HWM·client 수)을 완화하지 않는다. 벤치가 잘못 재면 벤치를 고친다.
- 결과와 판정은 집계기 출력만 인용한다.

## 8. 단계

| 단계 | 내용 | 산출 | 담당 |
| --- | --- | --- | --- |
| S0 | §11 결정 확정, 규격 개정(§3 실행 조건·§4 출력·§9 포트·새 §10 S2S 모델), `.NET` README를 §5 형식으로 | 규격 ko/en, .NET 문서 | 감독자 |
| S1 | 집계기 S2S 스키마 + `.NET` runner 개정 + `.NET` 3-run | 원본·표 | codex sol ×2 |
| S2 | Node·Java·Kotlin runner 개정과 3-run(정지·유실 결함은 재현되면 기록하고 멈춘다 — 고치는 것은 별도) | 원본·표 | codex sol/terra |
| S3 | C++ runner 개정 + `zlink-framework-cpp` 구현 + 3-run | 원본·표 | codex astra |
| S4 | `zlink-c` 기준선 안정화(원인 규명 후 최소 수정) | 판정 게재 가능 여부 | codex astra |
| S5 | 언어별 문서 4개 + 비교 보고서 ko/en | 공개 문서 | 감독자 |
| S6 | bench를 로컬 gate(`scripts/gate/framework-gate.sh`)에 빌드만 편입 | 재파손 방지 | 감독자 |

각 job은 원인 하나, 1.5시간 상한, 브리프는 `doc/plan/fw-bench-worklog/briefs/fwb2-<id>.prompt`,
요약은 `fw-bench-worklog/fwb2-<id>-summary.md`. job은 규격·계획·문서를 수정하지 않는다.

## 9. 위험

- B 머신의 binding 성능 캠페인이 관리형 binding 완료 전달 계층을 고치는 중이면, 이 캠페인의
  window·backpressure 셀은 그 결과에 종속된다. 시작 전에 B의 범위와 일정을 확인한다.
- `zlink-c` 분모가 안정되지 않으면 formula 1은 다시 게재 불가다. S4를 S1과 병행한다.
- Node framework 행은 codec 결함이 풀려야 참여한다. 보고서에 `unsupported`로 남기는 것을
  기본으로 두고, codec 수정은 별도 작업으로 뺀다.
- HTTP trigger listener를 언어마다 새로 쓰면 형식이 갈린다. 7축 규격 §5.1의 role config와
  §16의 endpoint 계약을 그대로 가져와 다섯 언어가 같은 요청·응답 JSON을 쓴다.

## 10. 범위 밖

- 운영 환경(mesh, TLS, 다중 노드) 측정.
- 7축 perf 규격의 나머지 축(CS, AC, PS, Spot local/worker). 이 계획은 S2S 축의 gRPC 비교만 다룬다.
- binding 완료 전달 결함의 수정(B 캠페인).

## 11. 결정이 필요한 항목 (사용자)

1. §0.1의 해석이 맞는가 — HTTP call은 부하 시작 신호이고 측정 operation은 A→B 메시징인가.
   (대안: HTTP 요청 하나마다 A→B 요청 하나를 보내고 HTTP 왕복을 재는 모델. 이 경우 HTTP
   서버 비용이 섞여 gRPC/ZLink 비교가 흐려진다.)
2. trigger 포트를 §9 대역의 예비 `+8`로 쓸지, 대역을 `+10`으로 넓힐지.
3. Kotlin을 1차처럼 "A만 Kotlin, B는 Java 공유"로 둘지, B도 Kotlin으로 둘지.
4. Node framework 행을 codec 수정 전까지 `unsupported`로 둘지, codec 수정을 이 캠페인에 포함할지.
5. 1차 보고서(`doc/plan/fw-bench-worklog/report-with-grpc-5lang.ko.md`)의 수치를 새 보고서에
   "client-driven 1차 결과"로 병기할지, 새 모델 결과만 실을지.
6. 시작 시점 — 현재 진행 중인 framework 0.11.0 배포·CI 복구·샘플 구조 작업 뒤인지.
