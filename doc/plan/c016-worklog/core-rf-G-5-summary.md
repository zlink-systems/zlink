# G-5 — 벤치 하네스 getenv 제거 (완료)

## 결과(수치)
- 원인: `perf_measurement_part_count()`가 매 호출마다 `std::getenv("PERF_PART_COUNT")` 재조회. 호출처는
  송신 경로(`perf_zlink_send_measurement_parts`/`perf_zlink_send_rid_measurement_parts`/
  `perf_zlink_publish_measurement_parts`/`perf_zlink_dealer_request_measurement_part`/
  `perf_zlink_router_request_measurement_part`/`perf_zlink_router_reply_measurement_part`)와
  수신 경로(`perf_single_one_way.hpp`의 recv tail 체크, `perf_multi_socket_reqrep.hpp`) 양쪽 — single·multi 공유.
- 축소 셀(single ROUTER_ROUTER tcp 1024B, callgrind, `--cache-sim=no`, 12s/4s 차분법, G-A와 동일 방법):
  - **getenv 호출/msg**: before(G-A 원보고서, main f127924578) **1.996~2.03회/msg** → after(본 fix, g5) **0.00014회/msg**(141/134회는 잔여 프로세스 기동 시 1회성 호출, 메시지 루프와 무관)
  - **Ir/msg**: before(G-A) **15,806** → after(본 측정) **14,324** (−1,482 Ir/msg, −9.4%; G-A가 추정한 getenv 자체 비용 1,135 Ir/msg(6.96%)와 방향·규모 일치, 나머지는 호출부 call/ret 오버헤드 제거분)
  - load average(after 측정 시작): 7.5~8.2 (다른 job의 dev 빌드 1개 동시 실행 중, PERF_LOCK로 직렬화)
- **14 스크린(1024B tcp runs 1, after-fix, g5 워크트리)**:

| 셀 | throughput(msg/s) |
|---|---|
| single PAIR | 341,272 |
| single PUBSUB | 341,488 |
| single DEALER_DEALER | 172,319 |
| single DEALER_ROUTER | 265,900 |
| single DEALER_ROUTER_REQREP | 132,015 |
| single ROUTER_ROUTER | 265,898 |
| single ROUTER_ROUTER_REQREP | 99,638 |
| multi DEALER_DEALER | 249,963.5 |
| multi DEALER_ROUTER_SENDSEND | 153,583.5 |
| multi ROUTER_ROUTER_SENDSEND | 133,954.5 |
| multi DEALER_ROUTER_REQREP | 126,648.5 |
| multi ROUTER_ROUTER_REQREP | 73,810 |
| multi PUBSUB | 518,438 |
| multi STREAM | 39,242.5 |

  (측정 조건: single duration 1s runs 1, multi CCU 20 duration 2s runs 1, 모두 tcp 1024B, PERF_LOCK 아래.
  이 값은 목적상 "축소된" 스모크에 가깝다 — 감독관이 idle runs 3으로 Phase 2G 기준을 다시 잡을 때 참고용.)

## 변경 파일
- `bindings/c/perf/common/perf_zlink_part_helpers.hpp` — `perf_measurement_part_count()`를 함수-스코프
  `static const size_t`(IIFE로 초기화)로 캐시. 다른 곳(`bench_debug_enabled()`)과 같은 기존 패턴.
- with_zmq 대칭: 불필요 — `bindings/c/bench/with_zmq`에는 이 헬퍼도 `PERF_PART_COUNT`도 없음(grep 확인, 별도 getenv 세트 BENCH_DEBUG 등뿐).

## 설계 비교와 선택 이유
- A안(채택): 함수-스코프 `static const` 캐시. 근거: 이미 파일 내 `bench_debug_enabled()`가 동일 패턴, 새 전역/새 초기화 훅 불필요, 규칙 수 증가 없음(POSDDD).
- B안(기각): 호출자(`send_active_samples`, `run_measurement_phase` 등)에서 값을 한 번 읽어 파라미터로 넘기기. 기각 사유: 호출 시그니처 다수(7개 helper + 다중 recv 경로)를 모두 바꿔야 하고, 상태를 파라미터로 흩뿌리는 방향은 기존 캐시 패턴과 중복·비대칭.
- PERF_PART_COUNT는 런치 스크립트(`run_benchmarks.sh`/`run_benchmarks_multi.sh`)에서만 설정되고 프로세스 시작 후 변경되지 않음(grep으로 setenv/putenv 없음 확인) — 정적 캐시가 안전.

## 실행한 테스트와 남은 실패
- Core 소스·공개 계약 미변경 — `ctest -R` 대상 suite 없음(perf 벤치 전용 변경).
- `bindings/c/perf/run_benchmarks.sh --pattern ALL --msg-sizes 1024 --duration 1 --runs 1`: single 7패턴×6트랜스포트 **42/42 success, 0 fail**.
- `bindings/c/perf/run_benchmarks_multi.sh --transports tcp --msg-sizes 1024 --duration 2 --runs 1 --clients 20`: multi 7패턴 **7/7 success, 0 fail**.
- callgrind 축소셀(RR single, d12/d4 차분): 위 결과. 잔여 실패 없음.

## 성능 표
위 "14 스크린" 표 참조. before(수정 전) 값은 이번 job 범위 밖(감독관이 gate에서 재기준). 이번 job은 G-A가 이미 특정한 getenv 오염 제거 확인이 목적.

## 재확인한 스펙 절
- Core 소스·`core/include/**`·`libzlink.vers` 미변경. 계약 동작(completion·READY/DISCONNECTED·POLLIN/POLLOUT·WRITABLE wake) 관련 문장 어느 것도 다른 동작이 되지 않았다 — 변경은 벤치 하네스 내부 캐싱뿐이며 측정 의미(옵션 값·기본값)는 동일(같은 반환값, 첫 호출 이후 값 불변 보장).

## 변경 분류
B(기존 결함 — 벤치 하네스가 측정 대상 코드가 아닌데도 매 메시지 getenv를 호출해 공개 수치를 오염시킨 하네스 버그).

## 멈춘 지점
없음. 상한 내 완료.
