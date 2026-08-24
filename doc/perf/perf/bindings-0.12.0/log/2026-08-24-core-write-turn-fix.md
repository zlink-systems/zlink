# Core write turn 수정 — 무제한 speculative write 회귀 (2026-08-24)

## 증상 (0.10.1 게이트, C perf, tcp)

REQREP throughput 65~84% (소형), RTT latency +40~94%, one-way 64B mean
latency 최대 260배 (in-flight ~17MB가 커널 버퍼에 상주).

## 원인

`asio_engine.cpp speculative_write()`가 session pipe를 커널로 배출하는
루프의 byte budget이 `stream_tcp_speculative` 조건(STREAM/tcp 전용)에
게이트되어, 일반 소켓(PAIR/DEALER/ROUTER/PUB)은 커널 송신 버퍼가 찰
때까지 무제한 드레인했다. `start_async_write()`가 매 write turn마다
재진입시키므로 턴 단위 상한(B안)으로는 막을 수 없다(8K~2M 스윕 전부
무효과 실측).

release asset bisect: REQREP 64B 절벽은 0.11.x에서 시작 (0.10.1 203K →
0.11.1 110K). engine 쓰기 경로는 0.10.1과 동일 — 결함은 잠복해 있었고,
byte-HWM이 application pipe를 깊게 만들면서 드레인이 폭주할 연료를
공급했다. 대기열이 zlink pipe(HWM 계량 지점)에서 커널 버퍼(계량 밖)로
이동해 byte-HWM 제어 모델 자체가 우회되었다(64B에서 478만 send 중 HWM
park 0건).

## 결정 (소유자, 2026-08-24)

A안 채택: 일반 소켓은 Proactor 비동기 쓰기 기본(03-io-thread 스펙 규범
그대로), STREAM은 스펙(08-stream:399-401)대로 budget형 speculative write
유지. 결정은 use_speculative_write_for() 정책 함수 하나가 소유하고,
budget 검사는 admitted turn 전체에 보편 적용된다. 구 동작은 진단용
opt-in(legacy_sync_write_opt_in)으로만 남는다. B안(budget 전 소켓 확장)은
실측 무효과로 기각.

## 검증

게이트 셀 (local A-default vs 0.10.1 asset, tcp, 3s, 3회 중앙값):

| 셀 | throughput | latency |
|---|---|---|
| DR_REQREP 64/256/1024B | 110/111/108% | 96~100% |
| RR_REQREP 64/256/1024B | 107/130/119% | 82~100% |
| PAIR 64/256/1024B | 110/113/101% | 53~79% |

전 셀 ±5% 목표 초과 달성. ctest 87/94 — 실패 7건 중 6건은 기지 집합,
1건(test_thread_safe_contract_policy)은 병행 중인 core/doc 스펙 통합
작업의 파일 이동으로 인한 문서 존재 검사 실패(코드와 무관).

STREAM speculative write ON/OFF (MULTI_STREAM tcp @200, 3s):
64B throughput ON +8.6%, p99 ON 우위(64B/64KB 각 +11%/+16% OFF 악화) —
스펙의 상시 on + 2MiB budget 존치 근거 재확인.
