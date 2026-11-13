# design.md §7 확인 측정 M1·M2·M4·M5 (2026-11-10 09:47–09:49Z, PAUSED6)

조건: request-backpressure 1 KiB, binding cap 32k·Framework cap 48k(실제 약 20–21k), 각 1회. 표: analysis.txt. 원자료: raw/<label>/.
창 A(uprobe 없음, 5 s): schedstat µs/msg, perf stat per thread(instructions, cycles, msr/aperf, msr/mperf, sendto). 창 B(uprobe, 2 s): M1. 창 C(Framework만, 2 s): perf record -g source.

## M1 제출 간격 g와 drain 창 W (창 B, uprobe 부착)
| | target g p25/p50/p75 µs | target W(반환→다음 sendto) p50 µs | 짝 공유 비율* | sendto/제출(창 B) |
|---|---|---|---|---|
| binding 기본 | 9/55/85 | 9 | 0.47 | 0.53 |
| Framework 기본 | 23/34/42 | 12 | 0.06 | 0.95 |
| binding S2 | 9/55/87 | 10 | 0.48 | 0.52 |
| binding S2D | 30/41/68 | 11 | 0.01 | 1.00 |
*연속한 두 제출이 같은 sendto에 든 비율. 모두 짝이면 0.5(짝 안 간격과 짝 사이 간격이 번갈아 나옴).
- binding은 짝 안 간격 약 9 µs가 W 약 9–10 µs 안에 든다. Framework는 g p25가 23 µs로 W 12 µs보다 길다 → §4.1 확정. W는 설계 추정(수 µs–수십 µs)의 아래쪽, 약 10 µs다.
- uprobe는 간격을 늘린다. Framework sendto/제출이 창 A 0.75에서 창 B 0.95로 올랐다. 분포와 비율로만 읽는다.

## M4 binding S2D(둘째 thread reply마다 20 µs busy) — 창 A, uprobe 없음
| | target IO/1 sendto/msg | IO/1 µs/msg | IO/1 instr/msg | source IO/0 sendto/msg |
|---|---|---|---|---|
| binding S2 | 0.496 | 11.55 | 18,521 | 0.496 |
| binding S2D | **0.961** | **20.03** | **33,364** | 0.497 |
| Framework 기본 | 0.748 | 18.24 | 23,731 | 0.749 |
- 간격만 넓히자 target sendto/msg가 0.50 → 0.96, IO/1 비용이 Framework보다 커졌다. 예상(0.6–0.75)보다 크게 오른 것은 20 µs가 W 약 11 µs보다 늘 길기 때문이다. Framework 코드 없이 "간격이 원인" 확정.
- **설계 §4.2와 다른 점:** S2D에서 target이 짝을 잃어도 binding source는 0.497 그대로다. source의 짝은 도착 짝이 아니라 pacer burst가 만든다. 따라서 Framework source 0.749는 target을 따라온 값이 아니라 Framework driver 자신의 제출 간격(g 25/28/34 µs, 창 B)이다. target을 고쳐도 source는 저절로 따라오지 않는다 → F4(driver 제출 구조)와 F3(완료 경로)이 source 쪽 수정 대상.

## M2 유효 주파수 (aperf/mperf × 2.0 GHz, 창 A)
| | 모든 활성 thread | cpuinfo 평균 MHz | target IO/1 IPC | IO/1 instr/msg |
|---|---|---|---|---|
| binding 기본 | 3.78–3.79 GHz | 2,998 | 0.62 | 18,625 |
| Framework 기본 | 3.09 GHz (−18 %) | 2,754 | 0.52 | 23,731 (+27 %) |
| binding S2 (busy thread +1) | 3.50 | 2,666 | 0.59 | 18,521 |
| binding S2D (busy thread +2) | 3.33 | 2,781 | 0.61 | 33,364 |
- ref-cycles는 이 CPU(Ryzen 7 5825U)에서 미지원이라 aperf/mperf를 썼다. aperf와 cycles는 같다(cycles는 core clock).
- 기준(binding 대비 10 % 이상 낮음) 충족 → §1.2 확정. 같은 창에서 프로세스 양쪽 모든 thread가 같은 주파수로 내려간다. busy thread를 하나 더할 때마다 내려간다(S2·S2D) → 활성 core 수가 주파수를 정한다.
- P1(±8–15 %)과 affinity(+30 %)가 엇갈렸던 IO/1 명령어 수는 이번에 +27 %다. affinity·S2C2 쪽이 맞다.
- 주의: S2·S2D의 수신 thread(bench_cpp_zlink)는 45.6 µs/msg·IPC 1.43으로 돈다. poller 이벤트에 pollout이 있어 바로 깨어나는 바쁜 루프다(벤치 변형의 부작용). sendto 결론에는 영향 없고 주파수는 낮춘다.

## M5 source의 thread 8개
- 8개 모두 boost::asio pool worker다(scheduler::run). 처리하는 것은 `executor_op<binder0<dispatch_completion>>`: request callback, receiver_state, envelope header decode, receive_record_t, task_shared_state::complete, completion_signal_t::notify.
- 각 3.37 µs/msg(이번 회차), 합 27 µs/msg. 표본의 54 %가 pthread_cond_wait/futex 경로(깨어남·잠듦)다.
- → completion dispatcher(provider_task_context dispatch_completion의 dispatcher post)다. executor worker가 아니다. F3(B1)의 근거.
