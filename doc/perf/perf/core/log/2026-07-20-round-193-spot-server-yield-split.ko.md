# Round 193: Spot server yield 원인 분리

## 목적

Round 170의 `strace`는 Spot REQREP에서 메시지당 `sched_yield` 약 5.23회를
관측했지만, 빈 ready 조회 뒤의 양보와 reply backpressure 재시도를 구분하지
못했다. 두 경계의 횟수를 임시 카운터로 분리했다.

## 결과

- 조건: tcp, 64바이트, 100 peer, active 3초, server/client I/O thread 각 1개
- 완료 처리량: 59,377.0 ops/s, 총 수신 178,131건
- reply backpressure 재시도 yield: 0회
- 빈 ready polling 뒤 idle yield: 23,211,721회
- 결과:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260720_091710_round193-yield-split-r2.txt`

현재 REQREP 실행에서는 reply가 `DONTWAIT` backpressure를 반환하지 않았다. 따라서
reply part를 복사하고 claim 수명을 연장하는 pending queue는 이 병목을 줄이지 못하며,
public reply-token 수명과 harness 복잡성만 늘린다.

idle yield는 매우 많지만, 같은 server를 blocking ready로 바꾼 이전 후보가 c100
처리량을 개선하지 못했다. 빈 `DONTWAIT` 조회용 Core atomic hint도 세 패턴의 공통
개선을 입증하지 못했다. 같은 후보를 반복하지 않고 다음 조사를 frame 구성·이동과
allocation 경계로 옮겼다.

임시 카운터는 측정 직후 제거했다. Core runtime, version과 package는 변경하지 않았다.
