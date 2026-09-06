# Framework gRPC 비교 bench 5언어 확장 캠페인 — .NET · Node · Java · Kotlin · C++

> 작성일: 2026-09-06 23:10 (main `3c19ab456a`, 0.17.0)
> 대상 규격: [`with-grpc-local.ko.md`](../../framework/doc/framework/common/bench/with-grpc-local.ko.md)
> 인접 규격: [Framework Performance 공통 규격](../../framework/doc/framework/common/perf/README.ko.md) (별도 캠페인, 이 문서와 수치를 합치지 않는다)
> 성능 정책: [`../perf/PERF_POLICY.md`](../perf/PERF_POLICY.md)
> 문서 규칙: [`../AGENTS.md`](../AGENTS.md) — 이 문서는 `plan/`이므로 공개 문서에서 링크하지 않는다

## 0. 요청 정리 (사용자, 2026-09-06 23:00~23:40)

| 항목 | 내용 |
|---|---|
| 목표 | 현재 `.NET`에만 있는 gRPC 비교 bench를 **Node, Java, Kotlin, C++** 로 확장하고, 언어별 비교와 **최종 보고서 작성**까지 끝낸다 |
| **비교 관점** | **서비스 측면 비교다.** 같은 업무를 각 스택으로 구현했을 때 실제로 치르는 비용을 잰다. 두 스택이 내부적으로 같은 메커니즘을 쓰는지는 묻지 않는다 |
| 비교 단위 | 언어마다 `grpc-<lang>` · `zlink-<lang>`(raw binding) · `zlink-framework-<lang>`(framework) 3자 비교 |
| 소켓 축 | ZLink 쪽은 **RouteMesh ROUTER↔ROUTER**의 request와 send. raw binding 행도 같은 ROUTER↔ROUTER로 맞춘다 |
| 실행 규칙 | **병렬 진행 금지.** 성능 측정이 목적이므로 job은 항상 한 번에 하나만 실행한다 |
| 역할 | 감독관(이 세션)은 **감독과 리뷰만** 한다. 실제 구현·빌드·측정은 **opus 또는 sonnet 서브에이전트**가 수행한다 |
| 산출물 | 언어별 측정 원본, 언어별 요약, 5언어 통합 보고서 |

### 0.1 확정된 설계 결정

| ID | 결정 | 근거 |
|---|---|---|
| FB-001 | ZLink raw binding 행의 소켓을 **DEALER→ROUTER에서 ROUTER↔ROUTER로 통일**한다. `bindings/c/bench/with_grpc`의 `zlink-c` 기준값도 같은 구성으로 맞춘다 | framework는 RouteMesh ROUTER↔ROUTER인데 raw가 DEALER→ROUTER면 `framework / raw` 비율이 framework 계층 비용이 아니라 소켓 패턴 차이까지 포함한다. 판정식(§2.2)이 의미를 가지려면 같은 패턴이어야 한다 |
| FB-002 | `send-saturation`의 gRPC 대응물은 **현행 unary `Command` → `Empty` 그대로 둔다.** client-streaming 셀을 추가하지 않는다 | 실제 서비스에서 응답이 필요 없는 호출은 unary + `Empty`로 구현한다. client-streaming은 업로드·적재용이며 이 용도로 쓰지 않는다. gRPC에 단방향 호출 원시 기능이 없어 왕복을 치르는 것은 **gRPC의 특성이며 서비스 측면 비교에서 그대로 결과에 드러나야 하는 값**이다 |
| FB-003 | 판정 기준 패턴은 `request-window`, 기준 비율은 규격 §7의 0.80 두 식을 그대로 쓴다 | 기존 규격을 바꾸지 않는다 |

## 1. 현재 상태

### 1.1 이미 있는 자산

| 자산 | 경로 | 상태 |
|---|---|---|
| bench 규격 | `framework/doc/framework/common/bench/with-grpc-local.ko.md` (167행) | 존재. 다만 제목과 본문이 `.NET` 전용으로 서술됨 |
| .NET 구현 | `framework/languages/dotnet/bench/with-grpc/` | 존재. client 1 + server 3(`GrpcServer`, `ZLinkRawServer`, `ZLinkServer`), `run_local.sh` |
| C 기준 bench | `bindings/c/bench/with_grpc/` | 존재. `grpc-c`, `zlink-c` 두 구현. framework 계층은 C에 없다 |
| 측정 원본 | `framework/languages/dotnet/bench/with-grpc/log/` | 비어 있음(WSL 재이관 이후 실측 이력 없음) |

.NET client `Program.cs`는 1730행이고 server 3개는 71·73·248행이다. client의 상당 부분은
패턴 구동이 아니라 집계와 표 출력 코드다.

### 1.2 언어별 전제조건 점검 (2026-09-06 실측)

| 언어 | gRPC 수단 | Framework 모듈 | Raw binding | protobuf codec | 판정 |
|---|---|---|---|---|---|
| .NET | ASP.NET Core gRPC | `framework/languages/dotnet/src/Zlink.Framework` | `bindings/dotnet` | `Zlink.Framework.Codecs.Protobuf` | 구현 완료, 실측 미수행 |
| Node | `@grpc/grpc-js` (npm 도달 확인 200) | `framework/languages/node/packages/framework` | `bindings/node` | `packages/framework-codec-protobuf` | 가능 |
| Java | grpc-java (maven 도달 확인 200) | `zlink-framework-core` | `bindings/java` | `zlink-framework-codec-protobuf` | 가능 |
| Kotlin | grpc-kotlin 또는 grpc-java | `zlink-framework-kotlin` | `bindings/kotlin` | Java 것을 공유 | 가능 |
| C++ | **시스템에 `libgrpc++-dev` 1.51.1, `grpc_cpp_plugin` 설치됨** | `framework/languages/cpp/framework` | `bindings/cpp` | `zlink::framework_codec_protobuf` | 가능. vcpkg로 gRPC를 빌드할 필요 없음 |

5개 언어 전부에 protobuf codec이 이미 있으므로, gRPC와 framework가 같은 `bench.proto` DTO를
쓸 수 있다. 언어를 넘어 codec 조건이 어긋나지 않는다는 뜻이며 이 캠페인이 성립하는 근거다.

C++ gRPC 1.51.1은 2022년 배포판이다. 비교는 유효하지만 버전을 결과에 반드시 남긴다.

## 2. 범위와 비교 축

### 2.1 측정하는 것

패턴 3종 × payload 2종을 언어마다 3개 구현으로 실행한다. 규격 §2를 그대로 따른다.

| 패턴 | 의미 |
|---|---|
| `request-serial` | 요청 하나를 보내고 reply 완료 뒤 다음 요청을 보낸다. 왕복 지연이 처리량을 결정한다 |
| `request-window` | 미완료 request를 최대 `request_window`(기본 100)개 유지한다. **판정의 중심 패턴** |
| `send-saturation` | reply payload가 없는 command 경로. server 수신 수로 처리량을 계산한다 |

payload는 `1024`, `4096` bytes. 한 언어의 한 셀은 `(언어, 구현, 패턴, payload)` 조합 하나다.
서로 다른 셀의 수치를 평균해 하나의 값으로 만들지 않는다. 언어당 3구현 × 3패턴 × 2 payload = 18셀이다.

`send-saturation`에서 gRPC는 unary `Command`로 `Empty` 응답까지 기다리고 ZLink는 단방향으로
제출한다. 이 차이는 측정 기법의 결함이 아니라 두 스택이 같은 업무에 요구하는 구현이 다르기
때문이다(FB-002). 보고서는 이 셀을 "전송 속도 차이"가 아니라 **"응답이 필요 없는 명령을
처리하는 데 각 스택이 치르는 비용"** 으로 서술한다.

### 2.2 비교 축 — 이것을 지키지 않으면 결과가 오독된다

이 bench는 **서비스 측면 비교**다. "같은 업무를 gRPC로 짜면 이 비용, ZLink로 짜면 이 비용"에
답한다. 두 스택의 내부 메커니즘이 같은지는 묻지 않으며, 한쪽에만 있는 기능 때문에 생기는
차이는 감추지 않고 그대로 결과로 낸다.

- **1차 축은 같은 언어 안의 3자 비교다.** `grpc-node` 대 `zlink-node` 대 `zlink-framework-node`는
  같은 런타임·같은 클라이언트 프로세스 조건이므로 유효하다.
- **`grpc-node` 대 `grpc-java`는 런타임 비교이지 ZLink 비교가 아니다.** 절대 처리량을 언어끼리
  가로로 읽지 않는다.
- **언어를 가로질러 읽을 수 있는 값은 비율이다.** `zlink-framework-<lang> / zlink-<lang>`는
  각 언어가 자기 자신을 기준으로 한 값이므로 "어느 언어의 framework 계층이 가장 비싼가"에
  답할 수 있다. 통합 보고서의 주 표는 이 비율 표다.
- **ZLink 두 행은 같은 소켓 패턴이어야 한다.** framework는 RouteMesh ROUTER↔ROUTER이므로 raw
  binding 행도 ROUTER↔ROUTER로 둔다(FB-001). 서로 다른 패턴을 나눈 값은 framework 계층 비용이
  아니다.
- **`zlink-c`가 바닥 기준이다.** `bindings/c/bench/with_grpc`의 `zlink-c` `request-window` 값을
  기준으로 각 언어 raw binding의 위치를 본다. 이 값도 ROUTER↔ROUTER로 맞춘 뒤에 쓴다.

판정 기준은 규격 §7을 그대로 쓴다.

```text
zlink-<lang> / zlink-c                     >= 0.80   binding 계층 통과
zlink-framework-<lang> / zlink-<lang>      >= 0.80   framework 추가 비용 통과
```

기준 패턴은 `request-window`다. 여기서 gRPC unary `Echo`와 ZLink request는 **같은 보장(서버
처리 확인)** 을 주므로 정면 비교가 성립한다. `request-serial`은 왕복 지연 보조 지표로 남긴다.

### 2.3 측정하지 않는 것

- 운영 환경의 mesh, TLS, L7 부하 분산, 다중 노드 분배, 네트워크 지연. loopback 전용이다.
- Framework Performance 공통 규격(§10 시나리오)의 Spot·Actor 셀. 그쪽은 별도 캠페인이며
  serializer(typed JSON)와 완료 결산 방식이 달라 이 bench와 수치를 합치지 않는다.
- gRPC 쪽 튜닝 경쟁. gRPC는 각 언어의 기본 서버 구성으로 두고 그 사실을 결과에 남긴다.

## 3. 역할과 실행 규칙

### 3.1 역할

| 역할 | 담당 | 하는 일 |
|---|---|---|
| 감독관 | Claude (이 세션) | **브리프 작성, job 투입과 생존 확인, 결과 리뷰(diff·규격 대조·보고서 검토), 채택 판정, 커밋·push, 결정 기록.** 코드를 직접 고치지 않고 빌드·측정도 직접 돌리지 않는다. 예외는 계획 문서·브리프·결정 기록 커밋뿐이다 |
| 구현·측정 job | **opus** | 새 언어의 client·server 구현, 공통 계약 정합, 실패 원인 진단, 측정 실행과 표 작성 |
| 기계적 작업 job | **sonnet** | 규격 문서 번역·정렬, 빌드 스크립트 정리, 의존성 추가, 반복 측정 실행, 결과 파일 수집 |
| 리뷰 보조 | opus(읽기 전용) | 감독관 리뷰 전에 규격 위반·측정 조건 불일치 독립 점검 |

감독관은 리뷰에서 발견한 정정도 직접 고치지 않고 job에 되돌려 보낸다.

### 3.2 직렬 실행 규칙 (절대 조건)

**이 캠페인의 모든 job은 한 번에 하나만 실행한다.**

- 성능 측정이 목적이므로 다른 job의 빌드·테스트가 같은 시각에 돌면 그 셀의 CPU 조건이
  달라진다. 언어별 job을 병렬로 돌려 시간을 줄이지 않는다.
- 구현만 하는 job도 빌드가 머신을 점유하므로 직렬로 둔다. 언어 하나를 끝내고 다음으로 간다.
- 측정 중에는 다른 빌드·gate·sample·E2E를 실행하지 않는다. 측정 job은
  `flock --exclusive /tmp/zlink-perf.lock`으로 자기 구간을 잠근다.
- 측정 시작 전 `loadavg < 2`를 확인하고 그 값을 결과에 기록한다. 넘으면 대기한다.
- job은 각자 detached worktree(`~/project/zlink-work/<job-id>`)에서 작업한다. 브랜치를 만들지
  않고 main에 단위별로 커밋·push한다.

job 상한은 1.5시간이다. 넘으면 중간 결과를 보고하고 감독관이 범위를 나눈다.

## 4. 공통 계약 (Phase 1이 확정한다)

언어가 늘어나면 규격이 `.NET` 서술로 남아 있는 한 표가 어긋난다. 아래를 언어 중립으로
확정한 뒤에 새 언어를 만든다.

| 항목 | 값 |
|---|---|
| 문서 제목 | "`.NET` messaging local bench 규격" → "messaging local bench 규격"으로 바꾸고 언어 축을 추가 |
| 구현 이름 | `grpc-<lang>`, `zlink-<lang>`, `zlink-framework-<lang>`. `<lang>`은 `dotnet`·`node`·`java`·`kotlin`·`cpp` |
| 스키마 | 서비스 `BenchService`, `Echo(BenchPayload) returns (BenchPayload)`, `Command(BenchPayload) returns (Empty)`, `bytes body = 1`. **RPC를 추가하지 않는다**(FB-002) |
| ZLink 소켓 축 | framework는 RouteMesh ROUTER↔ROUTER의 channel request/send. raw binding도 **ROUTER↔ROUTER**. `zlink-c` 기준값도 동일(FB-001) |
| 측정 header | payload 앞 29바이트, 규격 §6의 offset·의미 그대로 (magic `0x5A4C4E4B`, run id, phase, payload size, sequence, send timestamp ns) |
| 패턴 | `request-serial`, `request-window`, `send-saturation` |
| 기본값 | payload `1024,4096` / `request_window` 100 / send concurrency 8 / Release 빌드 / `127.0.0.1` |
| 공통 CLI | .NET client의 옵션을 정본으로 삼는다: `--implementation`, `--scenario`, `--payload-sizes`, `--duration-seconds`, `--warmup`, `--request-window`, `--send-concurrency`, `--latency-sample-limit`, `--timeout-seconds`, `--command-settle-ms`, `--output`, `--report-file` |
| 출력 | `RESULT,current,<scenario>,local,<payload_size>,<metric>,<value>` 라인과 표. metric 이름은 규격 §4의 9종 |
| 처리량 단위 | request 계열은 완료 수 기준 `KOPS`, `send-saturation`은 server 수신 수 기준 `KMSG/s` |

언어마다 다르게 두되 **반드시 기록**하는 값이 있다.

| 항목 | 이유 |
|---|---|
| warmup 길이 | JVM은 JIT 예열이 끝나야 정상 상태다. .NET과 같은 warmup을 강요하면 예열 전 상태를 재게 된다 |
| gRPC 서버 구성 | 언어마다 기본 서버 구현이 다르다(ASP.NET Core, grpc-js, grpc-netty-shaded, grpc++) |
| 런타임 버전 | SDK·런타임·gRPC 라이브러리 version |

### 4.1 집계 코드를 언어마다 복제하지 않는다

.NET client 1730행을 5벌로 복제하면 표가 미묘하게 어긋나 비교가 깨진다. Framework
Performance harness가 쓰는 구조를 따른다.

```text
  per-language client/server  ->  raw JSON per cell  ->  shared aggregator
  (language specific)             (common schema)        (one report)
```

- 언어별 프로세스는 셀 원본(JSON)과 `RESULT` 라인만 낸다.
- 표·비율·판정은 공용 집계 스크립트 하나가 만든다. 위치는
  `framework/languages/shared_sample`이 아니라 bench 전용 `framework/bench/tools/`로 새로 둔다.
- .NET client에서 표 출력 부분을 이 집계기로 옮기고, 옮긴 뒤 기존 출력과 같은 표가 나오는지
  먼저 확인한다.

## 5. 단계

각 Phase는 이전 Phase가 끝난 뒤에 시작한다. Phase 안의 job도 직렬이다.

### Phase 0 — raw 소켓 통일과 기준선 확보 (opus 1 job, ~1.5 h)

FB-001을 먼저 적용한 뒤 첫 숫자를 만든다. 오염된 기준을 5언어로 복제하지 않기 위해 순서가
이렇게 된다.

- `.NET` bench의 raw client를 DEALER에서 **ROUTER로 바꾸고** 상대 ROUTER의 routing id를 지정해
  보낸다. 서버(`ZLinkRawServer/Program.cs`)는 이미 ROUTER이므로 client 쪽만 바뀐다.
  대상: `framework/languages/dotnet/bench/with-grpc/Client/Program.cs:493,512,530`.
- `bindings/c/bench/with_grpc`의 `zlink-c`도 같은 구성으로 맞춘다.
  대상: `zlink/bench_zlink_client.cpp:530-531`. **먼저 `bindings/c/bench/BENCH_POLICY.md`를 읽고**
  기존 DEALER 셀을 없애야 하는지 옵션으로 병행해야 하는지 판단해 보고한다. 이 캠페인이 쓰는
  `zlink-c` 값은 ROUTER↔ROUTER 구성이어야 한다.
- 두 bench를 실행해 3패턴 × 2 payload 결과를 얻는다.
- 산출: 측정 원본, `zlink-dotnet / zlink-c`와 `zlink-framework-dotnet / zlink-dotnet` 비율.
- 규격 §7의 0.80 기준을 .NET이 만족하는지 본다. 만족하지 않으면 **원인을 기록만 하고 이
  캠페인에서 최적화하지 않는다**(별도 job 후보).
- ROUTER↔ROUTER 전환 전후 값을 둘 다 남긴다. 소켓 패턴이 결과에 준 영향을 보고서가 인용한다.

### Phase 1 — 규격 언어 중립화와 집계기 분리 (opus 1 job, ~1.5 h)

- 규격 문서를 §4의 공통 계약대로 개정한다(한국어·영어 두 파일). FB-001~FB-003을 규격 본문에
  반영하되 패턴·payload·header·metric 이름 같은 계약 값은 바꾸지 않는다.
- 공용 집계 스크립트를 만들고 .NET client를 원본 JSON 출력 + 집계기 구조로 바꾼다.
- **판정**: 개정 전 Phase 0 결과를 집계기에 넣었을 때 Phase 0과 같은 표가 나와야 한다.

### Phase 2 — Node (opus 1 job, ~1.5 h)

- `framework/languages/node/bench/with-grpc/`에 client 1 + server 3.
- gRPC는 `@grpc/grpc-js` + `@grpc/proto-loader`. framework 서버는 `packages/framework`의
  channel 요청 처리, raw 서버는 `bindings/node`의 DEALER/ROUTER 경로.
- **주의**: Node client는 단일 스레드다. transport가 아니라 client CPU가 상한이 될 수 있다.
  client CPU가 95%를 넘은 셀은 결과에 포화 표시를 남기고 처리량 비교에서 제외 판정한다.

### Phase 3 — Java (opus 1 job, ~1.5 h)

- `framework/languages/java/bench/with-grpc/`에 Gradle 모듈로 구성.
- gRPC는 grpc-java(`grpc-netty-shaded`, `grpc-protobuf`, `grpc-stub`) + protobuf gradle plugin.
- framework 서버는 `zlink-framework-core`의 typed channel 요청 처리, raw 서버는 `bindings/java`.
- **주의**: warmup을 .NET보다 길게 잡고 그 값을 결과에 남긴다. 예열 부족은 결과를 왜곡한다.

### Phase 4 — Kotlin (opus 1 job, ~1 h)

- Java Phase의 Gradle 구성과 proto 생성을 재사용한다.
- **결정 항목**: ZLink 쪽은 `zlink-framework-kotlin`의 suspend 인터페이스를 쓰므로 gRPC도
  grpc-kotlin coroutine stub을 쓴다. blocking stub을 쓰면 Kotlin 쪽에 유리하게 기운다.
  coroutine stub 사용이 불가능하면 그 사유와 함께 blocking stub을 쓰고 결과에 표시한다.

### Phase 5 — C++ (opus 1 job, ~1.5 h)

- `framework/languages/cpp/bench/with-grpc/`에 CMake 타깃으로 구성.
- gRPC는 시스템 `libgrpc++` 1.51.1과 `grpc_cpp_plugin`을 쓴다. vcpkg로 gRPC를 빌드하지 않는다.
- framework 서버는 `framework/languages/cpp/framework`, raw 서버는 `bindings/cpp`.
- **주의**: C++ raw binding은 C API의 얇은 계층이므로 `zlink-cpp / zlink-c`가 1.0 근처여야
  한다. 크게 낮으면 bench 구현 문제일 가능성이 먼저다.

### Phase 6 — 통합 측정과 보고서 (sonnet 측정 job 1 + opus 보고서 job 1, ~2 h)

- 5개 언어를 **한 번에 하나씩 순서대로** 다시 측정한다(구현 중 실측은 개발 중 값이므로 채택
  하지 않는다). 언어당 3회 실행하고 중앙값을 채택한다.
- 측정 사이에 다른 작업을 넣지 않는다. 전체 측정 구간의 시작·종료 시각과 loadavg를 남긴다.
- 보고서를 §7 형식으로 작성한다.

## 6. 게이트 (한 언어를 완료로 판정하는 조건)

| 조건 | 내용 |
|---|---|
| G1 계약 정합 | 3패턴 × 2 payload × 3구현 = 18셀이 모두 실행되고 `RESULT` 라인 metric 9종이 채워진다 |
| G2 header 검증 | request 계열은 client가 reply의 29바이트 header를 검증한다. 검증 실패 수를 결과에 남긴다 |
| G3 send 계열 무결성 | `send-saturation`의 처리량은 **server 수신 수**로 계산한다. client 제출 수로 계산하지 않는다 |
| G4 공개 API만 사용 | private 런타임·reflection·두 번째 poller·재시도 상태를 넣지 않는다. 넣으면 다른 경로를 잰 것이다 |
| G5 재현성 | 같은 조건 3회 실행의 중앙값 대비 각 실행이 ±10% 안이다. 넘으면 원인을 기록하고 재측정한다 |
| G6 포화 표시 | client CPU 95% 초과 셀은 포화로 표시하고 처리량 우열 판정에 쓰지 않는다 |
| G7 격리 | 측정 구간에 다른 빌드·job이 없었음을 로그로 보인다 |

G1~G4를 만족하지 못한 셀은 `unsupported`로 기록하고 완료 개수에 넣지 않는다. 수치를
만들기 위해 조건을 바꾸지 않는다.

## 7. 보고서 (최종 산출물)

### 7.1 파일 구성

| 산출물 | 위치 |
|---|---|
| 셀 원본 JSON, 서버 로그 | `framework/languages/<lang>/bench/with-grpc/log/<run-stamp>/` |
| 언어별 요약 | `doc/plan/fw-bench-worklog/bench-<lang>-summary.ko.md` |
| 결정 기록 | `doc/plan/fw-bench-worklog/decisions.ko.md` (항목 ID `FB-001`부터) |
| 통합 보고서 | `doc/plan/fw-bench-worklog/report-with-grpc-5lang.ko.md` |
| job 브리프 | `doc/plan/fw-bench-worklog/briefs/fwb-<id>.prompt` |

통합 보고서는 `plan/` 아래에 둔다. 공개 문서나 guide에 성능 우위 문장을 옮기는 것은 사용자
판단 사항이며, 옮길 때 규격 §7이 요구하는 환경 정보를 함께 옮긴다.

### 7.2 통합 보고서의 구성

1. **요약** — 세 문장 이내. 어떤 조건에서 무엇이 관찰되었는가.
2. **측정 조건** — CPU 모델, OS·커널, 각 언어 런타임과 gRPC 라이브러리 version, commit hash,
   payload, warmup·active duration, request window, send concurrency, 측정 시각과 loadavg.
3. **언어별 3자 표** — 언어마다 규격 §4 형식의 표 하나. 패턴별로 묶고 행에 구현 이름을 둔다.
4. **주 표: 계층 비용 비율** — 이 캠페인의 핵심 표다.

   | 언어 | `zlink-<lang>` / `zlink-c` | `zlink-framework-<lang>` / `zlink-<lang>` | `zlink-framework-<lang>` / `grpc-<lang>` | 판정 |
   |---|---|---|---|---|
   | dotnet | | | | |
   | node | | | | |
   | java | | | | |
   | kotlin | | | | |
   | cpp | | | | |

   앞 두 열이 0.80 이상이면 통과다. 세 번째 열은 gRPC 대비 위치이며 판정 기준이 아니라
   관찰값이다. 기준 패턴은 `request-window`, 기준 payload는 두 크기 모두 적는다.
   `request-window`는 gRPC unary `Echo`와 ZLink request가 같은 보장을 주는 셀이므로 이 표의
   근거로 쓸 수 있다.
5. **패턴별 관찰** — `request-serial`의 왕복 지연, `request-window`의 처리량,
   `send-saturation`의 단방향 수신량을 각각 따로 서술한다. 세 패턴을 평균내지 않는다.
6. **한계** — loopback 단일 머신, TLS·mesh 없음, gRPC 기본 구성, C++ gRPC 1.51.1,
   포화 셀 목록, 언어별 warmup 차이.
7. **후속 후보** — 이 캠페인에서 원인을 기록만 하고 고치지 않은 항목.

### 7.3 서술 규칙

쓰지 않는 문장.

- "ZLink가 gRPC보다 N배 빠르다" 같은 일반화. 결과는 "이 조건에서"로만 서술한다.
- 언어를 가로지른 절대 처리량 우열.
- 포화 셀이나 실패 셀을 근거로 한 우위 주장.

`send-saturation` 서술 규칙. 이 셀은 gRPC가 왕복하고 ZLink가 단방향이라는 사실을 포함한 값이다.

- 쓰지 않는다: "전송 속도가 N배" — 전송 속도를 잰 값이 아니다.
- 쓴다: "응답이 필요 없는 명령을 처리할 때, gRPC는 unary 왕복을 치러야 하고 ZLink는 단방향
  send로 끝난다. 이 조건에서 그 차이는 N배로 관찰됐다."

이 서술은 두 스택이 같은 업무에 요구하는 구현이 다르다는 사실을 그대로 전달한다. 비교를
공정하게 만들려고 gRPC 쪽에 client-streaming 같은 다른 사용 형태를 끼워 넣지 않는다(FB-002).

## 8. 위험과 대응

| 위험 | 대응 |
|---|---|
| Node client가 먼저 포화해 transport 비교가 되지 않는다 | client CPU를 매 셀 기록하고 95% 초과를 포화로 표시. 필요하면 `request_window`를 낮춘 보조 셀을 추가하되 주 표에는 기본값 셀만 쓴다 |
| JVM 예열 부족으로 Java·Kotlin이 낮게 나온다 | warmup을 언어별로 늘리고 그 값을 기록. 예열 판정은 warmup 구간 처리량이 안정되는지로 본다 |
| C++ gRPC 1.51.1이 오래되어 gRPC 쪽이 불리하다 | 버전을 결과에 명시. gRPC 비교는 관찰값이고 판정 기준은 ZLink 계층 간 비율이므로 결론이 뒤집히지 않는다 |
| 5언어 × 3구현 = 서버 프로세스가 많아 포트가 충돌한다 | 언어별 포트 대역을 규격에 고정하고 preflight에서 확인 |
| 측정이 다른 작업과 겹쳐 값이 흔들린다 | §3.2의 직렬 규칙과 `/tmp/zlink-perf.lock`. G7로 검증 |
| 집계기 분리 중 .NET 기존 출력이 달라진다 | Phase 1의 판정 조건이 "Phase 0과 같은 표". 다르면 채택하지 않는다 |
| ROUTER↔ROUTER 전환으로 raw 값이 크게 떨어져 0.80 판정이 바뀐다 | Phase 0에서 전후 값을 모두 남긴다. 판정이 바뀌면 원인을 기록하고 사용자 결정에 올린다. 값을 살리려고 소켓 구성을 되돌리지 않는다 |
| job이 규격을 임의로 바꾼다 | 브리프에 "규격을 바꿔야 하면 멈추고 보고"를 명시. 규격 변경은 감독관이 결정 기록에 남긴 뒤에만 |

## 9. 브리프 필수 항목 (`fw-bench-worklog/briefs/fwb-<id>.prompt`)

- 대상 언어와 Phase, 작업 상한 1.5시간.
- 소유 규격 조항(`with-grpc-local.ko.md`의 절 번호)과 바꾸면 안 되는 값.
- **"규격이나 공개 API를 바꿔야만 가능하면 멈추고 보고한다"**.
- 변경할 파일 범위, worktree 경로 `~/project/zlink-work/<job-id>`.
- 실행한 명령과 남은 실패, 브리프가 지정한 셀의 결과 표.
- **다른 job과 동시에 실행하지 않는다는 확인**과 측정 시작 시각의 loadavg.

## 10. 체크리스트

- [ ] Phase 0 — raw 소켓 ROUTER↔ROUTER 통일(.NET·C), 기준선 측정, `zlink-c` 기준값 확보
- [ ] Phase 1 — 규격 언어 중립화, 공용 집계기 분리, .NET 재검증
- [ ] Phase 2 — Node 구현과 18셀 통과
- [ ] Phase 3 — Java 구현과 18셀 통과
- [ ] Phase 4 — Kotlin 구현과 18셀 통과 (coroutine stub 결정 기록)
- [ ] Phase 5 — C++ 구현과 18셀 통과
- [ ] Phase 6 — 5언어 순차 재측정 3회, 중앙값 채택
- [ ] Phase 6 — 통합 보고서 §7.2 구성 전부 작성
- [ ] 결정 기록 `FB-*` 정리, 후속 후보 이관
