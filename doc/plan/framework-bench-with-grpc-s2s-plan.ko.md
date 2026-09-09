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

### 0.1 사용자 결정 (2026-09-09, §11에 대한 답)

| 항목 | 결정 |
| --- | --- |
| 측정 모델 | HTTP call은 부하 시작 신호, 측정 대상은 server A → server B 메시징 (§0.2) |
| 포트 | A/B 쌍 구조에 맞게 새로 정한다(§3.4). 기존 대역과 겹치지 않게 언어당 20개 |
| Kotlin | **전체 matrix에서 제외하고 보조 셀 하나만 잰다**(2026-09-09 확정). Kotlin은 Java와 같은 binding·server·codec을 쓰므로 차이는 A 쪽 호출 층뿐이다. 보고서에 그 사실을 명시하고, `grpc-kotlin`(coroutine stub)과 `zlink-framework-kotlin`(suspend 호출)의 `request-window @1024` 한 셀씩만 Java 행 옆에 보조로 싣는다. B는 Java 바이너리 공유 |
| Node framework 행 | codec `bytes` 미지원은 제품(framework Node codec) 결함이다. 수정 전까지 `unsupported`로 기록하고 codec 수정은 별도 작업 |
| 보고서 | 새 공개 보고서를 쓰고 README와 zlink.systems에서 참조한다. 1차 결과는 병기하지 않는다 |
| 버전 | 공개된 framework 0.10.0 + binding 0.17.6으로 시작하고, framework 0.11.0이 나오면 그 버전으로 다시 잰다 |
| 1차 결과의 취급 | 1차(2026-09-07)는 binding 결함(.NET 깊이 8·Node 정지·Java reply 유실)이 window 셀을 무효로 만든 시점의 기록이다. 그 결함은 **Core·binding 0.17.5에서 모두 수정됐다**(사용자 확인). 1차 보고서·언어별 요약·측정 원본은 **저장소에서 제거했다**(2026-09-09). 남긴 것은 1차 계획 문서와 `decisions.ko.md`의 결정 이력뿐이다 |
| 측정 시점 | 공개 binding 0.17.6·framework 0.10.0으로 바로 잰다. 기다릴 외부 조건은 없다 |
| 위치 | 언어별 `framework/languages/<lang>/bench/with-grpc/`에 흩어진 bench를 **`framework/bench/grpc/` 한 곳**으로 모은다. 문서·집계기·로그도 함께(§2.1) |

### 0.2 측정 모델의 해석

"server to server, 트리거는 HTTP call"은 공통 perf 규격
(`framework/doc/framework/common/perf/README.ko.md` §4.2 server-driven 부하)의 모델과 같다.
즉 측정 대상은 **server process A가 server process B에 보내는 메시징**이고, HTTP 호출은
**부하를 시작하라는 신호(control)**이며 측정 operation이 아니다. A는 HTTP 요청을 받으면
자기 안의 logical stream으로 B에 요청을 반복하고, 완료·지연을 자기가 집계한다. 사용자가
이 해석을 확정했다(§0.1).

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

### 1.2 언어별 구현 상태 (1차 캠페인이 남긴 구현 상태; 1차 수치는 제거됨)

| 언어 | grpc | zlink raw | zlink framework | 언어별 문서 | 상태 요약 |
| --- | --- | --- | --- | --- | --- |
| `.NET` | 구현 | 구현 | 구현 | `bench/with-grpc/README.ko.md` 있음 | 1차에서 raw window 깊이가 binding 제출 처리율에 묶였다(0.17.5에서 수정) |
| Node | 구현 | 구현 | **미구현**(codec이 bytes 미지원) | 없음 | 1차에서 window 셀의 client socket 정지(0.17.5에서 수정); 최소 재현 `bench/with-grpc/repro/`는 회귀 확인용으로 유지 |
| Java | 구현 | 구현 | 구현 | 없음 | 1차에서 window 셀의 reply 유실(0.17.5에서 수정); 재현 `bench/with-grpc/repro/` 유지 |
| Kotlin | 구현(A만) | 구현(A만) | 구현(A만) | 없음 | server는 Java 바이너리 공유 |
| C++ | 구현 | 구현 | **미구현** | 없음 | framework 행 6셀을 새로 구현해야 한다 |
| C 기준 | 구현 | 구현 | 해당 없음 | `bindings/c/bench/BENCH_POLICY.md` | 1차에서 request-window @4096의 3-run 재현성이 흔들렸다(원인 미규명, S4) |

1차 수치는 제거했으므로 여기에는 구현 유무와 결함 유형만 남긴다.

### 1.3 이미 있는 자산 중 재사용할 것

- 공용 집계기 `framework/bench/tools/`(테스트 28개). 언어 client의 자체 표는 판정 근거가
  아니다(규격 §7.1). 새 모델에서도 판정은 집계기 출력으로 한다.
- 7축 perf 규격의 HTTP trigger·admin 계약(§4.2, §5.1 role config, §16)과 그것을 구현한 .NET
  canonical runner(`framework/languages/dotnet/perf/`, c016 phase 1). trigger listener, reset
  sequence, 중복 trigger 처리, stats endpoint가 이미 설계·검증돼 있다.
- 측정 티켓 큐(`scripts/perf/perf-ticket.sh`)와 고정 Core prefix(D-BP33·D-BP45). 측정 중
  Core를 다시 빌드하지 않는다.
- 측정 원본 위치는 `framework/bench/grpc/log/<lang>/<stamp>/`로 새로 정한다. 1차 원본은 제거했다.

### 1.4 2차에서 재확인할 1차 관측과 남은 작업

binding 완료 전달 결함(Node 정지, Java reply 유실, .NET 제출 처리율)은 Core·binding 0.17.5에서
수정됐다(사용자 확인). 아래는 그 밖에 1차가 남긴 관측이며, 2차 측정이 각각을 재확인한다.
값이 다시 나오면 그때 결함으로 기록하고 고치는 것은 별도 작업이다.

| 구분 | 항목 | 2차에서의 처리 |
| --- | --- | --- |
| 재확인 | `zlink-c` request-window @4096의 3-run 재현성 | S4에서 원인 규명. 재현성이 안 나오면 formula 1은 게재하지 않고 직접 비교 표만 싣는다 |
| 재확인 | framework handler 생성 실패 시 메시지를 수락·폐기하고 성공을 돌려주는 동작 | 2차 셀의 오류·유실 수와 대조. 재현되면 framework 결함으로 별도 기록 |
| 재확인 | framework send 경로의 drain 지연 | send-saturation 셀의 drain 시간으로 확인 |
| 재확인 | framework request 경로가 설정 window보다 낮은 깊이에 머무는 현상 | window 셀의 관측 깊이로 확인 |
| 제품 제약 | Node framework codec의 bytes 미지원 | Node framework 행 `unsupported`, codec 수정은 별도 작업 |
| 작업 | `zlink-framework-cpp` 6셀 미구현 | S3 |
| 작업 | bench가 gate에 없음(.NET bench가 빌드 불가 상태로 커밋된 적 있음) | S6 |

## 2. 목표와 산출물

| 산출물 | 위치 | 소유 |
| --- | --- | --- |
| 규격 개정: server-driven 모델·HTTP trigger·S2S 측정 구간 정의 | `framework/bench/grpc/README.{ko,en}.md` (현재 `framework/doc/framework/common/bench/with-grpc-local.*`를 이동) | 감독자 |
| 언어별 bench 문서 4개(`dotnet`·`node`·`java`(Kotlin 보조 절 포함)·`cpp`, 같은 절 구성) | `framework/bench/grpc/doc/<lang>.{ko,en}.md` | 감독자(초안은 job이 보고서로 제출) |
| 언어별 runner 개정(server A: trigger + logical stream, server B: echo) | `framework/bench/grpc/<lang>/` | codex job |
| 공용 집계기 확장(S2S 셀 스키마, A/B stats 병합) | `framework/bench/grpc/tools/` | codex job |
| 언어별 측정 원본 | `framework/bench/grpc/log/<lang>/<stamp>/` | 티켓 큐 |
| gRPC 비교 보고서(공개 문서) | `framework/bench/grpc/doc/comparison.{ko,en}.md` (사이트·README에서 링크) | 감독자 |
| 결정 기록 | `doc/plan/fw-bench-worklog/decisions.ko.md` (FB-045~) | 감독자 |

비교 보고서는 **공개 문서**다. 근거 수치·조건·한계를 규격 §7.1대로 남기되, 캠페인 진행 기록은
담지 않는다.

### 2.1 위치 통합 — `framework/bench/grpc/`

현재 bench는 언어별 트리(`framework/languages/{cpp,dotnet,java,node}/bench/with-grpc/`, C 기준은
`bindings/c/bench/with_grpc/`)에 흩어져 있고 집계기는 `framework/bench/tools/`, 규격은
`framework/doc/framework/common/bench/`에 있다. 이를 한 곳으로 모은다.

```text
framework/bench/grpc/
├── README.ko.md / README.md      # 규격(현재 with-grpc-local.*를 이동·개정)
├── doc/
│   ├── dotnet.ko.md / .md         # 언어별 bench 문서(§5 공통 형식)
│   ├── node.ko.md / .md
│   ├── java.ko.md / .md          # Kotlin 보조 셀 포함
│   ├── cpp.ko.md / .md
│   └── comparison.ko.md / .md     # gRPC 비교 보고서(공개)
├── tools/                         # 공용 집계기(현재 framework/bench/tools 이동)
├── dotnet/  node/  java/  cpp/    # 언어별 runner·A/B server(현재 with-grpc 이동; kotlin A client는 java/ 아래)
├── c/                             # C 기준 bench(현재 bindings/c/bench/with_grpc 이동)
├── proto/                         # 다섯 언어가 공유하는 bench.proto (언어별 복제본 제거)
└── log/<lang>/<stamp>/            # 측정 원본
```

- 이동은 `git mv`로 하고, 각 언어의 build 정의(csproj·package.json·build.gradle.kts·CMake)가
  framework 소스를 참조하는 상대 경로를 새 위치에 맞게 고친다. 샘플과 같은 원칙으로
  **bench는 framework 기본 빌드·sln·workspace·CI에 포함하지 않는다.**
- `framework/languages/dotnet/bench/with-grpc/`(784 MB)·`java`(472 MB)에는 bin/obj/build
  산출물이 있다. 이동 전에 tracked 파일만 옮기고 산출물은 남기지 않는다.
- 사이트: `doc/site/docs/bench -> ../../../framework/bench/grpc/doc` 심링크와 mkdocs nav 항목을
  추가해 규격·언어별 문서·비교 보고서를 zlink.systems에서 본다. README(한/영)의 문서 표에
  비교 보고서 링크를 넣는다.
- `bindings/c/bench/BENCH_POLICY.md`는 C bench 정책이므로 C 기준 bench와 함께 옮기되 정책
  문장은 유지한다.
- `framework/bench/tools/tests/fixtures/`의 1차 fixture는 집계기 테스트 입력이므로 유지한다.

## 3. 측정 모델 (규격 개정안의 핵심)

### 3.1 역할

| 역할 | process | 구현 |
| --- | --- | --- |
| Trigger client | 1 (언어 무관, 공용 runner의 curl 또는 작은 script) | `POST http://127.0.0.1:<A trigger>/bench/start` 로 `{runId, cellId, pattern, payloadBytes, durationMs, warmup}` 전달 |
| Server A (source) | 1 per 구현 | HTTP trigger listener + stats endpoint + B로 향하는 client(gRPC stub / raw ROUTER / framework channel client) |
| Server B (target) | 1 per 구현 | echo(request) 또는 count(send) + stats endpoint |

구현 셋(`grpc-<lang>`, `zlink-<lang>`, `zlink-framework-<lang>`)마다 A·B 한 쌍이다. 같은
언어의 세 쌍은 §3.4의 포트 대역 안에서 offset으로 구분한다.

### 3.2 측정 구간과 집계

1차와 형식이 달라지는 점을 먼저 적는다. (1) 부하 위치가 client process에서 server A로 옮겨지고
측정 구간이 A의 outbound call 기준이 된다. (2) 셀 결과에 A·B 두 process의 원본이 들어가므로
`RESULT` 라인과 셀 JSON 스키마가 바뀌고 집계기를 그에 맞춘다. 1차 원본과 같은 표에 넣지 않는다.
(3) 공개 보고서의 중심은 비율 판정식이 아니라 gRPC/raw/framework의 직접 비교 표다(§6).
바뀌지 않는 것은 패턴 4종, payload 2크기, window 100, ROUTER↔ROUTER, 29바이트 header, 3-run·G5다.

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

### 3.4 포트 대역 (새 규격 §9가 된다)

구현 하나에 A trigger, A stats, B endpoint, B stats 네 포트가 필요하고(raw binding은 B의
request·command endpoint가 분리되어 다섯), 언어당 세 구현이므로 20개 대역을 잡는다. Kotlin
보조 셀은 Java B를 그대로 쓰므로 Java 측정과 같은 시간에 돌리지 않는다(한 번에 한 언어). 기존
1차 대역(5071~5119, 6071~6079)과 겹치지 않는다.

| 언어 | 대역 | grpc A trigger/stats, B endpoint/stats | zlink raw A trigger/stats, B request/command/stats | framework A trigger/stats, B endpoint/stats |
| --- | --- | --- | --- | --- |
| `dotnet` | 5200-5219 | 5200/5201, 5202/5203 | 5205/5206, 5207/5208/5209 | 5212/5213, 5214/5215 |
| `node` | 5220-5239 | 5220/5221, 5222/5223 | 5225/5226, 5227/5228/5229 | 5232/5233, 5234/5235 |
| `java` | 5240-5259 | 5240/5241, 5242/5243 | 5245/5246, 5247/5248/5249 | 5252/5253, 5254/5255 |
| `kotlin`(보조) | 5260-5279 | 5260/5261, (B는 java 대역 5242/5243) | 없음 | 5272/5273, (B는 java 대역 5254/5255) |
| `cpp` | 5280-5299 | 5280/5281, 5282/5283 | 5285/5286, 5287/5288/5289 | 5292/5293, 5294/5295 |
| C 기준 | 6200-6219 | 6200/6201, 6202/6203 | 6205/6206, 6207/6208/6209 | 없음 |

각 대역의 `+16`~`+19`는 예비다. runner는 시작 전에 자기 대역이 비어 있는지 확인하고, 사용
중이면 옮기지 않고 중단한다(현재 규격 §9와 같은 규칙).

## 4. 언어별 작업

| 언어 | runner 개정 | 추가로 필요한 것 | 예상 job |
| --- | --- | --- | --- |
| `.NET` | client → server A로 재구성. canonical perf runner의 `ServerSupport`(trigger·admin·stats)를 재사용 | `Zlink.Framework.Perf.ServerSupport`를 bench가 참조할 수 있게 공유 위치로 이동 또는 복제 없이 참조 | sol 1 |
| Node | 같음 | framework 행은 codec bytes 지원이 선행(제품 결함, 사용자 결정: 별도 작업) — 그 전에는 `unsupported`로 기록 | sol 1 |
| Java | 같음 | 1차의 reply 유실 재현(`repro/`)이 0.17.6에서 사라졌는지 확인 | sol 1 |
| Kotlin(보조) | A만 Kotlin, `request-window @1024` 두 셀(`grpc-kotlin`, `zlink-framework-kotlin`), B는 Java 바이너리 공유 | 없음 | Java job에 포함 |
| C++ | 같음 + `zlink-framework-cpp` 6셀 구현 | framework C++ HTTP hosting(trigger listener)은 framework 자체 기능으로 있음 | astra 1 |
| C 기준 | A/B 재구성 없이 유지(HTTP trigger 없음). formula 1 분모 안정화가 별도 항목 | 기준선 안정화 job | astra 1 |
| 집계기 | S2S 셀 스키마·A/B 병합·언어별 문서 표 생성 | 테스트 유지 | sol 1 |

문서·규격·보고서는 job이 쓰지 않는다. job은 보고서에 "언어별 문서에 들어갈 표와 값"을
제출하고 감독자가 같은 형식으로 옮긴다.

## 5. 언어별 bench 문서의 공통 형식

네 문서(`dotnet`, `node`, `java`, `cpp`)가 같은 절 번호와 제목을 갖는다. Kotlin 보조 셀은 Java 문서의 한 절로 둔다. 현재 `.NET` README를
이 형식으로 고쳐 기준으로 삼고 `framework/bench/grpc/doc/`에 둔다.

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
2. 결과 요약 — 언어 × 패턴 × payload 표(집계기 출력 그대로). 단위를 표 머리에 명시한다: request
   계열은 완료 수 기준 **KOPS/s**, `send-saturation`은 server 수신 수 기준 **KMSG/s**, 값은 warmup 뒤
   **5초 active 구간의 평균**(3-run 중앙값). request 행에는 평균·p95·p99 지연(ms)을 함께 싣는다.
   formula 1·2 판정은 부록으로 내린다(사용자 결정 2026-09-09)
3. 언어별 3자 표 — 규격 §7.1이 요구하는 동반 정보(warmup, 구성, 깊이, drain)
4. 읽는 방법 — §7.3 언어 간 비교 금지 규칙, `unsupported`의 의미
5. 한계 — 로컬 loopback, 단일 머신, 기준선 불안정 등
6. 원본 위치

1차 보고서는 제거했으므로 새 보고서는 2차 측정만으로 쓴다. 계측 규칙(표본화 시점, 실험 분리)은
규격 본문이 소유한다.

## 7. 측정 절차

- 모든 측정은 티켓 큐로 낸다(`scripts/perf/perf-ticket.sh submit -p 1 -o <owner> -d "<설명>" -- <명령>`).
  한 번에 한 언어. load average 10 미만에서 시작. Core는 `.artifacts/perf-queue/core-prefix.env`의
  고정 prefix(현재 0.17.5)이며 측정 중 재빌드하지 않는다.
- 셀당 3-run, 재현성 조건 G5(3-run 중앙값 대비 스프레드 ≤10%)는 1차 계획 §6의 게이트 정의를 그대로 쓴다.
- 조건(timeout·sleep·window·HWM·client 수)을 완화하지 않는다. 벤치가 잘못 재면 벤치를 고친다.
- 결과와 판정은 집계기 출력만 인용한다.

## 8. 단계

| 단계 | 내용 | 산출 | 담당 |
| --- | --- | --- | --- |
| S-1 | bench 위치 통합(§2.1): `git mv`, build 참조 경로 수정, 집계기 이동, 언어별 runner가 새 위치에서 빌드·1셀 smoke 통과 | `framework/bench/grpc/` | codex sol 1 |
| S0 | 규격 개정(§3 실행 조건·§4 출력·§9 포트·새 §10 S2S 모델)과 이동, `.NET` 문서를 §5 형식으로, 사이트 nav | 규격 ko/en, .NET 문서 | 감독자 |
| S1 | 집계기 S2S 스키마 + `.NET` runner 개정 + `.NET` 3-run | 원본·표 | codex sol ×2 |
| S2 | Node·Java runner 개정과 3-run + Kotlin 보조 셀 2개(1차 결함이 재현되면 회귀로 기록하고 멈춘다) | 원본·표 | codex sol |
| S3 | C++ runner 개정 + `zlink-framework-cpp` 구현 + 3-run | 원본·표 | codex astra |
| S4 | `zlink-c` 기준선 안정화(원인 규명 후 최소 수정) | 판정 게재 가능 여부 | codex astra |
| S5 | 언어별 문서 3개(Node·Java·C++; Java 문서에 Kotlin 보조 절) + 비교 보고서 ko/en, README·사이트 링크 | 공개 문서 | 감독자 |
| S6 | bench를 로컬 gate(`scripts/gate/framework-gate.sh`)에 빌드만 편입 | 재파손 방지 | 감독자 |

각 job은 원인 하나, 1.5시간 상한, 브리프는 `doc/plan/fw-bench-worklog/briefs/fwb2-<id>.prompt`,
요약은 `fw-bench-worklog/fwb2-<id>-summary.md`. job은 규격·계획·문서를 수정하지 않는다.

## 9. 위험

- 1차의 binding 결함이 0.17.6에서 재현되면 2차는 그 자리에서 멈추고 회귀로 보고한다. 조건을
  바꿔 값을 만들지 않는다.
- `zlink-c` 분모가 안정되지 않으면 formula 1은 다시 게재 불가다. S4를 S1과 병행한다.
- Node framework 행은 codec 결함이 풀려야 참여한다. 보고서에 `unsupported`로 남기는 것을
  기본으로 두고, codec 수정은 별도 작업으로 뺀다.
- HTTP trigger listener를 언어마다 새로 쓰면 형식이 갈린다. 7축 규격 §5.1의 role config와
  §16의 endpoint 계약을 그대로 가져와 다섯 언어가 같은 요청·응답 JSON을 쓴다.

## 10. 범위 밖

- 운영 환경(mesh, TLS, 다중 노드) 측정.
- 7축 perf 규격의 나머지 축(CS, AC, PS, Spot local/worker). 이 계획은 S2S 축의 gRPC 비교만 다룬다.
- framework 제품 결함(Node codec bytes, handler 생성 실패 처리 등)의 수정. 발견하면 기록만 한다.
- 측정 머신은 WSL(A 머신) 하나다. runner는 bash이며 Windows 실행은 범위 밖이다.

## 11. 결정 이력 (질문과 답은 §0.1에 반영됨; 아래는 질문 원문)

1. §0.1의 해석이 맞는가 — HTTP call은 부하 시작 신호이고 측정 operation은 A→B 메시징인가.
   (대안: HTTP 요청 하나마다 A→B 요청 하나를 보내고 HTTP 왕복을 재는 모델. 이 경우 HTTP
   서버 비용이 섞여 gRPC/ZLink 비교가 흐려진다.)
2. trigger 포트를 §9 대역의 예비 `+8`로 쓸지, 대역을 `+10`으로 넓힐지.
3. Kotlin을 1차처럼 "A만 Kotlin, B는 Java 공유"로 둘지, B도 Kotlin으로 둘지.
4. Node framework 행을 codec 수정 전까지 `unsupported`로 둘지, codec 수정을 이 캠페인에 포함할지.
5. 1차 보고서(`doc/plan/fw-bench-worklog/report-with-grpc-5lang.ko.md`)의 수치를 새 보고서에
   "client-driven 1차 결과"로 병기할지, 새 모델 결과만 실을지.
6. 시작 시점 — 현재 진행 중인 framework 0.11.0 배포·CI 복구·샘플 구조 작업 뒤인지.
