| 조건 | 완료율(전체 active) req/s | CPU 표본창 req/s | source process µs/요청 | target process µs/요청 | source/target I/O thread µs/요청 | source 요청 메시지/sendto, recvfrom | target 응답 메시지/sendto, recvfrom |
|---|---:|---:|---:|---:|---:|---|---|
| binding, 무제한 제출 | 248,289 | 279,707 | 8.87 | 8.65 | 5.21 / 5.36 | 10.41, 4.69 | 6.86, 5.86 |
| binding, 52,000 req/s pacing | 47,797 | 51,991 | 42.99 | 38.95 | 24.60 / 27.98 | 1.26, 1.26 | 1.26, 1.29 |
| binding, 100,000 req/s pacing | 95,327 | 96,684 | 26.34 | 23.84 | 16.37 / 16.73 | 1.77, 1.87 | 1.83, 1.89 |
| Framework, 무제한 제출 | 51,248 | 50,699 | 85.17 | 59.60 | 28.45 / 28.14 | 1.46, 1.45 | 1.34, 1.56 |

### 판정

Framework의 약 51k req/s와 같은 속도로 binding을 제출시키면 Core I/O CPU가 가까워졌다. source는 binding 24.60, Framework 28.45 µs/요청이고 target은 각각 27.98, 28.14 µs/요청이다. 따라서 Framework의 작은 TCP 묶음과 I/O CPU 증가는 대부분 낮은 처리율에서 생긴다. 다만 두 경로의 호출 수와 eventfd 동작은 같지 않다.

52k 조건에서 binding은 sendto 한 번당 약 1.26개 요청, 성공한 recvfrom 한 번당 약 1.26개 응답을 처리했다. Framework는 각각 약 1.46개와 1.45개다. 둘 다 무제한 속도 binding의 10.41개 요청/sendto 및 4.69개 요청/recvfrom보다 훨씬 작은 묶음이다. 52k binding은 Framework보다 sendto 호출이 요청당 0.80회 대 0.69회, recvfrom 호출이 1.24회 대 0.98회로 더 잦고, recvfrom EAGAIN도 요청당 0.45회 대 0.29회다. 작은 묶음이라는 동작은 수렴하지만 정확히 같지는 않다.

### rate 효과와 남은 비용

binding을 무제한 약 249k req/s에서 52k req/s로 낮췄을 때 process CPU는 source 8.87에서 42.99 µs/요청으로 34.12 증가했고, target은 8.65에서 38.95로 30.30 증가했다. 이 증가분은 제공된 Framework 수치 source 80.5 및 target 59.9 µs/요청의 각각 약 42%, 51%다. 52k에서 측정된 binding 전체 process CPU는 Framework 제공 수치의 약 53%, 65%다. 따라서 rate는 Framework의 비용 중 큰 부분을 설명하지만 전체 source/target 차이를 모두 설명하지 않는다.

동일 실행에서 Framework process CPU는 source 85.17, target 59.60 µs/요청이었다. 제공된 80.5/59.9와 비교하면 source는 약 6% 높고 target은 거의 같다. 이 실행에서 rate-matched binding 전체 process CPU와 Framework 전체 process CPU의 차이는 source 42.18, target 20.65 µs/요청이다. Framework의 source process에는 I/O thread 외 실행·handoff 비용이 남아 있다. 이 측정은 그 잔여 비용을 binding entry point별로 분해하지 않았다.

Core I/O가 아닌 eventfd 사용량도 동일하지 않았다. source의 I/O thread eventfd 쓰기는 binding 52k에서 요청당 0.41회, Framework에서 0.11회였다. source의 다른 thread eventfd 읽기/쓰기는 binding 2.20/0.39회, Framework 0.83/0.28회였다. target의 I/O thread eventfd 쓰기는 binding 0, Framework 0.27회였고, 다른 thread 읽기/쓰기는 binding 0, Framework 1.00/0.12회였다. 이 차이는 I/O thread CPU 합계가 비슷해도 wake 경로가 일치하지 않음을 보여준다.

요청당 전체 context switch는 source/target 순서로 binding 무제한 0.14/0.10회, binding 52k 1.51/1.97회, binding 100k 0.57/0.86회, Framework 2.44/1.26회였다. context switch는 각 process의 모든 thread에서 `/proc/<pid>/task/<tid>/status` delta를 합산했다.

제공된 Framework source의 binding/Core stage 40.3 µs/요청은 52k에서 측정한 binding source 전체 CPU 42.99 µs/요청과 같은 크기다. 이는 rate 효과가 그 stage의 크기 대부분을 만들 수 있음을 뒷받침한다. 다만 전체 Framework source 80.5 µs/요청을 설명하려면 앞서 측정된 18.8 µs/요청 handoff/wake와 기타 Framework 실행 비용도 남는다. 이 수치들을 서로 더해 별도 절감치처럼 취급하지 않았다.

제공된 binding 기준 249,272 req/s의 0.6배는 149,563 req/s다. 이 정확한 속도 조건은 요청된 네 조건에 포함되지 않아 **측정 못 함**. 100k pacing과 무제한 binding 측정 사이를 선형 보간하면 binding bench와 동일한 경로의 process CPU는 source 약 21.3, target 약 19.4 µs/요청이며, I/O thread CPU는 약 13.1/13.4 µs/요청이다. 이 값은 측정값이 아니라 두 관측점을 잇는 단순 추정이다.

### 조건별 syscall 및 wake 수치

아래 호출률은 성공한 TCP sendto 바이트를 요청 record 크기로 나눠 구한 메시지 수를 분모로 사용했다. binding은 요청/응답 각각 1,252B, Framework는 요청 1,427B와 응답 1,417B를 사용했다. `sendto`, `recvfrom`, EAGAIN은 요청당 횟수다. 메시지/sendto와 메시지/recvfrom은 위 표에 별도로 적었다. eventfd는 I/O thread와 나머지 thread를 나눴고 각 쌍은 읽기/쓰기 횟수다.

| 조건 / process | sendto / 요청 | recvfrom / 요청 | recvfrom EAGAIN / 요청 | I/O thread eventfd 읽기/쓰기 | 다른 thread eventfd 읽기/쓰기 |
|---|---:|---:|---:|---:|---:|
| binding 무제한 / source | 0.096 | 0.266 | 0.053 | 0.000 / 0.000 | 0.031 / 0.016 |
| binding 무제한 / target | 0.146 | 0.209 | 0.038 | 0.000 / 0.000 | 0.000 / 0.000 |
| binding 52k / source | 0.796 | 1.244 | 0.452 | 0.000 / 0.411 | 2.199 / 0.386 |
| binding 52k / target | 0.795 | 1.160 | 0.382 | 0.000 / 0.000 | 0.000 / 0.000 |
| binding 100k / source | 0.564 | 0.739 | 0.206 | 0.000 / 0.234 | 1.192 / 0.294 |
| binding 100k / target | 0.545 | 0.728 | 0.198 | 0.000 / 0.000 | 0.000 / 0.000 |
| Framework 무제한 / source | 0.686 | 0.975 | 0.287 | 0.000 / 0.112 | 0.833 / 0.284 |
| Framework 무제한 / target | 0.746 | 0.945 | 0.304 | 0.000 / 0.266 | 1.003 / 0.123 |

### 측정 방법과 제한

조건 B에서 source CPU 0–7, target CPU 8–15로 실행했다. 각 조건은 RUNS=1, payload 1,024B였다. 각 run은 10초 CPU 표본창의 thread별 schedstat delta와 8초 BPF syscall 창을 수집했다. source/target process CPU는 해당 창의 모든 thread runtime delta를 완료 요청 수로 나눴다. I/O thread CPU는 coreio `summarize_perf.py` 방식으로 `ZLINKbg/IO/*` thread들의 schedstat delta를 합산했다. context switch는 같은 표본창의 `/proc` status delta다. BPF syscall과 eventfd 횟수는 요청당 완료 수 대신 TCP 전송 바이트 기반 메시지 추정치를 분모로 사용했다.

전체 active 구간 처리율은 binding 무제한 248,289, binding 52k pacing 47,797, binding 100k pacing 95,327, Framework 51,248 req/s였다. CPU 표본창 처리율은 각각 279,707, 51,991, 96,684, 50,699 req/s였다. paced 조건은 설정 속도에 근접했고 Framework 실행 속도와 52k pacing CPU 창은 2.6% 차이다. 무제한 binding CPU 창 처리율은 전체 active 평균보다 높으므로 CPU 비용 비교에는 개별 표본창 처리율도 함께 적었다.

모든 run에서 source 완료 수와 target 수신 수가 일치했고 target 오류와 거부는 0이었다. 메모리는 실행 전후 12 GiB 기준을 넘었다. 각 측정 실행은 `/root/jobs/fx/cx/measure.lock`을 잡고 실행했다. 52k 조건은 처음 한 번 strace/uprobe 창도 포함했으며 이 두 창의 수치는 성능 표에 쓰지 않았다. 나머지 조건은 CPU 및 BPF 수집만 했다.

### 변경 파일

worktree에서 pacing 계측을 위해 수정한 파일은 다음 두 개다.

- `framework/bench/grpc/cpp/client/bench_cpp_client.cpp`
- `framework/bench/grpc/cpp/common/bench_async.hpp`

`ZLINK_CPP_BENCH_REQUEST_RATE`는 기본값이 비활성이고, 양의 정수일 때 raw C++ request-backpressure 제출 시각만 제한한다. 기존 무제한 in-flight 경로와 응답 poller 진행은 유지했다. 제품 런타임, bindings/, Core 헤더, spec 파일은 수정하지 않았다. 벤치는 `ZLINK_LOCAL_PACKAGE_ROOT=/root/worktree/zlink-ratio040/.artifacts/wsl`로 기존 1.23.0 패키지를 참조해 빌드했다. 테스트는 실행하지 않았다.

수집 및 분석 스크립트와 원자료는 `/root/jobs/fx/cx/v21/s033/cbind/`에 있다. worktree 안의 추가 변경은 벤치 build 산출물이다. commit과 push는 하지 않았다.
