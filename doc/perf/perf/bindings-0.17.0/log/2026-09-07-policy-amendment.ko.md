# perf 정책 문서 개정 기록 — 2026-09-07

대상: `doc/perf/PERF_POLICY.md`(v2.1 → v2.2), `doc/perf/PERF_SINGLE_TEST_POLICY.md`(v2.1 → v2.3),
`doc/perf/PERF_MULTI_TEST_POLICY.md`(v2.1 → v2.2).

입력: `doc/perf/perf/bindings-0.17.0/log/2026-09-07-runner-parity-design.ko.md` §4(P1~P6),
결정 D-BP1·D-BP2·D-BP3·D-BP4, D-095.

원칙(D-BP4): (1) 문안을 쓰기 전에 스펙·가이드를 확인하고 공개 계약에 없는 동작을 요구하지
않는다. (2) 최소 변경 — D-BP3가 요구하는 REQREP 제외와 P3~P6의 모호·미규정 조항 확정에만
손댄다. (3) 개정 조항은 개정 이후의 측정에 적용하며 완결된 paired 판정을 소급 무효화하지
않는다.

러너 코드(`bindings/*/perf/**`)는 수정하지 않았다. 계획서와 `decisions.ko.md`도 수정하지 않았다.

---

## 1. `PERF_SINGLE_TEST_POLICY.md`

### 1.1 REQREP 제외 (근거: D-BP3)

전수 검색(`reqrep|request|reply|requester|replier|왕복|ops/s`)으로 찾은 조항을 모두 처리했다.

| # | 위치(개정 전 행) | 처리 | before → after 요약 |
|---|---|---|---|
| S1 | 헤더 | 수정 | Policy Version 2.1/2026-08-28 → 2.3/2026-09-07 |
| S2 | §1 핵심 표 31~32 | 수정 | throughput/latency 정의에서 request-reply 항 삭제, one-way 정의만 남김 |
| S3 | §1 목적 불릿 뒤 | **추가** | "Single suite는 one-way 5 pattern만 측정하며 request/reply는 multi suite에서만 측정·판정한다"를 근거와 함께 명시. 근거로 `bindings/doc/spec/async-coroutine-policy.ko.md`의 request callback terminal 미제공을 인용하고 D-BP3를 참조 |
| S4 | §1.1 50~60 | 삭제 | "request/reply는 synchronous callback terminal과 completion poller를 사용한다 … 다시 제출할 수 있다" 문단과 "requester/replier", "request submit과 completion progress는 …" 문장 삭제. **async 금지 문장과 언어별 금지 조항(Go `LockOSThread`, Node `worker_threads`, Python `threading.Thread`, C++ `co_await`, .NET `Task`, Rust Future executor)은 그대로 유지** |
| S5 | §1.1 68~73 | 삭제 | `DEALER_ROUTER_REQREP`·`ROUTER_ROUTER_REQREP` 불릿 삭제. 5 pattern 불릿에 "request-reply completion 모델을 쓰는 패턴은 없다" 한 문장 추가 |
| S6 | §1.1 83~84 | 수정 | "기본 패턴은 one-way … 별도 `*_REQREP` 패턴으로 측정" → "모든 패턴은 one-way … multi suite의 `MULTI_*_REQREP`로 측정" |
| S7 | §1.1 102~126 | 삭제 | **raw request-reply 패턴** 다이어그램과 그 아래 6개 불릿 전부 |
| S8 | §1.1 공통 128~142 | 수정/삭제 | one-way backpressure 불릿에서 single reqrep 문장 삭제(대신 `PERF_POLICY.md § 7.2` 참조), "single reqrep 유효 집계" 불릿 삭제 |
| S9 | §1.1 공통 149~151 | 삭제 | "request-reply 패턴의 종료 drain은 …" 불릿 |
| S10 | §2 phase 표 218 | 삭제 | `completion drain` 행 |
| S11 | §2.0.1 표 232~233 | 삭제 | `DEALER_ROUTER_REQREP`·`ROUTER_ROUTER_REQREP` 행 |
| S12 | §2.0.1 259 | 삭제 | "completion drain: request-reply 패턴 공통으로 수행한다" |
| S13 | §2.1 288~291 | 삭제 | "single request-reply active 유효 완료 규칙" |
| S14 | §6.1 410~418 | 수정 | "7개 패턴" → "5개 패턴", REQREP 2개 제거, STREAM 제외 주석 옆에 request/reply 제외 주석 추가(§1, D-BP3 참조) |
| S15 | §6.1 recv mode 표·정책 | 수정 | `request-reply` 행 2개 삭제. "single request-reply 패턴은 별도 패턴명으로만 추가한다" → "single에 request-reply 패턴을 다시 추가하지 않는다" |
| S16 | §6.1 ready gate 표 454~455 | 삭제 | REQREP 행 2개 |
| S17 | §6.1 방향 분류 표·구현 참고 | 수정 | `request-reply (왕복)` 행 삭제, Kops/s·`× 2` bandwidth 설명 삭제 |
| S18 | §8 변경 이력 | 추가 | v2.3 항목(개정 내용 + D-BP4의 비소급 원칙) |

### 1.2 P3~P5 반영

| 항목 | 위치 | 처리 |
|---|---|---|
| P3.1 | §2.1 284~287 | 설계 문안을 **그대로** 채택. "recv 루프가 active window 안에서 처리한" → "**수신 시각이 active deadline 이전인**", 판정 시각은 recv 루프의 monotonic 처리 시각이며 `sent_ts_ns`가 아님, deadline 이후 recv는 stop token 도착 여부와 무관하게 제외 |
| P3.1 보강 | §1.4 drain 산문 | **추가 수정(설계 문서에 없음)**. 기존 "receiver 가 차례대로 **record** 한 뒤"라는 산문이 P3.1의 deadline 필터와 정면으로 충돌했다. "소비한 뒤"로 바꾸고 "이 소비는 종료 정리이며 §2.1의 유효 메시지 규칙만 집계에 들어간다"를 명시했다. 근거: 기존 §1.1 "active 결과 집계는 본 문서가 정의한 active 유효 메시지 조건을 계속 따른다" |
| P3.2 | §2.1 283 뒤 | 설계 문안을 **그대로** 채택. wire byte 길이 불일치 = 집계 제외(러너 중단 아님) |
| P4 | §7.2 | 중복 정의를 피하려고 **위치만 변경**. 표본 0개 규칙과 보간식은 `PERF_POLICY.md § 1.1`에 공통으로 두고 여기서는 참조 한 줄 |
| P5 (transient 재시도) | §1.1 공통 | 설계 문안 채택 + **예외 근거 보강**. 1 ms 대기 + `sent_ts_ns` 재stamp, busy retry와 1 ms 초과 backoff 금지. 여기에 "이 대기는 `PERF_POLICY.md § 1.1.2`의 hot loop sleep 금지에 대한 명시적 예외이며 blocking send terminal이 `PERF_SINGLE_SNDTIMEO_MS` 만료로 되돌아온 경우의 bounded 복구 절차"를 덧붙였다 |

---

## 2. `PERF_POLICY.md`

| # | 위치(개정 전 행) | 처리 | before → after 요약 |
|---|---|---|---|
| P1 | 헤더 | 추가 | v2.2. **개정 적용 시점** 문단 추가 — 개정 조항은 이후 측정에 적용, 완결된 paired 판정을 소급 무효화하지 않음(D-BP4). 개정 요약 한 문단 |
| P2 | §1 문서 표 29 | 수정 | single 설명 "recv/request-reply 모델" → "recv 모델(one-way 5 pattern)" |
| P3 | §1.1 118~126 | 삭제/수정 | single 실행 모델에서 request/reply 3문장 삭제, "single suite는 one-way 패턴만 측정하므로 측정 구간에 request/reply 경로가 없다"로 대체. **raw send blocking terminal 조항 유지** |
| P4 | §1.1 193~201 | 삭제 | send (single)에서 "request/reply는 synchronous callback terminal로 admission … 별도 OS thread로 나눠도 된다" 삭제. 나머지 async 금지·언어별 조항 유지 |
| P5 | §1.1 244~245 | 수정 | "request-reply completion은 C에서는 …" → "**multi의** request-reply completion은 …"(범위 한정) |
| P6 | §1.1 267~273 | 삭제 | "single reqrep은 같은 process 안에서 …" 하위 불릿 → "request/reply 패턴은 multi suite에만 있다(D-BP3)" 한 줄 |
| P7 | §1.1.2 375~379 | 수정 | flow-control 예외에서 "single synchronous request callback 또는" 삭제 |
| P8 | §1.3 607~610 | 수정 | `*_REQREP` 해석에서 "binding single은 synchronous callback terminal과 completion progress를 사용한다" 삭제, "multi suite 전용"으로 명시 |
| P9 | §1.3 640~642 | 수정 | binding single `--pattern ALL` 7개 → 5개 |
| P10 | §4.2 1046 | 수정 | bandwidth 설명의 "echo 패턴(single request-reply + multi echo)" → "echo 패턴(multi echo)", one-way는 "single 전 패턴 + multi one-way" |
| P11 | §7.1 1223 | 수정 | 재시도 금지 표에서 "synchronous request callback 또는" 삭제, single one-way transient 재시도는 §1.1.2의 1 ms 절차만 허용한다고 추가 |
| P12 | 144~149(비교 범위) | **유지** | Single이 one-way 전용이 되어도 문언 그대로 정합. bindings ↔ C 비교는 multi로 한정, single은 각 binding의 synchronous path·lifecycle 검증용이라는 서술이 그대로 성립한다 |

### 2.1 P4~P6 반영

| 항목 | 위치 | 처리 |
|---|---|---|
| P4.1 (표본 0개) | §1.1, p95/p99 sample 불릿 뒤 | 설계 문안 채택. cap `0`이나 유효 sample 0개면 p95·p99 = 평균 latency, `0` 금지, count·sum은 계속 누적. **위치를 suite 문서에서 공통 문서로 옮겼다** — single·multi 양쪽에 필요한 규칙이라 두 곳에 복제하면 다시 갈라진다 |
| P4.2 (보간식) | §1.1, 같은 위치 | 설계 문안을 **더 정밀하게 다시 썼다**. 설계 문안은 단일 경로만 `pos=(n-1)q` 선형 보간으로 못박고 병합 경로는 "같은 보간식 + 가중치"라고만 해 가중 경로에서 무엇이 "같은 식"인지 정해지지 않았다. 개정 문안은 가중 경로를 누적 weight 축 위의 같은 선형 보간으로 정의했다: weight = `유효 관측 수 / 보관 sample 수`, `c_i = Σ_{j<=i} w_j`, `W = Σ w_j`, `pos = (W-1)*q`, 위치 `p`의 sample은 `c_i - 1 >= p` 를 만족하는 가장 작은 `i`. 모든 weight가 1이면 단일 경로 식과 항등이다 |
| P5 (시간원) | §1.1, 새 불릿 | 설계 문안("경과 시간·deadline·timeout·drain은 monotonic")을 채택하고 **범위를 넓혔다**. metric header의 `sent_ts_ns`와 수신 판정 시각도 monotonic으로 못박았다(P3.1이 "수신 monotonic 시각"을 요구하고, C-8이 Go·Rust의 wall clock을 실제 위반으로 보고했기 때문). 요구 조건을 "같은 호스트의 perf 프로세스들이 같은 기준점을 공유하는 monotonic 시간원"으로 명시했다 — multi one-way latency가 client의 stamp를 server 프로세스가 비교하기 때문이다. 근거 D-095(WSL2 wall clock ±5 s 점프), framework liveness §2와 같은 규칙 |
| P5 (header 필드) | §1.1.1 | `sent_ts_ns` 설명 "(nanoseconds, **epoch**)" → "(nanoseconds, **monotonic**)" + 아래 불릿 1개. **이 "epoch" 표기가 Go·Rust가 wall clock을 쓴 직접 원인**이므로 반드시 함께 고쳐야 했다 |
| P5 (출력 정밀도) | §4.2 | 설계 문안 채택, **위치는 §1.1이 아니라 §4.2(RESULT line 형식)**로 옮겼다. throughput·bandwidth 소수 3자리, latency 3종 소수 6자리 **고정 소수점**. C 기준 구현이 `std::fixed` + `setprecision(3)/(6)`을 이미 쓴다는 사실을 확인하고 인용했다 |
| P5 (1 ms 재시도) | §1.1.2 | single 정책의 1 ms 절차를 hot loop sleep 금지의 **명시적 예외**로 등록했다. 설계 문서는 이 충돌을 다루지 않았다 — 기존 §1.1.2가 "sleep timer 추가 금지"라 예외 등록 없이 single에만 1 ms를 쓰면 두 문서가 모순된다 |
| P6 (UNSUPPORTED) | 464 뒤 | 설계 문안을 채택하되 **"먼저 binding public API 보강 가능 여부를 판정한다"를 앞에 넣었다**. 기존 `:453-455`가 "binding public API를 보강하거나 … `UNSUPPORTED`로 처리한다"고 이미 두 선택지를 주고 있어, 순서를 명시하지 않으면 개정 문안이 손쉬운 쪽(정책 표에 적고 제외)만 남긴다 |
| P2 | — | **정책 무수정.** 설계 문서 결론대로 정책이 canonical이고 C 러너가 위반이다 |

---

## 3. `PERF_MULTI_TEST_POLICY.md`

REQREP 조항은 **건드리지 않았다**. 추가 2건뿐이다.

| # | 위치 | 처리 |
|---|---|---|
| M1 | 헤더 | v2.2 / 2026-09-07 |
| M2 | §5.3 계산식 | 보간식·가중 병합·표본 0개 규칙을 `PERF_POLICY.md § 1.1` 참조로 연결하고, matched-client 가중 병합 경로도 같은 보간식을 쓴다고 명시(P4) |
| M3 | §12.3 송수신 제어 | `PERF_MULTI_SEND_DRAIN_TIMEOUT_MS` 행 추가(기본 5000). 새 제출 금지·RESULT 집계 증가 금지 명시(P5, 설계 §3.1 A-7) |

`PERF_POLICY.md`의 금지 단계 목록에서 `completion drain` 예외가 single request-reply를 가리키고
있었다. Single에서 REQREP이 사라지면 이 예외가 사라져 multi의 send drain이 금지 대상이 되므로,
예외를 **multi bounded drain으로 옮기고** `PERF_MULTI_SEND_DRAIN_TIMEOUT_MS`를 한도로 연결했다.
Single에는 이 예외가 없다고 명시했다.

---

## 4. 일관성 검증 결과

- 세 문서에서 Single REQREP을 전제한 문장은 남지 않았다
  (`grep -niE "reqrep|request|reply|requester|replier|왕복|ops/s"` 전수 확인).
  Single 문서에 남은 언급은 전부 "제외한다"는 서술이고, v2.2 변경 이력의 과거 기록
  ("공식 목록을 7개로 고정했다")은 이력이므로 보존했다.
- Single pattern 목록: Single §6.1 = 5개, `PERF_POLICY.md` §1.3 = 5개로 일치.
  Multi §8.1의 7개 패턴은 변경하지 않았다.
- 실행 모델: Single = 전용 OS thread + synchronous(one-way 전용),
  Multi = 비동기(`PERF_POLICY.md` 127~131) — 충돌 없음.
- 비교 범위: `PERF_POLICY.md:144-149`(bindings ↔ C 비교는 multi 한정)는 문언 그대로 유효하다.
- 절 번호·상호 참조: Single의 `§ 1.1 / 1.1.2 / 1.3.1 / 2.1 / 3 / 3.2 / 4.3 / 7 / 7.2 / 8`
  참조가 모두 실재하는 절을 가리키는지 확인했다. 삭제한 절은 없고 번호도 바뀌지 않았다
  (§2.0.1, §2.1, §6.1은 표 행만 삭제).
- `BINDINGS_OPTIMIZATION_GUIDE.ko.md`: Single REQREP을 전제한 서술이 **없다**. `:61`의
  "C REQREP 러너"는 completion drain 계약의 근거 사례이고 multi 러너에도 그대로 적용되며,
  `:120`의 single 스모크 목록은 이미 `PAIR·DEALER_ROUTER·PUBSUB`뿐이다. `:112-113`의 기각
  목록("인위적인 in-flight 상한이나 2단계 측정")은 이번 개정 방향(P2 러너 수정, 1 ms 재시도
  대신 busy-spin 금지)과 충돌하지 않는다. **무수정.**
- 영문판: `doc/perf/` 아래에 `*.en.md`가 **없다**(`PERF_POLICY.md`, `PERF_SINGLE_TEST_POLICY.md`,
  `PERF_MULTI_TEST_POLICY.md`, `BINDINGS_OPTIMIZATION_GUIDE.ko.md`뿐). 갱신 대상 없음.

---

## 5. 이 개정으로 러너 수정이 필요해진 항목

러너 담당 에이전트용 목록이다. 근거는 설계 문서 §3의 해당 항목.

### 5.1 REQREP 제외 (D-BP3)

| # | 대상 | 내용 |
|---|---|---|
| R1 | C single | `DEALER_ROUTER_REQREP`·`ROUTER_ROUTER_REQREP` 등록·소스·`--pattern ALL` 제거 |
| R2 | 7개 binding single | 같은 제거. 러너의 pattern 목록, dispatch, 실행 스크립트 기본값, 비교 스크립트 |
| R3 | single runner 완료 판정 | `expected_result_lines` 계산의 조합 수가 5 pattern 기준으로 줄어드는지 확인 |

### 5.2 P3 (active 유효 메시지·wire 길이)

| # | 대상 | 내용 |
|---|---|---|
| R4 | C single one-way | active deadline 필터가 **없다**(설계 §3.3 C-4). 수신 monotonic 시각 < active deadline 필터를 추가한다 |
| R5 | C++·.NET의 `DEALER_ROUTER`·`ROUTER_ROUTER`, Go routed one-way | 같은 필터 부재. 추가한다 |
| R6 | Java·Node·Python·Rust·Go(PAIR/DD/PUBSUB) | 이미 필터가 있으나 기준 시각이 개정 문언(수신 monotonic 시각)과 같은지 확인 |
| R7 | C single one-way | wire 길이 불일치를 **fatal**로 처리한다. 정책은 "집계 제외"이므로 fatal을 제거한다 |
| R8 | Java·.NET single | wire 길이 검증 자체가 없다. 추가한다 |

### 5.3 P4 (latency 통계 경계)

| # | 대상 | 내용 |
|---|---|---|
| R9 | Java single/multi | 표본 0개일 때 percentile fallback이 다르다(설계 §3.3 C-3). p95·p99 = mean으로 통일 |
| R10 | C multi matched-client | `perf_multi_weighted_latency.hpp`의 가중 분위수가 보간 없는 nearest-rank다. 개정된 누적 weight 선형 보간으로 바꾼다 |
| R11 | .NET single | latency sampler가 reservoir가 아니라 전수 축적(설계 §3.3 C-2). Algorithm R reservoir로 바꾼다 |
| R12 | C single | `perf_single_latency.hpp:100-111`의 `default_sample_cap()`이 음수 문자열을 거르지 않는다(단순 버그) |

### 5.4 P5 (상수·시간원·정밀도)

| # | 대상 | 내용 |
|---|---|---|
| R13 | Go·Rust | 시간원이 wall clock이다(`time.Now().UnixNano()`, `SystemTime::now()`). monotonic으로 바꾼다. **cross-process 비교 가능한 monotonic ns를 얻는 공개 경로를 각 언어에서 확인해야 한다(아래 §6-2 미확인)** |
| R14 | C++ ROUTER_ROUTER, .NET 4개 패턴, Node 전 패턴, Rust | transient 재시도가 busy-spin이다. 1 ms 대기 + `sent_ts_ns` 재stamp로 통일 |
| R15 | Python single | transient 재시도가 없고 예외를 전파한다. 같은 절차를 넣는다 |
| R16 | C++(2/3), Java(3/3/3/3/3), Node(3), .NET(문화권 기본 포맷) | RESULT 출력 정밀도를 throughput·bandwidth 3, latency 3종 6 고정 소수점으로 통일 |
| R17 | 전 binding multi | `PERF_MULTI_SEND_DRAIN_TIMEOUT_MS` 기본값 5000 통일(설계 §3.1 A-7) |

### 5.5 P6 (UNSUPPORTED)

| # | 대상 | 내용 |
|---|---|---|
| R18 | Node single | `inproc` 전면 `UNSUPPORTED`(Worker가 Context를 공유하지 못함). 개정 절차대로 (a) Node public API 보강으로 해결 가능한지 먼저 판정하고, (b) 불가능하면 Single §6.3 transport 표에 binding별 제외와 사유를 적은 뒤에만 `UNSUPPORTED`로 남긴다. **현재 표에 없으므로 지금 상태는 정책 위반이다** |

### 5.6 P2 (정책 무수정, 러너 위반)

| # | 대상 | 내용 |
|---|---|---|
| R19 | C single | 별도 1초 latency 단계 제거, throughput·latency를 같은 active 구간에서 집계(D-BP1 B2) |
| R20 | C single one-way | 하드코딩 `max_in_flight=1` 제거. `PERF_POLICY.md § 7.2`가 인위적 flow control을 금지한다 |

---

## 6. 남은 모순 / 감독자 판단이 필요한 항목

1. **Single 기준값 재측정 범위.** P3.1의 deadline 필터(R4)와 P2의 latency 단계 제거(R19)는
   C canonical Single 값을 바꾼다. D-BP4은 "새로 손대는 대상만 새로 짝지어 재고, 닫힌 판정은
   대표 셀 spot-check"라고 했다. Single 기존 `통과`·`미달` 표를 어디까지 보존하고 어디부터
   재측정할지는 감독자 판단이다. 정책 문안에는 "개정 조항은 개정 이후 측정에 적용"만 넣었다.
2. **[미확인] 언어별 cross-process monotonic 시간원.** 정책은 "같은 호스트의 perf 프로세스들이
   같은 기준점을 공유하는 monotonic 시간원"을 요구한다. C의 `steady_clock`이 이 조건을
   만족한다는 것은 현재 multi one-way latency가 client stamp를 server에서 비교하는 구조로
   동작한다는 사실로 확인했다. **7개 binding 각각에서 이 조건을 만족하는 공개 API가
   무엇인지는 검증하지 않았다.** 그래서 정책에 언어별 API 이름을 적지 않고 요구 조건과
   "만족하는 공개 시간원이 없는 언어가 확인되면 정책 예외로 기록해 결정을 받는다"만 넣었다.
   R13 착수 전에 언어별 확인이 필요하다.
3. **[미확인] `sent_ts_ns`의 int64 monotonic 표현.** header 필드는 `int64_le`이고 monotonic
   기준점은 boot이므로 표현 범위 문제는 없다고 보지만, Windows QPC 기반 구현에서 ns 변환
   방식이 언어마다 다를 수 있다. Windows 측정은 이번 조사 범위 밖이었다.
4. **P4.2 가중 보간식의 회귀 영향.** R10은 C multi matched-client 경로의 p95/p99 값을
   바꾼다(nearest-rank → 선형 보간). matched-client 경로로 낸 기존 판정이 있다면 그 셀의
   p95/p99가 미세하게 달라진다. throughput·mean은 영향 없다.
5. **P6과 Node `inproc`.** 정책 표에 Node `inproc` 제외를 적을지 여부는 판단하지 않았다.
   "Worker가 Context를 공유하지 못한다"가 Node binding 공개 API의 한계인지 러너 설계의
   한계인지 확인하지 않았기 때문이다. 근거 없이 제외를 정책에 적으면 회귀 은폐가 되므로
   비워 두었다(R18).
6. **설계 문서 §4-P1의 개정안 A·B·C는 모두 불채택**(D-BP3)이므로 정책에 반영하지 않았다.
   `ZLINK_POLLOUT`을 admission 경계로 쓰는 문안도 넣지 않았다. `core/doc/spec/core/05-polling.ko.md:54-61`이
   POLLOUT을 target별 admission 신호로 쓰는 것을 부정하고 있어, 그 문안을 넣었다면 다시
   "공개 계약이 뒷받침하지 않는 요구"가 됐을 것이다.

---

## 7. 변경 파일

- `doc/perf/PERF_POLICY.md` (v2.1 → v2.2)
- `doc/perf/PERF_SINGLE_TEST_POLICY.md` (v2.1 → v2.3)
- `doc/perf/PERF_MULTI_TEST_POLICY.md` (v2.1 → v2.2)
- `doc/perf/perf/bindings-0.17.0/log/2026-09-07-policy-amendment.ko.md` (본 문서)

`doc/perf/BINDINGS_OPTIMIZATION_GUIDE.ko.md`는 무수정(§4 참조).
