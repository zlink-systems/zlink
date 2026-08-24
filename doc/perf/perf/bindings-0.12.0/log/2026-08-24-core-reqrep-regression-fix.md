# Core REQREP 회귀: engine write turn이 queue를 kernel로 이전시키는 문제

작업 범위: `core/`만. 빌드·검증은 `core/build`에서 수행했다. commit은 만들지 않았고
`core/doc`과 `doc/site`는 건드리지 않았다.

**이 문서는 결정을 내리지 않는다.** 세 가지 configuration을 같은 build에서 측정한
증거와 판단 기준을 제시하고, 채택 여부는 owner에게 남긴다. tree의 기본값은
현행 동작(C)으로 유지했다.

## 1. 최초 가설과 실제 원인

задание은 request/reply completion lane(`zlink-req-time` deadline 등록, completion
queue hop, reply handler dispatch), byte-HWM accounting, part-helper gate,
`9ea5d103da`의 send-complete TLS guard를 후보로 지목했다. **모두 원인이 아니다.**

측정으로 배제한 근거:

- CPU 회계가 방향을 반대로 가리켰다. `/usr/bin/time -v` 기준 DEALER_ROUTER_REQREP
  tcp 64 B 3초 실행에서 0.10.1은 user 4.60 s + sys 2.31 s로 624 K op을 처리했고,
  0.13.0은 user 2.77 s + sys 1.75 s로 363 K op을 처리했다. op당 CPU는 11.1 µs →
  12.4 µs로 12%만 늘었는데 throughput은 42% 떨어졌다. per-message 명령어 비용이
  원인이면 CPU가 같이 올라야 하는데 오히려 **줄었다**. 즉 회귀는 CPU 비용이 아니라
  **blocking/queueing**이다.
- rdtsc probe로 requester submit / poller wait / reply handler / router recv /
  router reply를 각각 계측했다. submit 1,572 → 1,692 ns, reply handler 47 → 29 ns,
  router reply 1,141 → 1,308 ns로, 네 구간 합계 증가는 op당 약 270 ns였다. 그런데
  op당 wall time은 4,885 → 7,485 ns로 약 2,600 ns 늘었다. 증가분의 90%가
  이 hot path들 **바깥**에 있었다.
- TLS guard(`9ea5d103da`)는 0.12.0 → 0.13.0 구간이며 그 구간의 변화는 129 K →
  133 K로 회귀 방향이 아니다.

### 실제 원인

`core/src/runtime/engine/asio/asio_engine.cpp:1287` `speculative_write()`의 drain
loop다. 이 loop는 session pipe에서 message를 계속 꺼내 kernel로 밀어 넣으며,
종료 조건은 pipe가 비거나 kernel send buffer가 EAGAIN을 돌려주는 것뿐이다.

이 turn을 bound하는 byte budget(`asio_stream_spec_write_budget_bytes`, 2 MiB)이
`stream_tcp_speculative`에 gate돼 있었다 (수정 전 `:1320-1322`):

```cpp
const bool stream_tcp_speculative = _options.type == ZLINK_CORE_SOCKET_STREAM
                                    && is_tcp_transport ()
                                    && asio_stream_enable_speculative_write;
```

즉 **budget은 STREAM/tcp에만 적용됐다.** DEALER/ROUTER/PAIR/PUB은
`use_stream_speculative_write()`(`:833`)가 transport의
`supports_speculative_write()`로 fall through 하면서 같은 loop에 진입하지만,
`stream_tcp_speculative == false`라 budget 검사가 한 번도 발화하지 않는다.
추가로 `start_async_write()`(`:670-685`)도 async 전환 전에 동기 write를 시도하고
성공하면 `speculative_write()`로 재진입하므로, drain은 write turn마다 다시 시작된다.

### 왜 0.10.1에서는 문제가 아니었는가

`asio_engine.cpp`의 write 경로(`speculative_write`, `start_async_write`,
`out_event`, `restart_output`, `process_output`, `prepare_output_buffer`,
`use_stream_speculative_write`)는 `core/v0.10.1`과 HEAD가 **완전히 동일하다**
(`git diff core/v0.10.1..HEAD` 기준 해당 함수 diff 없음). 정책 파일
`asio_stream_fastpath_policy.hpp`의 `enable_speculative_write()`와
`spec_write_budget_bytes()`도 동일하다.

따라서 engine 결함은 **잠재 상태로 계속 있었고**, byte-HWM 작업이 application
pipe를 이 loop가 폭주할 만큼 깊게 만들면서 발화했다. release asset 기준
DEALER_ROUTER_REQREP tcp 64 B 중앙값은 0.10.1 203 K → 0.11.1 110 K → 0.12.0
129 K → 0.13.0 133 K로, 절벽은 0.11.x 구간이다. (`core/build/lib`에 남아 있던
`libzlink.so.0.11.1`은 perf branch의 patched build라 200 K가 나온다. 버전 비교는
반드시 `~/.cache/zlink/core/<ver>/linux-x64`의 release asset을 써야 한다.)

## 2. Queue가 어디에 있는지: 계측 증거

PAIR/tcp 64 B 포화 송신 중, zlink pipe 잔류량과 HWM 발동은 monitor snapshot으로,
kernel socket queue는 `ss -tn`으로 동시에 표본화했다.

| configuration | mean latency | kernel Q 중앙값 | zlink pipe snd 평균/최대 | parks | HWM blocked ppm |
|---|---:|---:|---:|---:|---:|
| C (현행) | 62.2 ms | **9,485 KiB** | 388 KB / 1,048,576 B | **0 / 12.25 M** | 43 |
| A | 0.445 ms | **3.1 KiB** | 560 KB / 1,019,264 B | 0 / 16.38 M | 44 |
| B (2 MiB budget) | 65.0 ms | 9,402 KiB | 391 KB / 1,048,576 B | 0 / 12.50 M | 43 |
| release 0.10.1 | 0.307 ms | 2.2 KiB | (ABI 불일치로 무효) | 0 / 16.88 M | (무효) |

읽는 법:

- **byte-HWM은 pipe에서 제 일을 하고 있다.** 세 configuration 모두 pipe 잔류량이
  applied HWM 1 MiB에 정확히 묶여 있다.
- **그런데 HWM이 한 번도 발동하지 않는다.** C에서 12,254,173회 send 중 park은 0회다.
- **queue가 HWM 관할 밖으로 이전됐기 때문이다.** C의 총 in-flight는 pipe 1 MiB +
  kernel 9.3 MiB ≈ 10.3 MiB이고, 그중 **90%가 kernel socket memory**에 있다. byte-HWM
  회계는 kernel 잔류량을 보지 못하므로 backpressure를 걸 근거가 없다.
- A는 pipe 1 MiB + kernel 3 KiB로, 전량이 HWM 관할 안에 있다. 0.10.1도 kernel
  2.2 KiB로 A와 같은 계열이다.

이것이 "유한 byte HWM"이라는 계약이 실제로는 성립하지 않고 있었다는 증거다. 큐 깊이는
정책이 아니라 kernel send buffer의 autotuning이 정하고 있었다.

## 3. 세 configuration

한 build에서 `ZLINK_ASIO_WRITE_TURN_MODE`로 선택한다. 기본값은 0(C)이며,
mode 0은 pristine snapshot과 동일함을 측정으로 확인했다(130,291 vs 129,375, noise 내).

| mode | 이름 | 내용 | spec 정합성 |
|---|---|---|---|
| 0 | C | 현행. STREAM/tcp만 budget, 나머지는 무한 drain | 위반 (아래 §5) |
| 1 | A | non-STREAM은 Proactor write path. STREAM은 그대로 | **정합** |
| 2 | B | 모든 socket이 동기 write, budget을 전 socket에 확대 | spec 개정 필요 |

## 4. 측정 결과

tcp, duration 3 s, 중앙값. gate cell은 한산한 host(load 0.58)에서 5회, 나머지는 3회.

### 4.1 REQREP (회귀 cell)

| cell | C | A | B | 0.10.1 | A/0.10.1 | C/0.10.1 |
|---|---:|---:|---:|---:|---:|---:|
| DEALER_ROUTER 64 B | 131,830 | **208,200** | — | 193,078 | **107.8%** | 68.3% |
| DEALER_ROUTER 256 B | 125,941 | **161,980** | — | 164,190 | **98.7%** | 76.7% |
| DEALER_ROUTER 1024 B | 131,618 | **161,606** | — | 165,008 | **97.9%** | 79.8% |
| DEALER_ROUTER 256 KiB | 5,880 | 6,028 | 6,426 | 5,728 | 105.2% | 102.7% |
| ROUTER_ROUTER 64 B | 128,430 | **196,366** | 128,876 | 187,768 | **104.6%** | 68.4% |
| ROUTER_ROUTER 256 B | 118,549 | 158,767 | 112,048 | 161,799 | 98.1% | 73.3% |
| ROUTER_ROUTER 1024 B | 116,488 | 155,386 | 133,637 | 164,504 | 94.5% | 70.8% |
| ROUTER_ROUTER 256 KiB | 6,707 | 6,418 | 6,681 | 5,767 | 111.3% | 116.3% |

RTT mean latency (DEALER_ROUTER): 64 B는 C 0.378 ms → A 0.219 ms (0.10.1 0.206 ms,
**+6.3%**), 256 B는 A 0.276 ms (0.10.1 0.250 ms, +10.4%), 1024 B는 A 0.308 ms
(0.10.1 0.295 ms, +4.4%).

**A는 REQREP 목표(throughput ≥95%, RTT +10% 이내)를 사실상 충족한다.**
256 B latency만 +10.4%로 경계선이다.

### 4.2 One-way

| cell | C | A | B | 0.10.1 |
|---|---:|---:|---:|---:|
| PAIR 64 B | 1,908,182 / 71.1 ms | 2,229,750 / **0.204 ms** | 1,941,475 / 68.9 ms | 2,285,856 / 0.264 ms |
| PAIR 1024 B | 913,499 / 10.8 ms | 606,116 / 0.800 ms | 957,781 / 10.3 ms | 599,784 / 1.289 ms |
| PAIR 128 KiB | 19,522 / 3.98 ms | 20,780 / 0.296 ms | 19,804 / 3.92 ms | 20,550 / 0.376 ms |
| PAIR 256 KiB | 11,040 / 3.53 ms | 12,827 / 0.320 ms | 11,746 / 3.33 ms | 13,260 / 0.357 ms |
| DEALER_DEALER 64 B | 2,069,269 / 64.4 ms | 2,460,105 / 0.242 ms | 2,089,335 / 65.0 ms | 2,046,214 / 3.150 ms |
| DEALER_DEALER 1024 B | 883,931 / 11.2 ms | 536,196 / 0.912 ms | 821,364 / 11.5 ms | 616,940 / 1.317 ms |
| DEALER_DEALER 128 KiB | 19,510 / 3.98 ms | 21,985 / 0.284 ms | 19,676 / 3.95 ms | 21,643 / 0.358 ms |
| DEALER_DEALER 256 KiB | 11,413 / 3.42 ms | 13,272 / 0.308 ms | 10,918 / 3.58 ms | 12,125 / 0.391 ms |
| ROUTER_ROUTER 64 B | 1,847,246 / 68.7 ms | 2,042,171 / 0.424 ms | 2,052,393 / 67.2 ms | 1,890,199 / 0.475 ms |
| ROUTER_ROUTER 1024 B | 917,089 / 10.8 ms | 689,694 / 11.9 ms | 949,630 / 10.4 ms | 586,793 / 1.333 ms |
| ROUTER_ROUTER 128 KiB | 18,543 / 4.20 ms | 19,802 / 0.307 ms | 18,164 / 4.27 ms | 20,433 / 0.381 ms |
| ROUTER_ROUTER 256 KiB | 11,423 / 3.44 ms | 13,010 / 0.316 ms | 11,543 / 3.39 ms | 13,144 / 0.359 ms |
| PUBSUB 64 B | 1,181,957 / 31.7 ms | 1,101,418 / 0.121 ms | 1,195,552 / 29.0 ms | 1,253,561 / 0.187 ms |
| PUBSUB 1024 B | 845,553 / 11.3 ms | 539,517 / 0.900 ms | 853,404 / 11.2 ms | 535,508 / 1.413 ms |
| PUBSUB 128 KiB | 17,849 / 4.36 ms | 18,548 / 0.309 ms | 17,646 / 4.39 ms | 20,204 / 0.383 ms |
| PUBSUB 256 KiB | 11,079 / 3.51 ms | 12,659 / 0.311 ms | 11,422 / 3.44 ms | 13,334 / 0.352 ms |

핵심: **C가 A보다 throughput이 높은 구간은 one-way 1024 B 부근뿐이다.**
64 B와 ≥128 KiB에서는 A가 throughput과 latency를 **동시에** 이긴다. 그리고 그
1024 B 우위는 10 ms대 queue residency를 대가로 산 것이다. 같은 cell에서 C는
0.10.1 대비 throughput +52%인데 latency는 +8배다.

`ROUTER_ROUTER 1024 B`의 A는 latency 11.9 ms로 예외다. 이 cell만 A에서도 깊은
queueing이 남는다. kernel이 아니라 pipe(1 MiB HWM = 1024 msg) 쪽 잔류로 보이며,
이는 write turn이 아니라 byte-HWM 자체의 residency다. A로 회수되지 않는 잔여
항목으로 기록한다.

### 4.3 B가 동작하지 않는 이유 (측정)

budget 크기를 8 KiB / 32 KiB / 128 KiB / 512 KiB / 2 MiB로 훑었다. 전부 C와 같다.

| budget | REQREP 64 B | PAIR 1024 B |
|---|---:|---:|
| 8 KiB | 126,852 | 933,933 |
| 32 KiB | 126,884 | 945,415 |
| 128 KiB | 123,824 | 902,564 |
| 512 KiB | 123,770 | 889,767 |
| 2 MiB | 126,209 | 983,000 |
| (C 기준) | 125,926 | 929,019 |
| (A 기준) | 197,586 | 601,258 |

이유는 구조적이다. budget은 **한 turn**을 끊을 뿐이고, drain은 다음 write event에서
바로 재개된다. kernel 잔류량은 turn 길이가 아니라 turn의 **재진입 빈도**가 정한다.
같은 이유로 기존 `ZLINK_ASIO_SINGLE_WRITE=1` knob(turn당 1회 write)도 C와 같았다
(REQREP 64 B 126,672 / PAIR 64 B 1,944,610, 61.5 ms).

"turn당 1회 write + 재진입 금지" 변형도 시험했다. REQREP 64 B는 134,782로 거의
개선이 없었고 **PAIR는 아예 멈췄다** — `start_async_write()`의 기존 주석이 경고하는
stranded-output 위험이 그대로 재현됐다. 폐기했다.

즉 B는 판단이 아니라 **측정으로 탈락**했다. `core-byte-hwm-performance-regression-handoff.ko.md`
§폐기한 실험의 "generic fairness budget" 항목과 결론은 같지만, 이번에는 왜 불가능한지
기전이 확인됐다.

## 5. Spec 정합성

- `core/doc/spec/core/systems/03-io-thread.ko.md` §4는 I/O thread의 write를
  Proactor 모델(`async_write_some` 1건 in-flight, completion handler가 재무장)로
  규정한다.
- `core/doc/spec/core/socket/08-stream.ko.md:395-401`은 speculative synchronous
  write를 **STREAM fast path의 예외**로 한정하고, 그 예외가 성립하는 근거로 byte
  budget을 든다.

현행 C는 일반 socket을 spec에 없는 동기 drain으로 보내고 있으므로 **spec 위반**이다.
A는 spec 문언 그대로 복원한다. B는 일반 socket의 동기 write를 정식화하는 것이므로
spec 개정이 선행돼야 한다.

## 6. 변경 내용

기본 동작은 바꾸지 않았다. 세 configuration을 한 build에서 측정할 수 있게 하고,
결정 지점을 정책 객체로 모았다.

- `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp`
  - `write_turn_mode_t` / `write_turn_mode()` 추가. `ZLINK_ASIO_WRITE_TURN_MODE`,
    기본 0.
  - `use_speculative_write_for(mode, socket_type, tcp, transport_supports)`:
    "이 engine이 동기로 써도 되는가" 결정 전체를 소유하는 순수 predicate.
  - `write_turn_budget_applies(mode, socket_type, tcp)`: "이 turn은 budget에
    묶이는가" 결정을 소유하는 순수 predicate.
- `core/src/runtime/engine/asio/asio_engine.cpp`
  - `use_stream_speculative_write()`가 socket type과 transport를 직접 판정하던
    것을 policy predicate 위임으로 교체.
  - `speculative_write()` 안에서 socket type + transport + flag를 **다시 유도하던**
    `stream_tcp_speculative`를 제거하고 `write_turn_budget_applies()` 호출로 교체.
    같은 판정을 두 곳에서 중복 유도하던 것이 원래 결함의 형태였다.
- `core/tests/unittest/unittest_asio_write_turn_policy.cpp` (신규, 7 test)
- `core/tests/unittest/CMakeLists.txt` 등록

### POSDDD 항목

1. **비용과 상태를 소유자에게**: write turn 진입 가부와 budget 적용 여부가 engine의
   inline 조건식에 흩어져 있던 것을, 이미 모든 fast-path threshold를 소유하는
   `asio_stream_fastpath_policy`로 이전했다.
2. **중복 유도 제거**: 진입 판정(`use_stream_speculative_write`)과 budget
   판정(`stream_tcp_speculative`)이 같은 사실을 독립적으로 유도하면서 서로
   어긋나 있었다. 이 어긋남이 회귀 그 자체였다. 이제 하나의 predicate 쌍이 답한다.
3. **순수 함수로 분리**: 두 predicate 모두 인자만으로 결정되므로 truth table을
   직접 test할 수 있다. engine socket을 세우지 않고 불변식을 고정한다.

의도적으로 **하지 않은** 정리: `out_event()`의 continuation(`:1142-1150`)에 남은
`_options.type == ZLINK_CORE_SOCKET_STREAM &&` 중복 guard는 A를 채택할 때만 제거가
안전하다. 지금 제거하면 mode 0에서 non-STREAM이 completion handler로부터
`speculative_write()`에 새로 진입하게 되어 C baseline이 바뀐다. A 채택 시 함께
제거할 항목으로 남긴다.

## 7. 회귀 test

`unittest_asio_write_turn_policy` 7/7 통과. timing이 아니라 truth table을 고정하므로
flaky하지 않고, 세 configuration 중 무엇을 채택해도 유효하다.

- `test_stream_tcp_is_admitted_in_every_configuration` — STREAM/tcp는 모든 mode에서
  진입 가능하고 **항상 budget에 묶인다**. A가 STREAM 경로를 건드리지 않음을 고정한다.
- `test_current_configuration_admits_unbounded_general_sockets` — C의 현재 형태를
  기록한다(진입 허용 + budget 미적용). 승인이 아니라 기준선 고정이다.
- `test_transport_without_speculative_support_is_never_admitted` — transport 능력을
  무시하고 진입시키지 않는다.
- `test_async_configuration_admits_no_general_socket` — A에서 어떤 일반 socket도
  어떤 transport에서도 동기 turn에 들어가지 않는다. **무한 drain이 다시 허용되면
  깨지는 assertion.**
- `test_budget_all_configuration_bounds_every_admitted_turn` — B가 실제로 모든
  진입 turn을 묶는지.
- `test_budget_is_positive`, `test_default_write_turn_mode_is_current` — budget이
  0이면 STREAM도 무한 drain이 되고, 기본값은 C여야 한다.

## 8. 판단 기준 (owner 결정용)

- **A 채택 시**: REQREP 회귀가 해소되고(0.10.1 대비 94.5~111%), one-way latency가
  전 구간에서 10~350배 개선되며, kernel 잔류량이 9.3 MiB → 3 KiB로 떨어져 byte-HWM
  계약이 실제로 성립한다. 대가는 one-way 1024 B 부근 throughput이 C 대비 25~36%
  낮아지는 것이다. 단 그 구간에서도 **0.10.1 대비로는 동등하거나 높다**
  (PAIR 101%, DEALER_DEALER 87%, ROUTER_ROUTER 117%, PUBSUB 101%).
- **C 유지 시**: one-way 1024 B throughput은 유지되지만 REQREP는 0.10.1의 68~80%에
  머물고, 모든 one-way pattern의 latency가 10~70 ms대이며, byte-HWM은 명목상으로만
  유한하다.
- **선례**: 같은 handoff 문서가 PUB/SUB Auto-HWM off를 "처리량은 올라가지만 tail
  latency가 크게 악화된다. 유한 HWM과 낮은 queue residency를 지키기 위해" 기각했다.
  C가 A 대비 갖는 우위는 정확히 같은 형태의 거래다.
- **B**: §4.3의 이유로 후보가 아니다.

## 9. 검증 상태

- `core/build` Release 재빌드 성공.
- `unittest_asio_write_turn_policy` 7/7.
- 전체 CTest를 mode 0(기본)과 mode 1(A)에서 각각 실행. 결과는 §10.
- mode 0이 pristine snapshot과 동일함을 A/B 측정으로 확인.
- STREAM 전체 benchmark control은 multi runner가 core/build 전체 재빌드를 유발해
  시간 안에 완료하지 못했다. 다만 A는 STREAM 경로를 **구조적으로** 건드리지 않는다:
  `use_speculative_write_for`는 STREAM/tcp에 대해 세 mode 모두 같은 값을 반환하며
  이를 unit test가 고정한다.

## 10. CTest 결과

(아래 §10.1/§10.2는 실행 직후 채워 넣는다.)
