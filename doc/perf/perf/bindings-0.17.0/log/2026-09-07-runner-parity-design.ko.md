# perf 러너 정책 준수(policy-compliance) 설계 — 2026-09-07

> 상태: **설계안**. 코드·정책 문서·계획서를 수정하지 않았고 perf를 실행하지 않았다.
> 범위: `bindings/c/perf`와 `bindings/{cpp,dotnet,java,node,go,rust,python}/perf`의 single·multi 러너.
> `framework/**`는 범위 밖.

## 0. 합격 기준과 전제

**유일한 합격 기준: perf 러너는 `doc/perf`의 정책 문서를 만족하게 작성되기만 하면 된다.**

- 판정은 "C 러너 코드와 같은가"가 아니라 "**정책 문서를 만족하는가**"다.
- **C 러너도 예외가 아니다.** C canonical 러너가 정책을 어기고 있으면 C 러너를 고친다.
- 정책이 침묵하는 부분에서만 C 구현을 준용하며, 그 경우 본문에 **`[정책 미규정 → C 구현 준용]`**
  으로 표시한다. 정책이 규정한 부분에서 C 구현과 정책이 다르면 **정책을 따른다**.

확정된 전제(재논의하지 않음):

| 전제 | 내용 | 근거 |
|---|---|---|
| B1 | 포화 제출 경계를 **공개 계약만으로** 정의한다. 새 공개 API를 추가하지 않는다 | 사용자 결정 |
| B2 | C single의 **별도 1초 latency 단계를 제거**한다. Single 기준값 전면 재측정 | 사용자 결정 |
| 실행 모델 | Single = 전용 OS thread + synchronous API / Multi = C만 nonblocking+poller, 나머지는 각 언어 비동기 실행 모델 | `PERF_SINGLE_TEST_POLICY.md:50-65`, `PERF_POLICY.md:127-131` |
| Gate 범위 | **Single suite는 cross-binding 성능 gate에서 제외**한다. 성능 판정은 Multi suite로만 한다 | `PERF_POLICY.md:144-149` + 사용자 결정 |
| Single 준수 | gate에서 빠지더라도 **7개 binding single 러너도 정책대로 전부 고친다** | 사용자 결정 |
| Core artifact | `core/build/lib/libzlink.so.0.17.0`, Build ID `af759a1c5532fb7100c6baede89144814200d798`, SHA-256 `0af61ad39b5830fdb3f2f8538aed9f26bea70487bbb862df2df1a6f6023dfd72`, commit `c39f50f6dc`, Release+LTO | `log/2026-09-07-environment.ko.md` |
| 실행 금지 | 이 캠페인에서 **`--core-version 0.17.0` 사용 금지.** perf 러너는 `ZLINK_CORE_SOURCE=local`에서 `core/build`를 직접 쓴다. `~/.cache/zlink/core/0.17.0/...`에는 `core/build-dev` 유래의 다른 artifact(RelWithDebInfo/LTO OFF)가 있다 | 감독자 확정 |

---

## 1. 결론 요약

정책이 요구하는 canonical 모델은 다음과 같다.

모든 러너는 `ready -> active(duration)` 단일 구간에서 throughput과 latency를 **같은 유효 메시지
집합**으로 산출한다(`PERF_SINGLE_TEST_POLICY.md:37`, `:293`; `PERF_MULTI_TEST_POLICY.md:673`).
active 구간의 **부하 수준**은 "응답을 기다리지 않고 admission backpressure 경계까지 연속 제출"로
고정한다(`PERF_SINGLE_TEST_POLICY.md:71-73`, `:128-136`; `PERF_MULTI_TEST_POLICY.md:161-168`;
`PERF_POLICY.md:1228-1243`). **실행 모델**은 suite별로 정책이 고정한다 — Single은 전 언어가
전용 OS thread + synchronous API이고 측정 구간에 coroutine/async task/Promise·Future executor/
event-loop yield를 쓰지 않으며(`PERF_SINGLE_TEST_POLICY.md:50-65`), Multi는 C만 nonblocking+poller
이고 나머지 binding은 각 언어의 비동기 실행 모델을 쓰되 `DONTWAIT`+`POLLOUT` pending table로
C를 복제하면 위반이다(`PERF_MULTI_TEST_POLICY.md:61-71`, `:85-86`; `PERF_POLICY.md:216-223`,
`:250-259`). latency sample cap 기본값·`0` 처리·bounded reservoir·percentile 보간
(`PERF_POLICY.md:95-97`, `:1498-1499`; `PERF_MULTI_TEST_POLICY.md:688-689`), header 검증 범위
(`PERF_POLICY.md:299-360`; `PERF_SINGLE_TEST_POLICY.md:270-293`), 수신 readiness 대기와
`DONTWAIT` drain(`PERF_SINGLE_TEST_POLICY.md:47-49`, `:185-195`), stop token·종료 protocol
(`PERF_SINGLE_TEST_POLICY.md:183-200`; `PERF_MULTI_TEST_POLICY.md:254-282`, `:370-397`),
그리고 실행 조건 전부(`PERF_POLICY.md:80-82`; `PERF_MULTI_TEST_POLICY.md:301-331`)는 전 언어
동일하다. 성능 판정은 Multi suite로만 한다(`PERF_POLICY.md:144-149`).

현재 상태 요약:

- **정책 위반은 C 러너를 포함해 8개 러너 전부에 있다.** 가장 광범위한 것은
  (i) Single REQREP의 in-flight 1 직렬화(7개 binding 전부), (ii) C single의 별도 latency 단계와
  `max_in_flight=1`(C 러너), (iii) Multi 종료 protocol 미준수(C++·.NET·Java·Node·Rust·Python),
  (iv) Single 실행 모델의 async 사용(다수 언어)이다.
- **측정 조건(HWM 보고, monitor HWM 이름, `default_stream_clients`, Effective Options key,
  `select_transports()`) 불일치가 별도로 존재**하며, 이는 의미 정합보다 **먼저** 고쳐야 한다.
- **정책 자체에 구현 불가능한 조항이 하나 있다.** `PERF_SINGLE_TEST_POLICY.md:55-58`,
  `PERF_POLICY.md:119-126`는 single request/reply에 "synchronous **callback** terminal"을 요구하지만
  7개 binding 공개 계약에는 그런 terminal이 없다
  (`bindings/doc/spec/async-coroutine-policy.ko.md:116` "request callback terminal을 제공하지 않는다").
  §4-P1의 정책 개정이 필요하다.

---

## 2. 정책 요구사항 추적표

### 2.0 표 읽는 법

- **상태**: `O` 충족 / `X` 위반 / `~` 부분 충족 / `?` 미확인 / `-` 해당 없음
- 각 조항은 `doc/perf` 문서의 **줄 번호**로 식별한다.
- 근거 파일:줄은 §3 위반 목록에 있다(표에는 상태만 둔다).
- 언어 열 순서: **C, C++, .NET, Java, Node, Go, Rust, Python**

### 2.1 공통 조항 (양 suite)

| # | 정책 조항 | 근거 | C | C++ | .NET | Java | Node | Go | Rust | Py |
|---|---|---|---|---|---|---|---|---|---|---|
| K1 | metric header 29B 고정 레이아웃·LE·magic `0x5A4C4E4B` | `PERF_POLICY.md:311-327` | O | O | O | O | O | O | O | O |
| K2 | `run_id`는 **1-based benchmark case ordinal**, 여러 case 순차 수행 시 `1,2,3,...`으로 증가 | `PERF_POLICY.md:349-358` | O | ~ | ~ | X | X | X | X | ? |
| K3 | receiver는 `magic`/`phase==active`/`msg_size`/`run_id` 일치만 집계 | `PERF_POLICY.md:328-329,359-360` | O | O | O | O | O | O | O | O |
| K4 | 공식 wire shape는 2-part `[payload, empty]` | `PERF_POLICY.md:304-307` | O | O | O | O | O | O | O | O |
| K5 | latency는 내부 ns 누적, RESULT/report는 ms 표기 | `PERF_POLICY.md:346-347` | O | O | O | O | O | O | O | O |
| K6 | RESULT line 필수 5 metric과 의미 불변 | `PERF_POLICY.md:1037-1049`, `:137` | O | O | O | O | O | O | O | O |
| K7 | bandwidth: echo `thr×size×2/1e6`, one-way `thr×size/1e6` | `PERF_POLICY.md:1046` | O | O | O | O | O | O | O | O |
| K8 | **inflight/outstanding 상한을 코드로 고정하지 않는다** | `PERF_POLICY.md:1228-1243` | **X** | **X** | **X** | **X** | O | **X** | ~ | **X** |
| K9 | 바이너리 내부 retry 금지(고정 횟수·무조건 즉시 반복) | `PERF_POLICY.md:1216-1226` | O | ~ | ~ | O | ~ | ~ | ~ | ~ |
| K10 | hot loop 안 sleep/yield/retry budget 금지 | `PERF_POLICY.md:366-379` | ~ | ~ | ~ | O | ~ | ~ | ~ | ~ |
| K11 | 측정 anchor 7종을 같은 의미로 유지 | `PERF_POLICY.md:106-115` | ~ | X | X | X | X | X | X | X |
| K12 | 기본 경로는 context auto-HWM, 숫자 override는 allow flag 필요 | `PERF_POLICY.md:80-82` | O | O | O | O | ~ | O | O | O |
| K13 | perf는 해당 binding의 **public API만** 사용 | `PERF_POLICY.md:70-79` | O | O | O | O | O | O | O | O |
| K14 | RESULT 5 metric은 percentile sample 상한과 무관하게 전체 count·sum 유지 | `PERF_POLICY.md:95-97` | O | O | **X** | ~ | O | O | O | O |
| K15 | runner CLI 옵션 이름·의미와 출력 포맷이 언어별로 달라지지 않는다 | `PERF_POLICY.md:134-138` | O | **X** | **X** | **X** | **X** | ~ | **X** | **X** |
| K16 | handshake stdout/stdin token·방향·start/stop 의미 동일 | `PERF_POLICY.md:139-143`, `:406-467` | O | X | X | X | X | ~ | X | X |

> S9 주: .NET은 직렬화가 아니라 **반대 방향 위반**이다 — `Async()`를 쓰되 admission 경계가 없어
> outstanding이 무제한 증가한다(§3.2 B-1). 정책이 요구하는 것은 "무제한"이 아니라 "admission
> backpressure 경계까지"이다.
>
> K2 주: C++·.NET은 프로세스가 case 1개만 수행하므로 하드코딩 `1`이 `PERF_POLICY.md:355`
> ("프로세스가 case 1개만 수행하는 경우 `run_id`는 반드시 `1`")를 만족한다. Java/Node/Go/Rust는
> 여러 size를 순차 수행하는 경로에서도 상수 `1`을 쓰므로 `:356-358` 위반이다.

### 2.2 Single suite 조항

| # | 정책 조항 | 근거 | C | C++ | .NET | Java | Node | Go | Rust | Py |
|---|---|---|---|---|---|---|---|---|---|---|
| S1 | 측정 모델 `ready -> active(duration)`, 별도 latency 단계 없음 | `PERF_SINGLE_TEST_POLICY.md:30`, `:206-210` | **X** | O | O | O | O | O | O | O |
| S2 | **같은 active 구간에서 동일 메시지 집합으로 throughput·latency 집계** | `PERF_SINGLE_TEST_POLICY.md:37`, `:293` | **X** | O | O | O | O | O | O | O |
| S3 | 수신은 recv 모델 — poller `POLLIN` readiness + nonblocking `recv` drain | `PERF_SINGLE_TEST_POLICY.md:47-49` | O | ~ | ~ | O | **X** | **X** | **X** | ~ |
| S4 | receiver poller wait timeout `-1` | `PERF_SINGLE_TEST_POLICY.md:185-190` | O | ~ | ~ | O | **X** | **X** | **X** | **X** |
| S5 | **전용 OS thread + synchronous API**; 측정 구간 coroutine/async task/Promise·Future executor/event-loop yield 금지 | `PERF_SINGLE_TEST_POLICY.md:50-60`, `PERF_POLICY.md:118-126` | O | O | **X** | O | ~ | O | **X** | O |
| S6 | 언어별 명시 조건(Go `LockOSThread`, Node `worker_threads`, Python `threading.Thread`, C++ `co_await` 금지, .NET `Task` 금지, Rust Future executor 금지) | `PERF_SINGLE_TEST_POLICY.md:61-65` | - | O | **X** | - | **X** | O | **X** | O |
| S7 | raw send는 **blocking terminal**(HWM 도달 시 Core가 sender thread 대기) | `PERF_SINGLE_TEST_POLICY.md:53-54`, `:96-97` | O | O | O | O | O | O | ~ | O |
| S8 | one-way에 `EAGAIN` 기반 pending 관리·send-ready handler 금지 | `PERF_SINGLE_TEST_POLICY.md:128-130` | O | O | O | O | O | O | O | O |
| S9 | **REQREP: 응답 1건 대기 RTT 루프 금지, public request API가 허용하는 만큼 연속 제출 + reply completion 계속 drain** | `PERF_SINGLE_TEST_POLICY.md:71-73`, `:111-116`, `:131-136` | O | **X** | **X** | **X** | **X** | **X** | **X** | **X** |
| S10 | REQREP 유효 집계: active window 안에서 제출·완료된 왕복만 | `PERF_SINGLE_TEST_POLICY.md:137-142`, `:288-291` | O | O | O | O | O | O | O | O |
| S11 | active 종료 후 bounded idle drain(one-way) / completion drain(reqrep) 필수 | `PERF_SINGLE_TEST_POLICY.md:144-151`, `:256-259` | O | ? | ? | ? | ? | ? | ? | ? |
| S12 | 종료는 wire-level stop token(`__zlink_perf_stop__`), atomic flag + 짧은 polling 금지 | `PERF_SINGLE_TEST_POLICY.md:183-195` | O | ~ | ~ | O | ~ | ~ | ~ | ~ |
| S13 | single에는 runner stdin/stdout `READY`/`CLIENT_READY`/`START` orchestration을 만들지 않는다 | `PERF_SINGLE_TEST_POLICY.md:236-238` | O | O | O | O | O | O | O | O |
| S14 | size마다 perf 바이너리를 다시 실행, 바이너리는 `RESULT` line만 출력 | `PERF_SINGLE_TEST_POLICY.md:155-159` | O | O | O | ? | ? | ? | ? | ? |
| S15 | ready gate는 raw socket `CONNECTION_READY`; PUBSUB만 bounded post-ready settle | `PERF_SINGLE_TEST_POLICY.md:214-215`, `:441-460` | O | O | O | O | O | O | O | O |
| S16 | latency sample cap 기본 1,000,000, `0`이면 sample 미보관 | `PERF_SINGLE_TEST_POLICY.md:519`, `PERF_POLICY.md:1499` | O | O | **X** | ~ | O | O | O | O |
| S17 | `PERF_IO_THREADS` 기본 `1` | `PERF_SINGLE_TEST_POLICY.md:493-495` | O | ? | ? | ? | ? | ? | ? | ? |
| S18 | active duration 기본 5s | `PERF_SINGLE_TEST_POLICY.md:216`, `:501` | O | O | O | O | O | O | O | O |
| S19 | 지원 패턴 정확히 7개, STREAM 제외 | `PERF_SINGLE_TEST_POLICY.md:410-420` | O | O | O | O | ~ | O | O | O |
| S20 | throughput 단위: one-way `Kmsg/s`, reqrep `Kops/s`; bandwidth 식 분리 | `PERF_SINGLE_TEST_POLICY.md:464-472` | O | O | O | O | O | O | O | O |

### 2.3 Multi suite 조항

| # | 정책 조항 | 근거 | C | C++ | .NET | Java | Node | Go | Rust | Py |
|---|---|---|---|---|---|---|---|---|---|---|
| M1 | 측정 모델 `ready -> active`, 같은 구간에서 throughput·latency | `PERF_MULTI_TEST_POLICY.md:29`, `:563`, `:673` | O | O | O | O | O | O | O | O |
| M2 | C reference는 nonblocking API + poller, `EAGAIN` → pending → `POLLOUT` 재개 | `PERF_MULTI_TEST_POLICY.md:49-54`, `:141-142` | O | - | - | - | - | - | - | - |
| M3 | **C 이외 binding은 public async terminal + 언어 async runtime**; sync `DONTWAIT`+binding-local `POLLOUT` pending으로 C 복제 금지 | `PERF_MULTI_TEST_POLICY.md:61-71`, `:85-86`; `PERF_POLICY.md:250-259` | - | O | O | O | O | O | O | **X** |
| M4 | **request/reply: inflight를 인위적으로 고정하지 않고 admission backpressure까지 연속 제출** | `PERF_MULTI_TEST_POLICY.md:161-168`; `PERF_POLICY.md:1240-1243` | O | ~ | **X** | **X** | **X** | **X** | ~ | **X** |
| M5 | echo record 수신이 send를 gate하지 않는다(1:1 ping-pong 금지) | `PERF_MULTI_TEST_POLICY.md:146-148` | O | O | O | O | O | O | O | O |
| M6 | PUB/XPUB publish와 raw reply는 **synchronous terminal** | `PERF_MULTI_TEST_POLICY.md:87-93`; `PERF_POLICY.md:237-243` | O | O | O | O | O | O | O | O |
| M7 | `MULTI_STREAM` server는 packet handler마다 public async terminal 1회, `DONTWAIT`/`POLLOUT`/pending deque/timer 재제출 금지 | `PERF_MULTI_TEST_POLICY.md:130-136`; `PERF_POLICY.md:293-297` | ? | O | O | ~ | ~ | ? | ? | ~ |
| M8 | `MULTI_STREAM` 외부 raw client는 연결당 unresolved echo 1개, 공용 C 바이너리 사용 허용 | `PERF_MULTI_TEST_POLICY.md:151-160`; `PERF_POLICY.md:161-167` | O | O | O | O | O | O | O | O |
| M9 | wire stop token으로 종료되는 recv/readiness loop의 poller wait는 `-1` | `PERF_MULTI_TEST_POLICY.md:209-215`, `:225` | O | ? | O | **X** | O | ~ | **X** | ~ |
| M10 | `MULTI_PUBSUB` receiver는 `min(100ms, remaining)` bounded wait | `PERF_MULTI_TEST_POLICY.md:228`, `:263-268` | O | ? | ? | ~ | ? | O | ~ | O |
| M11 | 짧은 timer tick 기반 fallback(1–25ms) 금지, empty-poll 예외만 허용 | `PERF_MULTI_TEST_POLICY.md:230` | O | ? | ? | ? | ? | ? | **X** | **X** |
| M12 | **종료 protocol: REQREP client는 `CLIENT_DONE` 출력 후 socket 유지 → runner가 server `STOP` → client `STOP` → client close** | `PERF_MULTI_TEST_POLICY.md:379`, `:381`, `:386-388`; `PERF_POLICY.md:448-451` | O | **X** | **X** | **X** | **X** | O | **X** | **X** |
| M13 | size마다 독립 server/client 프로세스 쌍, size 간 상태 공유 금지 | `PERF_MULTI_TEST_POLICY.md:593-601` | ~ | O | O | ? | ? | ? | ? | ? |
| M14 | `PERF_IO_THREADS` 기본 `4`(server·client 공통) | `PERF_MULTI_TEST_POLICY.md:308-310` | O | O | ? | ? | ? | ? | ? | ? |
| M15 | 기본 HWM은 context auto-HWM, 연결 수 반영 재계산 수행 | `PERF_MULTI_TEST_POLICY.md:301-315` | O | ~ | O | O | O | **X** | O | O |
| M16 | latency divisor: 왕복 `2`, one-way `received_count` | `PERF_MULTI_TEST_POLICY.md:677-680`, `:690-691` | O | O | O | O | O | O | O | O |
| M17 | mean은 전체 유효 record count·sum, p95/p99만 bounded reservoir | `PERF_MULTI_TEST_POLICY.md:687-689`, `:703` | O | O | O | ~ | O | O | O | O |
| M18 | multi latency sample cap 기본 65,536 | `PERF_MULTI_TEST_POLICY.md:1253`; `PERF_POLICY.md:1498` | O | ~ | O | O | O | O | O | O |
| M19 | header 필터: `phase==active`, `msg_size==expected`, `run_id==case ordinal` | `PERF_MULTI_TEST_POLICY.md:712-716` | O | O | O | O | O | O | O | O |
| M20 | active duration 기본 5s, active 이전 warmup phase 금지 | `PERF_MULTI_TEST_POLICY.md:571`, `:613-615` | O | O | O | O | O | O | O | O |
| M21 | 지원 패턴 정확히 7개 | `PERF_MULTI_TEST_POLICY.md:805-815` | O | ? | ? | ? | ? | ? | ? | ? |
| M22 | ready source dispatch — poller가 ready로 보고한 source만 처리 | `PERF_MULTI_TEST_POLICY.md:105-118` | O | O | O | ? | ? | ? | ? | ? |

### 2.4 표에 남은 `?`(미확인)의 처리

`?`는 "조사하지 않았다" 또는 "소스에서 확정하지 못했다"는 뜻이며 **충족으로 간주하지 않는다.**
§5의 작업 순서에서 각 러너를 고칠 때 해당 칸을 반드시 확정한다. 특히 S5·S6(Single 실행 모델),
K12·M14·M15(실행 조건), M21(패턴 inventory)은 전수 확인이 필요하다.

---

## 3. 위반 목록

분류: **(a) 측정 조건** / **(b) 실행 모델** / **(c) 집계·anchor** / **(d) handshake·출력 형식**

### 3.1 (a) 측정 조건 — **최우선. 의미 정합보다 먼저 고친다**

조건이 다르면 §3.2~§3.4를 모두 고쳐도 숫자는 무효다(계획서 §6 `:392-415`, §7.0.1 `:459`,
§12 `:1396-1399`).

#### A-1 (a) server DEALER의 보고된 effective HWM이 4배 다르다 — C++

- 관측: C `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_080912.txt:63-64`
  server `1048576/1048576`; C++ `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_081121.txt:63-64`
  server **`4096000/4096000`**(client는 양쪽 `1048576`).
- **`4096000`은 Core의 SNDHWM·RCVHWM 기본값이다** — `core/doc/spec/core/socket/README.ko.md:357-358`.
  즉 C++ server 행은 auto-HWM이 반영되지 않은 raw 기본값이다.
- **감독자 추정("monitor HWM을 server 소켓에 적용")은 소스로 지지되지 않는다.** C++ server의
  유일한 HWM 설정 경로는 `bindings/cpp/perf/multi/src/perf_dealer_dealer_server.cpp:24-31`이며
  `manual_socket_overrides_enabled()`가 켜졌을 때만 동작한다(기본 off). C도 동일하게 no-op이다
  (`bindings/c/perf/multi/common/perf_multi_runtime.hpp:562-565`). monitor HWM은 monitor handle
  에만 쓰인다(§A-2).
- **소스로 확정한 차이는 snapshot 채취 시점이다.**
  - C++: `recalculate_auto_hwm(ctx)` **직후, 트래픽 이전** 1회
    (`bindings/cpp/perf/multi/src/perf_dealer_dealer_server.cpp:145-148`).
  - C: active 수신 루프에서 **첫 유효 metric 메시지 수신 시** 1회
    (`bindings/c/perf/multi/src/perf_multi_dealer_dealer_server.cpp:136-142`).
  - 두 러너 모두 `ZLINK_SOCKET_MONITOR_EVENT_ALL`로 새 monitor를 열어
    `auto_hwm_applied_sndhwm_bytes`를 읽는다
    (`bindings/c/perf/multi/common/perf_multi_runtime.hpp:310-318`;
    `bindings/cpp/perf/multi/common/perf_common.hpp:388-393`, `:422-423`).
- **미확인**: 시점 차이만으로 raw 기본값이 보고되는지 — 즉 (i) 실제 effective HWM이 다른지,
  (ii) recalculate 직후 새로 연 monitor가 `auto_hwm_applied_*`를 아직 채우지 않아 **보고만**
  잘못된 것인지. 판별에는 Core의 monitor snapshot 채움 시점 확인이 필요하다(Core 범위).
- **조치(어느 쪽이든 동일)**: C++ 러너의 snapshot 채취 지점을 C와 같은 위치(active 수신 중
  첫 유효 메시지 1회)로 옮긴다. 그 전까지 이 조합의 paired 결과는
  `PERF_MULTI_TEST_POLICY.md:301-315`(M15)과 계획서 §12 `:1398`을 만족하지 못하므로 무효다.
  **나머지 6개 binding도 같은 위치인지 전수 확인**한다(§3.1 A-6).

#### A-2 (a) monitor HWM의 key 이름과 reporting 기본값 불일치 — C++

| 항목 | C | C++ |
|---|---|---|
| Effective Options key | `monitor_hwm_bytes` (`bindings/c/perf/run_comparison.py:4077`) | `monitor_hwm` (`bindings/cpp/perf/run_comparison.py:4038`) |
| `run_comparison.py` 기본값 | `4096000` | **`1000`** ← 버그 |
| shell wrapper 기본값 | `4096000` (`bindings/c/perf/run_benchmarks_multi.sh:800`) | `4096000` (`bindings/cpp/perf/run_binding_multi.sh:515`) |
| 바이너리 기본값 | `4096000` (`bindings/c/perf/multi/common/perf_multi_runtime.hpp:545`) | `4096000` (`bindings/cpp/perf/multi/common/perf_common_multi.hpp:234-235`) |
| CLI 옵션 | `--monitor-hwm-bytes` (`run_benchmarks_multi.sh:625`) | `--monitor-hwm` (`run_binding_multi.sh:286`) |
| 환경 변수 | `PERF_MULTI_MONITOR_HWM_BYTES`/`PERF_MONITOR_HWM_BYTES` | `PERF_MULTI_MONITOR_HWM`/`PERF_MONITOR_HWM` |
| 단위 | bytes | **bytes(동일)** — `zlink::byte_count_t::bytes(...)` (`bindings/cpp/perf/multi/common/perf_common.hpp:499-501`) |
| 적용 대상 | monitor handle only | **monitor handle only(동일)** (`perf_common.hpp:511-515`) |

- **판정**: 단위·적용 대상이 같으므로 **측정값에 주는 영향은 없다.** 위반은 두 가지다 —
  (1) `PERF_POLICY.md:134-138`(K15) "옵션 이름과 의미를 언어별로 바꾸지 않는다" 위반,
  (2) C++ `run_comparison.py` 기본값 `1000`이 실제 기본값 `4096000`과 달라 env 미설정 실행에서
  **Effective Options에 거짓값이 찍힌다**(단순 버그).
- **조치**: C canonical 이름(`monitor_hwm_bytes`, `--monitor-hwm-bytes`,
  `PERF_MULTI_MONITOR_HWM_BYTES`)으로 통일하고 기본값을 `4096000`으로 고친다.

#### A-3 (a) `default_stream_clients` 상수 불일치 — C++

- C `bindings/c/perf/run_comparison.py:3960` = `100`, C++ `bindings/cpp/perf/run_comparison.py:3921` = `10000`.
- 실행값은 양쪽 `100`이지만 report Effective Options가 갈린다
  (`perf_c_multi_linux_20260907_080912.txt:27` vs `perf_cpp_multi_linux_20260907_081121.txt:27`).
- **단순 버그.** `MULTI_STREAM` paired 측정 전에 C 값 `100`으로 통일한다. 실제로 STREAM을 돌리면
  client 수가 갈려 조건 불일치가 된다.

#### A-4 (a) Effective Options key 이름 불일치 — C++

- 확인: `monitor_hwm_bytes` ↔ `monitor_hwm`(A-2), `routed_echo_per_socket_payload`(C,
  `perf_c_multi_linux_20260907_080912.txt:69`) ↔ `routed_echo_borrow_payload`(C++).
- **이름만 다른지 의미까지 다른지는 미확인**(§3.1 A-6에서 확정). 이름이 다르면 계획서
  §12 `:1396`("옵션 일치 근거")의 자동 대조가 불가능하다. → C canonical 이름으로 통일.

#### A-5 (a) `select_transports()` 차이 — C++

- C만 `CONTROL_PLANE_PATTERNS`를 처리하고 `--transports` 지정 시 순서 규칙이 다르다.
- 현재 범위(`tcp` 단독)에서는 영향 없음. 전체 matrix에서는 **케이스 순서가 달라져
  `transport_transition_ms` cooldown 효과와 TIME_WAIT 상태가 달라진다**
  (`PERF_MULTI_TEST_POLICY.md:581`). → C 규칙으로 통일.

#### A-6 (a) 나머지 6개 binding의 같은 유형 — 전수 조사 결과

**핵심: monitor HWM 값을 데이터 소켓에 잘못 적용하는 binding은 하나도 없다.** 전 언어가 기본
경로에서 데이터 소켓 HWM을 context auto-HWM에 위임한다(K12). 실제 조건 차이는 아래 표와
그 뒤의 8건이다.

| 언어 | (a) 데이터 소켓 HWM 경로 | (a') auto-HWM recalc 호출 지점 수 | (b) monitor HWM key/CLI/env | (c) `default_stream_clients` | (d) `routed_echo_*` key | (e) transport 선택 |
|---|---|---|---|---|---|---|
| **C(기준)** | override gate, 기본 hwm `0`=auto (`bindings/c/perf/multi/common/perf_multi_runtime.hpp:562`; `bindings/c/perf/run_comparison.py:1248-1249`) | 5 (REQREP server 제외) | `monitor_hwm_bytes` / `--monitor-hwm-bytes` / `PERF_(MULTI_)MONITOR_HWM_BYTES`, 4096000 bytes | 100 (`run_comparison.py:3960`) | `routed_echo_per_socket_payload`, SENDSEND 2패턴만 (`run_comparison.py:3908-3918`) | `select_transports()` + `CONTROL_PLANE_PATTERNS`(빈 튜플), 사용자 지정 순서 유지 + base 교집합 (`run_comparison.py:669-687`) |
| C++ | 동일(override gate) | server는 START 직후 recalc, **snapshot 채취 시점이 C와 다름**(§A-1) | `monitor_hwm` / `--monitor-hwm` / `PERF_(MULTI_)MONITOR_HWM` — **이름만 다름**, 단 `run_comparison.py:4038` 기본값 `1000`은 버그 | **10000** (`bindings/cpp/perf/run_comparison.py:3921`) — 위반 | `routed_echo_borrow_payload` — **이름 다름** | `select_transports()` 있음, 순서 규칙 C와 다름 |
| .NET | 동일 (`bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/common/PerfCommonMulti.cs:178-198`, 기본 0 `.../PerfOptions.cs:154`) | 15 (C에 없는 REQREP 지점 포함, `PerfCommonMulti.cs:201-208`) | `monitor_hwm` / `--monitor-hwm` / `PERF_(MULTI_)MONITOR_HWM`, 4096000 bytes — **이름만 다름** | 100 (`.../PerfOptions.cs:186`) | **`routed_echo_borrow_payload` + 대상 패턴이 REQREP까지 확장** (`bindings/dotnet/perf/multi/run_benchmarks.sh:1646-1653`) — **의미까지 다름** | `select_transports` 없음, `--transports` **무필터** (`multi/run_benchmarks.sh:79`) |
| Java | 동일 (`bindings/java/perf/common/src/main/java/systems/zlink/perf/PerfTransport.java:80-115`, 기본 0 `PerfArgs.java:117-122`) | 22 | `monitor_hwm` / `--monitor-hwm` / `PERF_(MULTI_)MONITOR_HWM`, 4096000 bytes — **이름만 다름** | 100 (`PerfArgs.java:89-90`) | **key 없음** | `select_transports` 없음, **무필터** (`multi/run_benchmarks.sh:64-66`) |
| Node | override gate는 있으나 **override 시 기본 HWM `1000`** (`bindings/node/perf/multi/perf_multi_runtime.ts:121-126`) — C는 `0`. **의미까지 다름** | 22 | `monitor_hwm` / `--monitor-hwm` / `PERF_(MULTI_)MONITOR_HWM`, 4096000 bytes — 이름만 다름 | 100 (`perf_multi_policy.ts:77`) | **key 없음** | 패턴별 정적 테이블 (`perf_multi_policy.ts:47-55`), **ipc는 ROUTER_ROUTER 계열만** |
| Go | 동일 (`bindings/go/perf/internal/perfcommon/common.go:394-410`, 기본 0 `:345-351`) | **1 (STREAM만)** — dealer/router/pubsub server에서 phase별 recalc 누락 (`bindings/go/perf/multi/perf_multi_stream.go:47`) | `monitor_hwm_bytes` / `--monitor-hwm-bytes` / `PERF_(MULTI_)MONITOR_HWM_BYTES` — **C와 동일** | 100 (`run_benchmarks_multi.sh:1421`) | **key 없음** (+ `service_clients` 누락, `go_gomaxprocs*` 3개 추가) | 패턴별 함수 (`run_benchmarks_multi.sh:753-770`), ipc는 RR_SENDSEND·명시 요청만, **사용자 지정 시 순서가 base 순서로 고정** |
| Rust | 동일 (`bindings/rust/perf/multi/src/perf_common.rs:1063-1076`) | 3 (`perf_multi_stream_server.rs:74`, `perf_multi_socket_reqrep.rs:109,241`) | `monitor_hwm` / `--monitor-hwm`; **리포트가 리터럴 `4096000` 하드코딩** (`bindings/python/perf/perf_report.py:510`) — **의미까지 다름** | 100 (`run_benchmarks_multi.sh:86`) | **key 없음** (+ `service_clients` 누락) | `select_transports` 없음, **무필터·ipc 미지원** |
| Python | 동일 (`bindings/python/perf/multi/perf_multi_common.py:308-329`, 기본 0 `:233-238`) | 4 | `monitor_hwm` / `--monitor-hwm` / `PERF_(MULTI_)MONITOR_HWM`, 4096000 bytes — 이름만 다름 | 100 (`multi/run_benchmarks.py:460-461`) | **key 없음** (+ `service_clients` 누락, `smoke` 추가) | 패턴별 테이블 (`multi/run_benchmarks.py:167-178`), `_transports_for_pattern`(`:381-384`)이 **사용자 지정 시 무필터** |

추가로 확정한 조건 결함 8건:

| # | 결함 | 파일:줄 | 분류 |
|---|---|---|---|
| A-6-1 | 공용 리포트의 `default_stream_clients` fallback이 **10000** (C=100). env 미설정으로 직접 호출하면 잘못된 값이 찍힌다 | `bindings/python/perf/perf_report.py:474-476` | 단순 버그 |
| A-6-2 | Rust 리포트의 `monitor_hwm`이 리터럴 하드코딩 → `--monitor-hwm` override가 리포트에 반영되지 않음 | `bindings/python/perf/perf_report.py:510` | 단순 버그 |
| A-6-3 | Node override 모드 HWM fallback이 `1000`(C는 `0`=auto) | `bindings/node/perf/multi/perf_multi_runtime.ts:121-126` | 조건 차이 |
| A-6-4 | monitor HWM 네이밍 분기 — Go만 C와 동일, 나머지 5개는 `monitor_hwm`/`--monitor-hwm`/`PERF_(MULTI_)MONITOR_HWM`. 값·단위·기본값은 동일 | 위 표 | `PERF_POLICY.md:134-138` 위반(이름만) |
| A-6-5 | `routed_echo_per_socket_payload` key가 .NET에서 이름·대상 패턴 모두 다르고, Java/Node/Go/Rust/Python에는 **아예 없다** | `bindings/dotnet/perf/multi/run_benchmarks.sh:1646-1653` 외 | 조건 대조 불가 |
| A-6-6 | `service_clients` key 누락 — Go, Rust, Python | 위 표 | 조건 대조 불가 |
| A-6-7 | C에 없는 추가 key — `fail_fast`(.NET/Java/Node/Go/Rust), `smoke`(Python), `go_gomaxprocs*`(Go) | 위 표 | `PERF_POLICY.md:134-138` 위반 |
| A-6-8 | .NET `ApplyMultiSocketOptions`가 `sndTimeo`를 계산만 하고 소켓에 설정하지 않는다(`ReceiveTimeout`만 설정). C는 `SNDTIMEO`/`RCVTIMEO` 둘 다 적용 | `bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/common/PerfCommonMulti.cs:181`, `:197` vs `bindings/c/perf/multi/common/perf_multi_runtime.hpp:700` | 조건 차이 |

**Go의 auto-HWM recalc 누락(A-6 표 (a') 열)** 은 위 8건과 별개로 가장 무거운 조건 결함이다.
`PERF_MULTI_TEST_POLICY.md:313-315`는 "연결 수가 계획에 포함되는 패턴은 target 연결이 준비된 뒤
context auto-HWM을 다시 계산해야 한다"고 규정하는데, Go는 STREAM 1곳에서만 호출한다
(`bindings/go/perf/multi/perf_multi_stream.go:47`). → **M15 위반.**

#### A-7 (a) multi send drain timeout 기본값 불일치

- C `PERF_MULTI_SEND_DRAIN_TIMEOUT_MS` 기본 **5000**
  (`bindings/c/perf/multi/common/perf_multi_client_helpers.hpp:36-39`)
- C++ `default_send_drain_timeout_ms = 1000` (`bindings/cpp/perf/multi/common/perf_common_multi.hpp:22`)
- .NET `ResolveMultiSendDrainTimeoutMs` 기본 **1000**
  (`bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/common/PerfCommonMulti.cs:52-55`)
- 정책에 기본값 규정이 없다 **[정책 미규정 → C 구현 준용]**. drain 시간이 짧으면 완료 왕복이
  집계에서 빠지므로 조건 차이다. → C 값 `5000`으로 통일하고 §4-P5로 정책에 기본값을 명시한다.

#### A-8 (a) .NET `run_comparison.py`가 `--runs`를 하위 러너에 전달하지 않는다

- `bindings/dotnet/perf/run_comparison.py:58`, `:83-93` — 파싱만 하고 전달 안 함.
  이 경로로 호출하면 `runs>1`이 무시되어 median 집계(`PERF_SINGLE_TEST_POLICY.md:33`)가 성립하지 않는다.
- **단순 버그.**

### 3.2 (b) 실행 모델

#### B-1 (b) **Single REQREP이 in-flight 1 RTT 루프다 — 7개 binding 전부 위반**

정책: `PERF_SINGLE_TEST_POLICY.md:71-73`
> "requester는 응답을 하나 받을 때마다 다음 request를 보내는 RTT 전용 루프가 아니라, public
> request API가 허용하는 만큼 request를 연속 제출하고 reply completion을 계속 drain한다."

또한 `:131-136`("inflight request 수를 코드로 관리하거나 상한으로 고정하지 않는다 ...
1:1 ping-pong으로 직렬화하지 않는다"), `PERF_POLICY.md:1240-1243`.

| 언어 | 현재 구현 | 파일:줄 |
|---|---|---|
| C++ | `submit()`(blocking, reply까지 대기)를 루프에서 1건씩 | `bindings/cpp/perf/single/common/perf_single_reqrep.hpp:189-193`, blocking 근거 `bindings/cpp/src/Runtime/Messaging/request_reply.cpp:86-97` |
| .NET | `Async()`를 쓰지만 **in-flight 상한도 backpressure gate도 없어** deadline까지 무제한 제출 | `bindings/dotnet/perf/single/Zlink.BindingBench/src/PerfReqRep.cs:219-224`, `:472-489` |
| Java | `submit_sync()` 직렬 | `bindings/java/perf/single/Zlink.BindingBench/src/main/java/systems/zlink/perf/single/PerfSocketReqRep.java:111-139`, `:161-169` |
| Node | `submit_sync()` 직렬 | `bindings/node/perf/single/perf_socket_reqrep.ts:123-134`, `:45-48` |
| Go | `Submit(ctx)` 직렬 | `bindings/go/perf/single/perf_reqrep.go:37-62` |
| Rust | `submit_sync()` 직렬 | `bindings/rust/perf/single/src/common.rs:742-770` |
| Python | `submit_sync()` 직렬 | `bindings/python/perf/single/perf_single_reqrep.py:110-158`, `:88-90` |

- .NET은 **반대 방향 위반**이다 — 직렬화는 아니지만 상한이 없어 outstanding과 backpressure
  snapshot이 무제한 증가한다. 정책이 요구하는 것은 "무제한"이 아니라 "**admission backpressure
  경계까지**"다.
- **이 항목의 해결은 §4-P1(정책 결함) 없이는 불가능하다.** 상세는 §4-P1.

#### B-2 (b) **C single 러너의 별도 latency 단계와 `max_in_flight=1`** — C 러너 위반

정책: `PERF_SINGLE_TEST_POLICY.md:37`("같은 active 구간에서 동일 메시지 집합으로 latency도 함께
집계한다"), `:293`("throughput과 latency는 동일한 유효 메시지 집합을 사용한다"),
`:206-210`(phase 정의에 latency 단계 없음), `PERF_POLICY.md:1230-1236`(inflight 상한 금지),
`doc/perf/BINDINGS_OPTIMIZATION_GUIDE.ko.md:112-113`("인위적인 in-flight 상한이나 **2단계 측정**으로
backpressure를 우회하는 러너 변경"은 이미 기각 목록).

- **reqrep**: `bindings/c/perf/single/common/perf_single_reqrep.hpp:504-507`
  (`latency_phase_duration_seconds()=1`), `:537-548`(phase1 throughput → phase2 latency).
  throughput은 phase1의 `completed`, latency는 phase2 sample
  (`bindings/c/perf/single/src/perf_dealer_router_reqrep.cpp:155-157`).
- **one-way(더 심각)**: `bindings/c/perf/single/common/perf_single_one_way.hpp:228-236`
  (`latency_phase_duration_seconds()=1`, **`latency_phase_max_in_flight()=1`**), `:441-470`.
  실제 gate는 `:202-211`의 `sent - received < max_in_flight` 루프 — **인위적 in-flight 상한**이다.
- **제거 후 달라지는 것**

  | 항목 | 현재 | 제거 후 |
  |---|---|---|
  | one-way latency 의미 | in-flight 1의 무부하 편도 지연 | **포화 상태(HWM 깊이 포함) 편도 지연** — 값이 크게 오른다 |
  | reqrep latency 의미 | 포화 뒤 별도 1초 window | 같은 window의 왕복 지연 |
  | throughput | 변화 없음(phase1이 이미 포화) | 변화 없음 |
  | 케이스 wall time | duration + 1초 + drain | duration + drain (**케이스당 1초 단축**) |
  | 집합 일치 | throughput 집합 ≠ latency 집합 | **일치**(정책 준수) |
  | stop token 송신 | 케이스당 2회 | 1회 |

  포화 상태 latency는 정책과 모순되지 않는다. 정책은 latency를 "active 구간 수신 payload header
  timestamp 기반"(`PERF_SINGLE_TEST_POLICY.md:32`)으로만 정의하고 무부하 조건을 요구하지 않는다.
  `sent_ts_ns`는 송신 직전에 찍히므로(`perf_single_one_way.hpp:152-155`) 큐 대기가 포함되며,
  이는 `PERF_MULTI_TEST_POLICY.md:691`의 one-way 정의와 같다.
- **sample cap / reservoir / percentile은 영향 없음**(§3.3 C-6 참조).
- **multi에는 같은 분리가 없다.** `bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:640-653`
  의 `run_active_window()`는 `settings.duration_seconds` 하나로 `capture_latency = true`(`:557`)를
  써 같은 구간에서 집계한다. **multi가 이미 canonical이며 single만 고치면 두 suite가 일치한다.**

#### B-3 (b) Single 수신이 recv 모델(`POLLIN` readiness + `DONTWAIT` drain, wait `-1`)이 아니다

정책: `PERF_SINGLE_TEST_POLICY.md:47-49`, `:185-190`(표: receiver poller wait timeout **`-1`**).

| 언어 | 위반 내용 | 파일:줄 |
|---|---|---|
| Node | **poller 미사용.** 첫 recv를 blocking `RecvFlags.None`(RCVTIMEO 200ms), 이후 DontWait drain. 코드 주석이 "C reference bounds every individual recv by RCVTIMEO"라고 적었으나 현재 C는 `wait(-1)`이다 | `bindings/node/perf/single/perf_single_common.ts:326-347`, `:348-401` |
| Go | **poller 미사용.** `Recv(..., RecvFlagsNone)` + RCVTIMEO 200ms 폴링 루프 | `bindings/go/perf/single/perf_oneway.go:145`, `bindings/go/perf/internal/perfcommon/common.go:298`, `:411-418` |
| Go(routed) | 추가로 **DONTWAIT burst drain 자체가 없다** | `bindings/go/perf/single/perf_routed_oneway.go:100` |
| Rust | **poller 미사용.** deadline 전에는 blocking `recv(NONE)`, 이후 `DONT_WAIT` + 1ms busy-poll | `bindings/rust/perf/single/src/perf_pair.rs:74-103` 외 |
| Rust(PUBSUB) | DONTWAIT burst drain 없음(이벤트당 1건) | `bindings/rust/perf/single/src/perf_pubsub.rs:100-110` |
| Python | poller는 쓰지만 wait timeout이 `-1`이 아니라 **최대 50ms**, 그리고 stop token 없이도 2초 뒤 종료 | `bindings/python/perf/single/perf_common.py:474-479`, `:306` |
| C++ | ROUTER_ROUTER만 poller 없이 blocking `receive(message, 0)` | `bindings/cpp/perf/single/src/perf_router_router.cpp:206-208` |
| .NET | PUBSUB만 poller 미사용(blocking `TrySubscribe(..., RecvFlags.None)`) | `bindings/dotnet/perf/single/Zlink.BindingBench/src/PerfPubSub.cs:139`, `:175-176` |
| Java | 위반 없음 — `pollSet.poll(-1)` + DONT_WAIT drain | `.../PerfPair.java:65`, `:68-96` |

#### B-4 (b) Multi REQREP이 정책이 요구하는 비동기 모델·부하 수준이 아니다

정책: `PERF_MULTI_TEST_POLICY.md:161-168`(inflight 인위 고정 금지, admission backpressure까지 연속
제출), `:61-71`·`:85-86`(C 이외는 public async terminal, C 복제 금지),
`PERF_POLICY.md:250-259`.

| 언어 | 위반 내용 | 파일:줄 |
|---|---|---|
| Go | **socket당 goroutine이 blocking `Submit(ctx)` 1건씩** → in-flight 1 × N. `PERF_MULTI_TEST_POLICY.md:168`("응답을 받아야 다음 request를 보내는 1:1 ping-pong으로 직렬화하지 않는다") 정면 위반 | `bindings/go/perf/multi/perf_multi_socket_reqrep.go:178-237`, `:196` |
| Java | client당 in-flight **1로 고정**(`inFlight` 배열 gate) | `.../PerfMultiSocketReqRep.java:197-236`, `:207-208`, `:313` |
| .NET | 반대로 **gate가 전혀 없어** 매 반복 모든 슬롯에 무조건 제출 → outstanding 무제한 | `.../PerfMultiSocketReqRep.cs:327-359` |
| Node | in-flight 상한 없음, 이벤트루프 턴마다 소켓 수만큼 제출 → outstanding 무제한 | `bindings/node/perf/multi/perf_multi_socket_reqrep.ts:96-116` |
| Python | 매 턴 소켓 수만큼 `asyncio.create_task` → outstanding 무제한 | `bindings/python/perf/multi/perf_multi_reqrep_client.py:171-200` |
| Rust | 소켓당 턴당 1건 push, 이전 것이 pending이어도 계속 누적 → depth가 HWM이 아니라 **루프 회전 속도**로 결정 | `bindings/rust/perf/multi/src/perf_multi_socket_reqrep.rs:267-302` |
| C++ | 슬롯당 outstanding 1건 gate(`operation_active`) — C와 같은 형태이나 "backpressure까지"가 아니라 **1로 고정**이므로 `:161-168` 기준으로는 부분 위반 | `bindings/cpp/perf/multi/common/perf_multi_reqrep.hpp:310-311`, `:452` |

- 여기서도 "무제한"과 "1 고정" 둘 다 위반이다. 정책이 요구하는 것은
  **"admission backpressure 경계까지"** 이다. 그 경계의 공개 표현은 §4-P1에서 다룬다.

#### B-5 (b) Single 실행 모델(전용 OS thread + synchronous API) — 전수 판정

정책: `PERF_SINGLE_TEST_POLICY.md:50-60`(공통), `:61-65`(언어별 명시), `PERF_POLICY.md:118-126`.

| 언어 | 판정 | 근거 |
|---|---|---|
| C++ | **준수** | single 전체에 `co_await`/`async_result_t`/`.async(` 0건. 전부 `std::thread` — `bindings/cpp/perf/single/src/perf_pair.cpp:129`, `:179`; `bindings/cpp/perf/single/common/perf_single_reqrep.hpp:258`, `:301`, `:331` |
| .NET | **위반** | single REQREP 측정 루프가 `Task`+`await` — `bindings/dotnet/perf/single/Zlink.BindingBench/src/PerfReqRep.cs:221`, `:224`, `:485`(`Task<...> request = submit(message);`), `:509-515`(`async Task CompleteRequest` / `await request`). `PERF_SINGLE_TEST_POLICY.md:64-65` 정면 위반. one-way 6종은 전용 `Thread` + blocking sync로 준수 |
| Java | **준수** | send·request 전부 `submit_sync()`, `CompletionStage`/`CompletableFuture` 0건. 수신·송신 모두 전용 `Thread` — `.../PerfPair.java:61`, `:109`; `.../PerfSocketReqRep.java:102`, `:151-170` |
| Node | **형식 위반** | sender는 worker 준수(`bindings/node/perf/single/perf_single_common.ts:575` `new Worker(...)`, 본체 `perf_single_sender_worker.ts:142,146,150,153` 전부 `.submit_sync()`). 그러나 **recv 루프가 main thread**에서 돈다(`perf_single_common.ts:349` `drainRecvSocket`, 호출 `perf_pair.ts:83`). 루프 내부에 `await`/Promise/`setImmediate`는 없어 "coroutine/yield 금지"는 준수하지만, `PERF_SINGLE_TEST_POLICY.md:62-64`의 "Node는 `worker_threads`에서 synchronous terminal**과 recv loop**를 실행한다"는 문언은 미충족 |
| Go | **준수** | `runtime.LockOSThread()`가 main과 모든 역할 goroutine에 적용 — `bindings/go/perf/single/perf_main.go:27`, `perf_oneway.go:52-54`, `perf_routed_oneway.go:41-43`, `perf_pubsub.go:53-55`, `perf_reqrep.go:27-29`. single의 `go func`은 이 4곳뿐 |
| Rust | **위반** | one-way 4패턴의 active send가 **직접 구현한 Future executor**를 쓴다 — executor 본체 `bindings/rust/perf/single/src/common.rs:185-200`(`future.as_mut().poll(&mut context)` + `Poll::Pending => wait_for_send_progress()`), waker `:138-151`, 진입 `:121-133` `perf_submit_measurement!`, callsite `src/perf_pair.rs:60`, `src/perf_dealer_dealer.rs:62`, `src/perf_dealer_router.rs:93`, `src/perf_router_router.rs:111`. `PERF_SINGLE_TEST_POLICY.md:64-65` 위반. REQREP·PUBSUB는 `submit_sync()`/`submit()`으로 준수 |
| Python | **준수** | `threading.Thread`만 사용(`perf_pair.py:99`, `perf_dealer_dealer.py:93`, `perf_pubsub.py:107`, `perf_dealer_router.py:101`, `perf_router_router.py:117`, `perf_single_reqrep.py:211`). single 디렉터리 전체에 `asyncio`/`async def`/`await` 0건 |

추가로, **C++·.NET single REQREP은 러너가 별도 progress thread로 poller를 50ms 간격으로 돌리는
한편 실제 completion 소비는 binding 내부 runtime pump가 한다**
(`bindings/cpp/perf/single/common/perf_single_reqrep.hpp:293-329` +
`bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:679-709`;
`bindings/dotnet/perf/single/Zlink.BindingBench/src/PerfReqRep.cs:433-459` +
`bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:181`, `:203`).
→ `PERF_SINGLE_TEST_POLICY.md:117-121`("callback 결과는 전용 requester/progress thread가 poller를
구동할 때 집계")와 어긋나고, `PERF_POLICY.md:106-115`(anchor)의 "latency sample 채취 위치"가
C와 다른 thread가 된다. **K11 위반.**

#### B-6 (b) Multi one-way sender가 C의 sync `DONTWAIT`+`POLLOUT` 복제인지 여부

정책: `PERF_MULTI_TEST_POLICY.md:85-86`, `PERF_POLICY.md:222-223`, `:254-255`
("sync `DONTWAIT`와 binding-local `POLLOUT` pending table로 C reference를 복제하지 않는다").

- Go multi DEALER_DEALER client는 socket당 goroutine이 blocking `Send`를 반복한다
  (`bindings/go/perf/multi/perf_multi_dealer_dealer.go:198-240`) — Go에는 별도 async send terminal이
  없으므로 `PERF_POLICY.md:250-254`가 명시적으로 허용한 형태다. **위반 아님.**
- C++/.NET/Node/Rust/Python은 async terminal + socket당 admission 1건 gate로 구현되어 있으며
  `POLLOUT` pending table을 만들지 않는다. **위반 아님.**
- Java도 `CompletionStage` 기반 async send loop다
  (`bindings/java/perf/multi/Zlink.BindingBench.Multi/src/main/java/systems/zlink/perf/multi/PerfMultiAsyncSendLoop.java:23-25`, `:85-88`). **위반 아님.**
- **Python만 위반**: SENDSEND client 2종이 HWM-managed send를 **동기 blocking terminal로 직렬화**한다 —
  `bindings/python/perf/multi/perf_multi_dealer_router_client.py:77-84`,
  `bindings/python/perf/multi/perf_multi_router_router_client.py:78-84`가
  `send_routed(..., _sync=True, ...)`를 호출하고
  `bindings/python/perf/multi/perf_multi_common.py:382-383`이 `op.submit_sync()`로 내린다.
  `PERF_MULTI_TEST_POLICY.md:92-93`("이 예외는 HWM-managed send/request를 sync `DONTWAIT`로
  구현해도 된다는 뜻이 아니다") 위반이며 socket당 in-flight가 1로 고정된다.
- **PUB/XPUB publish와 raw reply는 7개 binding 전부 synchronous terminal**로 확인했다(M6 = O).
  publish: `bindings/cpp/perf/multi/src/perf_pubsub_server.cpp:95`,
  `bindings/dotnet/perf/multi/.../PerfMultiPubSubServer.cs:63-66`,
  `bindings/java/perf/multi/.../PerfMultiPubSub.java:186-189`,
  `bindings/node/perf/multi/perf_multi_runtime.ts:277-288`,
  `bindings/go/perf/multi/perf_multi_pubsub.go:239`,
  `bindings/rust/perf/multi/src/perf_multi_pubsub_server.rs:69-74`,
  `bindings/python/perf/multi/perf_multi_common.py:398-406`.
  raw reply: `bindings/cpp/perf/multi/common/perf_multi_reqrep.hpp:527`,
  `.../PerfMultiSocketReqRep.cs:412`, `.../PerfMultiSocketReqRep.java:85`,
  `bindings/node/perf/multi/perf_multi_runtime.ts:41-43`,
  `bindings/go/perf/multi/perf_multi_socket_reqrep.go:90`,
  `bindings/rust/perf/multi/src/perf_multi_socket_reqrep.rs:171`,
  `bindings/python/perf/multi/perf_multi_reqrep_server.py:19-30`.

### 3.3 (c) 집계·anchor

#### C-1 (c) `run_id`가 case ordinal이 아니라 상수 `1` — Java·Node·Go·Rust

정책: `PERF_POLICY.md:349-358`
> "`run_id`는 **1-based benchmark case ordinal** 이다. ... 프로세스가 여러 case를 순차 수행하는
> 경우 `run_id`는 case마다 `1,2,3,...` 순서로 증가해야 한다."

| 언어 | 파일:줄 | 비고 |
|---|---|---|
| Java | `bindings/java/perf/common/.../PerfMeasurement.java:17` `RUN_ID = 1` | 여러 size 순차 수행 경로에서 case 분리 불가 |
| Node | `bindings/node/perf/common/perf_measurement.ts` `createRunId(options.runId ?? 1)` | 실질 상수 1 |
| Go | `bindings/go/perf/internal/perfcommon/measurement.go:18` `MetricRunID uint32 = 1` | 상수 |
| Rust | `bindings/rust/perf/single/src/common.rs:40` `BENCHMARK_RUN_ID: u32 = 1` | 상수 |
| C++ / .NET | `bindings/cpp/perf/single/src/perf_pair.cpp:120`, `.../PerfPair.cs:10` 하드코딩 `1` | 프로세스가 case 1개만 수행 → `PERF_POLICY.md:355` 충족 |
| C | `bindings/c/perf/single/common/bench_common_runtime.hpp:205-212` `next_single_metric_run_id()` | 증가값 |

효과: inproc/ipc처럼 endpoint를 재사용하는 transport에서 **이전 실행 잔여 메시지를 걸러내지 못한다**
(`PERF_POLICY.md:359-360`).

#### C-2 (c) .NET single latency sampler가 reservoir가 아니라 전수 축적 — **중대**

정책: `PERF_POLICY.md:95-97`(sample 상한은 계측 메모리 보호),
`PERF_SINGLE_TEST_POLICY.md:519`(`0`이면 sample 미보관), 계획서 §7.0.1 `:457-458`, `:466-468`.

- `bindings/dotnet/perf/common/Zlink.BindingBench.Common/PerfShared.cs:75-85`
  ```
  public static void ReservoirSample(List<double> samples, double value,
      ref long seenCount, int cap, ref uint rngState)
  { _ = cap; _ = rngState; samples.Add(value); seenCount++; }
  ```
  **cap과 rng를 버리고 모든 표본을 무한 축적한다.**
- 결과: (1) `cap=0`이 무력화되어 "표본 없음 → p95=p99=mean"이 재현되지 않음(S16 위반),
  (2) 고처리량에서 `List<double>`이 수천만 원소까지 증가해 GC·메모리 압력이 측정에 개입(K14 위반),
  (3) p95/p99가 서브샘플 추정치가 아니라 전수 정확값이라 다른 러너와 원리적으로 비교 불가.
- .NET **multi**는 정상 reservoir다
  (`bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/common/PerfCommonMulti.cs:510-528`).
- **단순 버그.** 나머지 6개 binding의 reservoir는 C와 알고리즘·LCG 상수(`1664525`/`1013904223`)·
  seed(`0xA341316C`)·percentile 보간식(`pos=(n-1)q; lo=floor(pos); hi=min(lo+1,n-1);
  s[lo]+(s[hi]-s[lo])(pos-lo)`)까지 동일함을 확인했다.

#### C-3 (c) Java 표본 0개일 때 percentile fallback이 다르다

- Java `bindings/java/perf/common/.../PerfMetricsCollector.java:180-182` — 빈 배열이면 **`0.0`** 반환.
- C `bindings/c/perf/single/common/perf_single_latency.hpp:59-63` — **p95=p99=mean**.
- `PERF_SINGLE_TEST_POLICY.md:519`는 "`0`이면 sample을 보관하지 않는다"만 규정하고 그때의
  p95/p99 값을 정하지 않는다 **[정책 미규정 → C 구현 준용]**. §4-P4로 정책에 명시한다.

#### C-4 (c) one-way 수신 집계의 active deadline 필터가 러너마다 다르다

- **C에는 필터가 없다** — stop token 도착 전까지 in-flight tail을 전부 집계
  (`bindings/c/perf/single/common/perf_single_one_way.hpp:338-348`).
- 필터가 있는 러너: Java 전 패턴(`.../PerfPair.java:89-92` 등), Node(`perf_single_common.ts:394-400`),
  Python(`perf_common.py:349-350`), Rust 전 one-way(`src/common.rs:644-655`),
  Go의 PAIR/DEALER_DEALER/PUBSUB(`internal/perfcommon/runtime.go:38-51`).
- 필터가 없는 러너: Go의 routed one-way(`perf_routed_oneway.go:124-132`),
  C++의 DEALER_ROUTER·ROUTER_ROUTER, .NET의 DEALER_ROUTER·ROUTER_ROUTER.
- 정책은 `PERF_SINGLE_TEST_POLICY.md:284-287`에서
  > "single one-way active 유효 메시지 규칙은 '`phase == active` 이고, **recv 루프가 active window
  > 안에서 처리한** 유효 header 메시지'로 고정한다. idle drain 구간에서 추가로 recv한 메시지는
  > 종료 정리용으로만 소비"

  라고 규정한다. **문언대로면 필터가 있어야 하고, C 러너가 위반이다.** 다만 C는 stop token이
  active deadline 직후 도착하므로 실질 차이가 작다. 그럼에도 **C와 C++·.NET의 routed 패턴이 정책
  위반**이며, 같은 언어 안에서도 패턴별로 규칙이 달라 K11(anchor 동일)을 깬다.
  → §4-P3에서 정책 문언을 더 정확히 하고, 전 러너를 그 문언에 맞춘다.

#### C-5 (c) payload wire 길이 검증 강도가 다르다

- C: 길이 불일치는 **fatal** (`perf_single_one_way.hpp:122`, `:129-136`).
- C++/Node/Python/Go/Rust: **skip**. Java·.NET: **검증 자체가 없다**
  (`bindings/java/perf/common/.../PerfMetricHeader.java:89-93`은 header의 `msg_size` 필드만 비교;
  `bindings/dotnet/perf/single/.../PerfPair.cs:159-166`).
- 정책은 `PERF_SINGLE_TEST_POLICY.md:280-283`에서 "decode 실패 / `magic`·`phase`·`msg_size` 검증
  실패 메시지는 집계 제외"라고만 하고 **wire 길이 검증을 요구하지 않는다**
  **[정책 미규정 → C 구현 준용]**. 다만 "집계 제외"라고 했으므로 **fatal은 정책 문언보다 강하다**.
  → §4-P3에서 "집계 제외"로 통일하고 C를 그에 맞춘다.

#### C-6 (c) latency sample cap·reservoir·percentile은 나머지 전부 일치

확인 결과(§C-2·§C-3 제외):

| 항목 | 값 | 확인된 러너 |
|---|---|---|
| single cap 기본 | 1,000,000 | C, C++, Java, Node, Go, Rust, Python |
| multi cap 기본 | 65,536 | 전부 |
| `cap=0` | 표본 미보관, count·sum 유지, p95=p99=mean | C, C++, Node, Go, Rust, Python |
| reservoir | Algorithm R, LCG `x*1664525+1013904223`, seed `0xA341316C`, `slot = rng % seen` | 전부 |
| percentile | `pos=(n-1)q`, 선형 보간 | 전부 |
| mean | 전체 관측 `sum/count` | C, C++, .NET(multi), Java, Node, Go, Rust, Python |

C 내부 부수 결함(단순 버그): `bindings/c/perf/single/common/perf_single_latency.hpp:100-111`의
`default_sample_cap()`은 음수 문자열을 거르지 않아 `PERF_SINGLE_LATENCY_SAMPLE_CAP=-1`이
`strtoull` wrap으로 거대한 cap이 된다. 같은 목적의 `perf_single_reqrep.hpp:96-106`은 `'-'`와
`errno`를 검사한다. 하나로 통일한다.

C 내부 또 하나: matched-client 경로의 가중 percentile
(`bindings/c/perf/multi/common/perf_multi_weighted_latency.hpp:64-86`)은 **보간 없는 가중 분위수**라
단일 프로세스 경로의 선형 보간과 식이 다르다. 정책 `PERF_MULTI_TEST_POLICY.md:685-686`은 "p95는
샘플의 95th percentile"이라고만 해 보간법을 정하지 않는다 **[정책 미규정]**. → §4-P4.

#### C-7 (c) transient 재시도의 sleep·재스탬프 규칙이 다르다

정책: `PERF_POLICY.md:366-379`(hot loop에서 sleep/retry budget 금지, flow-control 예외만 허용),
계획서 §7.0.1 `:453-454`("transient 오류 후 새 timestamp·1ms retry").

- C: 전 one-way 패턴에서 **1ms sleep + 새 timestamp 재기록**
  (`perf_single_one_way.hpp:213-219` + `:152-155`).
- Java: 동일(`.../PerfPair.java:119-121` → `PerfUtil.java:320-328` `Thread.sleep(1)`).
- C++: ROUTER_ROUTER만 sleep 없이 busy-spin (`bindings/cpp/perf/single/src/perf_router_router.cpp:104-110`).
- .NET: DEALER_DEALER·DEALER_ROUTER·PUBSUB·ROUTER_ROUTER 4패턴이 busy-spin
  (`PerfDealerDealer.cs:183-184`, `PerfDealerRouter.cs:198-206`, `PerfPubSub.cs:117-120`, `PerfRouterRouter.cs:290-303`).
- Node: 전 패턴 busy-retry(sleep 없음) (`perf_single_sender_worker.ts:203-218`).
- Rust: sleep 없이 즉시 재시도 (`bindings/rust/perf/single/src/common.rs:691-710`).
- Go: `Submit`이 blocking이라 재시도 경로 자체가 실질 dead path.
- Python: 재시도 없음(예외 전파).
- **정책이 "1ms"를 직접 규정하지 않는다** **[정책 미규정 → C 구현 준용]**. 다만 busy-spin은
  `PERF_POLICY.md:366-370`("hot loop 안에서 sleep/yield 금지")와도, C의 1ms sleep과도 어긋난다.
  → §4-P5에서 정책에 명시한다.

#### C-8 (c) 시각원(clock source)이 다르다

- C: `steady_clock` (`bindings/c/perf/single/common/perf_single_metric_header.hpp:36-43`).
- Go: `time.Now().UnixNano()` (**wall clock**, `internal/perfcommon/measurement.go:89`, `:171`).
- Rust: `SystemTime::now()` (**wall clock**, `bindings/rust/perf/single/src/common.rs:204-209`).
- .NET: 프로세스 시작 시 `DateTime.UtcNow` + `Stopwatch` delta (`PerfShared.cs:44-50`) — 동일
  프로세스 안에서는 monotonic.
- 이 호스트는 WSL2에서 wall clock이 점프한 이력이 있다(D-095). **정책에 규정이 없다**
  **[정책 미규정]** → §4-P5에서 "경과 시간과 deadline은 monotonic clock으로 측정한다"를 명시한다.

#### C-9 (c) 출력 정밀도가 다르다

- C: throughput `setprecision(3)`, latency `setprecision(6)`
  (`bindings/c/perf/single/common/perf_single_monitor.hpp:173`, `:178`).
- C++: `(2)`/`(3)` (`bindings/cpp/perf/single/common/perf_single_report.hpp:33`, `:38`).
- Java: 5 metric 전부 `%.3f` (`.../PerfReport.java:41`).
- Node: `toFixed(3)` (`perf_measurement.ts:204`).
- .NET single: 고정 소수점이 아니라 `ToString(InvariantCulture)`
  (`bindings/dotnet/perf/single/.../PerfCommon.cs:133-134`).
- inproc처럼 latency가 µs 이하인 셀에서 ms 소수 3자리는 **유효숫자가 사라진다**.
  `PERF_POLICY.md:138`("사람이 읽는 테이블 형식 ... 의미를 바꾸지 않는다") 위반.
  정책이 자릿수를 규정하지 않으므로 **[정책 미규정 → C 구현 준용]** → §4-P5.

### 3.4 (d) handshake·출력 형식

#### D-1 (d) **Multi 종료 protocol 미준수 — C·Go를 제외한 6개 binding**

정책: `PERF_MULTI_TEST_POLICY.md:379`, `:381`, `:386-388`; `PERF_POLICY.md:448-451`
> "`MULTI_DEALER_ROUTER_REQREP`와 `MULTI_ROUTER_ROUTER_REQREP` client는 `RESULT`와 `CLIENT_DONE`을
> 출력한 뒤 request completion 대상 socket을 유지한다. runner는 server에 `STOP`을 보내 종료를
> 확인한 뒤 client에 `STOP`을 보낸다. client는 이 `STOP`을 받은 뒤 socket을 닫고 종료한다."

| 언어 | 상태 | 근거 |
|---|---|---|
| C | 준수 | `bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:792`(CLIENT_DONE) → `:722-731` `wait_for_runner_stop_after_done()` → `:798` close; runner `bindings/c/perf/run_comparison.py:2652-2665` |
| Go | 준수 | `bindings/go/perf/multi/perf_multi_socket_reqrep.go:172`, `:175`; runner `bindings/go/perf/run_benchmarks_multi.sh:1289`, `:1348`, `:1356` |
| C++ | **위반** | `CLIENT_DONE` 미출력, STOP 미대기, 즉시 반환 — `bindings/cpp/perf/multi/common/perf_multi_reqrep.hpp:250-259` |
| .NET | **위반** | `CLIENT_DONE` 없음, `finally`에서 즉시 Dispose — `.../PerfMultiSocketReqRep.cs:196-206` |
| Java | **위반** | REQREP는 `CLIENT_DONE`을 아예 내지 않고(`.../PerfMain.java:44-46`은 PUBSUB만 true) wire stop token으로 대체 — `.../PerfMultiSocketReqRep.java:238-251`, close `:146-152` |
| Node | **위반** | `CLIENT_DONE` 출력 후 **STOP 대기 없이** 즉시 close — `bindings/node/perf/multi/perf_multi_socket_reqrep.ts:124-129` |
| Rust | **위반** | `CLIENT_DONE` 자체가 없다 — `bindings/rust/perf/multi/src/perf_multi_socket_reqrep.rs:331-337`; runner는 client exit 후 server STOP (`run_benchmarks_multi.sh:990-1002`, `:1023`) |
| Python | **위반** | `CLIENT_DONE` 없음, STOP 대기 없음 — `bindings/python/perf/multi/perf_multi_reqrep_client.py:249-255`; runner `multi/run_benchmarks.py:1149`, `:835-849` |

효과: server가 마지막 reply를 쓰는 중에 client가 socket을 닫아 **queued reply가 completion 대상을
잃는다**. 이는 측정 안정성 문제이자 `PERF_POLICY.md:139-143`(handshake 계약) 위반이다.

#### D-2 (d) Multi `CLIENT_DONE`/`START` barrier의 패턴별 적용 범위가 다르다

- C는 `MULTI_DEALER_DEALER`(`bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp:593`),
  `MULTI_PUBSUB`(`perf_multi_pubsub_client.cpp:367`), REQREP 2종
  (`perf_multi_socket_reqrep.hpp:792`), matched RR(`perf_multi_router_router_matched_client.cpp:422`)
  에서 `CLIENT_DONE`을 낸다. SENDSEND 2종은 내지 않는다.
- Java는 PUBSUB만, .NET은 DEALER_DEALER·PUBSUB만, Rust는 DEALER_DEALER만 낸다.
- `PERF_POLICY.md:434-436`("C 기준에서 runner `START`를 쓰지 않는 패턴에 언어별 runner가
  `CLIENT_READY`/`START` barrier를 새로 추가하면 안 된다")와 `:457-461`(default 목록에서 빼거나
  SKIP으로 바꾸는 것은 회귀 은폐)에 따라 **패턴별 token 목록을 C 기준으로 통일**해야 한다.

#### D-3 (d) Node single REQREP이 `inproc`을 `UNSUPPORTED`로 처리한다

- `bindings/node/perf/single/perf_socket_reqrep.ts:63-68` — Node Worker가 Context를 공유하지 못해
  `{unsupported:true}`. one-way 패턴도 `inproc` 전체가 unsupported
  (`perf_single_common.ts:540-545`).
- C는 `inproc`을 측정한다(`PERF_SINGLE_TEST_POLICY.md:482`가 `inproc`을 지원 transport로 명시).
- `PERF_POLICY.md:462-464` — "`UNSUPPORTED`는 정책상 지원하지 않는 조합이나 아직 공식 matrix에
  올리지 않은 새 조합에만 쓴다. C 기준과 불일치하는 기존 구현을 숨기는 용도로 쓰면 안 된다."
  **→ 위반.** binding public API 부족이면 `PERF_POLICY.md:453-455`에 따라 **binding public API를
  보강**하거나 정책에서 Node `inproc`을 명시적으로 제외해야 한다(§4-P6).

#### D-4 (d) Rust single one-way가 `received == 0`에서도 `RESULT`를 낸다

- `bindings/rust/perf/single/src/common.rs:597-608` `print_result`는 유효성 검사 없이 출력.
  C는 `received == 0`이면 실패 처리(`perf_single_one_way.hpp:431-432`),
  Go도 FAIL 후 exit 1(`internal/perfcommon/common.go:151`).
- `PERF_SINGLE_TEST_POLICY.md:305-313`의 완료 판정이 `0.000` RESULT를 성공으로 세게 된다.
  **단순 버그.**

#### D-5 (d) Effective Options / CLI 이름 불일치

§3.1 A-2, A-4, A-6-4~A-6-7로 이미 다뤘다. `PERF_POLICY.md:134-138` 위반이며 계획서
§12 `:1396`("옵션 일치 근거")을 자동으로 만족시킬 수 없게 만든다.

---

## 4. 정책 자체의 결함과 개정 문안

여기에는 **정책을 고쳐야만 해결되는 항목만** 넣는다. 정책이 이미 명확하고 러너가 어긴 것은
§3에서 러너를 고치는 것으로 끝난다.

### P1 (구현 불가능) Single REQREP — "synchronous callback terminal"이 공개 계약에 없다

#### P1.1 모순의 구조

정책은 single request/reply에 대해 세 가지를 동시에 요구한다.

1. `PERF_SINGLE_TEST_POLICY.md:55-58`
   > "request/reply는 **synchronous callback terminal**과 completion poller를 사용한다.
   > callback terminal이 request ownership 이전의 backpressure를 즉시 알리면 같은 requester
   > thread가 completion progress를 구동한 뒤 같은 logical request를 다시 제출할 수 있다."

   (같은 취지: `PERF_POLICY.md:119-126`, `:195-197`, `:271-273`;
   `PERF_SINGLE_TEST_POLICY.md:117-121`)
2. `PERF_SINGLE_TEST_POLICY.md:59-60`
   > "측정 구간에는 coroutine, async task, Promise/Future executor, event-loop yield를 사용하지 않는다."
3. `PERF_SINGLE_TEST_POLICY.md:71-73`
   > "requester는 응답을 하나 받을 때마다 다음 request를 보내는 RTT 전용 루프가 아니라, **public
   > request API가 허용하는 만큼 request를 연속 제출하고 reply completion을 계속 drain한다.**"

그런데 7개 binding의 공개 request terminal은 **두 종류뿐**이다
(`bindings/doc/spec/async-coroutine-policy.ko.md:87-95`):

| Binding | Request terminal |
|---|---|
| C++ | `vector<message_t> submit() &&`, `async_result_t<vector<message_t>> async() &&` |
| .NET | `IReadOnlyList<Message> Submit()`, `Task<IReadOnlyList<Message>> Async(CancellationToken)` |
| Java/Kotlin | `CompletionStage<List<Message>> submit()`, `List<Message> submit_sync()` |
| Node | `Promise<Message[]> submit()`, `Message[] submit_sync()` |
| Python | `Awaitable[list[Message]] submit()`, `list[Message] submit_sync()` |
| Go | `Submit(context.Context) ([]*Message, error)` |
| Rust | `Future<Output=Result<Vec<Message>,ZlinkError>> submit()`, `Result<Vec<Message>,ZlinkError> submit_sync()` |

그리고 같은 문서 `:116`이 못박는다.
> "Send와 request terminal은 §6의 signature만 제공하며 send/request flags, send timeout과
> **request callback terminal을 제공하지 않는다**."

즉 **정책 (1)이 전제한 "synchronous callback request terminal"은 어느 binding에도 없다.**
남은 선택지는 동기형(reply까지 블로킹 → 정책 (3) 위반)과 비동기형(정책 (2) 위반)뿐이며,
**(1)(2)(3)을 동시에 만족하는 구현은 현재 공개 계약에서 존재하지 않는다.** 이것이 D-B127 B1과
R3 BLOCKER 4의 실체이며, 7개 binding 러너가 전부 in-flight 1로 수렴한 근본 원인이다.
새 공개 API 추가는 B1과 계획서 §5 `:366-371`이 금지한다.

#### P1.2 `ZLINK_POLLOUT` level은 admission 경계를 정확히 표현하지 못한다

사용자 결정 B1은 "포화 제출 경계 = 공개 poller의 raw socket `ZLINK_POLLOUT` level"을 제안했다.
**사양은 이를 정면으로 부정한다.**

- `core/doc/spec/core/05-polling.ko.md:54-59`
  > "여러 peer를 가진 raw socket의 `ZLINK_POLLOUT`은 socket 전체의 집계 readiness다. ... 따라서
  > 특정 target의 nonblocking submit이 backpressure를 반환한 뒤 `ZLINK_POLLOUT`을 관측해도
  > **그 target의 다음 submit 성공은 보장되지 않는다.**"
- `core/doc/spec/core/05-polling.ko.md:60-61`
  > "target별 재시도 신호는 `ZLINK_POLLOUT` bit가 아니라 wait token의 `ZLINK_COMPLETION_WRITABLE`
  > record다."
- `core/doc/spec/core/socket/README.ko.md:1000-1001` — 같은 취지.

세 하위 질문에 대한 답:

**(a) POLLOUT이 켜져 있는데 submit이 `ZLINK_SUBMIT_BACKPRESSURED`를 돌려주는 경우가 가능한가 —
사양상 가능하다.** 원인 셋:

1. **aggregate 원인**: 다른 peer의 credit으로 POLLOUT이 설 수 있다(`05-polling.ko.md:54-56`).
   이 perf suite의 requester socket은 target이 하나뿐이므로(단일 server endpoint;
   `bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:668-684`) 이 원인은 **실질적으로
   소멸한다**. DEALER의 target 단위는 candidate peer 집합, ROUTER request의 target은 지정 RID
   하나이므로(`socket/README.ko.md:1003`) aggregate == per-target으로 축약된다.
2. **읽지 않은 WRITABLE record가 POLLOUT을 level로 붙잡는 원인**: `socket/README.ko.md:996-997`
   > "이 record를 아직 꺼내지 않은 동안 socket의 `ZLINK_POLLOUT`과 `ZLINK_POLLCOMPLETION`은 모두
   > level로 true다."

   즉 POLLOUT이 켜진 이유가 "지금 쓸 수 있다"가 아니라 "예전 토큰의 회복 record가 큐에 남아 있다"
   일 수 있다. 재제출은 admission을 **한 번만** 시도하며 다시 거절되면 새 토큰을 만든다(`:998-999`).
3. **REQUEST 전용 원인 — correlation 예산**: `socket/README.ko.md:1082-1084`
   > "거절 원인이 pair의 correlation work·count 한도 부족이면 그 토큰은 **해당 pair의 correlation
   > reservation이 반환될 때(terminal reply·timeout·disconnect)만** WRITABLE을 발행하고,
   > physical write credit만 회복된 상태에서는 발행하지 않는다."

   POLLOUT은 physical writability에서 유도되는 aggregate hint이므로 **correlation 예산 고갈을
   구조적으로 표현할 수 없다.** multi REQREP은 socket 100개가 동시에 outstanding request를 쌓는
   조건이므로 이 원인이 실제로 발생할 수 있다. **이것이 "POLLOUT은 REQUEST admission 경계가
   아니다"의 결정적 근거다.**

   러너 동작: 토큰을 볼 수 있는 경로(C)에서는 같은 logical request를 보관한 채 그 토큰의
   WRITABLE을 기다렸다 재제출한다. 토큰이 보이지 않는 경로에서는 binding이 내부에서 같은 규칙을
   수행하므로(`bindings/java/src/main/java/systems/zlink/contracts/messaging/SendSubmitOperation.java:22-27`,
   `bindings/dotnet/src/Zlink/Contracts/Messaging/OperationContracts.cs:56-62`) 러너는 재제출을
   흉내 내지 않고 "그 operation이 아직 안 끝났다"만 관찰한다.

**(b) POLLOUT이 꺼졌는데 제출 여지가 남아 있는 경우가 가능한가 — 사양은 이를 정상 상태로 허용하지
않는다(단, 명시 문장은 없다 — 부분 미확인).** `05-polling.ko.md:47-49`는 raw socket POLLOUT을
"submit 재시도 가치가 있음"으로 정의하고, `:74-83`은 level-trigger와 lost-wake 금지를 명시한다
("Readiness가 참인데 caller가 timeout까지 잠드는 것(lost wake)은 계약 위반"). 따라서 "제출 여지가
있는데 POLLOUT이 false"는 계약 위반 상태이며, 관측되면 perf 우회가 아니라 Core 버그로 보고해야
한다(`PERF_POLICY.md:98-101`). **"POLLOUT false ⇒ submit 실패"를 단언하는 사양 문장은 확인하지
못했다.**

**(c) multi에서 aggregate hint가 per-peer admission과 어긋나는가 — client 쪽에서는 축약되지만
원인 2·3은 남는다.** server ROUTER socket은 peer가 N개라 aggregate가 per-peer와 어긋나지만,
server의 reply는 공개 계약상 synchronous terminal이라(`async-coroutine-policy.ko.md:79-80`, `:118`;
`PERF_MULTI_TEST_POLICY.md:87-93`) POLLOUT을 gate로 쓰지 않는다.

**(d) 더 정확한 공개 표현.** 세 가지가 있고, 경로마다 다르다.

| 경로 | 경계(정지 조건) | 재개 조건 | 정확도 |
|---|---|---|---|
| **T1** C raw send·request | `ZLINK_SUBMIT_BACKPRESSURED` + nonzero 대기 토큰 | 그 토큰의 `ZLINK_COMPLETION_WRITABLE`(`ZLINK_POLLCOMPLETION`으로 관측) | **정확** |
| **T2** binding async **send** | 해당 socket의 미완료 send terminal이 1개 있음 | 그 terminal의 완료 | **정확(동등)** |
| **T3** binding async **request** | raw socket `ZLINK_POLLOUT` level = false | `POLLOUT` level = true | **근사** |

- **T1은 C 러너가 이미 하고 있는 것이다.** C 러너는 POLLOUT을 경계로 쓰지 않는다 —
  single reqrep `bindings/c/perf/single/common/perf_single_reqrep.hpp:282-289`(토큰 보관),
  `:325-350`(WRITABLE 매칭 → `retry_ready`), `:527`(poller에 `ZLINK_POLLCOMPLETION` **단독** 등록),
  `:419-449`(포화 제출 루프); multi reqrep `perf_multi_socket_reqrep.hpp:320-334`, `:371-399`,
  `:697-703`, `:576-604`. multi one-way sender는 POLLOUT을 명시적으로 **강등**한다 —
  `bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp:47-50`
  > "DEALER POLLOUT is an aggregate hint. After one NO_DATA pull it is dropped from the interest set
  > so a writable peer cannot spin the loop; the token's WRITABLE record still wakes POLLCOMPLETION."

  구현은 `:279-282`(NO_DATA 뒤 `pollout_suppressed = true`), `:97-103`.
  `doc/perf/BINDINGS_OPTIMIZATION_GUIDE.ko.md:62`도 같은 규칙을 기록한다.
  **즉 POLLOUT을 경계로 쓰는 러너는 현재 C보다 부정확해진다.**
- **T2가 정확한 이유**: `bindings/doc/spec/async-execution-model.ko.md:21-22`
  > "고수준 send·request의 awaitable terminal은 native `DONTWAIT` 제출과 completion drain을 사용한다."

  같은 문서 `:147`
  > "즉시 admission된 send의 terminal은 한 번 성공하며 완료 record를 기다리느라 멈추지 않는다."

  `PERF_MULTI_TEST_POLICY.md:68-69`
  > "send coroutine은 admission 완료를 await한 뒤 다음 send를 제출할 수 있지만 echo 수신을
  > 기다리면 안 된다."

  따라서 **"socket당 미완료 send terminal 1개를 유지하고 완료 즉시 다음을 제출"** 은 C의
  **"socket당 retained message 1개를 유지하고 WRITABLE에서 재제출"**
  (`perf_multi_dealer_dealer_client.cpp:424-450`)과 정지·재개 조건이 같다. 새 API가 필요 없다.
- **T3이 근사에 그치는 이유**: request terminal이 admission과 reply를 하나의 awaitable로 합쳐
  (`async-coroutine-policy.ko.md:87-95`) "admission 되었으니 더 보내라"와 "reply를 기다리는 중"을
  러너가 구분할 수 없다. 남은 공개 수단은 raw socket `ZLINK_POLLOUT` 하나이며, 7개 binding 모두
  public poller에서 이 bit를 노출한다(`bindings/cpp/include/zlink_enum.h`,
  `bindings/dotnet/src/Zlink/Runtime/Eventing/Poller.cs`,
  `bindings/java/src/main/java/systems/zlink/contracts/eventing/EventEnums/PollEventFlags.java`,
  `bindings/node/src/zlink/contracts/eventing/poller.ts`, `bindings/go/include/zlink_enum.h`,
  `bindings/rust/src/contracts/eventing/poller.rs`,
  `bindings/python/src/zlink/contracts/eventing/codes.py`).
  최대 overshoot은 **두 POLLOUT 관측 사이에 제출한 건수**이고, correlation 예산 거절은
  표현하지 못한다.

#### P1.3 개정안 — 세 선택지

**모두 정책 개정이 필요하다. 감독자 결정 항목(§6-D1).**

| 안 | 내용 | 장점 | 단점 |
|---|---|---|---|
| **A** | Single REQREP에 한해 **binding async request terminal 사용을 허용**하고, 부하 경계를 T3(POLLOUT gate)로 정의한다. 실행 모델은 "전용 OS thread가 소유한 async 진행"으로 한정(별도 executor thread·event loop 금지) | 새 공개 API 없음(B1 충족). 정책 (3)의 포화 제출을 실제로 달성 | 정책 (2)의 "async task 금지"를 REQREP에만 완화해야 함. T3은 근사 |
| **B** | Single REQREP은 **in-flight 1을 공식 모델로 인정**하고, 정책 (3)의 "연속 제출"을 Multi에만 적용한다 | 현재 7개 러너를 거의 그대로 둘 수 있음. Single이 gate에서 빠졌으므로 영향 제한적 | Single REQREP throughput의 의미가 C(포화)와 영구히 달라짐 → C 러너도 in-flight 1로 바꿔야 일관됨 |
| **C** | binding 공개 계약에 **admission-only 단계를 노출하는 request terminal**을 추가한다 | 정책 (1)(2)(3)을 모두 만족. T1과 동등한 정확도 | **새 공개 API** → B1과 계획서 §5 `:366-371` 위반. 0.18 범위 |

**설계자 권고: A.** 이유 — (i) B1의 "새 공개 API 없음"을 지키고, (ii) 정책이 가장 중요하게 규정한
"부하 수준"(3)을 살리며, (iii) Single은 gate에서 빠졌으므로 실행 모델 완화의 비용이 가장 작다.
A를 택하면 C 러너도 T1을 유지한 채 그대로 두면 되고(POLLOUT이 아니라 토큰이므로 더 정확),
binding은 T3으로 같은 부하 수준에 도달한다.

**A안 채택 시 개정 문안**

`doc/perf/PERF_SINGLE_TEST_POLICY.md` §1.1, 현행 55-60행:

```text
(before)
  request/reply는 synchronous callback terminal과 completion poller를 사용한다.
  callback terminal이 request ownership 이전의 backpressure를 즉시 알리면 같은
  requester thread가 completion progress를 구동한 뒤 같은 logical request를 다시
  제출할 수 있다.
  측정 구간에는 coroutine, async task, Promise/Future executor, event-loop yield를
  사용하지 않는다.
```

```text
(after)
  request/reply는 그 binding이 공개한 request terminal과 completion 진행 경로를 사용한다.
  C처럼 submit 결과와 대기 토큰을 직접 노출하는 경로는 `ZLINK_SUBMIT_BACKPRESSURED`와
  nonzero 대기 토큰을 admission 경계로 삼고, 그 토큰의 `ZLINK_COMPLETION_WRITABLE`에서
  같은 logical request를 다시 제출한다. 공개 request terminal이 admission과 reply 완료를
  하나의 awaitable로 합쳐 admission 경계를 노출하지 않는 binding은, 같은 requester socket의
  공개 poller에서 관측한 raw socket `ZLINK_POLLOUT` level을 admission 경계로 사용한다.
  POLLOUT이 참인 동안 request를 연속 제출하고 거짓이 되면 제출을 멈춘 뒤 그 poller에서
  대기한다. 이 gate는 target별 정확한 신호가 아니라 socket 전체의 aggregate hint이며
  correlation 예산에 의한 거절을 표현하지 못하므로, single request/reply의 부하 수준은
  근사값으로 기록한다.
  측정 구간에는 event-loop yield와 별도 executor thread를 사용하지 않는다. 위 awaitable을
  진행시키는 주체는 반드시 그 역할의 전용 OS thread여야 하며, 그 thread가 공개 completion
  poller를 구동한다. raw send는 blocking terminal을 그대로 사용한다.
```

같은 문서 61-65행(언어별 명시)에는 다음 문장을 추가한다.

```text
(추가)
  위 request/reply 예외는 request terminal에만 적용한다. one-way raw send 경로에서는
  C++ `co_await`, .NET `Task`, Rust Future executor, Node Promise, Python asyncio를
  계속 사용하지 않는다.
```

`doc/perf/PERF_POLICY.md` 119-126행도 같은 취지로 고친다(문안은 위와 동일한 규칙을 공통 문서
어조로 옮긴다).

### P2 (정책 개정 불필요) C single의 별도 latency 단계 — **러너를 고친다**

R3 BLOCKER 5는 "`PERF_SINGLE_TEST_POLICY.md:137-142`·`:293`의 같은 구간 집계와 D-B125의 별도
latency 단계 중 무엇이 canonical인가"를 물었다. **답: 정책이 canonical이고 C 러너가 위반이다.**
근거는 `PERF_SINGLE_TEST_POLICY.md:37`, `:206-210`(phase 정의에 latency 단계 없음), `:293`,
`PERF_POLICY.md:1230-1236`, `BINDINGS_OPTIMIZATION_GUIDE.ko.md:112-113`(이미 기각 목록).
**정책 문서는 손대지 않고 C 러너에서 §3.2 B-2의 두 단계를 제거한다(B2 결정과 일치).**

### P3 (모호) one-way active 유효 메시지 규칙과 wire 길이 검증

**P3.1** `PERF_SINGLE_TEST_POLICY.md:284-287`은 "recv 루프가 active window 안에서 처리한 유효
header 메시지"라고 하는데, "active window 안"의 기준 시각이 **수신 시각인지 송신 시각인지**
명시하지 않는다. 그 결과 러너가 갈렸다(§3.3 C-4). 개정 문안:

```text
(before, :284-287)
- single one-way active 유효 메시지 규칙은 "`phase == active` 이고, recv 루프가
  active window 안에서 처리한 유효 header 메시지"로 고정한다. idle drain 구간에서
  추가로 recv한 메시지는 종료 정리용으로만 소비하며, active 집계 포함 여부는
  이 규칙을 바꾸지 않는다.
```

```text
(after)
- single one-way active 유효 메시지 규칙은 "`phase == active` 이고, **수신 시각이 active
  deadline 이전인** 유효 header 메시지"로 고정한다. 판정에 쓰는 시각은 recv 루프가 그
  메시지를 처리한 monotonic 시각이며, payload의 `sent_ts_ns`가 아니다. active deadline
  이후에 recv한 메시지는 stop token 도착 여부와 무관하게 집계에서 제외하고 종료 정리로만
  소비한다. 이 규칙은 모든 one-way 패턴과 모든 binding에 같은 의미로 적용한다.
```

**P3.2** `PERF_SINGLE_TEST_POLICY.md:280-283`은 "decode 실패 / `magic`·`phase`·`msg_size` 검증
실패 메시지는 집계 제외"라고만 하고 **wire 프레임 길이 검증을 규정하지 않는다.** C는 fatal,
다른 러너는 skip 또는 미검증이다(§3.3 C-5). 개정 문안:

```text
(추가, :283 뒤)
- 수신 프레임의 실제 byte 길이가 기대 payload 크기와 다른 메시지는 집계에서 제외한다.
  이 불일치는 실패가 아니라 집계 제외 사유이며, 러너를 중단시키지 않는다.
```

### P4 (미규정) latency 통계의 경계 규칙

**P4.1** cap이 `0`이거나 표본이 하나도 없을 때의 p95/p99 값이 규정되어 있지 않다(§3.3 C-3).

```text
(추가, PERF_SINGLE_TEST_POLICY.md:519 표 아래 / PERF_MULTI_TEST_POLICY.md §5.3)
- percentile sample을 하나도 보관하지 않은 경우 `latency_p95`와 `latency_p99`는 평균
  latency와 같은 값으로 보고한다. `0`으로 보고하지 않는다. 전체 count와 sum은 sample
  보관 여부와 무관하게 계속 누적한다.
```

**P4.2** percentile 보간법이 규정되어 있지 않아 단일 프로세스 경로(선형 보간)와 matched-client
가중 경로(보간 없음)가 다르다(§3.3 C-6).

```text
(추가, PERF_MULTI_TEST_POLICY.md §5.3 `- p95: 샘플의 95th percentile` 아래)
- percentile은 정렬한 sample에 대해 `pos = (n - 1) * q`, `lo = floor(pos)`,
  `hi = min(lo + 1, n - 1)` 로 선형 보간해 계산한다. 여러 프로세스의 reservoir를 병합하는
  경로도 같은 보간식을 사용하며, 병합 시 각 sample의 가중치를 그 프로세스의 관측 수로 둔다.
```

### P5 (미규정) 러너가 제각각 해석한 상수와 시간원

정책이 침묵해 러너마다 값이 갈린 항목이다. 모두 **C 구현을 준용해 정책에 못박는다.**

```text
(추가, PERF_SINGLE_TEST_POLICY.md §1.1 "공통" 블록)
- one-way 송신이 transient 오류(`EAGAIN`/`EINTR`/`ETIMEDOUT`)를 만나면 1 ms 대기한 뒤 같은
  sequence를 새 `sent_ts_ns`로 다시 stamp해 재제출한다. sleep 없는 즉시 반복(busy retry)과
  1 ms를 넘는 backoff는 모두 금지한다.
```

```text
(추가, PERF_POLICY.md § 1.1 공통 원칙)
- 모든 러너의 경과 시간, active deadline, timeout, drain 한도는 monotonic clock으로 측정한다.
  wall clock은 결과 파일의 timestamp 표기에만 사용한다.
- RESULT line의 throughput과 bandwidth는 소수점 이하 3자리, latency 계열 3개는 소수점 이하
  6자리 고정 소수점으로 출력한다.
```

```text
(추가, PERF_MULTI_TEST_POLICY.md § 12.3)
| `PERF_MULTI_SEND_DRAIN_TIMEOUT_MS` | active 종료 후 미완료 admission을 비우는 bounded drain 한도(ms) | 5000 |
```

### P6 (모호) binding별 미지원 조합의 처리

`PERF_POLICY.md:453-455`는 "특정 바인딩 public API 부족으로 C handshake를 구현할 수 없으면
binding public API를 보강하거나 해당 perf 조합을 `UNSUPPORTED`로 처리한다"고 하고, `:462-464`는
"`UNSUPPORTED`를 C 기준과 불일치하는 기존 구현을 숨기는 용도로 쓰면 안 된다"고 한다. Node의
`inproc` 전면 `UNSUPPORTED`(§3.4 D-3)가 어느 쪽인지 문언으로 갈리지 않는다.

```text
(추가, PERF_POLICY.md:464 뒤)
- 언어 runtime의 실행 모델 제약으로 특정 transport를 측정할 수 없는 경우에는, 그 조합을
  runner가 임의로 `UNSUPPORTED`로 내리지 않고 suite 정책 문서의 pattern/transport 표에
  binding별 제외 사유와 함께 명시한다. 표에 없는 조합을 runner가 `UNSUPPORTED`로 처리하면
  회귀 은폐로 본다.
```

---

## 5. 작업 순서

각 단계는 **그 단계의 검증이 끝나기 전에 다음으로 넘어가지 않는다.** perf 실행은 항상
직렬화한다(계획서 §7.0 `:440-443`).

### 단계 0 — 감독자 결정 확보 (측정 전)

- §6-D1(Single REQREP 정책 개정 A/B/C), §6-D2(계획서 §2·§9·§12 개정), §6-D3(Core monitor
  snapshot 확인 필요 여부)을 먼저 닫는다. 결정 전에는 **어떤 재측정도 기준값으로 쓰지 않는다.**
- 검증: 결정문을 `doc/plan/c016-worklog/decisions.ko.md`에 기록.

### 단계 1 — 측정 조건 통일 (§3.1) — **최우선, 측정 없이 가능**

순서:

1. **A-1**: C++ multi server의 auto-HWM snapshot 채취 지점을 C와 같은 위치(active 수신 중 첫 유효
   메시지 1회)로 옮긴다. 나머지 6개 binding의 채취 지점도 같은 위치로 맞춘다.
2. **A-6 Go recalc 누락**: Go multi의 dealer/router/pubsub server에 phase별
   `RecalculateAutoHwm` 호출을 추가한다(M15).
3. **A-2 / A-6-4**: monitor HWM 이름을 C canonical(`monitor_hwm_bytes`, `--monitor-hwm-bytes`,
   `PERF_MULTI_MONITOR_HWM_BYTES`)로 6개 binding에 통일하고, C++ `run_comparison.py` 기본값을
   `4096000`으로 고친다.
4. **A-3 / A-6-1**: `default_stream_clients`를 `100`으로 통일한다(C++ `run_comparison.py:3921`,
   `bindings/python/perf/perf_report.py:474-476`).
5. **A-6-2**: `bindings/python/perf/perf_report.py:510`의 `monitor_hwm` 하드코딩을 실제 값으로 고친다.
6. **A-6-3**: Node override 모드 HWM fallback을 `0`으로 고친다.
7. **A-4 / A-6-5 / A-6-6 / A-6-7**: Effective Options key 집합을 C와 동일하게 맞춘다
   (`routed_echo_per_socket_payload` 추가·의미 정렬, `service_clients` 추가, `fail_fast`/`smoke`/
   `go_gomaxprocs*` 제거 또는 C에도 추가).
8. **A-5**: `select_transports()` 규칙(사용자 지정 순서 유지 + base 교집합 필터)을 6개 binding에
   이식한다.
9. **A-7**: `PERF_MULTI_SEND_DRAIN_TIMEOUT_MS` 기본값을 `5000`으로 통일한다.
10. **A-6-8**: .NET multi가 `SNDTIMEO`를 실제로 설정하도록 고친다.
11. **A-8**: .NET `run_comparison.py`가 `--runs`를 전달하도록 고친다.

**검증**: 각 binding에서 `MULTI_DEALER_DEALER`/tcp/64B/1s/1run/100 clients smoke를 돌리고,
C report와 binding report의 `## Effective Options (start)` 블록을 **자동 diff**해서 key 집합과
값이 완전히 일치하는지 확인한다. `## Auto-HWM Detail`의 client·server 행이 C와 같은 값인지
확인한다. **이 diff가 깨끗해지기 전에는 단계 2로 가지 않는다.**

### 단계 2 — C 러너를 정책에 맞춘다 (§3.2 B-2, §3.3 C-4·C-5·C-6)

1. `bindings/c/perf/single/common/perf_single_reqrep.hpp`에서 `latency_phase_duration_seconds()`
   (`:504-507`)와 `run_requester()`의 2-phase 호출(`:537-548`)을 제거하고, 단일 phase에서
   `capture_latency = true`로 돌린다.
2. `bindings/c/perf/single/common/perf_single_one_way.hpp`에서
   `latency_phase_duration_seconds()`·`latency_phase_max_in_flight()`(`:228-236`)와
   `run_active_phase()`의 2-phase 호출(`:441-470`)을 제거한다. `send_active_samples()`의
   `max_in_flight_` 파라미터와 `:202-211`의 gate 루프도 함께 제거한다.
3. P3.1 개정 문안대로 one-way 수신 집계에 **active deadline 필터**를 추가한다(`:338-348`).
4. P3.2 개정 문안대로 wire 길이 불일치를 fatal이 아니라 **집계 제외**로 바꾼다(`:122`, `:129-136`).
5. `perf_single_latency.hpp:100-111`의 `default_sample_cap()`을 `perf_single_reqrep.hpp:96-106`과
   같은 검증으로 통일한다.
6. `perf_multi_weighted_latency.hpp:64-86`의 percentile을 P4.2 문안대로 선형 보간으로 바꾼다.

**검증**: C single smoke(전 패턴, tcp·wss, 1024B — `PERF_POLICY.md:919-921`)가
`status: complete`이고, RESULT 5 metric이 모두 0이 아니며, 케이스 wall time이 이전보다
**케이스당 약 1초 줄었는지** 확인한다. 그다음 C single·multi full matrix를 다시 측정해
**새 canonical 기준값**을 만든다(B2). 기존 Single 기준값은 전부 폐기한다.

### 단계 3 — Multi 종료 protocol 정합 (§3.4 D-1, D-2)

C·Go를 제외한 6개 binding REQREP client에 `CLIENT_DONE,<size>` 출력 → stdin `STOP` 대기 →
socket close 순서를 넣고, runner가 server `STOP` → client `STOP` 순서로 보내게 한다.
D-2의 패턴별 token 목록도 C 기준으로 맞춘다.

**검증**: 각 binding multi smoke에서 (i) `CLIENT_DONE`이 stdout에 나오는지, (ii) client 프로세스가
runner `STOP` 이후에 종료하는지, (iii) server 종료 로그에 미완료 reply 경고가 없는지 확인한다.
REQREP timeout 비율이 0%인지도 함께 본다.

### 단계 4 — Multi 부하 수준 정합 (§3.2 B-4, B-6)

- Go: socket당 goroutine 1개 blocking `Submit(ctx)` 구조를 **socket당 여러 request가 동시에
  진행되는** 구조로 바꾼다(goroutine 여러 개가 같은 socket을 소유하는 형태). Go에 async request
  terminal이 없으므로 `PERF_POLICY.md:250-254`가 허용한 "goroutine 하나가 blocking terminal 하나를
  소유"를 socket당 N개로 확장한다.
- Java: `inFlight` 배열 gate(`.../PerfMultiSocketReqRep.java:207-208`)를 제거하고 admission
  경계(T3)로 대체한다.
- .NET·Node·Python: 무제한 제출에 admission 경계(T3)를 넣는다.
- Rust: 턴당 1건 push를 admission 경계까지 연속 제출로 바꾼다.
- C++: 슬롯당 1건 고정(`operation_active`)을 admission 경계까지 연속 제출로 바꾼다.
- Python SENDSEND 2종의 `_sync=True`를 async terminal로 되돌린다(§3.2 B-6).

**검증**: 각 binding에서 REQREP·SENDSEND smoke를 돌려 (i) outstanding request 수가 시간에 따라
발산하지 않고 정상 상태에 수렴하는지(러너 debug 로그), (ii) `PERF_MULTI_REQREP_TIMEOUT_MS`
초과가 0%인지, (iii) 메모리 사용이 duration에 비례해 증가하지 않는지 확인한다.

### 단계 5 — Single 러너 정책 정합 (§3.2 B-1, B-3, B-5; §3.3 C-1·C-2·C-7·C-9; §3.4 D-3·D-4)

Single은 gate에서 빠지지만 정책 준수는 별개다(사용자 결정).

1. §6-D1 결정에 따라 7개 binding의 Single REQREP 부하 모델을 확정한다.
2. B-3: Node·Go·Rust·Python·C++(RR)·.NET(PUBSUB)의 수신을 poller `POLLIN` + `wait(-1)` +
   `DONTWAIT` drain으로 바꾼다. Python의 50ms 상한과 2초 stop 유예도 제거한다.
3. B-5: .NET REQREP의 `Task` 경로, Rust one-way의 수동 executor, Node의 main-thread recv 루프를
   정책대로 고친다.
4. C-1: `run_id`를 case ordinal로 바꾼다(Java·Node·Go·Rust).
5. C-2: .NET `PerfShared.ReservoirSample`을 실제 Algorithm R로 고친다.
6. C-7: busy-retry를 1ms sleep + 재stamp로 통일한다(C++ RR, .NET 4패턴, Node 전체, Rust).
7. C-9: 출력 정밀도를 P5 문안대로 통일한다.
8. D-3: Node `inproc`을 정책 표에 명시하거나 binding을 보강한다.
9. D-4: Rust single one-way의 `received == 0` FAIL 처리를 추가한다.

**검증**: 각 binding single smoke가 `status: complete`이고, C 기준과 `## Effective Options` diff가
깨끗하며, latency p95/p99가 mean보다 크고 0이 아닌지 확인한다.

### 단계 6 — 재측정과 판정

- 계획서 §7.0 `:426-443`의 "한 번에 하나의 `binding + suite + pattern + transport`" 규칙을 지켜
  C → binding 순으로 paired 측정한다.
- **성능 판정은 Multi suite로만 한다**(`PERF_POLICY.md:144-149` + 사용자 결정).
- Single은 `status: complete`와 정책 준수만 확인하고 표에 기록하되 통과/미달 판정에 넣지 않는다.

---

## 6. 위험과 감독자 결정이 필요한 항목

### D1 (결정 필요) Single REQREP 정책 개정 — A / B / C

§4-P1.3의 세 안 중 하나를 골라야 한다. **설계자 권고는 A**(binding async request terminal을
Single REQREP에만 허용 + POLLOUT gate). 결정 전에는 7개 binding의 Single REQREP을 고칠 수 없다.

### D2 (결정 필요) 계획서와 `PERF_POLICY.md:144-149`의 충돌

`PERF_POLICY.md:144-149`
> "**bindings ↔ C 성능 비교는 multi suite로 한정한다.** ... single 결과는 각 binding의
> synchronous path와 lifecycle을 검증하는 데 사용하고 **cross-binding ratio에는 사용하지 않는다.**"

그러나 계획서는 Single suite에 통과/미달 gate를 건다.

| 계획서 위치 | 충돌 내용 |
|---|---|
| §2.1 `:182-200` | Pattern 그룹 표에 Single 패턴(`PAIR`, `PUBSUB`, `DEALER_DEALER`, `DEALER_ROUTER`, `ROUTER_ROUTER`, `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`)이 포함되고, 언어별 `최소 기준 / 중앙값 목표` 표가 이들에 적용된다 |
| §2.1 `:141-145` | "현재 비교 대상의 aggregate mean이 목표보다 낮으면 다음 transport·pattern·언어로 이동하지 않는다" — Single 셀에도 적용된다 |
| §2.2 `:285-291` | Single `PUBSUB`/`DEALER_DEALER`/`DEALER_ROUTER`의 latency ratio 예외표 |
| §9.x `:723` 등 | "Single 상태: `미달`(REQREP만)" 처럼 Single에 통과/미달 판정 |
| §9.x.1 `:727-` | 언어별 "Single suite" 상세 표에 비율과 판정 |
| §12 `:1395` | "모든 binding 상세 표에 `미측정` 또는 `미달`이 없다" — Single 상세 표 포함 |

**사용자 결정에 따라 `PERF_POLICY.md`가 이긴다.** 계획서를 다음과 같이 고쳐야 한다
(**계획서 수정은 감독자가 한다 — 이 설계 문서는 수정하지 않았다**).

1. §2.1 Pattern 그룹 표에서 Single 패턴 7종을 제거하고 Multi 7종만 남긴다. 언어별 목표표의
   `단순 one-way`/`routed one-way`/`socket request/reply` 항목을 Multi 패턴 기준으로 재정의한다.
2. §2.2 latency 예외표에서 `Single ...` 행 5개를 제거한다.
3. §9.x의 "Single 상태" 항목을 "Single 정책 준수 상태(참고)"로 바꾸고 통과/미달 어휘를 쓰지 않는다.
4. §9.x.1 Single 상세 표를 "정책 준수 점검표 + 참고 수치"로 재정의하고 비율 gate를 제거한다.
5. §12 완료 기준 `:1395`를 "**Multi** 상세 표에 `미측정` 또는 `미달`이 없다"로 한정하고,
   Single에 대해서는 "정책 준수 점검이 끝나고 report가 `status: complete`이다"를 별도 항목으로 둔다.

### D3 (확인 필요) C++ server auto-HWM 값이 실제 차이인지 보고 차이인지

§3.1 A-1의 미확인 항목. Core의 monitor snapshot이 `auto_hwm_applied_*`를 언제 채우는지 확인해야
(i) 실제 effective HWM 불일치인지, (ii) 보고만 잘못된 것인지 판별된다. **Core 담당 범위.**
어느 쪽이든 단계 1의 조치는 동일하지만, (i)이면 지금까지의 C↔C++ multi paired 결과가 **전부**
무효이고 (ii)이면 보고만 고치면 되므로 **과거 결과의 취급이 달라진다.**

### D4 (위험) B2 적용 후 Single latency 값이 크게 오른다

§3.2 B-2 표대로 one-way latency는 "in-flight 1의 무부하 지연"에서 "포화 큐 깊이를 포함한 지연"으로
바뀐다. 절대값이 수십~수백 배 커질 수 있다. Single이 gate에서 빠졌으므로 판정에는 영향이 없지만,
**과거 Single 측정값과 직접 비교하면 안 된다.** 기존 Single 기준값은 전부 폐기한다.

### D5 (위험) T3(POLLOUT gate)의 정확도 한계를 결과에 표기해야 한다

§4-P1.2대로 POLLOUT은 correlation 예산 거절을 표현하지 못하고 overshoot이 있다. Multi REQREP은
socket 100개가 동시에 outstanding을 쌓으므로 이 원인이 실제로 발생할 수 있다. T3를 쓰는 셀은
결과에 "부하 경계 = POLLOUT 근사"를 함께 기록해야 하며, C(T1)와의 비율을 절대적 성능비로 읽으면
안 된다.

### D6 (위험) 단계 4의 Go 재구성이 D-B121·D-B123 결정을 뒤집는다

D-B123은 "REQREP 러너를 socket당 goroutine으로" 바꾸는 것을 승인했고 그것이 현재 구현이다.
단계 4는 이를 socket당 N goroutine으로 다시 바꾸라고 요구한다. **D-B123을 대체하는 새 결정이
필요하다.**

### D7 (미확인 잔여)

표에 `?`로 남은 칸은 충족으로 간주하지 않는다. 특히 다음은 단계 1~5에서 반드시 확정한다.

- S11(bounded idle/completion drain) 7개 binding 전부
- S14(size마다 바이너리 재실행) Java·Node·Go·Rust·Python
- S17(single `PERF_IO_THREADS=1`) 7개 binding 전부
- M13(size마다 독립 프로세스 쌍) Java·Node·Go·Rust·Python. C는 REQREP client가 한 프로세스에서
  여러 size를 루프하므로(`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:769-793`)
  `PERF_MULTI_TEST_POLICY.md:594`("각 size 케이스는 반드시 독립된 server/client 프로세스 쌍으로
  실행한다")와 어긋날 소지가 있다 — **C 러너 위반 후보. 확인 필요.**
- M14(multi `PERF_IO_THREADS=4`) .NET·Java·Node·Go·Rust·Python
- M21(지원 패턴 inventory) 7개 binding 전부
- M7(STREAM server의 async terminal 1회 호출) C·Go·Rust·Python
- Node single `inproc` 제외가 정책 예외로 인정되는지(D-3)

---

## 7. 부록 — 이 문서가 사용한 조사 범위

- 정책: `doc/perf/PERF_POLICY.md`(1504행), `doc/perf/PERF_SINGLE_TEST_POLICY.md`(544행),
  `doc/perf/PERF_MULTI_TEST_POLICY.md`(1419행), `doc/perf/BINDINGS_OPTIMIZATION_GUIDE.ko.md`(122행)
- Core 사양: `core/doc/spec/core/socket/README.ko.md`(§Part send·§Request와 reply·§Completion pull),
  `core/doc/spec/core/05-polling.ko.md`
- binding 공개 계약: `bindings/doc/spec/async-execution-model.ko.md`,
  `bindings/doc/spec/async-coroutine-policy.ko.md`, `bindings/doc/spec/README.ko.md` §Perf 정책
- 캠페인 문맥: 계획서 §5·§7.0·§7.0.1·§2·§9·§12, `doc/plan/c016-worklog/decisions.ko.md`
  D-B121·D-B123·D-B125·D-B127, `doc/plan/c016-worklog/spec-review/R3-bindings-summary.md` §성능 정책의
  계약 불일치·BLOCKERS
- 러너 소스: `bindings/c/perf/` 전체, `bindings/{cpp,dotnet,java,node,go,rust,python}/perf/`
- 실측 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_080912.txt`,
  `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_081121.txt`

**조사하지 않은 것**: Core 구현 내부(monitor snapshot 채움 시점 포함), `framework/**`,
각 러너 셸/파이썬 스크립트의 환경 변수 전 경로, `MULTI_STREAM`의 latency 계산(공용 C stream
client 소유), `bindings/javascript`·`bindings/kotlin`(perf 디렉터리가 없고 정책 문서에도 없음).
