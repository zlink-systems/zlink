가설 아니오. 같은 실제 처리율(중앙값 약 21.94k msg/s)에서 가장 바쁜 활성 Core I/O thread의 target CPU는 Framework 17.05 µs/msg, binding 7.42 µs/msg이고 source CPU는 Framework 17.95 µs/msg, binding 7.35 µs/msg입니다.

측정은 commit 39bbfe470d2131b0b741d3fd5404441ac8f5a199에서 수행했습니다. 모든 수치는 source와 target 프로세스의 모든 스레드별 schedstat 실행 시간을 기록했습니다. I/O thread 요약에는 활성 데이터 경로 worker를 별도로 표시했습니다.

## 빌드와 처리율

C++ Core와 binding 1.23.0 패키지 빌드 및 C++ gRPC bench Release 빌드가 성공했습니다. Release flags는 -O3 -DNDEBUG였고 -g 플래그는 없었습니다.
libzlink SHA-256은 6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01입니다.
비율 제한 없는 request-backpressure 1 KiB 결과는 binding 253.20k msg/s, Framework 53.28k msg/s였습니다. 이 두 rate는 서로 달라 직접 비교로 쓰지 않았습니다. 이 캡처의 busiest Core I/O worker는 binding target/source 3.48/2.63 µs/msg, Framework target/source 15.37/15.16 µs/msg였습니다.
동일 처리율 비교는 binding --target-rate 32000, Framework --target-rate 48000으로 pacing 상한을 보정해 수행했습니다. 캡처 구간 처리율 중앙값은 binding 21,928.4 msg/s, Framework 21,966.3 msg/s로 0.17% 차이였습니다. 각 구현에서 3회 실행했고 여섯 셀 모두 완료·수신 개수가 일치했습니다.
추가로 같은 --target-rate 10000 설정을 양쪽에 적용한 3회 측정도 수행했습니다. 실제 capture rate 중앙값은 binding 9,952.4 msg/s, Framework 9,990.0 msg/s였습니다.
Pacing은 bench client에만 추가한 --target-rate 인자로 제출 시점을 제한했습니다. warmup과 active 구간 모두 같은 pacing을 사용했습니다. 32k와 48k는 상한이며, 비교 값은 캡처 구간의 실제 완료율입니다.

동일 처리율 비교의 Core I/O 요약은 각 캡처에서 CPU 실행 시간이 0.05초를 넘은 IO/n worker를 활성 데이터 경로 I/O로 분류하고, 역할별 가장 바쁜 worker 및 활성 worker 합계를 계산했습니다. 모든 IO thread와 나머지 스레드의 원시 CPU 시간은 아래 per-thread 표에 포함했습니다. stats HTTP host가 별도 Core context를 만들기 때문에 같은 IO/n 이름이 두 번 나타날 수 있으며 두 항목을 모두 남겼습니다.

| 구현 | 처리율 중앙값 (msg/s) | target 가장 바쁜 I/O (µs/msg) | target 활성 I/O 합 (µs/msg) | source 가장 바쁜 I/O (µs/msg) | source 활성 I/O 합 (µs/msg) |
|---|---:|---:|---:|---:|---:|
| C++ binding | 21,928.4 | 7.42 | 14.33 | 7.35 | 12.57 |
| C++ Framework | 21,966.3 | 17.05 | 29.27 | 17.95 | 30.55 |

활성 Core I/O thread별 3회 중앙값입니다. CPU 시간은 초이며 `µs/msg`는 표의 같은 thread에서 측정한 중앙값입니다.

| 구현 | 프로세스 | comm | CPU 중앙값 (s) | µs/msg 중앙값 |
|---|---|---|---:|---:|
| binding | target | ZLINKbg/IO/0 | 1.233734 | 6.9143 |
| binding | target | ZLINKbg/IO/1 | 1.320742 | 7.4163 |
| binding | source | ZLINKbg/IO/0 | 1.305202 | 7.3454 |
| binding | source | ZLINKbg/IO/2 | 0.930611 | 5.2210 |
| Framework | target | ZLINKbg/IO/0 | 2.147429 | 12.0280 |
| Framework | target | ZLINKbg/IO/1 | 3.041793 | 17.0464 |
| Framework | source | ZLINKbg/IO/0 | 3.193319 | 17.9530 |
| Framework | source | ZLINKbg/IO/2 | 2.241496 | 12.6018 |

따라서 Framework의 busiest target I/O worker는 약 18.8 µs/msg에 가깝지만, equal-rate binding은 ≤4.0 µs/msg가 아니라 7.42 µs/msg였습니다. 같은 rate에서 차이는 약 2.30배였습니다. Source I/O도 Framework가 약 2.44배였습니다.

### 동일 처리율 1 KiB의 스레드별 결과

CPU s는 수집기 before/after의 /proc/<pid>/task/<tid>/schedstat 첫 필드 차이이며, µs/msg는 해당 CPU s를 같은 캡처의 source completed 차이로 나눈 값입니다. 표에는 target과 source 프로세스의 모든 스레드가 있으며 I/O worker를 먼저 표시했습니다.
각 캡처는 flock -o /root/jobs/fx/cx/measure.lock 안에서 순차 실행했습니다. 수집기는 source affinity 0–7, target affinity 8–15를 확인했고, 모든 capture 시작 시 MemAvailable 12 GiB 이상을 확인했습니다.

#### C++ binding run1, 21,907.5 msg/s

| process | TID | comm | 분류 | CPU (s) | µs/msg | wchan |
|---|---:|---|---|---:|---:|---|
| target | 1261426 | ZLINKbg/IO/1 | Core I/O | 1.250605 | 7.0391 | ep_poll |
| target | 1261425 | ZLINKbg/IO/0 | Core I/O | 1.203439 | 6.7736 | ep_poll |
| target | 1261411 | ZLINKbg/IO/2 | Core I/O | 0.000691 | 0.0039 | futex_wait_queue |
| target | 1261412 | ZLINKbg/IO/3 | Core I/O | 0.000669 | 0.0038 | futex_wait_queue |
| target | 1261410 | ZLINKbg/IO/1 | Core I/O | 0.000658 | 0.0037 | futex_wait_queue |
| target | 1261427 | ZLINKbg/IO/2 | Core I/O | 0.000582 | 0.0033 | futex_wait_queue |
| target | 1261409 | ZLINKbg/IO/0 | Core I/O | 0.000578 | 0.0033 | futex_wait_queue |
| target | 1261428 | ZLINKbg/IO/3 | Core I/O | 0.000495 | 0.0028 | futex_wait_queue |
| source | 1261476 | ZLINKbg/IO/0 | Core I/O | 1.305202 | 7.3464 | ep_poll |
| source | 1261478 | ZLINKbg/IO/2 | Core I/O | 0.927603 | 5.2210 | ep_poll |
| source | 1261458 | ZLINKbg/IO/1 | Core I/O | 0.000709 | 0.0040 | futex_wait_queue |
| source | 1261459 | ZLINKbg/IO/2 | Core I/O | 0.000643 | 0.0036 | futex_wait_queue |
| source | 1261456 | ZLINKbg/IO/0 | Core I/O | 0.000639 | 0.0036 | futex_wait_queue |
| source | 1261460 | ZLINKbg/IO/3 | Core I/O | 0.000633 | 0.0036 | futex_wait_queue |
| source | 1261479 | ZLINKbg/IO/3 | Core I/O | 0.000616 | 0.0035 | ep_poll |
| source | 1261477 | ZLINKbg/IO/1 | Core I/O | 0.000604 | 0.0034 | ep_poll |
| target | 1261407 | ZLINKbg/Reaper | Core background | 0.000565 | 0.0032 | futex_wait_queue |
| target | 1261423 | ZLINKbg/Reaper | Core background | 0.000545 | 0.0031 | futex_wait_queue |
| target | 1261408 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261424 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261454 | ZLINKbg/Reaper | Core background | 0.000682 | 0.0038 | futex_wait_queue |
| source | 1261474 | ZLINKbg/Reaper | Core background | 0.000574 | 0.0032 | futex_wait_queue |
| source | 1261455 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261475 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261480 | zlink-req-time | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261429 | bench_cpp_zlink | application/runtime thread | 1.048103 | 5.8993 | futex_wait_queue |
| target | 1261398 | bench_cpp_zlink | application/runtime thread | 0.034619 | 0.1949 | hrtimer_nanosleep |
| target | 1261420 | bench_cpp_zlink | application/runtime thread | 0.011555 | 0.0650 | futex_wait_queue |
| target | 1261387 | bench_cpp_zlink | application/runtime thread | 0.000457 | 0.0026 | hrtimer_nanosleep |
| target | 1261433 | bench_cpp_zlink | application/runtime thread | 0.000148 | 0.0008 | futex_wait_queue |
| target | 1261430 | bench_cpp_zlink | application/runtime thread | 0.000129 | 0.0007 | futex_wait_queue |
| target | 1261421 | bench_cpp_zlink | application/runtime thread | 0.000055 | 0.0003 | ep_poll |
| target | 1261390 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261391 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261392 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261393 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261394 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261395 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261396 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261397 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261399 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261400 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261401 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261402 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261403 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261404 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261405 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261406 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261413 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261414 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261415 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261416 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261417 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261418 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1261419 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261653 | bench_cpp_clien | application/runtime thread | 1.049384 | 5.9065 | hrtimer_nanosleep |
| source | 1261445 | bench_cpp_clien | application/runtime thread | 0.035283 | 0.1986 | hrtimer_nanosleep |
| source | 1261434 | bench_cpp_clien | application/runtime thread | 0.000534 | 0.0030 | hrtimer_nanosleep |
| source | 1261472 | bench_cpp_clien | application/runtime thread | 0.000176 | 0.0010 | futex_wait_queue |
| source | 1261470 | bench_cpp_clien | application/runtime thread | 0.000066 | 0.0004 | ep_poll |
| source | 1261461 | bench_cpp_clien | application/runtime thread | 0.000061 | 0.0003 | futex_wait_queue |
| source | 1261437 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261438 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261439 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261440 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261441 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261442 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261443 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261444 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261446 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261447 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261448 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261449 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261450 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261451 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261452 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261453 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261462 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261463 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261464 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261465 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261466 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261467 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261468 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1261469 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | ep_poll |
| source | 1261471 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |

#### C++ binding run2, 21,930.2 msg/s

| process | TID | comm | 분류 | CPU (s) | µs/msg | wchan |
|---|---:|---|---|---:|---:|---|
| target | 1262071 | ZLINKbg/IO/1 | Core I/O | 1.320742 | 7.4286 | ep_poll |
| target | 1262070 | ZLINKbg/IO/0 | Core I/O | 1.254606 | 7.0566 | ep_poll |
| target | 1262055 | ZLINKbg/IO/1 | Core I/O | 0.000621 | 0.0035 | futex_wait_queue |
| target | 1262073 | ZLINKbg/IO/3 | Core I/O | 0.000576 | 0.0032 | futex_wait_queue |
| target | 1262054 | ZLINKbg/IO/0 | Core I/O | 0.000557 | 0.0031 | futex_wait_queue |
| target | 1262057 | ZLINKbg/IO/3 | Core I/O | 0.000521 | 0.0029 | futex_wait_queue |
| target | 1262056 | ZLINKbg/IO/2 | Core I/O | 0.000521 | 0.0029 | futex_wait_queue |
| target | 1262072 | ZLINKbg/IO/2 | Core I/O | 0.000509 | 0.0029 | futex_wait_queue |
| source | 1262121 | ZLINKbg/IO/0 | Core I/O | 1.274394 | 7.1679 | ep_poll |
| source | 1262123 | ZLINKbg/IO/2 | Core I/O | 1.096913 | 6.1697 | ep_poll |
| source | 1262103 | ZLINKbg/IO/1 | Core I/O | 0.000599 | 0.0034 | futex_wait_queue |
| source | 1262102 | ZLINKbg/IO/0 | Core I/O | 0.000574 | 0.0032 | futex_wait_queue |
| source | 1262122 | ZLINKbg/IO/1 | Core I/O | 0.000570 | 0.0032 | ep_poll |
| source | 1262124 | ZLINKbg/IO/3 | Core I/O | 0.000522 | 0.0029 | ep_poll |
| source | 1262105 | ZLINKbg/IO/3 | Core I/O | 0.000481 | 0.0027 | futex_wait_queue |
| source | 1262104 | ZLINKbg/IO/2 | Core I/O | 0.000461 | 0.0026 | futex_wait_queue |
| target | 1262052 | ZLINKbg/Reaper | Core background | 0.000574 | 0.0032 | futex_wait_queue |
| target | 1262068 | ZLINKbg/Reaper | Core background | 0.000509 | 0.0029 | futex_wait_queue |
| target | 1262053 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262069 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262099 | ZLINKbg/Reaper | Core background | 0.000565 | 0.0032 | futex_wait_queue |
| source | 1262119 | ZLINKbg/Reaper | Core background | 0.000528 | 0.0030 | futex_wait_queue |
| source | 1262101 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262120 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262125 | zlink-req-time | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262074 | bench_cpp_zlink | application/runtime thread | 1.053916 | 5.9278 | futex_wait_queue |
| target | 1262043 | bench_cpp_zlink | application/runtime thread | 0.035187 | 0.1979 | hrtimer_nanosleep |
| target | 1262065 | bench_cpp_zlink | application/runtime thread | 0.011050 | 0.0622 | futex_wait_queue |
| target | 1262032 | bench_cpp_zlink | application/runtime thread | 0.000422 | 0.0024 | hrtimer_nanosleep |
| target | 1262078 | bench_cpp_zlink | application/runtime thread | 0.000167 | 0.0009 | futex_wait_queue |
| target | 1262075 | bench_cpp_zlink | application/runtime thread | 0.000125 | 0.0007 | futex_wait_queue |
| target | 1262066 | bench_cpp_zlink | application/runtime thread | 0.000052 | 0.0003 | ep_poll |
| target | 1262035 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262036 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262037 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262038 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262039 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262040 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262041 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262042 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262044 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262045 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262046 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262047 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262048 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262049 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262050 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262051 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262058 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262059 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262060 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262061 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262062 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262063 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262064 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262299 | bench_cpp_clien | application/runtime thread | 1.039043 | 5.8442 | hrtimer_nanosleep |
| source | 1262090 | bench_cpp_clien | application/runtime thread | 0.034900 | 0.1963 | hrtimer_nanosleep |
| source | 1262079 | bench_cpp_clien | application/runtime thread | 0.000449 | 0.0025 | hrtimer_nanosleep |
| source | 1262117 | bench_cpp_clien | application/runtime thread | 0.000143 | 0.0008 | futex_wait_queue |
| source | 1262106 | bench_cpp_clien | application/runtime thread | 0.000060 | 0.0003 | futex_wait_queue |
| source | 1262115 | bench_cpp_clien | application/runtime thread | 0.000048 | 0.0003 | ep_poll |
| source | 1262082 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262083 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262084 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262085 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262086 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262087 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262088 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262089 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262091 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262092 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262093 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262094 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262095 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262096 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262097 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262098 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262107 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262108 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262109 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262110 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262111 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262112 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262113 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262114 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | ep_poll |
| source | 1262116 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |

#### C++ binding run3, 21,928.4 msg/s

| process | TID | comm | 분류 | CPU (s) | µs/msg | wchan |
|---|---:|---|---|---:|---:|---|
| target | 1262715 | ZLINKbg/IO/1 | Core I/O | 1.323313 | 7.4163 | ep_poll |
| target | 1262714 | ZLINKbg/IO/0 | Core I/O | 1.233734 | 6.9143 | 0 |
| target | 1262699 | ZLINKbg/IO/1 | Core I/O | 0.000623 | 0.0035 | futex_wait_queue |
| target | 1262701 | ZLINKbg/IO/3 | Core I/O | 0.000616 | 0.0035 | futex_wait_queue |
| target | 1262698 | ZLINKbg/IO/0 | Core I/O | 0.000570 | 0.0032 | futex_wait_queue |
| target | 1262700 | ZLINKbg/IO/2 | Core I/O | 0.000555 | 0.0031 | futex_wait_queue |
| target | 1262717 | ZLINKbg/IO/3 | Core I/O | 0.000508 | 0.0028 | futex_wait_queue |
| target | 1262716 | ZLINKbg/IO/2 | Core I/O | 0.000470 | 0.0026 | futex_wait_queue |
| source | 1262765 | ZLINKbg/IO/0 | Core I/O | 1.310654 | 7.3454 | ep_poll |
| source | 1262767 | ZLINKbg/IO/2 | Core I/O | 0.930611 | 5.2155 | ep_poll |
| source | 1262766 | ZLINKbg/IO/1 | Core I/O | 0.000647 | 0.0036 | ep_poll |
| source | 1262748 | ZLINKbg/IO/3 | Core I/O | 0.000600 | 0.0034 | futex_wait_queue |
| source | 1262747 | ZLINKbg/IO/2 | Core I/O | 0.000584 | 0.0033 | futex_wait_queue |
| source | 1262768 | ZLINKbg/IO/3 | Core I/O | 0.000569 | 0.0032 | ep_poll |
| source | 1262745 | ZLINKbg/IO/0 | Core I/O | 0.000548 | 0.0031 | futex_wait_queue |
| source | 1262746 | ZLINKbg/IO/1 | Core I/O | 0.000504 | 0.0028 | futex_wait_queue |
| target | 1262696 | ZLINKbg/Reaper | Core background | 0.000660 | 0.0037 | futex_wait_queue |
| target | 1262712 | ZLINKbg/Reaper | Core background | 0.000503 | 0.0028 | futex_wait_queue |
| target | 1262697 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262713 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262763 | ZLINKbg/Reaper | Core background | 0.000682 | 0.0038 | futex_wait_queue |
| source | 1262743 | ZLINKbg/Reaper | Core background | 0.000658 | 0.0037 | futex_wait_queue |
| source | 1262744 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262764 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262769 | zlink-req-time | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262718 | bench_cpp_zlink | application/runtime thread | 0.992477 | 5.5622 | futex_wait_queue |
| target | 1262687 | bench_cpp_zlink | application/runtime thread | 0.035664 | 0.1999 | hrtimer_nanosleep |
| target | 1262709 | bench_cpp_zlink | application/runtime thread | 0.010938 | 0.0613 | futex_wait_queue |
| target | 1262676 | bench_cpp_zlink | application/runtime thread | 0.000418 | 0.0023 | hrtimer_nanosleep |
| target | 1262722 | bench_cpp_zlink | application/runtime thread | 0.000166 | 0.0009 | futex_wait_queue |
| target | 1262719 | bench_cpp_zlink | application/runtime thread | 0.000138 | 0.0008 | futex_wait_queue |
| target | 1262710 | bench_cpp_zlink | application/runtime thread | 0.000064 | 0.0004 | ep_poll |
| target | 1262679 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262680 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262681 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262682 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262683 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262684 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262685 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262686 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262688 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262689 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262690 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262691 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262692 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262693 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262694 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262695 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262702 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262703 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262704 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262705 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262706 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262707 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1262708 | bench_cpp_zlink | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262943 | bench_cpp_clien | application/runtime thread | 1.046918 | 5.8673 | hrtimer_nanosleep |
| source | 1262734 | bench_cpp_clien | application/runtime thread | 0.034890 | 0.1955 | hrtimer_nanosleep |
| source | 1262723 | bench_cpp_clien | application/runtime thread | 0.000506 | 0.0028 | hrtimer_nanosleep |
| source | 1262761 | bench_cpp_clien | application/runtime thread | 0.000153 | 0.0009 | futex_wait_queue |
| source | 1262749 | bench_cpp_clien | application/runtime thread | 0.000065 | 0.0004 | futex_wait_queue |
| source | 1262759 | bench_cpp_clien | application/runtime thread | 0.000062 | 0.0003 | ep_poll |
| source | 1262726 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262727 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262728 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262729 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262730 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262731 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262732 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262733 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262735 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262736 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262737 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262738 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262739 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262740 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262741 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262742 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262750 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262751 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262752 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262754 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262755 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262756 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262757 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1262758 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | ep_poll |
| source | 1262760 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |

#### C++ Framework run1, 21,980.9 msg/s

| process | TID | comm | 분류 | CPU (s) | µs/msg | wchan |
|---|---:|---|---|---:|---:|---|
| target | 1263668 | ZLINKbg/IO/1 | Core I/O | 3.041793 | 17.0464 | ep_poll |
| target | 1263667 | ZLINKbg/IO/0 | Core I/O | 2.228988 | 12.4914 | ep_poll |
| target | 1263670 | ZLINKbg/IO/3 | Core I/O | 0.000896 | 0.0050 | futex_wait_queue |
| target | 1263669 | ZLINKbg/IO/2 | Core I/O | 0.000817 | 0.0046 | futex_wait_queue |
| source | 1263732 | ZLINKbg/IO/0 | Core I/O | 3.217346 | 18.0302 | ep_poll |
| source | 1263734 | ZLINKbg/IO/2 | Core I/O | 2.259181 | 12.6606 | 0 |
| source | 1263733 | ZLINKbg/IO/1 | Core I/O | 0.000811 | 0.0045 | ep_poll |
| source | 1263712 | ZLINKbg/IO/1 | Core I/O | 0.000799 | 0.0045 | futex_wait_queue |
| source | 1263714 | ZLINKbg/IO/3 | Core I/O | 0.000795 | 0.0045 | futex_wait_queue |
| source | 1263711 | ZLINKbg/IO/0 | Core I/O | 0.000792 | 0.0044 | futex_wait_queue |
| source | 1263713 | ZLINKbg/IO/2 | Core I/O | 0.000763 | 0.0043 | futex_wait_queue |
| source | 1263735 | ZLINKbg/IO/3 | Core I/O | 0.000763 | 0.0043 | ep_poll |
| target | 1263665 | ZLINKbg/Reaper | Core background | 0.000865 | 0.0048 | futex_wait_queue |
| target | 1263666 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263683 | zlink-timer | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1263709 | ZLINKbg/Reaper | Core background | 0.000802 | 0.0045 | futex_wait_queue |
| source | 1263730 | ZLINKbg/Reaper | Core background | 0.000797 | 0.0045 | futex_wait_queue |
| source | 1263710 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1263731 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1263739 | zlink-timer | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1263742 | zlink-req-time | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263671 | bench_cpp_frame | application/runtime thread | 3.005882 | 16.8452 | 0 |
| target | 1263783 | bench_cpp_frame | application/runtime thread | 2.916725 | 16.3455 | futex_wait_queue |
| target | 1264029 | bench_cpp_frame | application/runtime thread | 0.910126 | 5.1004 | 0 |
| target | 1263656 | bench_cpp_frame | application/runtime thread | 0.049168 | 0.2755 | hrtimer_nanosleep |
| target | 1263673 | bench_cpp_frame | application/runtime thread | 0.015908 | 0.0891 | futex_wait_queue |
| target | 1263684 | bench_cpp_frame | application/runtime thread | 0.003628 | 0.0203 | futex_wait_queue |
| target | 1263645 | bench_cpp_frame | application/runtime thread | 0.000665 | 0.0037 | hrtimer_nanosleep |
| target | 1263688 | bench_cpp_frame | application/runtime thread | 0.000201 | 0.0011 | futex_wait_queue |
| target | 1263657 | bench_cpp_frame | application/runtime thread | 0.000072 | 0.0004 | futex_wait_queue |
| target | 1263685 | bench_cpp_frame | application/runtime thread | 0.000058 | 0.0003 | ep_poll |
| target | 1263648 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263649 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263650 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263651 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263652 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263653 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263654 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263655 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263658 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263659 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263660 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263661 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263662 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263663 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263664 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263674 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263675 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263676 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263677 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263678 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263679 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1263680 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1263737 | bench_cpp_clien | application/runtime thread | 3.503637 | 19.6346 | do_poll.constprop.0 |
| source | 1263906 | bench_cpp_clien | application/runtime thread | 3.119414 | 17.4814 | hrtimer_nanosleep |
| source | 1263703 | bench_cpp_clien | application/runtime thread | 0.321509 | 1.8018 | futex_wait_queue |
| source | 1263707 | bench_cpp_clien | application/runtime thread | 0.318417 | 1.7844 | futex_wait_queue |
| source | 1263702 | bench_cpp_clien | application/runtime thread | 0.317885 | 1.7814 | futex_wait_queue |
| source | 1263708 | bench_cpp_clien | application/runtime thread | 0.317186 | 1.7775 | futex_wait_queue |
| source | 1263706 | bench_cpp_clien | application/runtime thread | 0.316080 | 1.7713 | 0 |
| source | 1263704 | bench_cpp_clien | application/runtime thread | 0.315633 | 1.7688 | 0 |
| source | 1263705 | bench_cpp_clien | application/runtime thread | 0.314933 | 1.7649 | 0 |
| source | 1263701 | bench_cpp_clien | application/runtime thread | 0.314001 | 1.7597 | futex_wait_queue |
| source | 1263700 | bench_cpp_clien | application/runtime thread | 0.045614 | 0.2556 | hrtimer_nanosleep |
| source | 1263729 | bench_cpp_clien | application/runtime thread | 0.045479 | 0.2549 | hrtimer_nanosleep |
| source | 1263694 | bench_cpp_clien | application/runtime thread | 0.003525 | 0.0198 | futex_wait_queue |
| source | 1263740 | bench_cpp_clien | application/runtime thread | 0.003185 | 0.0178 | futex_wait_queue |
| source | 1263696 | bench_cpp_clien | application/runtime thread | 0.002560 | 0.0143 | futex_wait_queue |
| source | 1263699 | bench_cpp_clien | application/runtime thread | 0.002519 | 0.0141 | futex_wait_queue |
| source | 1263692 | bench_cpp_clien | application/runtime thread | 0.002519 | 0.0141 | futex_wait_queue |
| source | 1263693 | bench_cpp_clien | application/runtime thread | 0.002477 | 0.0139 | futex_wait_queue |
| source | 1263695 | bench_cpp_clien | application/runtime thread | 0.002465 | 0.0138 | futex_wait_queue |
| source | 1263698 | bench_cpp_clien | application/runtime thread | 0.002411 | 0.0135 | futex_wait_queue |
| source | 1263697 | bench_cpp_clien | application/runtime thread | 0.002324 | 0.0130 | futex_wait_queue |
| source | 1263736 | bench_cpp_clien | application/runtime thread | 0.000699 | 0.0039 | futex_wait_queue |
| source | 1263741 | bench_cpp_clien | application/runtime thread | 0.000675 | 0.0038 | futex_wait_queue |
| source | 1263689 | bench_cpp_clien | application/runtime thread | 0.000616 | 0.0035 | hrtimer_nanosleep |
| source | 1263727 | bench_cpp_clien | application/runtime thread | 0.000181 | 0.0010 | futex_wait_queue |
| source | 1263719 | bench_cpp_clien | application/runtime thread | 0.000066 | 0.0004 | futex_wait_queue |
| source | 1263725 | bench_cpp_clien | application/runtime thread | 0.000063 | 0.0004 | ep_poll |
| source | 1263715 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1263716 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1263717 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1263718 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1263720 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1263721 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1263722 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1263723 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | ep_poll |
| source | 1263726 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |

#### C++ Framework run2, 21,945.6 msg/s

| process | TID | comm | 분류 | CPU (s) | µs/msg | wchan |
|---|---:|---|---|---:|---:|---|
| target | 1264347 | ZLINKbg/IO/1 | Core I/O | 2.932869 | 16.4273 | ep_poll |
| target | 1264346 | ZLINKbg/IO/0 | Core I/O | 2.147429 | 12.0280 | ep_poll |
| target | 1264349 | ZLINKbg/IO/3 | Core I/O | 0.000852 | 0.0048 | futex_wait_queue |
| target | 1264348 | ZLINKbg/IO/2 | Core I/O | 0.000821 | 0.0046 | futex_wait_queue |
| source | 1264412 | ZLINKbg/IO/0 | Core I/O | 3.100851 | 17.3682 | 0 |
| source | 1264414 | ZLINKbg/IO/2 | Core I/O | 2.197484 | 12.3084 | ep_poll |
| source | 1264391 | ZLINKbg/IO/0 | Core I/O | 0.000800 | 0.0045 | futex_wait_queue |
| source | 1264394 | ZLINKbg/IO/3 | Core I/O | 0.000776 | 0.0043 | futex_wait_queue |
| source | 1264392 | ZLINKbg/IO/1 | Core I/O | 0.000763 | 0.0043 | futex_wait_queue |
| source | 1264415 | ZLINKbg/IO/3 | Core I/O | 0.000708 | 0.0040 | ep_poll |
| source | 1264393 | ZLINKbg/IO/2 | Core I/O | 0.000704 | 0.0039 | futex_wait_queue |
| source | 1264413 | ZLINKbg/IO/1 | Core I/O | 0.000683 | 0.0038 | ep_poll |
| target | 1264344 | ZLINKbg/Reaper | Core background | 0.000834 | 0.0047 | futex_wait_queue |
| target | 1264345 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264362 | zlink-timer | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1264389 | ZLINKbg/Reaper | Core background | 0.000787 | 0.0044 | futex_wait_queue |
| source | 1264410 | ZLINKbg/Reaper | Core background | 0.000734 | 0.0041 | futex_wait_queue |
| source | 1264390 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1264411 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1264419 | zlink-timer | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1264422 | zlink-req-time | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264351 | bench_cpp_frame | application/runtime thread | 2.884836 | 16.1583 | 0 |
| target | 1264350 | bench_cpp_frame | application/runtime thread | 2.325446 | 13.0251 | 0 |
| target | 1264908 | bench_cpp_frame | application/runtime thread | 1.616043 | 9.0516 | futex_wait_queue |
| target | 1264335 | bench_cpp_frame | application/runtime thread | 0.047211 | 0.2644 | hrtimer_nanosleep |
| target | 1264352 | bench_cpp_frame | application/runtime thread | 0.017515 | 0.0981 | futex_wait_queue |
| target | 1264363 | bench_cpp_frame | application/runtime thread | 0.003316 | 0.0186 | futex_wait_queue |
| target | 1264324 | bench_cpp_frame | application/runtime thread | 0.000631 | 0.0035 | hrtimer_nanosleep |
| target | 1264368 | bench_cpp_frame | application/runtime thread | 0.000249 | 0.0014 | futex_wait_queue |
| target | 1264364 | bench_cpp_frame | application/runtime thread | 0.000084 | 0.0005 | ep_poll |
| target | 1264327 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264328 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264329 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264330 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264331 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264332 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264333 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264334 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264336 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264337 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264338 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264339 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264340 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264341 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264342 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264343 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264353 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264354 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264355 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264356 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264357 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264358 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1264359 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1264417 | bench_cpp_clien | application/runtime thread | 3.373079 | 18.8930 | do_poll.constprop.0 |
| source | 1264809 | bench_cpp_clien | application/runtime thread | 3.103710 | 17.3842 | 0 |
| source | 1264386 | bench_cpp_clien | application/runtime thread | 0.308453 | 1.7277 | futex_wait_queue |
| source | 1264383 | bench_cpp_clien | application/runtime thread | 0.307711 | 1.7235 | futex_wait_queue |
| source | 1264384 | bench_cpp_clien | application/runtime thread | 0.307065 | 1.7199 | futex_wait_queue |
| source | 1264382 | bench_cpp_clien | application/runtime thread | 0.305868 | 1.7132 | futex_wait_queue |
| source | 1264385 | bench_cpp_clien | application/runtime thread | 0.305762 | 1.7126 | futex_wait_queue |
| source | 1264388 | bench_cpp_clien | application/runtime thread | 0.305179 | 1.7093 | futex_wait_queue |
| source | 1264387 | bench_cpp_clien | application/runtime thread | 0.303846 | 1.7019 | futex_wait_queue |
| source | 1264381 | bench_cpp_clien | application/runtime thread | 0.302842 | 1.6962 | futex_wait_queue |
| source | 1264409 | bench_cpp_clien | application/runtime thread | 0.044543 | 0.2495 | hrtimer_nanosleep |
| source | 1264380 | bench_cpp_clien | application/runtime thread | 0.044079 | 0.2469 | hrtimer_nanosleep |
| source | 1264379 | bench_cpp_clien | application/runtime thread | 0.003323 | 0.0186 | futex_wait_queue |
| source | 1264373 | bench_cpp_clien | application/runtime thread | 0.003272 | 0.0183 | futex_wait_queue |
| source | 1264374 | bench_cpp_clien | application/runtime thread | 0.003143 | 0.0176 | futex_wait_queue |
| source | 1264372 | bench_cpp_clien | application/runtime thread | 0.003037 | 0.0170 | futex_wait_queue |
| source | 1264420 | bench_cpp_clien | application/runtime thread | 0.002919 | 0.0163 | futex_wait_queue |
| source | 1264376 | bench_cpp_clien | application/runtime thread | 0.002502 | 0.0140 | futex_wait_queue |
| source | 1264377 | bench_cpp_clien | application/runtime thread | 0.002406 | 0.0135 | futex_wait_queue |
| source | 1264375 | bench_cpp_clien | application/runtime thread | 0.002320 | 0.0130 | futex_wait_queue |
| source | 1264378 | bench_cpp_clien | application/runtime thread | 0.002297 | 0.0129 | futex_wait_queue |
| source | 1264416 | bench_cpp_clien | application/runtime thread | 0.000720 | 0.0040 | futex_wait_queue |
| source | 1264421 | bench_cpp_clien | application/runtime thread | 0.000686 | 0.0038 | futex_wait_queue |
| source | 1264369 | bench_cpp_clien | application/runtime thread | 0.000588 | 0.0033 | hrtimer_nanosleep |
| source | 1264407 | bench_cpp_clien | application/runtime thread | 0.000229 | 0.0013 | futex_wait_queue |
| source | 1264400 | bench_cpp_clien | application/runtime thread | 0.000067 | 0.0004 | futex_wait_queue |
| source | 1264405 | bench_cpp_clien | application/runtime thread | 0.000060 | 0.0003 | ep_poll |
| source | 1264396 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1264397 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1264398 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1264399 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1264401 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1264402 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1264403 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1264404 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | ep_poll |
| source | 1264406 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |

#### C++ Framework run3, 21,966.3 msg/s

| process | TID | comm | 분류 | CPU (s) | µs/msg | wchan |
|---|---:|---|---|---:|---:|---|
| target | 1265225 | ZLINKbg/IO/1 | Core I/O | 3.069821 | 17.2587 | ep_poll |
| target | 1265224 | ZLINKbg/IO/0 | Core I/O | 2.136855 | 12.0135 | ep_poll |
| target | 1265226 | ZLINKbg/IO/2 | Core I/O | 0.000840 | 0.0047 | futex_wait_queue |
| target | 1265227 | ZLINKbg/IO/3 | Core I/O | 0.000807 | 0.0045 | futex_wait_queue |
| source | 1265289 | ZLINKbg/IO/0 | Core I/O | 3.193319 | 17.9530 | ep_poll |
| source | 1265291 | ZLINKbg/IO/2 | Core I/O | 2.241496 | 12.6018 | 0 |
| source | 1265271 | ZLINKbg/IO/2 | Core I/O | 0.000809 | 0.0046 | futex_wait_queue |
| source | 1265270 | ZLINKbg/IO/1 | Core I/O | 0.000806 | 0.0045 | futex_wait_queue |
| source | 1265269 | ZLINKbg/IO/0 | Core I/O | 0.000793 | 0.0045 | futex_wait_queue |
| source | 1265292 | ZLINKbg/IO/3 | Core I/O | 0.000775 | 0.0044 | ep_poll |
| source | 1265290 | ZLINKbg/IO/1 | Core I/O | 0.000765 | 0.0043 | ep_poll |
| source | 1265272 | ZLINKbg/IO/3 | Core I/O | 0.000760 | 0.0043 | futex_wait_queue |
| target | 1265222 | ZLINKbg/Reaper | Core background | 0.000788 | 0.0044 | futex_wait_queue |
| target | 1265223 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265240 | zlink-timer | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1265267 | ZLINKbg/Reaper | Core background | 0.000827 | 0.0046 | futex_wait_queue |
| source | 1265287 | ZLINKbg/Reaper | Core background | 0.000741 | 0.0042 | futex_wait_queue |
| source | 1265268 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1265288 | ZLINKbg/core-ct | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1265296 | zlink-timer | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1265299 | zlink-req-time | Core background | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265229 | bench_cpp_frame | application/runtime thread | 3.121649 | 17.5501 | 0 |
| target | 1265228 | bench_cpp_frame | application/runtime thread | 2.067206 | 11.6219 | 0 |
| target | 1265538 | bench_cpp_frame | application/runtime thread | 1.985869 | 11.1647 | futex_wait_queue |
| target | 1265213 | bench_cpp_frame | application/runtime thread | 0.049698 | 0.2794 | hrtimer_nanosleep |
| target | 1265230 | bench_cpp_frame | application/runtime thread | 0.017025 | 0.0957 | futex_wait_queue |
| target | 1265241 | bench_cpp_frame | application/runtime thread | 0.003650 | 0.0205 | futex_wait_queue |
| target | 1265202 | bench_cpp_frame | application/runtime thread | 0.000627 | 0.0035 | hrtimer_nanosleep |
| target | 1265245 | bench_cpp_frame | application/runtime thread | 0.000232 | 0.0013 | futex_wait_queue |
| target | 1265242 | bench_cpp_frame | application/runtime thread | 0.000068 | 0.0004 | ep_poll |
| target | 1265214 | bench_cpp_frame | application/runtime thread | 0.000062 | 0.0003 | futex_wait_queue |
| target | 1265205 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265206 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265207 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265208 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265209 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265210 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265211 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265212 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265215 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265216 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265217 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265218 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265219 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265220 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265221 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265231 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265232 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265233 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265234 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265235 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265236 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| target | 1265237 | bench_cpp_frame | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1265294 | bench_cpp_clien | application/runtime thread | 3.514158 | 19.7568 | 0 |
| source | 1265449 | bench_cpp_clien | application/runtime thread | 3.105261 | 17.4579 | 0 |
| source | 1265262 | bench_cpp_clien | application/runtime thread | 0.318892 | 1.7928 | futex_wait_queue |
| source | 1265260 | bench_cpp_clien | application/runtime thread | 0.315953 | 1.7763 | futex_wait_queue |
| source | 1265264 | bench_cpp_clien | application/runtime thread | 0.314064 | 1.7657 | futex_wait_queue |
| source | 1265265 | bench_cpp_clien | application/runtime thread | 0.313836 | 1.7644 | futex_wait_queue |
| source | 1265263 | bench_cpp_clien | application/runtime thread | 0.313457 | 1.7623 | futex_wait_queue |
| source | 1265258 | bench_cpp_clien | application/runtime thread | 0.312883 | 1.7590 | futex_wait_queue |
| source | 1265261 | bench_cpp_clien | application/runtime thread | 0.312132 | 1.7548 | futex_wait_queue |
| source | 1265259 | bench_cpp_clien | application/runtime thread | 0.311071 | 1.7489 | futex_wait_queue |
| source | 1265286 | bench_cpp_clien | application/runtime thread | 0.045958 | 0.2584 | hrtimer_nanosleep |
| source | 1265257 | bench_cpp_clien | application/runtime thread | 0.045810 | 0.2575 | hrtimer_nanosleep |
| source | 1265252 | bench_cpp_clien | application/runtime thread | 0.003388 | 0.0190 | futex_wait_queue |
| source | 1265297 | bench_cpp_clien | application/runtime thread | 0.003154 | 0.0177 | futex_wait_queue |
| source | 1265253 | bench_cpp_clien | application/runtime thread | 0.002698 | 0.0152 | futex_wait_queue |
| source | 1265256 | bench_cpp_clien | application/runtime thread | 0.002691 | 0.0151 | futex_wait_queue |
| source | 1265251 | bench_cpp_clien | application/runtime thread | 0.002579 | 0.0145 | futex_wait_queue |
| source | 1265255 | bench_cpp_clien | application/runtime thread | 0.002440 | 0.0137 | futex_wait_queue |
| source | 1265254 | bench_cpp_clien | application/runtime thread | 0.002381 | 0.0134 | futex_wait_queue |
| source | 1265249 | bench_cpp_clien | application/runtime thread | 0.002253 | 0.0127 | futex_wait_queue |
| source | 1265250 | bench_cpp_clien | application/runtime thread | 0.002208 | 0.0124 | futex_wait_queue |
| source | 1265293 | bench_cpp_clien | application/runtime thread | 0.000773 | 0.0043 | futex_wait_queue |
| source | 1265298 | bench_cpp_clien | application/runtime thread | 0.000673 | 0.0038 | futex_wait_queue |
| source | 1265246 | bench_cpp_clien | application/runtime thread | 0.000626 | 0.0035 | hrtimer_nanosleep |
| source | 1265284 | bench_cpp_clien | application/runtime thread | 0.000206 | 0.0012 | futex_wait_queue |
| source | 1265277 | bench_cpp_clien | application/runtime thread | 0.000058 | 0.0003 | futex_wait_queue |
| source | 1265282 | bench_cpp_clien | application/runtime thread | 0.000055 | 0.0003 | ep_poll |
| source | 1265273 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1265274 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1265275 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1265276 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1265278 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1265279 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1265280 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |
| source | 1265281 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | ep_poll |
| source | 1265283 | bench_cpp_clien | application/runtime thread | 0.000000 | 0.0000 | futex_wait_queue |

Linux comm은 15자에서 잘리며, 이름을 지정하지 않은 Framework/bench worker는 executable 이름으로 보입니다. 해당 스레드도 TID와 CPU 값으로 모두 기록했습니다.

## Frame과 byte 수

계수 전용 실행에서 LD_PRELOAD shim으로 public zlink_request와 zlink_reply 호출을 감쌌습니다. shim은 public zlink_msg_size로 각 part 길이를 합산하고 호출별 frame 수와 byte 길이를 프로세스 로그에 집계했습니다. 이 계수 실행은 CPU timed run과 분리했습니다.

| 구현 | 방향 | frame/msg | part byte 길이 | byte/msg |
|---|---|---:|---|---:|
| C++ binding | source → target request | 2 | 201 + 1,027 | 1,228 |
| C++ binding | target → source reply | 2 | 201 + 1,027 | 1,228 |
| C++ Framework | source → target request | 2 | 13 + 평균 1,388.65 | 평균 1,401.65 |
| C++ Framework | target → source reply | 2 | 21 + 평균 1,370.65 | 평균 1,391.65 |

Framework part 길이는 envelope metadata의 varint 길이에 따라 계수 실행에서 1 byte 정도씩 달라졌으므로 호출별 길이를 빈도 가중 평균했습니다. Binding part 길이는 매 호출에서 일정했습니다. 계수 실행에는 warmup과 active traffic이 함께 있으나 양쪽에서 같은 payload와 protocol path를 계수했습니다.

## 선택 측정

4 KiB request-backpressure와 1 KiB send-saturation도 unpaced 1회 및 10k/s paced 1회씩 수행했습니다. 아래 CPU 값은 각 한 번의 캡처에서 활성 Core I/O worker 중 가장 바쁜 thread의 µs/msg입니다. 모든 thread 행은 per-thread CSV와 all_thread_cpu.csv에 있습니다.

| pattern | rate mode | 구현 | actual msg/s | target busiest I/O µs/msg | source busiest I/O µs/msg |
|---|---|---|---:|---:|---:|
| request-backpressure 4 KiB | unpaced | binding | 161,291.4 | 6.12 | 6.00 |
| request-backpressure 4 KiB | unpaced | Framework | 47,206.8 | 17.61 | 18.35 |
| request-backpressure 4 KiB | paced | binding | 9,937.0 | 14.16 | 16.16 |
| request-backpressure 4 KiB | paced | Framework | 9,987.9 | 19.53 | 20.69 |
| send-saturation 1 KiB | unpaced | binding | 608,588.7 | 1.48 | 1.57 |
| send-saturation 1 KiB | unpaced | Framework | 182,908.2 | 3.31 | 4.15 |
| send-saturation 1 KiB | paced | binding | 9,999.9 | 12.27 | 12.77 |
| send-saturation 1 KiB | paced | Framework | 9,999.1 | 11.89 | 13.56 |

## 로컬 bench 변경과 제외한 calibration

로컬 변경은 framework/bench/grpc/cpp/client/bench_cpp_client.cpp의 --target-rate pacing과 send-saturation slot pacing뿐이며 Core나 Framework runtime 코드는 수정하지 않았습니다. run_local_capture.sh와 runner_common.capture.sh는 raw에 둔 복사본이고, 기존 collect.py 기반 수집기를 호출합니다. 단위 test와 CI는 실행하지 않았습니다.
48k와 20k 상한의 초기 비교는 처리율이 달라 equal-rate 표본으로 사용하지 않았습니다. 48k/20k 초기 Framework 실행에서 warmup drain이 남은 경우는 local bench pacer의 deadline 분기에서 발생해 drain 경로를 유지하도록 고쳤습니다. 수정 후 32k binding과 48k Framework 상한의 3회 비교가 모두 정상 완료됐습니다. 유효하지 않은 calibration 결과도 raw 아래에 보존했습니다.

## Raw 파일

모든 timed capture에는 before.json, after.json, samples.json, meta.json이 있으며 해당 benchmark cell에는 results.json, source.log, target.log가 있습니다. 전체 41개 capture의 전체 스레드 표는 /root/jobs/fx/cx/v21/s033/xio/raw/all_thread_cpu.csv에 있습니다. 이 파일은 calibration과 선택 측정도 포함해 3,564개 thread row를 담습니다.

주요 위치는 다음과 같습니다.

- 빌드 로그: /root/jobs/fx/cx/v21/s033/xio/raw/build-packages.log, build-bench.log, build-paced-patch.log, build-paced-patch2.log, build-send-pacing.log
- 기준 unpaced: /root/jobs/fx/cx/v21/s033/xio/raw/unpaced/
- 동일 실제 약 21.94k/s 세 실행: /root/jobs/fx/cx/v21/s033/xio/raw/equal22k-binding/, equal22k-framework/
- 추가 10k/s 세 실행: /root/jobs/fx/cx/v21/s033/xio/raw/paced10k/
- 선택 4 KiB 및 send-saturation: /root/jobs/fx/cx/v21/s033/xio/raw/step5-4k-unpaced/, step5-4k-paced/, step5-send1k-unpaced/, step5-send1k-paced/
- frame 계수 실행과 shim: /root/jobs/fx/cx/v21/s033/xio/raw/counts/, count_frames_agg.c, count_frames_agg.so
- copied collection scripts and runner: /root/jobs/fx/cx/v21/s033/xio/raw/collect-base.py, collect-ctrecv.py, run_local_capture.sh, runner_common.capture.sh
