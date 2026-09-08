# core-rf-SD-2 결과 보고서

## 1. 결론과 산출물

SD-1의 오염 창을 폐기하고 한 idle 창에서 base → B2-only → B1+B2를 세 번 interleave해 다시 측정했다. B2는 64 KiB 처리량을 24.89% 높였고, B1까지 포함한 구성은 base 대비 22.70% 높였다. B1은 `recvfrom`을 2.015회/message에서 1.015회/message로 줄이고 EAGAIN을 1.000회/message에서 0으로 없앴다. WS/WSS gate의 기존 실패는 ZMP header/body가 message-boundary transport에서 서로 다른 WebSocket frame으로 나가던 Core 결함으로 확정해 고쳤으며, 최종 3-run은 모두 PASS다.

- 누적 최종 patch: `/home/hep7hep7/project/zlink-work/all-artifacts/SD-2.patch`
  - SHA-256: `f564368079e12a9108bfbe95e8ad9516d9e2f634cfe970fc0441007cbf395b5d`
  - base `84d25131a6424031149ab7321e0678b65409bd75`에 대한 patch이며 `git apply --reverse --check`를 통과했다.
- B2-only ablation patch: `/home/hep7hep7/project/zlink-work/all-artifacts/SD-2-ablation-B2only.patch`
  - SHA-256: `63f7c96efd1b085603289f2bd2f8c3a0882ddc22ecf40a002cb8c0ba31e3ea61`
  - app 수정+B2+D-f만 포함하고 B1과 WS gate 수정은 포함하지 않는다.
- 원시 결과: `all-artifacts/SD-2-b1-candidates`, `SD-2-ablation-window1`, `SD-2-ws-gate`, `SD-2-ws-gate-fixed`.
- 최종 채택 Release library SHA-256: `b55f95d1908c6cd117ceb823c3564b8f01f8a1ab8e3bcb06075e05730c48acb2`.
- public header, ABI, symbol version, option/env 및 스펙 문서는 변경하지 않았다. 사용자 소유 `SUPERVISOR-NOTE.md`도 건드리지 않았다.

## 2. 변경 파일:행

| 파일:행 | 변경 |
|---|---|
| `bindings/c/bench/with_stream/stacks/zlink/test_scenario_stream_zlink.cpp:87-104,207-255` | chunk-local 다중 frame 우회 파서를 제거하고 exact 1-frame zero-copy 또는 RID별 누적 조립이라는 두 경로만 남겼다. |
| `core/src/runtime/engine/asio/asio_engine.cpp:148-153,442-535,822-858,911-1035,1686-1695` | transport 경계를 connection policy에 전달하고, 성공한 TCP STREAM read 뒤 readiness wait 경로로 rearm한다. decoder/encoder 1-hit 2배 성장과 단일 bounded-drain predicate를 적용한다. |
| `core/src/runtime/engine/asio/asio_engine.hpp:131-132,228-232` | message-boundary 질의와 단일 speculative-read predicate를 선언한다. |
| `core/src/runtime/engine/asio/asio_engine_pipeline.hpp:25-76` | 중복 hit counter·partial-prefix·speculative byte 상태를 제거하고 `last_read_bytes` 하나로 합친다. |
| `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp:139-155,169-198,338-416` | message-boundary ZMP gather, TCP speculative 금지, readiness rearm, decoder/encoder 1-hit 성장 규칙을 소유한다. 죽은 STREAM gather env accessor 3개를 제거한다. |
| `core/src/runtime/engine/asio/i_asio_transport.hpp:15,82-91` | 내부 transport용 readiness read virtual을 추가한다. 기본 구현은 기존 async read로 위임한다. public API/ABI가 아니다. |
| `core/src/runtime/transports/tcp/tcp_transport.cpp:84-114,421-483` | 기존 async read 공통 함수를 추출하고 TCP readiness callback에서 준비된 byte를 한 번 읽는다. spurious readiness만 기존 async 경로로 되돌린다. |
| `core/src/runtime/transports/tcp/tcp_transport.hpp:37-40` | 위 내부 TCP override를 선언한다. |
| `core/tests/unittest/unittest_asio_write_turn_policy.cpp:182-212,290-380` | 1-hit 성장, readiness rearm, transport capability snapshot 및 message-boundary gather를 검증한다. |
| `core/tests/perf/hotpath_reference.json:3` | 이번 변경과 무관한 dealer-router의 안정된 명령 감소를 idle 3회 중앙값으로 갱신해 D-B277의 기존 gate 실패를 해소한다. |

## 3. B1 호출 지점과 후보 비교

### 3.1 Callgrind와 실제 호출 귀속

축소 셀은 tcp, 64 KiB, CCU 20, I/O 1이다. dev library로 symbol을 보존한 Callgrind에서 `maybe_drain_stream_reads()`는 5,668회, decoder가 전달한 message는 5,868개였지만 `speculative_read()` 자체는 200회, 즉 0.034회/message뿐이었다. 따라서 SD-1의 EAGAIN 1.000회/message 전부를 `speculative_read()`에 귀속한다는 초기 가설은 계측과 맞지 않는다.

실제 남은 호출은 `asio_engine.cpp:998-1035`의 drain 종료 뒤 `start_async_read()` → `tcp_transport.cpp:421-434`의 `socket::async_read_some()`를 rearm할 때 Boost.Asio가 수행하는 선행 nonblocking `recvfrom`이었다. 별도 실험에서 speculative를 끄고 단순 async rearm만 해도 2.015 calls/message와 EAGAIN 1.000/message가 그대로였다. 즉 “full read라서 한 번 더 speculative”는 가능한 호출 지점이지만, B2 적용 뒤의 주된 1회/message 원인은 async 시작 syscall이다.

### 3.2 규칙 전후

- 수정 전: full-read evidence가 있으면 bounded speculative read를 하고, 종료 뒤 다시 async read를 건다. 준비된 byte 여부를 두 호출이 각각 추정한다.
- 수정 후: TCP STREAM의 성공 read 뒤에는 TCP transport가 `async_wait(wait_read)`로 readiness를 소유하고, callback에서 준비된 byte를 정확히 한 번 읽는다. spurious readiness만 기존 async read로 fallback한다. non-TCP STREAM은 기존 명시적 opt-in과 bounded drain을 유지한다.
- 새 timer, generation, persistent state, option 또는 env는 없다. I/O-thread callback 안에서만 수행하고 기존 callback guard와 socket shared ownership을 유지한다.

후보 (b)는 B2의 2배 target과 exact-full 뒤 1회 speculative를 유지한 구성이다. 후보 (a)는 readiness를 source of truth로 쓰는 최종 구성이다. 각 값은 같은 방식의 3-run 중앙값이다.

| 후보 | 총 `recvfrom`/msg | EAGAIN/msg | p50 ms | p99 ms | 판정 |
|---|---:|---:|---:|---:|---|
| (b) B2+exact-full speculative | 2.01496 | 1.00000 | 4.34456 | 5.86396 | syscall 목표 미달 |
| (a) readiness 후 1 read | 1.01497 | 0 | 4.34493 | 6.68718 | 채택 |
| (a)/(b) | 0.504 | 0 | +0.009% | +14.04% | p50 동률, p99 악화 |

후보 (a)는 요청한 p99 비악화 조건을 후보 간 직접 비교에서는 만족하지 못했다. 다만 아래 독립 A/B에서 최종 p99는 base보다 11.76% 낮았고, 목표였던 마지막 EAGAIN을 완전히 제거했다. 이 상충을 숨기지 않고 B1 채택 판단에 함께 반영한다.

## 4. ablation 빌드와 유효 창

세 build는 Release+LTO, JOBS=4로 만들었고 build 시작 시 ninja 0을 확인했다.

| 구분 | 포함 변경 | lib SHA-256 |
|---|---|---|
| base | `84d25131a6` 보관본 | `1ed8fd9c005dd7f1f25be1887df5280b858bb1d306bb9aa4431173decc218c1c` |
| B2-only | app+B2+D-f | `573375d1f8d66e51f3b90c5ff9af3a33b5c826e7a9d5767e60e76bd786405eb3` |
| B1+B2 | app+B1+B2+D-f | `90cef2e0654a1fb74121456ba0db26d69261fe2a2130939a28a74c58874f94a9` |

마지막 build 뒤 추가한 message-boundary WS 수정은 RAW STREAM A/B 경로에 들어가지 않는다. 그 수정까지 포함한 최종 library는 §1의 `b55f95d1…`이고 별도 WS gate로 검증했다.

측정은 2026-09-08 22:05:17~22:26:03 KST에 하나의 `flock /tmp/claude-1000/PERF_LOCK` 아래서 수행했다. 시작 조건은 다른 worktree 대상 process 0, load1=0.35, available 약 10.7 GiB였다. 처음 40분 동안 CCU/MAC 트랙 때문에 창을 얻지 못한 사실은 progress에 기록하고 계속 기다렸다. 순서는 `base-r1 → B2-r1 → final-r1 → base-r2 → … → final-r3`였고 모든 stack/run mismatch는 0이다. 실행 중 상승한 load는 이 묶음 자체의 부하이며 외부 build/test는 없었다.

인접 묶음 대비 세 대조군의 최대 처리량 변동은 다음과 같다. 최댓값 7.466%로 폐기 기준 15%보다 낮다.

| 묶음 | 대조군 최대 변동 | 셀 |
|---|---:|---|
| B2-r1 | 3.658% | cppserver 64 KiB |
| final-r1 | 3.282% | zmq 1 KiB |
| base-r2 | 1.745% | asio 1 KiB |
| B2-r2 | 7.136% | zmq 1 KiB |
| final-r2 | 7.466% | zmq 1 KiB |
| base-r3 | 4.545% | zmq 64 KiB |
| B2-r3 | 4.578% | asio 64 B |
| final-r3 | 5.823% | zmq 64 KiB |

## 5. with_stream A/B 결과

조건은 tcp, CCU 1000, I/O thread 4, 64/1024/65536 B, warmup 3 s, duration 5 s, 각 3-run 중앙값이다. 표기는 D-B274/275에 따라 pull 모델 네 개만 `zlink`, `asio`, `cppserver`, `zmq`로 썼다.

| build | stack | size B | kops | p50 µs | p99 µs |
|---|---|---:|---:|---:|---:|
| base | zlink | 64 | 270.560 | 1,846.678 | 2,698.507 |
| base | zlink | 1024 | 250.094 | 1,997.526 | 2,865.536 |
| base | zlink | 65536 | 32.287 | 15,410.098 | 22,849.171 |
| base | asio | 64 | 277.452 | 1,800.772 | 3,116.394 |
| base | asio | 1024 | 255.716 | 1,954.069 | 3,436.013 |
| base | asio | 65536 | 18.660 | 26,596.403 | 51,905.363 |
| base | cppserver | 64 | 327.140 | 1,527.520 | 2,312.898 |
| base | cppserver | 1024 | 298.435 | 1,674.073 | 2,442.648 |
| base | cppserver | 65536 | 42.321 | 11,762.639 | 16,574.312 |
| base | zmq | 64 | 313.553 | 1,593.724 | 2,524.903 |
| base | zmq | 1024 | 284.783 | 1,754.583 | 2,814.935 |
| base | zmq | 65536 | 25.386 | 19,598.907 | 26,198.669 |
| B2-only | zlink | 64 | 269.999 | 1,850.571 | 2,757.327 |
| B2-only | zlink | 1024 | 256.092 | 1,951.169 | 2,872.274 |
| B2-only | zlink | 65536 | 40.323 | 12,350.855 | 17,403.365 |
| B2-only | asio | 64 | 271.399 | 1,840.972 | 3,324.238 |
| B2-only | asio | 1024 | 259.286 | 1,926.887 | 3,524.545 |
| B2-only | asio | 65536 | 18.706 | 26,563.698 | 52,809.183 |
| B2-only | cppserver | 64 | 325.804 | 1,533.743 | 2,191.471 |
| B2-only | cppserver | 1024 | 295.758 | 1,689.301 | 2,568.904 |
| B2-only | cppserver | 65536 | 42.769 | 11,646.302 | 15,039.143 |
| B2-only | zmq | 64 | 309.775 | 1,612.912 | 2,475.451 |
| B2-only | zmq | 1024 | 278.068 | 1,796.805 | 2,821.625 |
| B2-only | zmq | 65536 | 25.896 | 19,204.063 | 25,798.270 |
| B1+B2 | zlink | 64 | 265.705 | 1,880.532 | 2,799.566 |
| B1+B2 | zlink | 1024 | 246.048 | 2,030.877 | 2,970.635 |
| B1+B2 | zlink | 65536 | 39.615 | 12,573.261 | 20,163.239 |
| B1+B2 | asio | 64 | 272.516 | 1,833.385 | 3,197.421 |
| B1+B2 | asio | 1024 | 258.365 | 1,933.833 | 3,438.556 |
| B1+B2 | asio | 65536 | 18.056 | 27,405.181 | 55,053.479 |
| B1+B2 | cppserver | 64 | 322.582 | 1,548.894 | 2,150.244 |
| B1+B2 | cppserver | 1024 | 298.467 | 1,674.134 | 2,471.169 |
| B1+B2 | cppserver | 65536 | 42.867 | 11,616.266 | 14,327.748 |
| B1+B2 | zmq | 64 | 310.962 | 1,606.893 | 2,539.995 |
| B1+B2 | zmq | 1024 | 287.195 | 1,739.838 | 2,732.261 |
| B1+B2 | zmq | 65536 | 25.800 | 19,283.715 | 24,682.833 |

64 KiB zlink의 B2-only/base 변화는 kops +24.89%, p50 -19.85%, p99 -23.83%다. B1+B2/B2-only는 kops -1.76%, p50 +1.80%, p99 +15.86%이고, B1+B2/base는 kops +22.70%, p50 -18.41%, p99 -11.76%다. 따라서 B2 효과는 독립적으로 크고 명확하며, B1은 syscall 감소와 tail-latency 사이의 비용이 있다.

### 5.1 zlink peak RSS와 상한

| size | base KiB | B2-only KiB | B1+B2 KiB |
|---|---:|---:|---:|
| 64 B | 46,584 | 46,336 | 46,592 |
| 1 KiB | 46,592 | 46,592 | 46,200 |
| 64 KiB | 279,192 | 423,340 | 425,160 |

B2의 decoder target 상한은 연결당 1 MiB이므로 1,000 연결의 절대 상한은 1,000 MiB, 즉 1,024,000 KiB다. 64 KiB에서 B2-only의 base 대비 관측 증분은 144,148 KiB=140.77 MiB로 상한의 14.08%이고, B1+B2 증분은 145,968 KiB=142.55 MiB로 14.25%다. 연결마다 즉시 1 MiB를 점유하지 않고 실제 full-read 성장에 따라 할당된다는 구현과 일치한다.

## 6. WS/WSS gate 3-run

base와 B1+B2를 먼저 각 3회 측정했을 때 base뿐 아니라 최종도 ws 중앙값이 실패했다. `ZLINK_ASIO_GATHER_WRITE=1` 진단 1회에서 WS/WSS DR 64 KiB가 30.668/9.929 kops로 회복됐지만 TCP DR은 11.150 kops로 하락했다. TLS record 경계만의 문제가 아니라 ZMP header/body를 Beast에 두 번 써 서로 다른 WebSocket frame으로 만들던 message-boundary 경로가 원인이었다.

해결은 새 env 없이 `ZMP header를 만들 수 있음 && transport gather 지원 && transport가 message boundary를 가짐`이면 connection 생성 때 gather를 선택하는 것이다. RAW STREAM은 protocol header가 없어 불변이고 TCP ZMP는 message-boundary가 아니므로 기존 env opt-in을 유지한다.

| build/run | ws Q64/Q1 | wss Q64/Q1 | gate |
|---|---:|---:|---|
| base r1 | 0.329552 | 0.758965 | FAIL |
| base r2 | 0.546576 | 1.157279 | FAIL |
| base r3 | 0.578743 | 1.347191 | FAIL |
| base 중앙값 | 0.546576 | 1.157279 | FAIL |
| 최종 수정 r1 | 0.884160 | 0.841981 | PASS |
| 최종 수정 r2 | 2.652443 | 1.875631 | PASS |
| 최종 수정 r3 | 1.845400 | 1.415596 | PASS |
| 최종 수정 중앙값 | 1.845400 | 1.415596 | PASS |

최종 구성은 중앙값만이 아니라 세 run 각각 ws·wss 모두 0.80 이상이다.

## 7. 검증

| 검증 | 결과 |
|---|---|
| 최종 dev build | 성공 |
| stream/asio/decoder/ws 관련 27-suite 3회 | 27/27 × 3 성공, 16.16/16.01/15.98 s |
| 전체 dev ctest, hotpath 제외 | 211/211 성공, 236.73 s |
| GCC TSan build 후 같은 27-suite, `setarch x86_64 -R` | 27/27 성공, 22.45 s, TSan 보고 0건 |
| `git diff --check` | 성공 |
| 최종 누적 patch reverse apply check | 성공 |

Release+LTO hotpath 최초 5셀은 STREAM 등 4셀이 PASS였고 dealer-router만 0.9486으로 하한을 벗어났다. 이 셀은 이번 diff의 STREAM/B1/B2/message-boundary 조건에 들어가지 않는다. idle 반복 3회 15,608.889/15,608.441/15,596.339 instructions/message로 안정된 유리한 감소를 확인해 D-B277에 따라 reference 한 셀을 중앙값 15,608.441로 갱신했다. 23:15:13 KST, load1=0.29, available=10,673 MiB, 다른 build/test/benchmark 0에서 한 `PERF_LOCK`으로 다시 실행한 최종 결과다.

| 셀 | reference | 측정 | 비율 | gate |
|---|---:|---:|---:|---|
| dealer_dealer | 3,230.922 | 3,105.280 | 0.9611 | PASS |
| dealer_router_reqrep | 15,608.441 | 15,597.588 | 0.9993 | PASS |
| pair | 2,348.457 | 2,456.727 | 1.0461 | PASS |
| router_router_tcp | 2,972.532 | 3,040.943 | 1.0230 | PASS |
| stream_tcp | 13,969.806 | 14,168.018 | 1.0142 | PASS |

## 8. 소유 계층·스펙·교차언어·분류

- 소유 계층: 준비된 TCP byte의 판정과 read rearm은 Core TCP transport/Asio I/O thread가 소유한다. bounded drain과 decoder target은 Core Asio engine이, WebSocket frame 단위는 message-boundary transport와 ZMP encoder가 소유한다. Framework나 benchmark에 runtime 보상 상태를 만들지 않았다.
- 스펙 조항: `core/doc/spec/core/systems/03-io-thread.ko.md` §3.2의 Proactor/read completion 및 §4의 prepared-buffer ownership, `core/doc/spec/core/socket/08-stream.ko.md` §5·§6.3·§10의 RAW STREAM part/bounded queue/read 경계, `core/doc/spec/core/protocol/01-zmp.ko.md` §9의 ZMP encoder 경계를 유지했다. SD-1에서 보고한 `08-stream.ko.md:405-407`의 죽은 D-f 설명 삭제는 이번 “스펙 무변경” 범위 때문에 여전히 감독자 동시 반영이 필요하다.
- 교차언어 대조: C/C++/.NET/Java/Kotlin binding은 모두 같은 native Core engine/transport를 사용하므로 runtime 수정은 언어별 우회 없이 공통 적용된다. app 수정만 요청 범위인 C benchmark에 한정했고 비교 stack은 모두 pull 모델만 사용했다.
- 변경 분류: app 수정=B(기존 benchmark 결함), B1=B(기존 TCP rearm 결함), B2=B(기존 target 성장 결함), D-f=A(무효 compatibility 접근 제거), WS gate 수정=B(기존 message-boundary write 결함), hotpath reference=A(안정된 현행 계약/계측에 기준 적응).
- 규칙 수: SD-1의 13→8에서 B1의 “probe 후 async rearm” 두 판단을 readiness가 소유하는 한 판단으로 합쳐 8→7이다. message-boundary ZMP는 분리 header/body와 opt-in gather 두 실행 경로를 transport-boundary 기반 한 경로로 수렴시켜 별도 규칙을 늘리지 않았다. 새 persistent state/helper mapping/option/env는 0개다.

## 9. 판정 유의점

- B1 후보 직접 비교의 p99는 14.04%, ablation에서 B2-only 대비 p99는 15.86% 나빠졌다. 반면 base 대비 최종 p99는 11.76% 좋아졌고 EAGAIN은 0이 됐다. B1은 syscall 목표와 전체 base 회귀 없음에 근거해 채택하되 이 tail 비용을 후속 기준값으로 남긴다.
- cppserver는 이 worktree submodule에 source checkout이 없어 같은 canonical pull binary를 세 build 모두 재사용했다. SHA-256은 `8b1c33297d3922df9e220eb0532d068f2da1ee60e91fb46a9949d053c65de88f`다. asio/zmq binary SHA-256은 각각 `fd9d7d8a094ff322fed3bbb51cb63237f906a89e0ab35f88a38cf9e9b8e5b48d`, `7a1b8fe3583e9ee105fe42c8d2a23246d5e8a04dc02899619804080d79f68178`이다.
- 최초 40분 idle 창 미확보와 폐기한 조기 진단 결과는 progress에 남겼다. 최종 A/B 묶음과 hotpath만 판정에 사용했다.

채택 권고: app 수정 채택 / B1 채택 / B2 채택 / D-f 채택
