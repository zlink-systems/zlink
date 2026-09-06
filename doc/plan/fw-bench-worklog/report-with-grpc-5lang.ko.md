# with-grpc 5언어 통합 보고서 — .NET · Node · Java · Kotlin · C++

> 작성일: 2026-09-07 (main `64d2ad5b67`, 0.17.0)
> 대상 규격: [`with-grpc-local.ko.md`](../../../framework/doc/framework/common/bench/with-grpc-local.ko.md)
> 계획: [`../framework-bench-with-grpc-5lang-plan.ko.md`](../framework-bench-with-grpc-5lang-plan.ko.md)
> 결정 기록: [`decisions.ko.md`](./decisions.ko.md)
> 언어별 요약: [`.NET`](./bench-dotnet-summary.ko.md) · [`Node`](./bench-node-summary.ko.md) ·
> [`Java`](./bench-java-summary.ko.md) · [`Kotlin`](./bench-kotlin-summary.ko.md) · [`C++`](./bench-cpp-summary.ko.md)
> 문서 규칙: 이 문서는 `plan/` 아래에 있으므로 공개 문서에서 링크하지 않는다

## 1. 요약

다섯 언어 90셀 가운데 78셀을 측정해 규격 §7.2의 판정 두 식을 계산했다. **게재 조건을 만족한
판정은 스무 개 중 두 개뿐이고 그 둘은 모두 기준 0.80에 미달한다**(`.NET` 0.084, C++ 0.774).
나머지 열여덟 개는 `unsupported`이며 **막힌 이유가 언어마다 다르다는 것이 이 캠페인의 결과다.**

가장 큰 관측은 판정표 밖에 있다. **동시 미완료 요청 100개를 설정했을 때 C API를 직접 지나는
두 실행(C, C++)은 오류 없이 그 깊이를 지탱하고, 관리형 런타임 binding 셋(`.NET`·Node·Java)은
서로 다른 세 가지 방식으로 무너진다.** 이 대비가 조사 범위를 Core 공통 경로에서 각 관리형
binding의 완료 전달 계층으로 옮긴다.

이 수치들은 다섯 개의 서로 다른 측정 구간에서 나왔고, 한 commit에서 언어당 1회씩 도는
통합 검증 구간으로 그 구간을 가로지른 비교가 성립하는지 시험했다. **자기 구간에서 재현성
조건을 통과한 행은 하나도 빠짐없이 ±10% 안에서 재현됐다**(§2.4).

판정 기준 자체도 현재 기준선 위에서는 도달할 수 없다. formula 1의 공유 분모인 `zlink-c`
request-window @4096이 독립된 세 측정 구간에서 재현성 조건을 넘지 못했고, 규격 §7.2가 두
payload 크기를 모두 요구하므로 **어느 언어도 자기 품질과 무관하게 그 판정을 게재할 수 없다.**

## 2. 측정 조건

### 2.1 데이터는 다섯 구간에서 나왔다

**이 보고서의 수치는 하나의 측정 구간에서 나오지 않았다.** 언어마다 자기 전용 gated 구간이
있었고 다섯 구간의 commit이 서로 다르다. 이 사실을 먼저 적는 이유는, 언어를 가로지르는 결론
(§4)이 구간을 가로지른 비교 위에 서 있기 때문이다.

| 구간 | 언어 | 측정 commit | 측정 시각 (KST) | run 구성 |
|---|---|---|---|---|
| Phase 0 | `.NET` | `d96e4b7031` | 2026-09-07 00:33:48 ~ 00:52:17 | ROUTER 3회 + DEALER 1회 (총 8 run) |
| Phase 2 | Node | `9b47698915` | 2026-09-07 02:04:43 ~ 02:12:23 | ROUTER 3회 + DEALER 1회 |
| Phase 3 | Java | `dcded04dbe` | 2026-09-07 03:06:55 ~ 03:53:18 | ROUTER 3회 + DEALER 1회 + C 기준선 3회 |
| Phase 4 | Kotlin | `129627f8a5` | 2026-09-07 04:13:38 ~ 04:53:14 | ROUTER 3회 + DEALER 1회 |
| Phase 5 | C++ | `85e7a7613e` | 2026-09-07 05:23:04 ~ 05:33:47 | ROUTER 3회 + DEALER 1회 |

대표값은 각 구간 ROUTER 3회의 중앙값이다. Kotlin과 C++ 구간은 `zlink-c` 기준선을 다시 재지
않고 Java 구간의 3회를 재사용했고, 그래서 **`zlink-c`의 독립 측정은 세 번뿐이다**(§6).

### 2.2 통합 검증 구간

구간이 다르다는 사실이 남긴 위험은 하나다. 각 job이 "구간 사이에 바뀐 것은 bench 코드와
집계기와 문서뿐"이라고 확인했으나 **그 확인은 논증이지 측정이 아니다.** 그래서 다섯 언어를
한 commit에서 언어당 1회씩 순차로 실행하는 통합 검증 구간을 한 번 두었다. 목적은 새 중앙값을
만드는 것이 아니라 **구간을 가로지른 비교가 성립하는지 시험하는 것**이다.

논증 쪽도 이 보고서가 직접 확인했다. 다섯 측정 commit 각각에서 `64d2ad5b67`까지, `doc/`와
bench 디렉터리 밖에서 바뀐 파일은 **0개**다. 곧 Core·binding·framework source는 다섯 구간
내내 동일하다.

```text
git diff --name-only <span-commit>..64d2ad5b67 | grep -v -E '(^doc/|/doc/|bench)'
  d96e4b7031  0 files
  9b47698915  0 files
  dcded04dbe  0 files
  129627f8a5  0 files
  85e7a7613e  0 files
```

bench 코드 쪽은 사정이 다르며 이것이 검증이 필요한 두 번째 이유다. `.NET` harness는 자기
측정 이후 `28445bfd68`에서 142행이 늘었다(포화 계측기 선언과 진단값 구조화). 그 commit은
측정 경로를 바꾸지 않았다고 적고 인수 fixture로 Phase 0을 재현하지만, 검증 run은 그 주장을
실제로 시험한다.

**검증 결과는 §2.4에 싣는다. 검증이 통과하더라도 이 보고서의 데이터는 다섯 구간에서 나온
데이터이며, 한 구간에서 잰 것처럼 쓰지 않는다.**

### 2.3 고정 조건과 환경

| 항목 | 값 |
|---|---|
| CPU | Intel Core Ultra 7 265K, 논리 core 20 |
| OS·커널 | Ubuntu 24.04.4 LTS, 6.6.87.2-microsoft-standard-WSL2 |
| transport | loopback `127.0.0.1` |
| payload | 1024, 4096 bytes (protobuf `bytes body` 기준) |
| `request_window` | 100 |
| send concurrency | 8 |
| active duration | 5초 (C 기준 bench는 3초) |
| request timeout | 30초 |
| drain 상한 | 30초 |
| 빌드 | Release |
| ZLink Core | 0.17.0 |
| 대표값 | ROUTER 3회 중앙값 |

포트 대역은 규격 §9대로 언어마다 고정한다(`dotnet` 5071–5077, `node` 5081–5087, `java`
5091–5097, `kotlin` 5101–5107, `cpp` 5111–5117). 대역이 점유돼 있으면 runner는 다른 포트로
옮기지 않고 중단한다. 옮기면 기록된 endpoint와 측정된 endpoint가 어긋난다.

### 2.3.1 언어별 런타임과 gRPC 라이브러리

| 언어 | 런타임 | gRPC | protobuf | framework |
|---|---|---|---|---|
| `.NET` | SDK 8.0.130 | Grpc.AspNetCore 2.62.0, Grpc.Net.Client 2.62.0 | Google.Protobuf 3.29.1 | `Zlink.Framework` |
| Node | v22.23.2 | `@grpc/grpc-js` 1.14.4, `@grpc/proto-loader` 0.7.15 | — | `packages/framework` (측정 불가, §3) |
| Java | Temurin 22.0.2 | grpc-java 1.72.0 (`grpc-netty-shaded`) | protobuf-java 4.30.2 | `zlink-framework-core` 0.10.0 |
| Kotlin | Temurin 22.0.2, Kotlin 2.2.21, coroutines 1.9.0 | grpc-java 1.72.0 + **grpc-kotlin 1.4.1 coroutine stub** | protobuf-java 4.30.2 | `zlink-framework-kotlin` 0.10.0 |
| C++ | GCC 13.3.0, C++20 | **시스템 `libgrpc++` 1.51.1** (2022년 배포판) | 3.21.12 | 미구현 |
| C (기준) | GCC 13.3.0 | grpc++ 1.51.1 | 3.21.12 | 해당 없음 |

gRPC 서버는 다섯 언어 모두 **각 언어의 기본 구성**이며 튜닝하지 않았다(ASP.NET Core,
`@grpc/grpc-js` `Server`, `io.grpc.ServerBuilder.forPort`, `grpc::ServerBuilder` 동기 server).
이 사실은 결과 해석의 조건이다.

Kotlin은 규격 §8.1이 요구하는 coroutine stub을 실제로 사용했다. blocking stub으로 대체하지
않았으므로 사유를 기록할 항목이 없다. ZLink 쪽이 suspend 인터페이스인데 gRPC만 blocking
stub을 쓰면 비교가 Kotlin에 유리하게 기운다.

### 2.3.2 warmup은 언어마다 다르고, 근거를 각자 냈다

| 언어 | warmup | 근거 |
|---|---|---|
| `.NET` | 1000회 | Phase 0 정본 |
| Node | 1000회 | `.NET` 정본을 따름 |
| Java | **20초** (2초 구간 10개) | `grpc-java-request-serial`이 2240 → 5998로 2.7배 상승 후 평탄, 6~8초에 안정 |
| Kotlin | **20초** (2초 구간 10개) | `grpc-kotlin-request-serial`이 1580 → 5004로 3.2배 상승 후 평탄, 약 6초에 안정 |
| C++ | **5초** (0.5초 구간 10개) | 첫 0.5초 구간이 이미 active 값의 98.6%, 상승 추세 없음. JIT가 없다 |

JVM 두 언어에 `.NET`의 warmup 1000회를 그대로 적용했다면 `grpc-java-request-serial`은 약
0.2초만 예열되어 정상 상태의 40% 수준을, `grpc-kotlin`은 32% 수준을 측정했을 것이다. 규격
§8.2가 warmup을 언어별로 두게 한 근거가 그 구간별 자료에 그대로 나타난다.

### 2.3.3 측정 격리 (G7)

다섯 구간과 검증 구간 전부가 `flock --exclusive /tmp/zlink-perf.lock` 아래에서 실행됐고,
모든 빌드는 구간 **밖**에서 끝났다. run마다 시작 직전 1분 load average를 확인하고 2.0 이상이면
대기했으며, 판독값을 전부 기록했다. `dotnet build`가 남기는 MSBuild node 재사용 daemon은 잠금
file descriptor를 상속해 실행이 끝난 뒤에도 잠금을 유지하므로 `MSBUILDDISABLENODEREUSE=1`로
막았다. 이 설정이 없으면 측정 직렬화가 조용히 무력화된다.

### 2.4 통합 검증 구간의 결과 — 다섯 언어 전부 통과

**결론: 자기 구간에서 재현성 조건을 통과한 행은 하나도 빠짐없이 검증 run에서 ±10% 안에
들어왔다. 따라서 구간을 가로지른 비교가 성립하며, §3~§8의 기존 데이터를 그대로 쓴다.**

구간 정보는 아래와 같다.

| 항목 | 값 |
|---|---|
| commit | `64d2ad5b67` (다섯 구간 전부와 동일한 Core·binding·framework source) |
| 구간 | 2026-09-07T05:54:29+09:00 ~ 06:27:10+09:00, `flock --exclusive /tmp/zlink-perf.lock` 단일 구간 |
| 구성 | 언어당 ROUTER 1 run + `zlink-c` 기준선 1 run, 순차 실행 |
| 빌드 | 전부 구간 **밖**에서 완료. 구간 안에서 어떤 컴파일도 실행되지 않았다 |
| 종료 코드 | 6개 run 전부 rc=0 |

run마다 시작 직전 loadavg를 판독하고 2.0 이상이면 대기했다. 게이트가 실제로 세 번 발동했다.

| run | 최초 판독 | 대기 | 시작 판독 |
|---|---|---|---|
| `dotnet` | 0.86 | — | 0.86 |
| `node` | 4.86 | 70초 | 1.71 |
| `java` | 1.72 | — | 1.72 |
| `kotlin` | 5.19 | 70초 | 1.78 |
| `cpp` | 4.86 | 90초 | 1.91 |
| `c` (기준선) | 2.70 | 30초 | 1.77 |

측정이 실제로 실행됐다는 것은 로그가 아니라 프로세스 표본으로 확인했다. run이 도는 동안
10초마다 `ps`로 client와 server 프로세스를 표본화했고, 판독은 `dotnet` 12/13, `node` 21/22,
`java` 56/57, `kotlin` 56/57, `cpp` 13/14, `c` 6/7이다(마지막 표본은 프로세스 종료 직후라
비어 있다).

#### 언어별 대조 결과

각 언어의 검증 run을 그 언어의 기존 중앙값과 행 단위로 대조했다. 허용 범위는 G5와 같은
±10%다.

세 칸은 겹치지 않으며 합이 그 언어의 행 수다. 정지 셀은 처리량이 성립하지 않으므로 범위
판정 대상이 아니고 따로 센다.

| 언어 | 행 수 | 범위 안 | 범위 밖 | 정지 셀 | 판정 |
|---|---|---|---|---|---|
| `.NET` | 18 | 17 | 1 | 0 | **통과** |
| Node | 12 | 10 | 0 | 2 | **통과** |
| Java | 18 | 16 | 0 | 2 | **통과** |
| Kotlin | 18 | 15 | 1 | 2 | **통과** |
| C++ | 12 | 12 | 0 | 0 | **통과** |
| C (기준선) | 12 | 10 | 2 | 0 | **통과** |

Node와 C++의 행 수가 12인 것은 framework 여섯 셀이 존재하지 않기 때문이다(§3). Node의 정지
셀 두 개는 집계기가 각각 +44.0%·-82.9%로 계산하지만 그 값은 처리량이 아니므로 범위 밖으로
세지 않는다.

**범위를 벗어난 행이 왜 통과인가.** 허용 범위 밖으로 계산된 행은 정지 셀까지 합쳐 일곱
개인데 **그 일곱 개가 전부 두 부류 가운데 하나이고, 예외가 없다.**

| 벗어난 행 | 검증 run 편차 | 자기 구간에서의 상태 |
|---|---|---|
| `zlink-framework-dotnet` request-window @1024 | +18.1% | **G5 29.6% 미달**(§9) |
| `zlink-framework-kotlin` send-saturation @1024 | +31.2% | **G5 12.8% 미달**(§9) |
| `zlink-c` request-serial @1024 | +33.3% | **G5 37.9% 미달**(§9) |
| `zlink-c` request-window @4096 | **-27.6%** | **G5 25.7~75.7% 미달**(§6) |
| `zlink-node` request-window @1024 | +44.0% | **정지 셀.** 처리량이 성립하지 않는다 |
| `zlink-node` request-window @4096 | -82.9% | **정지 셀** |
| `zlink-kotlin` request-window @4096 | 0.0 → 35.2 | **정지 셀** |

곧 **자기 구간에서 이미 재현성 조건을 넘지 못한 행이 넷이고, 처리량이 성립하지 않는 정지
셀이 셋이다.** 정지 셀의 숫자는 완료를 멈춘 socket을 active 구간 길이로 나눈 산술이므로
±10%를 적용할 대상이 아니며, 그 값이 크게 흔들리는 것은 §5.2가 기록한 정지의 성질 그대로다.
**자기 구간에서 G5를 통과한 행 가운데 검증 run에서 범위를 벗어난 것은 하나도 없다.**

정지도 그대로 재현됐다. `zlink-java` request-window @1024와 `zlink-kotlin` request-window
@1024는 검증 run에서도 아무것도 완료하지 않았고, `zlink-node`는 두 크기 모두에서 다시
정지했다. **정지는 특정 구간의 사고가 아니다.**

#### 게재된 두 판정의 입력이 모두 재현된다

| 판정 | 분자 편차 | 분모 편차 |
|---|---|---|
| `zlink-dotnet / zlink-c` @1024 | -5.6% | -3.9% |
| `zlink-cpp / zlink-c` @1024 | +1.6% | -3.9% |

분자와 분모가 모두 허용 범위 안이므로 두 게재값의 근거는 유지된다.

**다만 C++ 쪽에서 반드시 함께 적어야 할 것이 하나 나왔다.** 분자와 분모가 각각 허용 범위
안인데도 **두 값으로 계산한 비율은 0.774에서 0.819로 움직여 기준선 0.80을 넘어간다.**
`.NET`은 0.084에서 0.083으로 사실상 그대로다.

이 관측을 정확히 읽어야 한다.

- **C++가 통과했다는 뜻이 아니다.** 검증 run은 **1회**이고 게재 조건인 G5는 3회를 요구한다.
  1회 값으로는 어떤 판정도 만들 수 없다. 규격 §7.2가 두 payload 크기를 모두 요구하는데
  @4096은 여전히 분모 때문에 게재할 수 없다(§6). **게재값은 0.774 그대로다.**
- **이 관측이 실제로 말하는 것은 §3.1의 경고를 한 번 더 확인했다는 것이다.** 이 판정은
  분자와 분모가 각각 재현성 조건 안에서 움직이는 것만으로도 기준선을 넘나들 만큼 임계값에
  가깝다. 포화 제외선을 0.00003 차이로 비켜간 것과 같은 성격의 취약함이며, **0.774도
  0.819도 이 셀의 확정값으로 인용해서는 안 된다.**

#### 기준선의 불안정이 다시 확인됐다

`zlink-c` request-window @4096이 검증 run에서 기존 중앙값보다 **27.6% 낮게** 나왔다. 이는
독립된 네 번째 판독이며, **§6이 세 구간에서 기록한 그 행의 불안정을 이 구간이 새로 확인한
것이다.** Kotlin·C++ 구간이 Java 구간의 값을 재사용했던 것과 달리 이 판독은 새로 측정한
값이므로 근거를 하나 더한다. formula 1의 공유 분모가 안정되기 전까지 어떤 언어도 @4096
판정을 게재할 수 없다는 결론은 그대로이며, 오히려 강해진다.

`.NET` harness가 자기 측정 이후 142행 늘었다는 §2.2의 항목도 이 구간이 답한다. **바뀐
harness로 다시 잰 `.NET` 12행이 전부 허용 범위 안이고**, 유일하게 벗어난 행은 자기 구간에서
이미 G5 29.6%로 미달했던 framework request-window다. 곧 그 commit이 "측정 경로를 바꾸지
않았다"고 적은 것은 실측으로 확인된다.

#### 이 검증이 바꾸지 않는 것

**검증이 통과했다고 해서 이 보고서의 데이터가 한 구간에서 나온 것이 되지는 않는다.**
§3~§8의 수치는 여전히 다섯 개의 서로 다른 구간에서, 서로 다른 commit에서 측정된 값이고,
대표값은 각 구간의 ROUTER 3회 중앙값이다. 이 검증 구간이 보인 것은 **그 구간들을 가로질러
비교해도 결론이 바뀌지 않는다는 것**이지, 다섯 구간이 하나가 됐다는 것이 아니다.
검증 run 자체의 값은 1회 측정이므로 어떤 표에도 중앙값으로 들어가지 않는다.

## 3. 주 표 — 판정 가능 여부와 그 이유

이 캠페인의 핵심 표다. 계획 초안의 주 표는 "언어별 계층 비용 비율"이었으나 실측이 그 표를
쓸 수 없게 만들었다. **값이 없어서가 아니라, 각 칸이 비어 있는 이유가 서로 다르고 그 이유들이
이 캠페인의 답이기 때문이다.** 빈칸을 추정값으로 채우지 않는다.

판정식은 규격 §7.2 그대로이고 기준 패턴은 `request-window`다.

```text
formula 1   zlink-<lang> / zlink-c                  >= 0.80   binding 계층
formula 2   zlink-framework-<lang> / zlink-<lang>   >= 0.80   framework 추가 비용
```

게재 조건은 FB-011대로 **분자와 분모가 모두 G5(3회 중앙값 대비 ±10%)를 통과**할 때이고,
언어 통과 조건은 FB-005대로 **두 payload 크기 모두**에서 기준을 만족할 때다.

| 언어 | formula 1 @1024 | formula 1 @4096 | formula 2 @1024 | formula 2 @4096 | 게재하지 못한 칸을 막은 행 |
|---|---|---|---|---|---|
| **`.NET`** | **0.084 게재 · 미달** | `unsupported` | `unsupported` | `unsupported` | f1@4096: 분모 `zlink-c-request-window@4096` G5 25.7%. f2 양쪽: 분자 `zlink-framework-dotnet-request-window` G5 29.6%(@1024)·11.5%(@4096) |
| **Node** | `unsupported` | `unsupported` | `unsupported` | `unsupported` | f1 양쪽: 분자 `zlink-node-request-window`가 처리량을 내지 않는다(client socket 정지). 같은 셀이 event loop 사용률 1.000으로 포화 셀이기도 하다. f2 양쪽: 분자 `zlink-framework-node`를 측정할 수 없다 — 공개 protobuf codec에 bytes 형이 없다 |
| **Java** | `unsupported` | `unsupported` | `unsupported` | `unsupported` | f1 양쪽: 분자 `zlink-java-request-window`가 처리량을 내지 않는다(`peak_in_flight` 100 / abandoned 100 / 완료 0). f2 양쪽: **분모가 같은 행**이다. f1@4096은 분모 `zlink-c` G5 28.7%로도 막힌다 |
| **Kotlin** | `unsupported` | `unsupported` | `unsupported` | `unsupported` | f1 양쪽: 분자 `zlink-kotlin-request-window`가 처리량을 내지 않는다(`peak_in_flight` 100 / abandoned 100 / 완료 0). f2 양쪽: **분모가 같은 행**이다. f1@4096은 분모 `zlink-c` G5 28.7%로도 막힌다. 이 분자는 Java와 같은 `systems.zlink:zlink` artifact 위에 있으므로 독립 사례가 아니다 |
| **C++** | **0.774 게재 · 미달** | `unsupported` | `unsupported` | `unsupported` | f1@4096: 분모 `zlink-c-request-window@4096` G5 28.7%. f2 양쪽: 분자 `zlink-framework-cpp`가 **이 Phase에서 미구현** |

**스무 칸 중 게재된 것은 둘이고 둘 다 기준 미달이다. 통과한 언어는 없다.**

이 표에서 읽어야 하는 것은 `unsupported`의 개수가 아니라 사유의 종류다. 다섯 언어가 서로
다른 다섯 지점에서 막혔다.

| 언어 | 막힌 성격 |
|---|---|
| `.NET` | **측정됐고 게재됐고 미달했다.** 나머지 세 칸은 재현성이 막았다 — 자기 framework 행 하나와 공유 분모 하나 |
| Node | **정지와 포화가 함께 분자를 무효로 만들었고**, framework 쪽은 제품의 선언 격차다. 공개 codec이 규격이 정한 payload를 표현하지 못한다 |
| Java | **분자 행이 아무것도 완료하지 않는다.** reply가 만들어진 뒤 사라진다 |
| Kotlin | Java와 같다. 더하는 것은 새 사례가 아니라 **호출 형태와 소켓 종류를 바꿔도 같다**는 사실이다 |
| C++ | **깊이는 완전히 지탱하는데 요청당 비용에서 미달했다.** framework 칸은 결함이 아니라 단순 미구현이다 |

Node의 f2와 C++의 f2를 같은 `unsupported`로 읽으면 안 된다. **Node는 제품의 공개 codec이
이진 payload를 실을 수 없다는 제약이고, C++는 이 Phase가 구현하지 않았을 뿐이다.** 앞은
제품 항목이고 뒤는 작업 항목이다.

### 3.1 게재된 두 판정은 각각 조건을 달고 읽어야 한다

**`.NET` `zlink-dotnet / zlink-c` @1024 = 0.084 (기준 0.80 미달).** 이 값은 한 번 철회됐다가
다시 게재됐다. 처음에는 "동시 요청 8개를 유지한 실험을 83개를 유지한 실험으로 나눈 값"이라는
이유로 `unsupported`가 됐다. 그 뒤 계측이 전제를 반증했다 — `peak_in_flight`가 100/100에
도달하고 abandoned가 0이며 제출 경로에 잠금도 제한된 queue도 없다. **깊이 8은 harness 결함이
아니라 `.NET` raw binding의 실제 성질이고, formula 1은 바로 그것을 재려고 존재하는 식이다.**
원인이 규명된 실패값을 감추는 것이 오히려 정보를 버리는 일이므로 실패로 게재한다.

**C++ `zlink-cpp / zlink-c` @1024 = 0.774 (기준 0.80 미달). 이 값은 아래 두 조건과 함께
읽지 않으면 오독된다.**

- **포화 제외선을 0.00003 차이로 비켜갔다.** 이 셀의 선언 계측기 판독은 세 run에서
  0.954 / 0.944 / 0.950이고 중앙값은 **0.94997**, 제외 임계값은 **0.95**다. 곧 이 판정은
  client 포화 배제를 사실상 동점에서 면한 상태로 게재된 값이다. **run 하나만 달랐어도 Node처럼
  `unsupported`가 됐을 값으로 취급해야 한다.** 이 client 구성이 transport보다 먼저 자기
  application thread를 채웠다는 사실과 반드시 함께 읽는다.
- **격차는 깊이에서만 나타난다.** `zlink-cpp`는 request-serial에서 `zlink-c`와 같거나 낫고
  (8.97 대 8.29, 8.79 대 8.35 KOPS), send-saturation에서도 같거나 낫다(696.4 대 699.4,
  514.7 대 485.8 KMSG/s). 벌어지는 곳은 request-window 하나이고, 거기서 **C++는 더 많이
  미완료로 유지하면서(99.9 대 86.9) 더 적게 완료한다.** 요청당 client 비용이 C보다 크다는
  뜻이다. 요청마다 coroutine frame과 공유 상태가 하나씩 생기는 C++ 경로의 성질이 후보이지만
  프로파일하지 않았으므로 후보로만 적는다.

- **통합 검증 구간이 같은 취약함을 다른 방식으로 한 번 더 보였다.** 검증 run에서 분자는
  +1.6%, 분모는 -3.9%로 **둘 다 허용 범위 안**인데 그 둘로 계산한 비율은 **0.819**가 되어
  기준선 0.80을 넘어간다(§2.4). 곧 이 판정은 분자와 분모가 재현성 조건 안에서 움직이는
  것만으로도 기준선을 넘나든다. **0.819를 통과로 읽으면 안 된다** — 1회 측정이고 게재
  조건은 3회를 요구한다. 같은 이유로 **0.774도 이 셀의 확정값이 아니다.**

**따라서 0.774를 "C++ binding이 C의 77%를 낸다"로 인용하면 안 된다.** 자료가 뒷받침하는 문장은
"이 조건의 request-window 한 셀에서 `zlink-cpp`의 완료율이 `zlink-c`의 0.774배였고, 그 셀은
client 포화 제외선 바로 위에 있었으며, 기준선 0.80과의 거리가 재현성 허용 범위보다 작다"이다.

## 4. 완료 전달 관측 — 이 캠페인의 중심 결과

§3의 사유들을 하나로 묶는 관측이다. 판정표 밖에 있지만 이 캠페인이 낸 가장 큰 결과다.

`request_window`를 100으로 설정했을 때 각 raw binding이 **실제로 유지한 미완료 요청 수**와
그때의 오류·유실을 나란히 놓는다. 실제 깊이는 처리량 × 평균 지연으로 계산한 값이다
(Little's law).

| 실행 | C API와의 거리 | 설정 window 100에서의 관측 | 오류 | abandoned |
|---|---|---|---|---|
| `zlink-c` | 직접 | 깊이 **86.9 ~ 90.7** 유지 (Phase 0 자체 표는 83.5·87.6) | 0 | 0 |
| `zlink-cpp` | 얇은 wrapper | 깊이 **99.9**(@1024) · **99.8**(@4096) 유지 | **0** | **0** |
| `zlink-dotnet` | 관리형 | 깊이 **8.0**(@1024) · 7.7(@4096)에 묶임. **유실 없음** | 0 | 0 |
| `zlink-node` | 관리형 | 깊이 8까지 정상, 그 위에서 **client socket 정지**. 여덟 셀 중 일곱이 정지 | 25~100 | 0 |
| `zlink-java` | 관리형 | 미완료 요청 2 이상에서 유실 시작, window 100에서 **완료 0** | 100 | **100** |
| `zlink-kotlin` | 관리형 | Java와 같은 서명. **Java binding 재사용이므로 독립 관측이 아니다** | 100 | 100 |

`zlink-c`의 깊이는 구간마다 83.5에서 90.7 사이로 움직인다. 위 표의 86.9~90.7은 완료 전달을
대비할 때 인용한 두 구간의 값이고, Phase 0 자체 표는 같은 행을 @1024 83.5, @4096 87.6으로
기록했다. **어느 판독에서도 오류는 0이고 깊이는 설정값 100의 8할을 넘는다.** 이 항목이 답하는
질문에서 갈리는 것은 그 폭이 아니라 8과 100의 차이다.

**C API를 직접 지나는 두 실행은 모두 깊이를 지탱하고, 관리형 런타임 binding 셋은 모두
무너진다.** 그리고 무너지는 방식이 셋 다 다르다.

- `.NET`은 **유실하지 않는다.** 깊이가 오르지 않을 뿐이다. `peak_in_flight`는 100/100에
  도달하고 abandoned는 0이며, 요청당 호출 thread CPU 32.2 µs를 왕복 지연 221 µs로 나눈
  예측 깊이 6.9가 관측값 8.0과 일치한다. 동시성 상한이 아니라 **제출 처리율 상한**이다.
  같은 조건 C는 요청당 4.0 µs다.
- **Node는 정지한다.** 완료가 전진을 멈추고 남은 요청이 request timeout 30초에 그대로
  도달한다. p50은 끝까지 1 ms 미만이므로 느려진 것이 아니라 멈춘 것이다.
- **Java는 reply를 잃는다.** 프로세스 안 최소 재현에서 **server가 101건을 모두 회신했는데
  client는 43건만 완료했다.** 회신은 만들어졌고 server의 제출과 client의 완료 사이에서
  사라진다.

Kotlin은 다섯 번째 독립 관측이 아니다. `bindings/kotlin`에는 자체 native binding이 없고
`zlink-java`와 같은 `systems.zlink:zlink` artifact를 사용한다. **Kotlin이 더하는 것은 새
사례가 아니라, 같은 실패가 다른 호출 형태와 두 소켓 종류에서 모두 나타난다는 사실이다.**
Java는 `CompletionStage`를 블로킹 `get()`으로 기다렸고 Kotlin은 coroutine `await()`로
기다렸다. 서로 다른 코드인데 같은 지점에서 멈추고, ROUTER와 DEALER 양쪽에서 멈춘다.

**이 관측에는 반드시 두 한계를 함께 적는다.**

1. **이 실험이 Core를 지목하지 않는다는 것이지, Core가 건전하다는 것이 아니다.** C와 C++가
   지탱한 것은 이 bench가 만든 부하 형태에서다. 다른 사용 형태에서 Core가 같은 결함을 내지
   않는다는 근거는 이 자료에 없다.
2. **세 실패의 서명이 서로 다르므로 하나의 공통 원인을 가정할 수 없다.** 묶이지 않음·정지·
   유실은 다른 증상이다. "관리형 binding의 완료 전달 계층"은 조사 범위이지 원인이 아니다.

대조군 두 가지를 함께 남긴다. 첫째, **gRPC 행은 다섯 언어 전부에서 깊이를 유지한다**
(`grpc-c` 98.9, `grpc-dotnet` 93.6, `grpc-java` 97.6, `grpc-kotlin` 92.0, `grpc-cpp` 99.9,
모두 오류 0). 같은 harness가 같은 run에서 그 깊이를 내므로 window 로직 자체는 동작한다.
둘째, **`.NET`의 framework 행은 같은 프로세스에서 깊이 102.7 / 98.7을 유지한다.** 곧 `.NET`의
깊이 8은 런타임의 성질이 아니라 raw 제출 경로에 한정된 성질이다.

framework 쪽에는 반대 방향의 관측이 하나 더 있다. **Java와 Kotlin의 framework 행은 설정
window 100에 대해 `peak_in_flight` 10~12, 실제 깊이 4.5에서 멈춘다**(abandoned 0). 같은
binding 위의 raw 행이 정지하는 그 구간에 **framework는 애초에 들어가지 않는다.** 상위 계층이
더 견고해서가 아니라 덜 깊게 들어가기 때문이다. Java의 `CompletionStage` 경로와 Kotlin의
suspend 경로가 같은 4.5에서 멈추므로 **그 깊이 상한을 정하는 것은 client API 계층이 아니다.**
다만 Kotlin 행이 Java 행의 framework server를 공유하므로 상한이 server 쪽인지 framework core의
client 쪽인지는 이 캠페인이 가르지 않는다.

## 5. 언어별 3자 표

각 언어 ROUTER 3회의 중앙값이다. 처리량 단위는 request 계열이 `KOPS`, `send-saturation`이
`KMSG/s`다. client CPU는 논리 core 20개 기준 백분율이고, 포화 판정은 그 값이 아니라 언어별
선언 계측기로 한다(§5.6). `*`는 포화 셀, `깊이`는 처리량 × 평균 지연이다.

**정지한 셀에는 처리량을 싣지 않는다.** 그 자리에 남는 숫자는 그 경로가 지탱하는 속도가
아니라 완료를 멈춘 socket을 active 구간 길이로 나눈 산술 결과이며, 정상 셀과 나란히 놓으면
같은 종류의 값으로 읽힌다.

### 5.1 `.NET`

| 패턴 | 구현 | 처리량 | 평균 ms | p95 ms | p99 ms | client CPU% | 깊이 | drain ms |
|---|---|---|---|---|---|---|---|---|
| request-serial @1024 | `grpc-dotnet` | 6.758 | 0.146 | 0.272 | 0.323 | 6.5 | 1.0 | — |
| request-serial @1024 | `zlink-dotnet` | 7.595 | 0.130 | 0.156 | 0.188 | 3.6 | 1.0 | — |
| request-serial @1024 | `zlink-framework-dotnet` | 1.995 | 0.500 | 0.656 | 0.834 | 3.4 | 1.0 | — |
| request-window @1024 | `grpc-dotnet` | 198.787 | 0.471 | 1.442 | 1.926 | 36.9 | 93.6 | — |
| request-window @1024 | `zlink-dotnet` | 36.034 | 0.221 | 0.356 | 0.420 | 5.8 | **8.0** | — |
| request-window @1024 | `zlink-framework-dotnet` | 3.663 | 28.045 | 104.709 | 174.524 | 3.7 | 102.7 | — |
| send-saturation @1024 | `grpc-dotnet` | 45.335 | 0.090 | 0.112 | 0.147 | 9.5 | — | 240 |
| send-saturation @1024 | `zlink-dotnet` | 411.875 | 0.377 | 1.141 | 9.716 | 16.0 | — | 360 |
| send-saturation @1024 | `zlink-framework-dotnet` | 46.629 | 1770.695 | 3359.400 | 3513.137 | 31.9 | — | **16674** |
| request-serial @4096 | `grpc-dotnet` | 6.857 | 0.143 | 0.190 | 0.234 | 5.7 | 1.0 | — |
| request-serial @4096 | `zlink-dotnet` | 7.534 | 0.130 | 0.158 | 0.235 | 4.0 | 1.0 | — |
| request-serial @4096 | `zlink-framework-dotnet` | 2.207 | 0.451 | 0.554 | 0.646 | 3.6 | 1.0 | — |
| request-window @4096 | `grpc-dotnet` | 127.193 | 0.753 | 1.732 | 2.097 | 33.9 | 95.8 | — |
| request-window @4096 | `zlink-dotnet` | 34.390 | 0.225 | 0.362 | 0.439 | 5.7 | **7.7** | — |
| request-window @4096 | `zlink-framework-dotnet` | 2.458 | 40.134 | 105.397 | 129.874 | 2.8 | 98.7 | — |
| send-saturation @4096 | `grpc-dotnet` | 41.515 | 0.102 | 0.129 | 0.171 | 9.7 | — | 216 |
| send-saturation @4096 | `zlink-dotnet` | 383.077 | 0.615 | 4.302 | 8.406 | 19.0 | — | 363 |
| send-saturation @4096 | `zlink-framework-dotnet` | 43.863 | 1475.071 | 3071.936 | 3190.037 | 30.8 | — | **13286** |

18셀 전부 실행됐고 실패 0, 오염 0이다. 포화 셀은 없다.

`zlink-framework-dotnet`의 send 셀 drain 시간 16.7초를 특히 남긴다. 이 값은 harness 주석이
아니라 결과다. 규격 §3의 settle 계약을 고정 200 ms 대기에서 server drain 확인으로 바꾸기
전에는, 5초 send 셀 뒤 약 5.5초 일찍 반환해 **셀 하나가 다음 셀을 죽이고 있었다.**

### 5.2 Node

| 패턴 | 구현 | 처리량 | 평균 ms | p95 ms | p99 ms | ELU | 깊이 | drain ms |
|---|---|---|---|---|---|---|---|---|
| request-serial @1024 | `grpc-node` | 4.19 | 0.238 | 0.405 | 0.581 | 0.660 | 1.0 | — |
| request-serial @1024 | `zlink-node` | 9.99 | 0.100 | 0.160 | 0.267 | **0.983\*** | 1.0 | — |
| request-window @1024 | `grpc-node` | 15.43 | 6.483 | 8.537 | 13.264 | 0.868 | 100 | — |
| request-window @1024 | `zlink-node` | **처리량 없음 — 정지** | — | — | — | **1.000\*** | — | — |
| send-saturation @1024 | `grpc-node` | 11.37 | 0.314 | 0.468 | 0.598 | 0.701 | — | 219 |
| send-saturation @1024 | `zlink-node` | 291.43 | 71.199 | 94.267 | 98.894 | 0.808 | — | 609 |
| request-serial @4096 | `grpc-node` | 4.52 | 0.221 | 0.326 | 0.505 | 0.646 | 1.0 | — |
| request-serial @4096 | `zlink-node` | 10.20 | 0.098 | 0.126 | 0.222 | **0.985\*** | 1.0 | — |
| request-window @4096 | `grpc-node` | 11.52 | 8.684 | 11.721 | 17.741 | 0.811 | 100 | — |
| request-window @4096 | `zlink-node` | **처리량 없음 — 정지** | — | — | — | **1.000\*** | — | — |
| send-saturation @4096 | `grpc-node` | 10.76 | 0.333 | 0.492 | 0.699 | 0.708 | — | 210 |
| send-saturation @4096 | `zlink-node` | 168.08 | 0.519 | 3.406 | 5.490 | **1.000\*** | — | 293 |

`zlink-framework-node`의 여섯 셀은 **측정하지 않았다.** framework 호스트 자체는 동작하지만
공개 codec `packages/framework-codec-protobuf`에 bytes 형이 없어 규격 §2가 payload 크기로
고정한 protobuf `bytes body`를 실을 수 없다. 실측하면 1024 bytes가 **20,412 bytes(19.9배)** 로
인코딩되고 디코딩하면 bytes가 아닌 일반 object로 돌아온다. 다른 payload로 바꿔 재지 않았고
비공개 경로로 우회하지도 않았다(G4).

정지의 run별 관측은 아래와 같다. 여덟 셀 중 일곱이 정지했다.

| run | payload | 완료 | error | 평균 ms | p95 ms | 판정 |
|---|---|---|---|---|---|---|
| router-1 | 1024 | 10886 | 36 | 275.399 | 1.642 | 정지 |
| router-1 | 4096 | 1339 | 69 | 2131.655 | 30000.726 | 정지 |
| router-2 | 1024 | 1877 | 59 | 1550.342 | 30000.627 | 정지 |
| router-2 | 4096 | 11160 | 100 | 267.500 | 2.137 | 정지 |
| router-3 | 1024 | 260 | 98 | 8380.685 | 30001.510 | 정지 |
| router-3 | 4096 | 1828 | 55 | 1594.068 | 30000.451 | 정지 |
| dealer-1 | 1024 | 150 | 25 | 17143.678 | 30001.265 | 정지 |
| **dealer-1** | **4096** | **424853** | **0** | **0.860** | **1.472** | **정상** |

정상인 한 셀이 **424,853건을 오류 없이 p99 1.888 ms로 완료했다.** 정지하지 않을 때 이 경로가
무엇을 하는지 보여주는 유일한 셀이고, 정지 셀의 산술값을 이 경로의 성능으로 읽으면 안 되는
이유이기도 하다. 정지는 DEALER 구성에서도 발생했으므로 소켓 종류에 딸린 성질이 아니다.

정지의 위치는 bench 밖 최소 재현으로 분리했다. `@zlink-systems/zlink`만 사용하는 재현 코드가
같은 현상을 만들고, 정지한 socket이 멈춰 있는 동안 **같은 server에 새로 연결한 client ROUTER는
100건을 1.5 ms에 완료한다.** server는 정상이고 멈춘 것은 client socket이다. 동시 요청 100건을
한 번에 제출하면 1.1 ms에 모두 완료되므로 깊이 자체가 원인도 아니다.

### 5.3 Java

| 패턴 | 구현 | 처리량 | 평균 ms | p95 ms | p99 ms | client CPU% | 깊이 | drain ms |
|---|---|---|---|---|---|---|---|---|
| request-serial @1024 | `grpc-java` | 5.66 | 0.176 | 0.224 | 0.272 | 2.4 | 1.0 | — |
| request-serial @1024 | `zlink-java` | 6.37 | 0.157 | 0.200 | 0.263 | 2.6 | 1.0 | — |
| request-serial @1024 | `zlink-framework-java` | 0.49 | 2.057 | 2.526 | 2.683 | 1.7 | 1.0 | — |
| request-window @1024 | `grpc-java` | 117.31 | 0.832 | 1.012 | 1.240 | 15.5 | 97.6 | — |
| request-window @1024 | `zlink-java` | **처리량 없음 — 정지** | — | — | — | 0.7 | — | — |
| request-window @1024 | `zlink-framework-java` | 2.37 | 1.906 | 2.745 | 2.980 | 5.1 | **4.5** | — |
| send-saturation @1024 | `grpc-java` | 41.34 | 0.105 | 0.155 | 0.184 | 9.8 | — | 313 |
| send-saturation @1024 | `zlink-java` | 477.22 | 0.073 | 0.215 | 0.272 | 10.2 | — | 308 |
| send-saturation @1024 | `zlink-framework-java` | 6.70 | 2319.980 | 2492.296 | 2524.796 | 13.3 | — | 2122 |
| request-serial @4096 | `grpc-java` | 5.78 | 0.173 | 0.219 | 0.267 | 2.4 | 1.0 | — |
| request-serial @4096 | `zlink-java` | 6.35 | 0.157 | 0.199 | 0.257 | 2.6 | 1.0 | — |
| request-serial @4096 | `zlink-framework-java` | 0.49 | 2.039 | 2.487 | 2.562 | 1.7 | 1.0 | — |
| request-window @4096 | `grpc-java` | 98.50 | 0.984 | 1.349 | 1.487 | 14.4 | 96.9 | — |
| request-window @4096 | `zlink-java` | **처리량 없음 — 정지** | — | — | — | 0.7 | — | — |
| request-window @4096 | `zlink-framework-java` | 2.36 | 1.920 | 2.764 | 2.985 | 5.2 | **4.5** | — |
| send-saturation @4096 | `grpc-java` | 39.66 | 0.110 | 0.163 | 0.191 | 9.8 | — | 311 |
| send-saturation @4096 | `zlink-java` | 318.94 | 0.083 | 0.239 | 0.295 | 10.6 | — | 362 |
| send-saturation @4096 | `zlink-framework-java` | 8.95 | 808.894 | 880.459 | 900.983 | 13.3 | — | 1000 |

네 run 모두 18셀을 실행했고 실패 0, 오염 0이다. 포화 셀은 없다. Java의 framework 행은 규격이
정한 payload를 그대로 싣는다 — `zlink-framework-codec-protobuf`가 protobuf 생성 클래스를
직렬화하므로 Node를 막은 codec 격차가 없다.

정지의 run별 관측이다. **여덟 셀 전부가 `peak_in_flight` 100에 abandoned 100이다.** window는
채워졌고 채워진 100개가 하나도 완료되지 않았다.

| run | payload | 완료 | error | abandoned | peak_in_flight | 판정 |
|---|---|---|---|---|---|---|
| router-1 | 1024 / 4096 | 0 / 0 | 100 / 100 | 100 / 100 | 100 / 100 | 정지 |
| router-2 | 1024 / 4096 | 0 / 113 | 100 / 100 | 100 / 100 | 100 / 100 | 정지 |
| router-3 | 1024 / 4096 | 0 / 100 | 100 / 100 | 100 / 100 | 100 / 100 | 정지 |
| dealer-1 | 1024 / 4096 | 0 / 0 | 100 / 100 | 100 / 100 | 100 / 100 | 정지 |

같은 socket을 쓰는 `zlink-java` request-serial이 6.37 KOPS로 정상이므로 **정지는 미완료 요청이
둘 이상일 때에만 나타난다.** bench 밖 최소 재현이 그 경계를 직접 보인다.

| 미완료 요청 수 | 프로세스 안 echo server | bench raw server 대상 |
|---|---|---|
| 4 | 4/4 완료 | 4/4 완료 |
| 16 | 16/16 완료 | 0/16, 5/16, 10/16 (run마다 다름) |
| 100 | **43/100 완료, server는 101건 전부 회신** | 0/100, 5/100 |

### 5.4 Kotlin

Kotlin 행은 **client 프로세스만 Kotlin으로 구현하고 server 세 개는 Java 행의 바이너리를
kotlin 포트 대역에서 실행한다.** 이 Phase가 답하려는 질문이 client API 비용이기 때문이다.
그래서 formula 2는 Java 행과 같은 server 쪽 비용을 포함한 값이고, §4의 깊이 일치도 server를
공유한 상태의 일치다.

| 패턴 | 구현 | 처리량 | 평균 ms | p95 ms | p99 ms | client CPU% | 깊이 | drain ms |
|---|---|---|---|---|---|---|---|---|
| request-serial @1024 | `grpc-kotlin` | 4.74 | 0.211 | 0.261 | 0.324 | 3.0 | 1.0 | — |
| request-serial @1024 | `zlink-kotlin` | 5.32 | 0.188 | 0.230 | 0.292 | 2.6 | 1.0 | — |
| request-serial @1024 | `zlink-framework-kotlin` | 0.47 | 2.121 | 2.589 | 2.783 | 1.7 | 1.0 | — |
| request-window @1024 | `grpc-kotlin` | 93.75 | 0.982 | 1.239 | 1.818 | 28.2 | 92.0 | — |
| request-window @1024 | `zlink-kotlin` | **처리량 없음 — 정지** | — | — | — | 0.6 | — | — |
| request-window @1024 | `zlink-framework-kotlin` | 2.36 | 1.920 | 2.769 | 2.994 | 5.3 | **4.5** | — |
| send-saturation @1024 | `grpc-kotlin` | 35.18 | 0.120 | 0.167 | 0.194 | 13.8 | — | 310 |
| send-saturation @1024 | `zlink-kotlin` | 372.46 | 0.076 | 0.232 | 0.291 | 10.1 | — | 311 |
| send-saturation @1024 | `zlink-framework-kotlin` | 5.67 | 2482.409 | 2655.419 | 2675.103 | 13.1 | — | 2464 |
| request-serial @4096 | `grpc-kotlin` | 4.89 | 0.204 | 0.241 | 0.308 | 3.0 | 1.0 | — |
| request-serial @4096 | `zlink-kotlin` | 5.40 | 0.185 | 0.223 | 0.297 | 2.7 | 1.0 | — |
| request-serial @4096 | `zlink-framework-kotlin` | 0.49 | 2.032 | 2.505 | 2.578 | 1.7 | 1.0 | — |
| request-window @4096 | `grpc-kotlin` | 79.46 | 1.172 | 1.555 | 1.851 | 24.4 | 93.1 | — |
| request-window @4096 | `zlink-kotlin` | **처리량 없음 — 정지** | — | — | — | 0.7 | — | — |
| request-window @4096 | `zlink-framework-kotlin` | 2.30 | 1.952 | 2.800 | 3.020 | 5.5 | **4.5** | — |
| send-saturation @4096 | `grpc-kotlin` | 33.75 | 0.126 | 0.177 | 0.206 | 13.7 | — | 333 |
| send-saturation @4096 | `zlink-kotlin` | 256.76 | 0.084 | 0.233 | 0.285 | 10.4 | — | 375 |
| send-saturation @4096 | `zlink-framework-kotlin` | 8.67 | 819.060 | 897.205 | 918.998 | 13.3 | — | 1051 |

네 run 모두 18셀을 실행했고 실패 0, 오염 0이다. 포화 셀은 없다. raw request-window 여덟 셀은
Java와 같은 서명으로 전부 정지했고(`peak_in_flight` 100 / abandoned 100), 그 여덟 셀 밖의
오류는 네 run 전부에서 0이다. **warmup 20초의 열 구간이 모두 0이므로 정지는 active 구간에서
생긴 것이 아니라 처음부터 있었다.**

### 5.5 C++

`zlink-framework-cpp`는 **이 Phase에서 구현하지 않았고 따라서 측정하지 않았다.** 감독관 지시에
따라 깊이 판정을 우선했다. 추정값을 채워 넣지 않는다. 그래서 C++는 run마다 12셀을 실행했고
G1(18셀)을 충족하지 못한다.

| 패턴 | 구현 | 처리량 | 평균 ms | p95 ms | p99 ms | client CPU% | 깊이 |
|---|---|---|---|---|---|---|---|
| request-serial @1024 | `grpc-cpp` | 16.25 | 0.060 | 0.081 | 0.123 | 1.9 | 1.0 |
| request-serial @1024 | `zlink-cpp` | 8.97 | 0.111 | 0.141 | 0.215 | 2.3 | 1.0 |
| request-window @1024 | `grpc-cpp` | 65.31 | 1.530 | 1.712 | 1.821 | 3.5 | 99.9 |
| request-window @1024 | `zlink-cpp` | 331.44 | 0.301 | 0.353 | 0.408 | 9.5 | **99.9** |
| send-saturation @1024 | `grpc-cpp` | 45.38 | 0.112 | 0.164 | 0.205 | 3.2 | — |
| send-saturation @1024 | `zlink-cpp` | 696.42\* | 0.443 | 0.987 | 1.100 | 8.2 | — |
| request-serial @4096 | `grpc-cpp` | 14.94 | 0.065 | 0.084 | 0.122 | 1.9 | 1.0 |
| request-serial @4096 | `zlink-cpp` | 8.79 | 0.113 | 0.147 | 0.206 | 2.3 | 1.0 |
| request-window @4096 | `grpc-cpp` | 63.34 | 1.577 | 1.802 | 1.911 | 3.6 | 99.9 |
| request-window @4096 | `zlink-cpp` | 278.28 | 0.359 | 0.412 | 0.447 | 10.4 | **99.8** |
| send-saturation @4096 | `grpc-cpp` | 44.51 | 0.114 | 0.165 | 0.212 | 3.3 | — |
| send-saturation @4096 | `zlink-cpp` | 514.68 | 0.378 | 0.492 | 0.585 | 8.3 | — |

네 run 48셀 전체에서 오류 0, header 검증 실패 0, abandoned 0, 오염 0이다.

`zlink-c` 기준 행(같은 구간에서 재사용)은 request-serial 8.29 / 8.35 KOPS, request-window
428.14 / 311.80 KOPS, send-saturation 699.35 / 485.77 KMSG/s다.

### 5.6 포화 계측기는 언어마다 다르고, 그 사실 자체가 결과다

규격 §5.1은 각 언어 harness가 **무엇을 재는지와 그 상한을 함께 선언**하게 한다. 이 캠페인은
그 규칙이 옳았다는 것을 세 번 확인했다.

| 언어 | 선언 계측기 | 상한 | 프로세스 CPU가 함께 센 것 |
|---|---|---|---|
| `.NET` | `client_cores` (프로세스 사용 core 수) | 선언 병렬도 | — |
| Node | `event_loop_utilization` | 1.0 | binding의 native I/O thread |
| Java · Kotlin | `jvm_thread_cores` (제출 thread만) | request 1, send 8 | GC·JIT thread |
| C++ | `submit_thread_cores` | 1 | binding의 I/O thread |

**세 언어에서 세 가지 다른 이유로 프로세스 CPU가 틀린 계측기였다.** 다섯 언어 중 넷이 프로세스
CPU가 아닌 것을 선언했으므로, **ZLink client에서 프로세스 CPU는 포화 계측기로 쓸 수 없다**는
것이 이제 언어 중립적 결론이다.

문제는 임계값이 아니라 판정식이 나누는 두 행 사이의 비교 가능성이다. C++ 실측이 그것을 가장
짧게 보인다. 같은 run, 같은 셀 좌표에서 `zlink-cpp`는 선언 계측기 0.950인데 프로세스 core는
1.90이고, `grpc-cpp`는 0.700과 0.70으로 거의 같다. 그 차이는 client 런타임이 더 바쁘기
때문이 아니라 **어느 행이 Core를 링크하는가**다. 프로세스 core로 판정하면 표시가 client
포화가 아니라 그 사실을 보고하게 된다.

포화로 표시된 셀은 **Node 다섯 개와 C++ 한 개**다. `.NET`·Java·Kotlin에는 없다.

| 언어 | 포화 셀 | 판독값 |
|---|---|---|
| Node | `zlink-node` request-serial @1024·@4096 | 0.983 · 0.985 |
| Node | `zlink-node` request-window @1024·@4096 | 1.000 · 1.000 |
| Node | `zlink-node` send-saturation @4096 | 1.000 |
| C++ | `zlink-cpp` send-saturation @1024 | 0.964 |

Node의 request 계열 네 셀이 전부 포화라는 것은 **그 client에서 상한을 정한 것이 transport가
아니라 user 코드가 도는 JS thread**라는 뜻이다. 같은 run의 `grpc-node`는 0.646~0.868로 포화에
이르지 않는다. 규격 §5.1이 포화 셀을 처리량 우열 판정에서 제외하므로, Node의 formula 1은
분자가 정지하지 않았더라도 게재 조건을 만족하지 못했을 것이다.

## 6. 판정 기준 자체가 현재 기준선 위에서는 도달할 수 없다

이것은 어느 언어의 결과도 아니고 **캠페인 수준의 사실**이다.

`zlink-c` request-window @4096은 formula 1(`zlink-<lang> / zlink-c`)의 **공유 분모**다. 이 행이
독립된 세 측정 구간에서 재현성 조건을 계속 넘지 못했다.

| 측정 | `zlink-c` request-window @4096 3회 값 | G5 스프레드 | 독립성 |
|---|---|---|---|
| Phase 0 `gated` | 214.7 / 396.4 / 225.6 KOPS | **75.7%** | 독립 |
| Phase 0 `gated2` | 387.9 / 310.5 / 230.8 KOPS | **25.7%** | 독립 |
| Phase 3 Java 구간 | — | **28.7%** | 독립 |
| Phase 4·5 (Kotlin·C++) | 위 값을 재사용 | 28.7% | **독립 측정 아님** |
| 통합 검증 구간 (§2.4) | 225.9 KOPS 1회 | 직전 중앙값 대비 **-27.6%** | 독립 (1회이므로 스프레드는 아니다) |

**독립 측정은 세 번이고, 통합 검증 구간이 네 번째 판독을 더한다.** Kotlin과 C++ 구간은
Java 구간의 기준선을 재사용했으므로 근거를 더하지 않지만, 검증 구간은 새로 측정했다.
그 1회 값이 직전 중앙값에서 27.6% 떨어졌다는 것은 G5 스프레드는 아니지만 **이 행이 구간을
바꾸어도 계속 흔들린다는 관측**이다.

구조적 결과는 이렇다. FB-011에 따라 분모가 G5를 통과하지 못하면 판정을 게재할 수 없고,
FB-005에 따라 두 payload 크기를 모두 만족해야 통과다. 따라서 **@4096 분모가 안정되기 전까지
어떤 언어도 formula 1을 통과할 수 없다. 그 언어의 품질과 무관하다.** 이 캠페인에서 자기 셀이
완벽했던 C++가 그 자리에서 막힌 것이 그 증거다 — `zlink-cpp` 12행 전부가 G5를 통과했고 최대
스프레드가 3.8%인데도 @4096 판정을 게재하지 못했다.

**이것을 판정 기준을 완화할 근거로 쓰지 않는다.** 답은 분모를 안정시키는 것이지 임계값을
낮추는 것이 아니다. 기준선 안정화는 이 캠페인의 범위 밖이며 후속 항목으로 올린다(§10).

`zlink-c`의 불안정은 이 행 하나에 국한되지 않는다. 같은 기준 bench에서 request-serial @1024가
37.9%, @4096이 12.6%, send-saturation @1024가 20.9%로 미달한 구간이 있다. 판정 경로 밖이지만
기준 bench 전반의 재현성 문제라는 신호이므로 함께 기록한다.

## 7. 패턴별 관찰

세 패턴을 각각 따로 서술한다. **평균내지 않는다.** 서로 다른 셀의 수치를 하나로 합치면
그 값은 어느 실험도 가리키지 않는다.

### 7.1 `request-serial` — 왕복 지연이 처리량을 정한다

요청 하나를 보내고 reply 완료 뒤 다음을 보내므로 처리량이 곧 왕복 지연의 역수다. 판정 패턴이
아니라 보조 지표다.

| 언어 | `grpc-<lang>` | `zlink-<lang>` | `zlink-framework-<lang>` |
|---|---|---|---|
| `.NET` | 6.758 | **7.595** | 1.995 |
| Node | 4.19 | **9.99**(포화) | 미측정 |
| Java | 5.66 | **6.37** | 0.49 |
| Kotlin | 4.74 | **5.32** | 0.47 |
| C++ | **16.25** | 8.97 | 미구현 |
| C (기준) | **15.28** | 8.29 | 해당 없음 |

@1024 KOPS. **관리형 런타임 네 언어에서는 ZLink raw가 gRPC보다 왕복이 짧고, C와 C++에서는
반대로 gRPC가 약 1.8배 짧다.** 뒤집힘을 감추지 않는다. 다만 언어를 가로질러 절대값을 읽지
않는다 — `grpc-cpp` 16.25와 `grpc-node` 4.19를 나란히 놓은 값은 런타임 비교이지 ZLink 비교가
아니다.

framework 행은 어느 언어에서도 raw 행의 4분의 1 아래다(`.NET` 0.26배, Java 0.077배,
Kotlin 0.088배). JVM 두 언어의 framework 왕복이 약 2.0~2.1 ms로 raw의 약 0.16 ms보다 한
자릿수 크다.

### 7.2 `request-window` — 판정의 중심이고, 다섯 언어가 갈리는 곳

여기서 gRPC unary `Echo`와 ZLink request는 **서버 처리 확인이라는 같은 보장**을 주므로 정면
비교가 성립한다. 그래서 규격 §7.2가 이 패턴을 판정에 쓴다.

그리고 이 패턴에서 다섯 언어가 갈린다. §4가 그 내용이다. 처리량으로 요약하면 `zlink-c`
428.14, `zlink-cpp` 331.44, `zlink-dotnet` 36.03이고, Node·Java·Kotlin은 **처리량이 성립하지
않는다.** 이 세 언어의 칸을 0에 가까운 숫자로 채우면 "느리다"로 읽히지만 실제로는 "완료가
전진하지 않는다"이며 다른 종류의 관측이다.

gRPC 쪽은 다섯 언어 전부에서 정상 동작하고 깊이 92~99.9를 유지한다. **같은 harness가 같은
run에서 그 깊이를 내므로 window 로직 자체는 동작한다.**

### 7.3 `send-saturation` — 응답이 필요 없는 명령의 비용

이 셀을 **전송 속도 차이로 서술하지 않는다.** gRPC는 응답이 필요 없는 호출에도 unary `Command`
→ `Empty` 왕복을 치르고 ZLink는 단방향 send로 끝난다. gRPC에 단방향 호출 원시 기능이 없어
왕복을 치르는 것은 gRPC의 특성이며, 서비스 측면 비교에서 그 비용은 결과에 그대로 드러나는 것이
맞다. 비교를 공정하게 만들려고 gRPC 쪽에 client-streaming 같은 다른 사용 형태를 끼워 넣지
않는다.

정확한 문장은 이렇다. **응답이 필요 없는 명령을 처리할 때, gRPC는 unary 왕복을 치러야 하고
ZLink는 단방향 send로 끝난다. 이 조건에서 그 차이는 아래와 같이 관찰됐다.**

| 언어 | @1024 | @4096 |
|---|---|---|
| `.NET` | 9.1배 (411.9 대 45.3 KMSG/s) | 9.2배 (383.1 대 41.5) |
| Node | 25.6배 (291.4 대 11.4) | 15.6배 (168.1 대 10.8) |
| Java | 11.5배 (477.2 대 41.3) | 8.0배 (318.9 대 39.7) |
| Kotlin | 10.6배 (372.5 대 35.2) | 7.6배 (256.8 대 33.8) |
| C++ | 15.3배 (696.4 대 45.4, 포화 셀) | 11.6배 (514.7 대 44.5) |

C 기준 bench의 send 셀 네 개는 **판정에서 제외된다.** `run_send_loop`가 client 자신의 제출
수를 세는데 규격 §5는 server 수신 수를 요구한다(G3 미달, 기존 결함). 위 표의 C++ 행은 server
수신 수다.

framework 행의 send는 성격이 다르다. 처리량은 `.NET` 46.6, Java 6.70, Kotlin 5.67 KMSG/s인데
**수신 지연이 초 단위**이고(`.NET` 평균 1.77초, Java 2.32초, Kotlin 2.48초) drain이 `.NET`에서
16.7초다. 이는 sender가 receiver를 크게 앞지를 때 나타나는 모양이며, 이 값 하나로 결함을
단정하지 않는다. 판정은 KMSG/s로 하고 지연은 함께 기록한다.

## 8. 계측이 바로잡은 것

**이 항목이 앞의 수치를 신뢰할 근거다.** 아래는 이 캠페인이 중간에 정정한 값과, 정정하지
않았다면 이 보고서에 실렸을 값이다.

정정의 방향을 그대로 적는다. **ZLink 쪽에 유리하던 값을 내린 정정이 있고**(framework send의
겉보기 우위, §8.1), **소켓 구성을 맞추느라 raw 행 처리량을 27% 낮춘 정정도 있으며**(§8.3),
**ZLink에 불리해 보이던 값을 실패값으로 되살린 정정도 있다**(§8.2). 어느 쪽으로도 값을 만들려고
조건을 바꾸지 않았다는 것이 이 목록의 요지다.

### 8.1 framework send의 겉보기 우위 2.8배는 표본화 시점 오류였다

`send-saturation`의 처리량을 **drain이 끝난 뒤** server 통계에서 읽고 있었다. 그래서 표의 값은
server의 소비율이 아니라 "결국 server가 받은 것으로 걸러진 client 제출률"이었다. 규격 §5는
server가 **active phase에서** 받은 messages 수라고 정하므로, 규격을 바꾼 것이 아니라 구현을
규격에 맞췄다.

한 run의 실측이 격차를 그대로 보인다.

```text
zlink-framework-dotnet send-saturation @1024
  active window 경계에서의 수신 수     233,143
  drain 이후의 수신 수                 712,881
  drain 시간                            16,674 ms
```

| 구현 | payload | 정정 전 | 정정 후 | 변화 |
|---|---|---|---|---|
| `grpc-dotnet` | 1024 | 45.620 | 45.335 | -0.6% |
| `zlink-dotnet` | 1024 | 411.127 | 411.875 | +0.2% |
| **`zlink-framework-dotnet`** | 1024 | **125.881** | **46.629** | **-63%** |
| `grpc-dotnet` | 4096 | 40.473 | 41.515 | +2.6% |
| `zlink-dotnet` | 4096 | 395.952 | 383.077 | -3.3% |
| **`zlink-framework-dotnet`** | 4096 | **53.002** | **43.863** | **-17%** |

**뒤집힌 결론을 그대로 적는다. framework send가 gRPC보다 약 2.8배 빠르다는 값은 계측 artefact였고,
정정 뒤 그 우위는 사라진다.** payload 1024에서 framework 46.629, gRPC 45.335로 사실상 같다
(비 1.03). **framework가 gRPC보다 빠르다고 서술할 근거가 없다.**

gRPC와 raw 행이 거의 움직이지 않은 이유도 자료가 설명한다. 두 행의 drain은 0.2~0.5초라
표본화 시점을 어디로 잡아도 값이 같고, framework만 drain이 16.7초다. **정정 폭 자체가 각
구현의 drain 특성을 보고하는 값이다.** 정정은 재현성도 개선했다 — framework send 행의
스프레드가 22.8%에서 2.0%로, raw send 행이 13.3%에서 5.0%로 줄었다.

### 8.2 `.NET` raw의 겉보기 비율 0.087은 서로 다른 두 실험을 나눈 값이었다

`zlink-dotnet` request-window가 설정 window 100에 대해 실제로는 미완료 요청 **8.4개**만
유지하고 있었다. 같은 harness가 같은 run에서 gRPC 91.8, framework 99.0을 유지했으므로 처음
판독은 harness 결함이었다.

| 행 | 처리량/s | 평균 ms | 실제 깊이 |
|---|---|---|---|
| `grpc-c` | 64,567 | 1.529 | 98.7 |
| `zlink-c` | 425,907 | 0.213 | 90.7 |
| `grpc-dotnet` | 196,191 | 0.468 | 91.8 |
| `zlink-framework-dotnet` | 3,017 | 32.818 | 99.0 |
| **`zlink-dotnet`** | 37,113 | 0.225 | **8.4** |

그 상태로 계산한 0.087은 **깊이 8 실험을 깊이 91 실험으로 나눈 값**이고, 그래서 판정에서
철회했다. 이 대목이 이 캠페인에서 가장 중요한 계측 항목이다. `peak_in_flight`와 abandoned
한 줄이 없었다면 **잘못된 전제를 그대로 안고 네 언어 harness를 복제했을 것이고, formula 1이
다섯 언어 전부에서 무의미해졌을 것이다.**

그 뒤 같은 계측이 철회를 다시 뒤집었다. `peak_in_flight`가 여섯 셀 중 넷에서 100/100에
도달하고 abandoned가 0이며, 제출 경로에 잠금·semaphore·channel이 없고, 요청당 client CPU
비용으로 예측한 깊이가 관측값과 일치한다.

| 행 | 처리량/s | client cores | µs CPU/request |
|---|---|---|---|
| `zlink-c` | 430,617 | 1.74 | **4.0** |
| `grpc-c` | 65,026 | 1.76 | 27.1 |
| `zlink-dotnet` | 36,034 | **1.16** | **32.2** |
| `grpc-dotnet` | 198,787 | **7.38** | 37.1 |

예측 깊이 = 왕복 지연 221 µs ÷ 제출 비용 32.2 µs = 6.9(관측 8.0). `grpc-dotnet`은 요청당
비용이 더 큰데도 7.38 코어로 퍼져 5.5배 처리량을 낸다. **곧 깊이 8은 동시성 상한이 아니라
단일 thread 제출 처리율 상한이며, 측정 결과이지 harness 결함이 아니다.** 그래서 0.084를
실패값으로 게재한다.

### 8.3 함께 바로잡은 것들

| 항목 | 정정 전 상태 | 정정 |
|---|---|---|
| `.NET` bench 빌드 | **커밋된 상태로 빌드되지 않았다.** `ZLinkRawServer`가 binding에서 사라진 멤버를 사용했고 runner가 `set -euo pipefail`이라 빌드 단계에서 전체가 실패했다. 곧 한 번도 실행된 적이 없다 | 계약에 맞춰 수정. bench가 어떤 정기 gate에도 걸려 있지 않아 파손이 드러나지 않았다 |
| C bench의 server CPU·memory | `setsid`가 fork하므로 `$!`가 곧 사라지는 wrapper의 PID였다. **server 자원 열이 전부 무의미한 값이었다** | 각 server가 자기 PID를 pidfile에 기록. **과거 C bench 결과의 server CPU·memory를 인용하지 않는다** |
| 셀 사이 오염 | settle이 고정 200 ms였고 server가 비워지지 않아도 반환했다. framework send 셀 뒤 약 5.5초 일찍 반환해 **셀 하나가 다음 셀을 timeout으로 죽였다** | settle을 server drain 확인으로 바꾸고, 상한 초과 시 **같은 server를 쓰는** 다음 셀을 오염으로 표시 |
| C bench의 send 처리량 | client 자신의 제출 수를 셌다. 규격 §5는 server 수신 수를 요구한다 | 기존 결함으로 기록하고 해당 4셀을 판정에서 **`unsupported`** 로 둔다. 값을 만들려고 규격을 바꾸지 않았다 |
| raw 행의 소켓 | raw가 DEALER→ROUTER, framework가 ROUTER↔ROUTER였다. **두 행을 나눈 값에 framework 계층 비용과 소켓 패턴 비용이 함께 들어갔다** | 양쪽을 ROUTER↔ROUTER로 통일. raw 행 처리량이 @1024에서 약 27% 낮아졌고 그 값을 그대로 채택했다 |
| 포화 계측기 | 논리 core 20개 대비 백분율이었다. 단일 thread client가 코어 하나를 완전히 채워도 약 5%로 읽혀 **95% 임계값이 영원히 발동하지 않았다** | 언어별 선언 계측기와 상한으로 바꿈(§5.6). 세 언어에서 세 가지 다른 이유로 프로세스 CPU가 틀린 계측기였다 |
| 진단값의 전달 경로 | `peak_in_flight`·abandoned·drain·오염 표시가 stdout에만 있어 집계기가 사람이 읽는 텍스트를 정규식으로 긁었다 | 공통 셀 schema를 표준 입력 통로로 삼음. **게재 여부를 정하는 값이 산문을 거치지 않는다** |
| 규격 §3의 raw wire 서술 | raw가 payload를 "protobuf envelope 없이" 보낸다고 적혔다. 실제 두 구현은 envelope 헤더와 protobuf 인코딩 `BenchPayload`를 두 part로 보낸다 | **구현이 맞고 규격 문장이 틀렸다.** formula 1이 두 행을 나누므로 wire 모양이 다르면 서로 다른 실험을 나누게 된다. 규격을 구현에 맞춰 고쳤다 |

한 항목은 반대 방향의 교훈이다. framework channel send가 무성 손실된다는 진단이 우선순위 0에
올랐다가 **20분 계측으로 반증됐다.** 실제 원인은 harness의 bean 등록 방식이었고 배선을 고친
뒤 같은 셀이 정상 처리량을 냈다. 다만 그 조사가 제품 쪽 결함을 하나 더 정확하게 만들었다 —
handler 생성이 실패하면 framework가 메시지를 **수락하고, 추적하고, 버리고, sender에게 성공을
돌려준다.** **진단은 계측 전까지 가설이다.**

## 9. 한계

**측정 환경**

- **loopback 단일 머신이다.** 운영 환경의 mesh, TLS, L7 부하 분산, 다중 노드 분배, 네트워크
  지연은 이 자료에 없다. `127.0.0.1` 전용이다.
- **gRPC는 각 언어의 기본 서버 구성이며 튜닝하지 않았다.** gRPC 쪽 튜닝 경쟁은 범위 밖이다.
- **C++ gRPC는 시스템 `libgrpc++` 1.51.1로 2022년 배포판이다.** 다만 같은 구간에서 `grpc-cpp`와
  `grpc-c`가 같은 시스템 라이브러리를 쓰고 request-window에서 65.31 대 65.86 KOPS로 사실상
  같은 값을 내므로, gRPC 쪽 수치는 두 bench 사이에서 일관된다.
- **WSL2 커널이다.** 이 저장소에서 벽시계가 튀는 사례가 관측된 환경이므로 지연 값은 harness가
  단조 시계로 잰 값이다.
- **warmup이 언어마다 다르다**(§2.3.2). `.NET`·Node는 1000회, Java·Kotlin은 20초, C++는 5초다.
  값을 통일하지 않은 것은 의도이며 각 언어가 자기 구간별 자료로 근거를 냈지만, **그 결과 각
  행이 서로 다른 예열 상태에서 측정됐다는 사실은 남는다.** 같은 언어 안의 3자 비교는 같은
  warmup을 쓰므로 영향을 받지 않고, 언어를 가로지르는 절대값 비교는 어차피 하지 않는다.

**서술 제약**

- **언어를 가로지른 절대 처리량 우열을 읽지 않는다.** `grpc-java` 117.31 KOPS와 `grpc-c`
  65.86 KOPS를 나란히 놓은 값은 런타임 비교이지 ZLink 비교가 아니다. 언어를 가로질러 읽을 수
  있는 값은 각 언어가 자기 자신을 기준으로 낸 비율뿐이다.
- **"ZLink가 gRPC보다 N배 빠르다" 같은 일반화를 쓰지 않는다.** 모든 결과는 "이 조건에서"로만
  서술한다.
- **포화 셀이나 정지 셀을 근거로 우위를 주장하지 않는다.**

**자료 자체의 결손**

- **`zlink-framework-node` 여섯 셀은 존재하지 않는다.** 공개 codec이 규격이 정한 이진 payload를
  표현하지 못한다. 제품의 제약이며 다른 payload로 바꿔 재지 않았다.
- **`zlink-framework-cpp` 여섯 셀은 존재하지 않는다.** 이 Phase가 구현하지 않았다. Node의
  경우와 성격이 다르다.
- **`zlink-node`·`zlink-java`·`zlink-kotlin`의 request-window 처리량은 존재하지 않는다.** 정상
  표본이 하나도 없어 G5를 적용할 대상 자체가 없다. Node에는 dealer @4096 한 셀이 정상이지만
  표본이 하나라 재현성을 말할 수 없다.
- **C 기준 bench의 send 셀 네 개는 판정에서 제외됐다**(G3 미달). **C 기준 bench의 server
  CPU·memory 열은 인용하지 않는다**(기존 결함).
- **Kotlin 행은 client만 Kotlin이다.** server 세 개는 Java 행의 바이너리다. 따라서 Kotlin의
  formula 2는 Java와 같은 server 쪽 비용을 포함하고, Kotlin은 완료 전달 관측에 독립 사례를
  더하지 않는다.
- **`zlink-c` 기준선의 독립 측정은 세 구간뿐이다.** Kotlin·C++ 구간이 Java 구간의 값을
  재사용했다.

**포화·재현성 미달 셀**

포화 셀은 Node 다섯 개와 C++ 한 개다(§5.6). 이 셀들은 처리량 우열 판정에 쓰지 않는다.
C++ `zlink-cpp` request-window @1024는 포화 임계값을 0.00003 차이로 비켜간 상태에서 게재됐다.

G5 미달 행은 아래와 같다. 정지한 셀은 처리량이 성립하지 않으므로 재현성 판정 대상이 아니며
이 목록에 넣지 않는다.

| 행 | payload | 스프레드 |
|---|---|---|
| `zlink-framework-dotnet` request-window | 1024 · 4096 | 29.6% · 11.5% |
| `zlink-java` request-serial | 4096 | 10.6% |
| `zlink-framework-kotlin` send-saturation | 1024 | 12.8% |
| `zlink-c` request-window | 4096 | 25.7% ~ 75.7% (§6) |
| `zlink-c` request-serial | 1024 · 4096 | 37.9% · 12.6% |
| `zlink-c` send-saturation | 1024 | 20.9% |
| `grpc-c` request-serial | 1024 · 4096 | 173.8% · 165.0% (판정 경로 밖, 원인 미확인) |

**원인을 규명하지 않은 항목**

정지·유실의 근본 원인은 어느 언어에서도 특정하지 못했다. binding과 Core 중 어디인지 계측하지
않았다. `zlink-framework-*`의 깊이 4.5 상한, `zlink-cpp`의 요청당 비용, `zlink-c` 기준선의
불안정, `grpc-c` request-serial의 편차도 원인을 적지 않았다.

## 10. 후속 후보

원인을 기록만 하고 고치지 않은 항목이다. 이 캠페인은 값을 만들기 위해 조건을 바꾸지 않았고,
발견한 결함도 고치지 않았다.

| 우선순위 | 항목 | 내용 |
|---|---|---|
| 0 | framework의 handler 생성 실패 처리 | 생성이 실패하면 메시지를 **수락하고 추적하고 버리고 sender에게 성공을 돌려준다.** DEBUG 로그에도 남지 않는다. 사용자 코드의 handler 생성이 운영에서 실패하면 같은 일이 일어난다 |
| 0 | 관리형 binding의 완료 전달 | Node client socket 정지, Java reply 유실, `.NET` 단일 thread 제출 한계. **§4가 조사 범위를 Core 공통 경로에서 각 binding의 완료 전달 계층으로 좁혔다.** 조사자는 Core가 아니라 거기서 시작한다. 증상이 셋 다 다르므로 하나로 묶어 고치지 않는다 |
| 0 | `zlink-c` 기준선 안정화 | request-window @4096이 세 독립 구간에서 G5 미달. **이 행이 풀리기 전에는 어느 언어도 formula 1을 통과할 수 없다.** request-serial 두 행의 37.9%·12.6%도 함께 본다 |
| 1 | Node framework codec의 bytes 미지원 | 공개 codec이 이진 payload를 표현하지 못한다. bench의 문제가 아니라 제품의 제약이며, Node framework 행이 이 비교에 참여하려면 필요하다 |
| 1 | framework send 경로의 backpressure와 drain | send-saturation에서 수신 지연이 초 단위이고 `.NET`의 drain이 16.7초다. 세 언어에서 같은 성격으로 관측됐다 |
| 1 | framework request 경로의 깊이 상한 | 설정 window 100에 대해 `peak_in_flight` 10~12, 실제 깊이 4.5. client API 계층이 원인이 아니라는 것까지 좁혀졌다 |
| 1 | bench를 정기 gate에 편입 | `.NET` bench가 커밋된 상태로 빌드되지 않는 것을 아무 gate도 잡지 못했다. 다섯 언어로 늘어난 지금 같은 파손이 다시 숨을 수 있다 |
| 2 | `zlink-cpp`의 요청당 client 비용 | 깊이 99.9에서 `zlink-c`의 77.4%. wrapper의 고정 비용인지 이 harness의 driver 모양 때문인지 프로파일로 분리한다 |
| 2 | `zlink-framework-cpp` 여섯 셀 | 구현과 측정. Release 라이브러리는 이미 빌드돼 있다 |
| 2 | framework 연결 유지 | saturation 부하 뒤 RouteMesh peer 연결이 끊기고 재연결되지 않는 간헐 결함. ROUTER 3회에서는 재현되지 않고 DEALER run에서 나왔다 |
| 2 | C 기준 bench의 두 기존 결함 | send 셀이 client 제출 수를 센다(G3). server 자원 열은 이미 고쳤으나 과거 값은 인용 불가다 |
| 3 | `zlink-framework-kotlin` send @1024 | G5 12.8% 미달. warmup 구간도 함께 흔들린다. 원인 미규명 |
| 3 | `grpc-c` request-serial | G5 165% 이상. 판정 경로 밖이며 조사하지 않았다 |

## 부록 — 원본 자료의 위치

규격 §7.1이 결과와 함께 남기라고 정한 항목 가운데 원본 JSON의 위치다.

| 구간 | 경로 |
|---|---|
| `.NET` Phase 0 (인수 fixture) | `framework/bench/tools/tests/fixtures/gated2/` |
| Node Phase 2 | `framework/languages/node/bench/with-grpc/log/20260907_020443/` |
| Java Phase 3 | `framework/languages/java/bench/with-grpc/log/20260907_030655/` |
| Kotlin Phase 4 | `framework/languages/java/bench/with-grpc/log/20260907_041338/` |
| C++ Phase 5 | `framework/languages/cpp/bench/with-grpc/log/20260907_052304/` |
| C 기준선 (세 구간이 공유) | `bindings/c/bench/with_grpc/log/20260907_030655/c-router-{1,2,3}` |
| 정지 최소 재현 (Node) | `framework/languages/node/bench/with-grpc/repro/` |
| 정지 최소 재현 (Java) | `framework/languages/java/bench/with-grpc/repro/` |
| 공용 집계기 | `framework/bench/tools/` |

비교와 판정은 언제나 공용 집계기의 출력으로 한다. 언어 client가 스스로 출력하는 표는 run
하나를 바로 확인하기 위한 편의이며 판정의 근거가 아니다.
