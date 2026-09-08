# Core 리팩토링 캠페인 (0.17.0 후반) — 전반 성능 개선 · POSDDD · 불필요 코드 정리 (STREAM 우선)

> 작성일: 2026-09-06 21:10, 개정 21:40 · 22:00 (머신 B, main `285f37792d`)
> 선행 계획: [`core-0.17.0-dontwait-contract-and-perf-plan-b.ko.md`](archive/core-0.17.0-dontwait-contract-and-perf-plan-b.ko.md)
> 결정 기록: [`c016-worklog/decisions.ko.md`](c016-worklog/decisions.ko.md) (이 캠페인은 D-B140부터 이어 쓴다)
> 원칙: [`../principal/dev/posddd.ko.md`](../principal/dev/posddd.ko.md), [`../principal/dev/zlink-system-design-principles.ko.md`](../principal/dev/zlink-system-design-principles.ko.md),
> [`08-posd-module-structure.ko.md`](../../core/doc/spec/core/systems/08-posd-module-structure.ko.md), [`10-hot-path.en.md` §5](../../core/doc/spec/core/systems/10-hot-path.en.md)
> 성능 정책: [`../perf/PERF_POLICY.md`](../perf/PERF_POLICY.md), [`../perf/PERF_MULTI_TEST_POLICY.md`](../perf/PERF_MULTI_TEST_POLICY.md)

## 0. 요청 정리 (사용자, 2026-09-06 21:00 / 21:30)

| 항목 | 내용 |
|---|---|
| 대상 | `core/` 라이브러리(`core/src/api`, `core/src/runtime`) |
| 관점 | ① 성능 개선 ② POSDDD(깊은 모듈·경계·정보 은닉) ③ 불필요한 코드 정리(dead code·중복·no-op) |
| 절대 조건 | **공개 인터페이스(헤더·ABI·export 심볼)를 바꾸지 않는다. spec gap을 만들지 않는다.** 공개 C API·헤더 주석·`core/doc/spec` 계약과 다른 동작이 생기는 변경은 채택하지 않는다. 계약을 바꿔야만 가능한 개선은 D(spec gap)로 분류해 사용자 결정에 올리고 그 항목은 멈춘다. |
| 판정 기준 | **현재 main이 기준이다. 과거 버전 비교·회귀 유입 커밋 추적은 하지 않는다.** STREAM은 `bindings/c/bench/with_stream`에서 **같은 asio 기반의 다른 스택(asio·zmq)** 대비 약 20% 낮으므로 그 격차를 없애는 것이 목표다. **그 외 모든 패턴·transport는 외부 비교군 없이 `bindings/c/perf`(C 러너 single·multi)로 현재 main 대비 개선만 확인한다**(with_zmq는 패턴·transport 범위가 달라 쓰지 않는다). |
| 채택 규칙 | 성능이 오르지 않아도 **구조가 좋아졌으면 채택**. 성능이 게이트(§4) 밖으로 떨어지면 불채택. |
| 범위 | 세 축 모두 **Core 전체**가 대상이다: 성능은 7 패턴 × 4 transport 전부, 리팩토링은 `api`·`runtime` 전 모듈. STREAM은 순서만 첫 번째다. |
| 최우선 | **STREAM 소켓 성능**. 다른 스택과 같은 asio 위에서 돌면서 20% 낮고, 예전에는 더 높았던 적도 있으므로 구조상 불가능한 격차가 아니다. |
| 역할 | 감독관(Claude Fable, 이 세션) = 초기 분석·브리프·감독·리뷰·게이트·커밋. 코드 변경·측정·프로파일은 **서브에이전트(opus·sonnet)**. |

감독관은 코드를 직접 고치지 않고 빌드·측정도 직접 돌리지 않는다. 예외는 결정 기록·계획 문서·브리프 커밋뿐이다. 리뷰에서 발견한 정정도 job에 되돌려 보낸다.

## 1. 현재 상태

### 1.1 STREAM 격차 (with_stream, CCU 1000, 4 io threads, runs 3 median, `results/20260906_core_after_a_release`)

| 크기 | zlink | zlink_packet | asio | zmq | zlink/asio | zlink 서버 CPU | asio 서버 CPU |
|---|---|---|---|---|---|---|---|
| 64 B | 271.3 kops | 266.0 | **349.0** | 320.9 | 0.78 | 353% | 303% |
| 1024 B | 246.5 kops | 244.2 | **316.1** | 287.7 | 0.78 | 358% | 303% |
| 65536 B | 29.1 kops | 33.3 | **37.2** | 24.7 | 0.78 | 228% | 327% |

읽는 법:

- 세 크기 모두 asio 대비 **0.78**로 비율이 같다. 바이트 복사 비용이 아니라 **메시지당 고정 비용**(핸드오프·wake·잠금·할당·per-packet 처리)이 원인이라는 뜻이다.
- 64·1024 B에서 zlink 서버가 asio보다 CPU를 17% 더 쓰면서 22% 덜 처리한다. 메시지당 CPU 비용이 asio의 약 1.5배다. 즉 "CPU가 놀아서"가 아니라 "메시지당 일이 많아서"다. 개선 척도는 **메시지당 명령 수**다.
- 64 KiB에서는 zlink 서버 CPU가 228%로 asio(327%)보다 낮다. 여기서는 I/O 스레드가 놀고 있다는 뜻이므로 wake·flow control(credit)·write batching 쪽이 병목이다. `zlink_packet`이 zlink보다 15% 높은 것도 앱 쪽 프레임 재조립이 비용임을 보여 준다.
- 벤치 서버(`stacks/zlink/test_scenario_stream_zlink.cpp`)는 poller → `zlink_recv_part` → 프레임 판정 → `zlink_send_part_rid` 에코 구조이며 앱 스레드가 별도다. asio 스택은 io_context 워커 안에서 read→write를 바로 잇는다. zlink는 구조상 I/O 스레드 ↔ 앱 스레드 핸드오프(ypipe + mailbox wake)가 한 번씩 더 있으므로 그 핸드오프를 **메시지마다가 아니라 묶음마다** 치르게 하는 것이 격차를 메우는 핵심 방향이다.

목표(2026-09-07 개정): **구조가 같은 pull 모델인 zmq 대비 세 크기 모두 ≥ 1.0**이 1차 목표(idle G-0b 기준 0.91 / 0.98 / 1.27). asio(push, 핸드오프 없음) 대비는 참고 지표로만 기록한다. 남은 격차의 대부분은 비경합 잠금 ~15쌍(1쌍 ≈ 처리량 1.5~2 %)이다. thread-safe 소켓 계약은 설계 철학이므로 바꾸지 않는다(사용자, 2026-09-07). 단 **그 계약이 요구하는 잠금은 연산당 1~2쌍**(libzmq thread-safe 소켓이 증거)이고 나머지는 credit·flow-state·route shard·3겹 API 직렬화가 각자 잠금을 든 구현 방식의 결과다(D-B176) — 계약 안에서 통합·무잠금화 대상(G-11).

### 1.2 STREAM 데이터 경로와 파일

수신: `engine/asio/asio_raw_engine.cpp`(126행) → `protocol/raw_decoder.cpp` → `core/session_base_pipe_io.cpp`(252) → `core/pipe.cpp`(4112) → `sockets/stream/stream.cpp` `decode_packet_bytes`(360~611행) / `pump_packet_receive_queue` / `xrecv_routed` → `api/socket/socket_message_recv_api.cpp` → poller wake(`core/socket_poller.cpp`, `mailbox.cpp`, `signaler.cpp`).
송신: `api/socket/socket_message_send_api.cpp` → `sockets/common/socket_send_submit.cpp`(797) · `socket_send_complete.cpp`(555) → `stream.cpp` `xsend_routed`(route shard 잠금·RID 조회) → `pipe.cpp`(credit) → `session_base.cpp` → `raw_encoder.cpp` → `asio_engine.cpp`(2035) write.
STREAM 전용 파일은 4개(`stream.cpp` 1298, `stream.hpp` 163, `stream_batch_policy.hpp`, `stream_dispatch_lifecycle.cpp` 15행)이고 나머지는 모든 패턴이 공유하므로 STREAM 개선이 다른 패턴을 건드린다. 그래서 모든 job에서 1024 B 경량 3셀(§4)을 같이 본다.

### 1.3 메시지당 비용 후보 (Phase 1이 프로파일로 확정할 가설, 결론 아님)

1. **wake 빈도**: 패킷마다 mailbox/eventfd wake와 poller 재진입(수신 쪽), 앱 send마다 I/O 스레드 wake(송신 쪽). 묶음당 1회로 줄일 수 있는지.
2. **수신 pump**: `7738b8fd41`의 64-chunk bounded step과 재wake, `packet_record_t` 큐 이동, fragment 조립 복사, `decode_packet_bytes`의 분기 수.
3. **송신 경로 잠금·조회**: API 잠금 → command-owner → receive 잠금 순서, route shard 잠금, RID→pipe 조회, credit 확인(`1b8816a72f`, `0add1dd621`)이 메시지마다.
4. **할당**: 패킷당 msg_t/버퍼 할당·해제, encoder/decoder 버퍼 재사용 여부, 64 B에서의 소형 할당.
5. **write batching**: 에코 응답이 pipe에 쌓였을 때 asio write 한 번에 얼마나 내보내는지(`stream_batch_policy.hpp`), 64 KiB의 I/O 스레드 idle.
6. **관측 비용**: per-packet monitor/notify/flow-state 회계(`socket_base_flow_state.cpp` 811, `socket_base_monitor.cpp` 874)가 구독자 없을 때도 실행되는지.

### 1.4 전반 성능 — 현재 상태와 접근

외부 비교군 없이 **현재 main의 perf/c 값이 기준**이다. 셀은 perf/c가 지원하는 전부: single 7 패턴(PAIR, DEALER_DEALER, DEALER_ROUTER, ROUTER_ROUTER, PUBSUB, DEALER_ROUTER_REQREP, ROUTER_ROUTER_REQREP) + multi 7 패턴(DEALER_DEALER, DR/RR_SENDSEND, DR/RR_REQREP, PUBSUB, STREAM) × transport(tcp, tls, ws, wss, single은 inproc·ipc 포함) × size(64, 256, 1024, 4096, 65536). 전부를 매번 재지 않는다. **1024 B tcp 전 패턴을 스크린 셀**로 두고, 개선 job이 노린 패턴·transport만 전 size로 확정한다.

접근은 STREAM과 같다. 프로파일로 메시지당 비용을 분해하고 원인 하나당 job 하나로 줄인다. STREAM 이후 순서는 이미 알려진 약점부터: DEALER_DEALER 4096 B 포화 구간 p95/p99와 1024 B latency(D-B83), REQREP 64 KiB latency 잔여(D-B88), PUBSUB fan-out, tls/wss 암호화 경로의 buffer 재사용, ws 프레이밍. 이 목록은 Phase 1의 공통 경로 프로파일(job G-A)이 실측 순서로 바꾼다. STREAM 경로 개선의 대부분(wake 묶음, 잠금 순서, 할당, write batching)은 공유 코드라 다른 패턴에도 그대로 이득이 돌아오므로 Phase 2S가 끝난 뒤 스크린 셀을 먼저 다시 재고 남은 격차만 job으로 만든다.

### 1.5 리팩토링 후보 모듈 (STREAM 관련도 순, Core 전체)

| 순서 | 모듈 | 크기 | 초기 관찰 |
|---|---|---|---|
| R1 | `runtime/sockets/stream` | 4파일 1.5k | 250행 단일 함수 `decode_packet_bytes`, `packet_record_t` 수동 이동 구현, 15행짜리 `stream_dispatch_lifecycle.cpp`, route shard·notify·connect event가 한 클래스 |
| R2 | `runtime/protocol/raw_*`, `engine/asio/asio_raw_engine`, `asio_engine.cpp`(2035) | | raw 경로는 얇고, 공통 엔진이 큼. ZMP 엔진과 책임 경계 확인 |
| R3 | `runtime/core/pipe.cpp` | 4112행 | ypipe·credit·flow-state·stream packet state가 한 파일. 0.16.0 rf3 BLOCKER pipe 2건 잔존(D-B61) |
| R4 | `runtime/sockets/common/socket_base*` | 14파일, 헤더 1540행 | 파생 socket에 노출되는 표면이 넓음. `socket_send_submit`/`socket_send_complete`/`socket_base_msg` 중복 후보 |
| R5 | `api/socket/socket_request_reply_*` | 10파일 ~7k | pending pool 제거(D-B85) 뒤 no-op ABI(`PENDING_MAX_*`)와 `..._pending_*` 파일명 잔존 |
| R6 | `runtime/core/session_base*`, `ctx_physical_queue_registry.cpp`(1263) | | credit/registry. R3와 경계 |

이 6개 뒤에 나머지 모듈도 같은 절차로 돈다: R7 `api/core`(옵션 dispatch·mapping 7파일) · R8 `runtime/sockets/{dealer,router,pubsub,pair,proxy,internal}` · R9 `runtime/transports/{tcp,tls,ws,ipc,asio}` · R10 `runtime/core/{ctx_*,options_*,object,own,io_thread,mailbox,signaler,socket_poller}` · R11 `runtime/utils`. Phase 3 인벤토리 job이 dead code·중복·얕은 모듈 목록으로 확정한다.

## 2. 역할과 작업자

| 역할 | 담당 | 하는 일 |
|---|---|---|
| 감독관 | Claude Fable(이 세션) | 초기 분석(이 문서 §1)까지만 직접. 그 뒤로는 **브리프 작성, job 투입과 3분 간격 생존 확인, 결과 리뷰(diff·스펙 대조·보고서), 채택 판정, 커밋·push, decisions 기록**만 한다. 빌드·측정·게이트 실행·테스트·코드 수정은 전부 서브에이전트(사용자 지시 2026-09-06 22:05) |
| 분석·설계·성능 job | **opus** | 프로파일·비용 분해, 원인 1개 = job 1개 수정, 경계를 다시 긋는 POSDDD 리팩토링(R1·R3·R4) |
| 기계적 정리·측정·게이트 job | **sonnet** | dead code 삭제, 중복 helper 통합, 파일 분할·이름 정리(R2·R5·R6와 opus job 후속), with_stream·perf/c 실행과 표 작성, hotpath STREAM 셀 추가, **채택 전 게이트 일괄 실행**(main 포팅 빌드, ctest 전체, 변경 suite 5회, mirror cmp, hotpath_gate, 성능 확인 셀)과 결과 표 보고 |
| 리뷰 보조 | opus(읽기 전용) | 감독관 리뷰 전에 계약 위반·숨은 동작 변화 독립 점검(불일치 시 감독관이 코드로 확정) |

job 규칙(CONTRIBUTING §10): **원인 하나 = job 하나, 1.5 h 상한**, 게이트·측정 루프는 감독관이 한 번. 동시 job ≤ 4, 빌드 `JOBS≤6`(11 GB), **측정 중 다른 빌드·job·벤치 금지**. job은 각자 detached worktree(`~/project/zlink-work/<job>`)에서 겹치지 않는 파일만. 브랜치 없음, main에 단위별 커밋·push.

브리프 필수 항목: 소유 계층·spec 조항, 변경 분류(A 계약 적응 / B 기존 결함 / C 우회 / D spec gap) 한 줄, "계약을 바꿔야 하면 멈추고 보고", 변경 파일, 실행한 테스트와 남은 실패, 브리프가 지정한 셀의 성능 표.

## 3. 단계

### Phase 0 — 준비 (감독관 + sonnet 1 job, ~1 h)

1. main `285f37792d` Release+LTO 빌드(`scripts/build-core.sh release --lib-only`), with_stream 빌드(`--reuse-build` 가능 여부 확인).
2. **기준 측정**(sonnet, 조용한 머신): `with_stream/run_benchmarks.sh --stack zlink,zlink_packet,asio,zmq --size all --ccu 1000 --runs 3`. §7.1에 기록(load average 포함). 이후 모든 STREAM 판정은 이 표와 같은 조건.
3. perf/c 기준(sonnet): **스크린 셀** 1024 B tcp single 7 + multi 7 runs=1(§7.4)과, 그중 경량 3셀(single ROUTER_ROUTER, multi RR_SENDSEND, multi RR_REQREP)은 매 job 확인용. 전 size·전 transport 기준은 Phase 2G 시작 시 잰다(Phase 0에서 다 재면 1시간 넘게 걸림).
4. hotpath_gate에 `stream_tcp` 셀 추가(sonnet): `router_router_tcp` 셀과 같은 구조, STREAM 서버 + 최소 raw tcp 클라이언트(4-byte 길이 프레이밍), 1024 B, 메시지당 명령 수. reference는 main 측정값. 결정적 지표라 wall-clock 편차 없이 job 결과를 판정할 수 있다.

### Phase 1 — 메시지당 비용 분해 (opus 3 job 병렬, ~1.5 h)

- **job S-A(서버 프로파일)**: with_stream zlink 서버 1024 B·64 KiB를 `perf record -g`(I/O 스레드·앱 스레드 분리)로 잡고, 같은 조건의 asio 스택 서버와 **메시지당 명령 수·syscall 수·wake 수·할당 수**를 나란히 표로 만든다. 심볼별 상위 30개와 §1.3 가설별 실측 비용.
- **job S-B(경로 정적 분석)**: 수신·송신 경로를 함수 단위로 따라가며 메시지마다 실행되는 잠금·원자 연산·할당·분기·가상 호출을 센다. asio 스택에는 없는 단계 목록을 만들고 각 단계가 계약(spec 조항)이 요구하는 것인지, 구현 편의인지 표시한다.
- **job G-A(공통 경로 프로파일)**: perf/c 1024 B tcp로 ROUTER_ROUTER single, DEALER_DEALER multi, DR_REQREP multi, PUBSUB multi 4개를 `perf record`로 잡아 패턴별 상위 심볼과 **패턴 공통으로 나오는 비용**(잠금·wake·할당·monitor 회계)을 분리한다. 결과는 "STREAM job이 고치면 같이 좋아지는 것"과 "패턴 고유 원인"의 두 목록.
- 감독관이 세 결과를 합쳐 **원인별 job 목록**을 만든다(D-B141): STREAM 쪽 S-1, S-2, …와 전반 쪽 G-1, G-2, …를 예상 이득 순으로. 계약을 바꿔야 하는 항목은 D로 분리해 사용자에게 올린다.

### Phase 2S — STREAM 성능 job (opus, 원인당 1개, 파일이 겹치지 않으면 2개 병렬)

각 job: worktree → 수정 → 관련 suite(`test_stream_*`, 공개 C STREAM 계약 테스트) 5회 → with_stream `--stack zlink,asio --size all --runs 1` → 요약(메시지당 비용 변화 포함).
감독관 리뷰 → 게이트 job(sonnet): main 포팅 → dev `ctest -j2` 전체 → with_stream runs=1(개선 확인 시 runs=3 확정) → 1024 B 경량 3셀 → hotpath_gate(5셀) → 결과 표 → 감독관 판정, 채택 시 커밋·push, D-B14x 기록.
반복 종료 조건: 세 크기 zlink/asio ≥ 0.95, 또는 남은 원인이 모두 D(계약 변경 필요)이거나 설계 과제(auto-HWM 예산·I/O batching 정책 등)여서 사용자 결정이 필요할 때. 설계 과제는 선택지·예상 이득·계약 영향을 한 표로 올린다.

### Phase 2G — 전반 성능 job (opus, 원인당 1개)

Phase 2S 종료 뒤 perf/c **스크린 셀(1024 B tcp, single 7 + multi 7)** 을 Phase 0 기준과 비교해 STREAM 작업이 다른 패턴에 준 이득과 남은 격차를 표(§7.4)로 만든다. 그다음 Phase 1의 G-목록을 예상 이득 순으로 job 하나씩: worktree → 수정 → 관련 suite 5회 → 해당 패턴 perf/c 전 size(tcp) runs=1 → 요약. 감독관 게이트는 2S와 같고, 성능 확인은 해당 패턴 전 size·전 transport runs=1(개선 확인 시 runs=3) + 스크린 셀 전체(다른 패턴 손해 없음). 종료 조건: G-목록 소진, 또는 남은 것이 D·설계 과제.

### Phase 3 — POSDDD · 불필요 코드 정리 (모듈별, R1 → R11, Core 전체)

각 모듈은 두 job:

1. **인벤토리 job(sonnet, 읽기 전용)**: dead code(호출 없는 함수, 항상 같은 값의 분기, no-op 옵션), 중복(같은 일의 helper 둘 이상), 얕은 모듈(POSDDD 스멜 카탈로그: pass-through·정보 누출·긴 매개변수·250행 함수), 잘못된 소유(다른 계층이 알 필요 없는 상태 노출). 항목마다 `file:line`, 근거, 제안, 변경 반경, 계약 영향(없음 / 있음→D). 감독관이 채택 항목을 고른다.
2. **적용 job(경계 재설계 = opus, 삭제·통합·분할 = sonnet)**: 채택 항목만, 동작 변화 없음. 관련 suite 5회 + 1024 B 경량 3셀. R1·R2는 with_stream zlink runs=1도.

채택: 구조가 좋아졌으면(파일·함수 크기, 표면, 규칙 수, 이름-개념 일치) 성능 0이어도 채택. 게이트 밖이면 원인을 분리해 다시 하고 못 하면 그 항목만 뺀다. 0.16.0 rf1~rf3 BLOCKERS(D-B60·D-B61)는 해당 모듈 인벤토리에 포함.

### Phase 4 — 마무리

- with_stream 4스택 runs=3 최종 표, perf/c **전 패턴 × 전 transport × 전 size** single·multi runs=1(−5% 셀만 runs=3)로 Phase 0 대비 최종 표(§7.4). 어느 셀도 기준 아래 −5%를 넘지 않고, 손댄 패턴은 개선이 확인돼야 한다.
- hotpath_gate 5셀 PASS, `ctest -j2` 전체, 변경 suite 5회, mirror cmp(8 헤더 × 4), `git diff --check`, release lib 재링크, c·cpp `run_tests.sh` 스모크(ABI 불변이므로 다른 binding은 생략).
- 스펙은 **문구 정합만**(내부 구조·파일 배치 서술). 계약 문장은 손대지 않는다. 알려진 정합 대상: 08-stream "STREAM gather-write는 유지됨"(실제로 raw 엔진은 gather한 적 없음, S-4), with_stream README의 `ZLINK_CORE_SOURCE` 기본값(러너는 release 기본 = 404, local 명시 필요, G-0), `scripts/build-core.sh` 머리 주석·CONTRIBUTING §9의 "테스트 링크 구조 재작업 전" 잔재(`ddf9ff7e95`로 완료됨).
- **버전 범프 → 0.17.2**(사용자 승인 2026-09-07): 캠페인 중간에 머신 A 고정용으로 **0.17.1**을 먼저 발행했으므로(D-B171, 태그 `core/v0.17.1`), Phase 4 종료 커밋은 0.17.2다. 공개 인터페이스 불변이므로 patch. CONTRIBUTING 체크리스트 한 커밋 — `VERSION`, `core/CMakeLists.txt`, `core/include/zlink/common.h`, `core/include/zlink.h`(버전 매크로만, 표면 불변), 계약 테스트 `bindings/cpp/tests/contract/test_cpp_contract_common_header_version.cpp`, 바인딩 매니페스트 + `scripts/local-package/build-wsl.sh --sync-versions`, `BINDINGS_VERSION`.
- 소요: 조용한 머신 기준 2.5~3 h(측정 직렬). Phase 4 시작 전에 모든 job을 종료한다.
- decisions 종결 항목, §8 체크리스트 완료.

## 4. 게이트 (변경 하나를 채택하는 조건)

| 게이트 | 기준 | 실행자 |
|---|---|---|
| 계약 | §4.1의 5개 검사를 모두 통과. 하나라도 걸리면 그 변경은 채택하지 않고 D로 기록 | 감독관 리뷰(+opus 독립 점검) |
| 테스트 | `scripts/build-core.sh dev` + `ctest -j2` 전체 green, 변경 suite 5회, pipe·engine을 만진 job은 ASan/TSan | 감독관 |
| 성능(STREAM job) | with_stream 세 크기 모두 기준(§7.1) 이상, 최소 한 크기 개선. 다른 크기 −3% 초과 하락 불가 | sonnet 측정, 감독관 판정 |
| 성능(전반 job) | 노린 패턴은 전 size·전 transport에서 개선(size 집계 상승), 스크린 셀 전체 −5% 이내 | 동일 |
| 성능(모든 job) | perf/c 1024 B 경량 3셀 −5% 이내 + hotpath_gate 5셀 ±5%(개선은 reference 갱신) | 동일 |
| 구조 | POSDDD 지표(파일·함수 길이, 헤더 표면, 규칙 수) 악화 없음. 성능 0이어도 통과하면 채택 | 감독관 |

**Ir(명령 수)는 절대 기준이 아니다(사용자, 2026-09-07 20:40).** hotpath_gate의 Ir/msg는 이 머신에서 부하와 무관하게 재현되는 유일한 지표라 셀별 회귀 **검출**에 쓰는 대리 지표이며, 채택 여부의 최종 기준은 실제 성능(처리량·지연)과 구조다. 다음 규칙으로 판정한다.
- Ir가 늘어도 같은 경로의 실측 성능이 개선됐거나, 그 비용이 다른 경로의 더 큰 개선을 위해 설계상 불가피하면 채택한다(근거를 결정 문서에 기록).
- Ir 증가를 조사하는 경우는 "이득이 없는 경로에서만 비용이 늘었을 때"다(예: MP-4가 multipart를 쓰지 않는 single 경로에 +4.5 % Ir). 이때도 목표는 Ir 감소가 아니라 "그 경로가 새 코드를 타는 이유를 찾아 원래 fast path로 되돌리기"이며, 조사 결과 불가피하면 받아들인다.
- ±5 % 게이트는 "이 범위를 넘는 회귀는 설명 없이 채택하지 않는다"는 뜻이지, 범위 안의 증감을 개선/악화로 판정하는 기준이 아니다.

### 4.1 spec gap 검사 (변경마다, 채택 전)

spec gap = 코드 동작이 `core/doc/spec`·공개 헤더 주석·공개 계약 테스트가 말하는 것과 달라지는 것. 성능·구조를 위해 이것을 만드는 일은 없다. 검사는 다음 다섯 가지이고 전부 통과해야 한다.

1. **계약 테스트 불변**: `core/tests`의 integration/contract/C 공개 API 테스트는 **기대값을 한 줄도 바꾸지 않고** green이어야 한다. 테스트 기대값을 바꿔야 통과하는 변경은 그 자체가 spec gap이다(unit 테스트는 내부 구조를 따라가므로 이동·삭제 가능, 단 삭제된 검증은 어디로 갔는지 보고).
2. **스펙 문장 대조**: job은 자기가 만진 경로가 소유된 스펙 절(socket README, 08-stream, 02-raw, 10-hot-path, 06-auto-hwm, 05-polling 등)을 브리프에 적고, 결과 보고에 "이 절의 어느 문장도 다른 동작이 되지 않았다"를 문장 단위로 확인한다. 감독관이 같은 절을 코드와 다시 대조한다.
3. **관찰 가능한 순서·타이밍 보존**: 완료(completion)·이벤트(READY/DISCONNECTED/monitor)·POLLIN/POLLOUT level·WRITABLE wake의 **순서와 조건**은 그대로여야 한다. wake를 묶거나 batching을 늘려도 "언제 깨어나는가"의 계약 조건(예: 거절한 자원의 회복)은 바뀌지 않는다. 지연·순서를 바꾸는 최적화는 D.
4. **공개 인터페이스 절대 불변**(사용자 지시 2026-09-06 21:55): 공개 헤더(`core/include/zlink/**`)의 함수·시그니처·옵션·enum 값·struct 레이아웃·errno 매핑·export 심볼(`core/src/libzlink.vers`)은 **추가·삭제·의미 변경 모두 금지**. 성능을 위한 새 옵션·새 플래그·새 함수도 금지. 확인: `git diff --stat -- core/include core/src/libzlink.vers`가 비어 있어야 하고, mirror cmp(8 헤더 × 4)와 bindings c·cpp `run_tests.sh` 스모크가 green. 공개 인터페이스를 바꿔야만 얻는 개선은 D로 §7.5에 기록만 한다.
5. **스펙 diff 0**: 이 캠페인의 커밋에 `core/doc/spec` 변경이 들어간다면 내부 구조 서술(파일 배치·모듈 설명)뿐이어야 하고, 계약 문장 변경은 없어야 한다. 감독관이 커밋 전에 spec diff를 읽고 판정한다.

이 검사에서 걸린 개선은 버리는 것이 아니라 D(spec gap) 항목으로 §7.5에 모아 "어떤 계약을 어떻게 바꾸면 얼마를 얻는가"를 적어 사용자 결정에 올린다. 결정 전에는 구현하지 않는다.

측정 규칙: perf 프로세스 하나, 다른 빌드·job 정지, load average 기록. WSL2 tail(p95/p99) 편차는 throughput·mean 판정에 쓰지 않는다. runs=1로 걸러내고 채택 직전에만 runs=3.

## 5. 브리프 템플릿 (`c016-worklog/briefs/core-rf-<id>.prompt`)

```
목표(한 문장) / 원인 또는 항목 하나 / 상한 1.5 h
소유 계층·spec 조항: <파일, 절>
금지: 계약 변경(§4.1 다섯 검사), 공개 계약 테스트 기대값 수정, 새 옵션·새 규칙, 게이트 루프, 범위 밖 파일 수정
범위 파일: <목록>
절차: 읽기 → 설계 두 가지 비교(POSDDD) → 구현 → 관련 suite 5회 → (STREAM job) with_stream zlink,asio size all runs 1
보고: 결과, 변경 파일, 테스트·남은 실패, 성능 표(메시지당 비용 포함), 변경 분류(A/B/C/D), 계약을 바꿔야 했다면 어디서 멈췄는지
진행 파일: <worktree>/progress.md (3분마다 갱신)
```

## 6. 위험과 대응

- **머신 A와 충돌**: A가 Core 결함 수정을 push할 수 있다. 측정·포팅 전 `git pull --rebase`. STREAM drain 경계는 `7738b8fd41`의 규칙(도착한 fragment로 조립, bounded step)을 유지한 채 비용만 줄인다(D-099).
- **정확성 수정 되돌리기 금지**: bounded pump·credit 재admission 같은 것은 결함 수정이 이유다. 같은 규칙을 더 싸게 구현하는 방향만 허용.
- **벤치 서버 쪽 개선 유혹**: `stacks/zlink` 서버 코드를 고쳐 얻는 수치는 Core 개선이 아니다. 벤치 서버는 고정하고 Core만 바꾼다(벤치 서버의 명백한 낭비는 별도 항목으로 보고만).
- **메모리 11 GB**: 동시 job ≤ 4, JOBS≤6, valgrind job 단독. `pkill -f` 자기 패턴 금지, 3분 간격 프로세스 확인.
- **시간**: Phase 0+1 ≈ 2.5 h, Phase 2는 원인 수 × 1.5 h, Phase 3는 모듈당 인벤토리 1 h + 적용 1.5 h. 진행은 D-B14x와 §8에만, 과정 로그는 `c016-worklog/`.

## 7. 측정 표 (감독관이 채움)

### 7.1 with_stream (CCU 1000, 4 io threads, runs 3 median, kops/s)

**비교 스택(2026-09-08 15:10, 사용자)**: `zlink`, `asio`(순수 asio 서버, 참고), `zmq`(같은 pull 모델, 목표 1.0 근접), **`cppserver`**(asio 위 라이브러리 계층 — zlink와 같은 층위의 기준; zlink보다 높으면 그 차이는 asio가 아니라 zlink 계층 비용). 0.17.4부터 4 스택으로 측정한다(측정 job `measure-cppserver`, D-B253).

| 시점 | commit | 64 B zlink/asio | 1024 B zlink/asio | 65536 B zlink/asio | zlink 서버 CPU% (64/1024/64K) | load avg |
|---|---|---|---|---|---|---|
| 참고(A, 09-06 오전) | `core_after_a_release` | 271.3 / 349.0 = 0.78 | 246.5 / 316.1 = 0.78 | 29.1 / 37.2 = 0.78 | 353 / 358 / 228 | — |
| **Phase 0 기준** | `285f37792d` (lib 20:41), results/20260906_214620 | 268.9 / 322.0 = **0.835** | 243.0 / 316.4 = **0.768** | 30.4 / 39.2 = **0.775** | 332 / 334 / 223 (asio 283 / 290 / 311) | 측정 job 보고 참조 |
| S-4+S-10 (`597f134d68`) | 게이트 s4-s10 | 267.8 / 342.5 = 0.782 | 252.5 / 318.0 = 0.794 | 31.2 / 39.3 = 0.793 | — | 2.7 |
| +S-2+S-9 (`e1db6f1f72`) | 게이트 s2-s9 | 기준 이상(6셀) | | | — | — |
| +S-1 (`baaa68d67b`) | 게이트 s1 | 286.7 / 350.4 = **0.818** | 262.4 / 327.2 = **0.802** | 33.5 / 40.6 = **0.824** | — | — |
| **Phase 2S 종료, idle runs 3** (`2529709db6`, G-0b, D-B158) | results/20260907_0459xx~0503xx | 289.7 / 352.8 = **0.821** | 267.8 / 325.2 = **0.823** | 32.6 / 41.4 = **0.787** | — | 0.3~0.9 |
| **G-11b 채택, idle runs 3** (`5304885197`, measure-g11b3, D-B202; zmq 330.0 / 307.8 / 27.7 → zlink/zmq 0.90 / 0.90 / 1.22) | results/G-11b3-after-measure-20260907_180800 | 298.5 / 366.7 = **0.814** | 277.5 / 339.5 = **0.817** | 33.9 / 41.4 = **0.820** | — | 0.08 시작 |
| **MP 게이트, runs 1** (`29f4d8b45c` 직전 staged, gate-mp; zmq 342.8 / 308.0 / 28.9 → zlink/zmq 0.88 / 0.89 / 1.17) | results/20260908_032921 | 301.8 / 386.8 = 0.780 | 275.2 / 340.2 = 0.809 | 33.8 / 40.5 = 0.834 | — | 0.51 시작 |
| **0.17.2 idle runs 3** (`dca377aa5e`, measure-0172, D-B217; zmq 339.3 / 300.9 / 28.4 → zlink/zmq 0.86 / 0.91 / 1.22) | results/20260908_035802 | 290.7 / 370.2 = **0.785** | 272.9 / 334.7 = **0.815** | 34.7 / 41.5 = **0.836** | — | 1.94 시작(빌드 직후) |
| **6 스택 idle runs 3** (0.17.3 `0761c1d4d0` lib, D-B261, results/20260908_164225; asio_pull 271.7/260.4/17.2, cppserver 344.8/322.0/39.3, cppserver_pull 324.2/304.2/42.2, zmq 310.0/290.1/26.0) | zlink/asio_pull **1.005/0.942/1.911**, zlink/cppserver_pull **0.842/0.806/0.778** | 273.0 / 335.5 = 0.814 | 245.3 / 319.7 = 0.767 | 32.8 / 38.6 = 0.850 | pull 변형 서버 CPU +30~70 pp | 0.58 시작 |

### 7.2 perf/c 1024 B 경량 3셀 (tcp, Phase 0 기준 대비 비율)

| 셀 | Phase 0 | S-1 뒤 | R1 뒤 | … |
|---|---|---|---|---|
| ROUTER_ROUTER single | 1.00 (744.4 Kmsg/s) | | | |
| ROUTER_ROUTER_SENDSEND multi | 1.00 (111.5 Kops/s) | | | |
| ROUTER_ROUTER_REQREP multi | 1.00 (73.0 Kops/s) | | | |

### 7.3 hotpath_gate

| 셀 | reference(Phase 0 → 갱신) | 최종(MP 게이트 `gate-mp-summary.md`, `aef7015e0f` 기준) |
|---|---|---|
| stream_tcp (신설, D-B142) | 15540.39 → 14623.47 (S-1, D-B150) → **13969.81** (`aef7015e0f`) | 13969.81 (Phase 0 대비 −10.1 %) |
| router_router_tcp | 2972.88 → 2972.53 | 2966.84 (0.998) |
| dealer_dealer_inproc | 3455.38 → 3230.92 (G-2) | 3287.92 (1.018) |
| dealer_router_reqrep_inproc | 12054.89 → 18663.51 (G-2 셀 재정의) → **16455.38** (`aef7015e0f`) | 16455.38 (MP-9: async mailbox 왕복 4,996→70/5,000) |
| pair_inproc | 2505.36 → 2348.46 (G-2) | 2367.78 (1.008) |

### 7.4 perf/c 스크린 셀 (1024 B tcp, Phase 0 기준 대비 비율; Phase 2G 시작·Phase 4 종료 시 전 size로 확장)

Phase 0 절대값(1024 B tcp, runs 1, 22:02, 파일 `perf_c_single_linux_20260906_220210_phase0-screen.txt` / `perf_c_multi_linux_20260906_220258_phase0-screen.txt`):

| 셀 | 처리량 | mean latency | 2S 뒤 | 2G 뒤 | Phase 4 |
|---|---|---|---|---|---|
| single PAIR | 890.7 Kmsg/s | 0.027 ms | | | |
| single PUBSUB | 646.0 | 0.047 | | | |
| single DEALER_DEALER | 787.4 | 0.048 | | | |
| single DEALER_ROUTER | 767.6 | 0.049 | | | |
| single ROUTER_ROUTER | 744.4 | 0.048 | | | |
| ~~single DR_REQREP~~ | ~~418.7 Kops/s~~ | | 정책 제외 | | |
| ~~single RR_REQREP~~ | ~~396.8~~ | | 정책 제외 | | |
| multi DEALER_DEALER | 561.4 Kmsg/s | 54.6 | | | |
| multi DR_SENDSEND | 166.6 Kops/s | 1.18 | | | |
| multi RR_SENDSEND | 111.5 | 1.30 | | | |
| multi DR_REQREP | 95.2 | 1.68 | | | |
| multi RR_REQREP | 73.0 | 1.32 | | | |
| multi PUBSUB | 541.5 Kmsg/s | 1459 | | | |
| multi STREAM (CCU 100) | 124.2 Kops/s | 0.40 | | | |

**Phase 2G 기준(idle runs 3, D-B158·D-B159, HEAD `2529709db6`)** — Phase 0 multi 값은 부하 오염이라 폐기하고 이 값을 이후 판정 기준으로 쓴다: single PAIR 883.8 / PUBSUB 626.5 / DD 769.8 / DR 760.7 / RR 732.2 (DR_REQREP·RR_REQREP는 2026-09-07 정책 개정으로 single suite 제외 — `PERF_SINGLE_TEST_POLICY.md` §1); multi DD 905.1 / DR_SENDSEND 273.9 / RR_SENDSEND 242.5 / DR_REQREP 208.5 / RR_REQREP 170.1 / PUBSUB 1009.0 / STREAM 227.1. 전 size 절대 기준 파일: `perf_c_multi_linux_20260907_044847_phase2g-fullsize.txt`, `perf_c_single_linux_20260907_045258_phase2g-fullsize.txt`(이 둘은 인벤토리 job과 동시 측정이라 −5 % 판정 시 idle 재측정으로 확정).

**0.17.2 idle 재측정(2026-09-08 04:06~04:50, `measure-0.17.2-idle-summary.md`, D-B217)** — 1024 B tcp, runs 3: single PAIR 753.8 / PUBSUB 670.9 / DD 772.9 / DR 775.1 / RR 733.1 (Phase 2G 대비 85.3 / 107.1 / 100.4 / 101.9 / 100.1 %); multi DD 908.7 / DR_SENDSEND 221.6 / RR_SENDSEND 181.2 / DR_REQREP 170.8 / RR_REQREP 137.5 / PUBSUB 848.8 / STREAM 210.2 (Phase 2G 대비 100.4 / 80.9 / 74.7 / 81.9 / 80.9 / 84.1 / 92.5 %). **판정 유보**: Phase 2G 기준(09-07 04:xx)은 머신 A의 러너 정합(측정 모델 변경, `87153dd4f3`·`d634417a37`·`d51c16b285` 등) 이전 값이라 같은 조건이 아니다. 같은 보고서의 전 size raw 대비는 single PAIR(0.91/0.68/0.78/0.92)를 제외하면 대부분 1.0 이상. 귀속은 attrib-0172(같은 러너, lib만 `5304885197`↔0.17.2 교대)로 확정한다.

**귀속 결과(attrib-0172, `attrib-0.17.2-summary.md`, D-B219)**: 같은 러너 binary·`LD_PRELOAD` 고정, base(`5304885197`)↔0.17.2 교대 2회. 두 교대 모두 −5 % 이하인 셀 없음 → **기준 불일치**. single PAIR 65536 B 99.7 %, DD 65536 B 100.9 %(65536 B 하락 없음); multi DR_REQREP 79.7 % → 98.4 %(1회 오염), RR_SENDSEND 94.7/97.2 %. 단, single 1024 B는 PAIR 93.7/96.4 %(합산 95.0 %), DD 94.0/97.3 %(95.6 %)로 두 교대 모두 current가 낮았다(current는 항상 base 직후 실행돼 잔류 load 1.4~1.7). 게이트(−5 %) 안이지만 **관찰 항목**으로 남기고 0.17.3에서 순서를 뒤집은(current→base) idle 교대로 재확인한다. **이후 §7.4 기준 = 0.17.2 idle 값(위 표)**로 갱신한다(러너 정합 이후 조건).


### 7.5 D(spec gap) 후보 — 사용자 결정 대기

**D-B270(09-08)**: STREAM 관련 대기 항목(D-S1·D-a·D-b·D-f)은 0.17.5에서 항목별 A/B 구현·측정(with_stream 6 스택·hotpath·RSS, idle 3-run) 결과로 확정한다. 계약 변경이 있는 D-a·D-b는 실험 patch로만 측정하고 착지하지 않는다. 순서 B1·B2 → D-f → D-S1 → D-a·D-b.

| # | 발견 job | 바꿔야 하는 계약(스펙 절·문장) | 예상 이득 | 결정 |
|---|---|---|---|---|
| D-a | S-B | 08-stream §4 118-120·README part send: 앱 send를 N개/T µs 묶어 I/O 스레드에 알림 → 제출 경계 지연 관측(§4.1-3) | 핸드오프 command 수 감소 | 대기 |
| D-b | S-B | 05-polling POLLIN/POLLOUT level 조건: `poller_wait`의 command drain을 rdtsc로 스킵 | poller 비용 | 대기 |
| ~~D-c~~ | S-B | ~~핸드오프 제거~~ — **철회(2026-09-07, 사용자 지적)**: zlink는 pull 모델이라 I/O↔앱 핸드오프는 설계 자체이며 zmq도 같다. 비교군을 asio(push)가 아니라 zmq(pull)로 두고 핸드오프 단가(command 2회/msg, eventfd, ctxsw 2×)를 줄이는 것만 대상 | — | 철회 |
| D-d | S-B | 06-auto-hwm 스냅샷 정의: credit published store를 경계에서만 | 소 | 대기 |
| D-e | S-11 | 04-thread-safety 소유권: 공개 receive lease가 command owner를 배타하도록 할지(`receive_once_guarded`·fq active partition의 TSan race). 계약 문장 변경이 아니라 소유 규칙 결정 + 성능 예산 | 정확성(잠재 race 제거), 성능은 −일 수 있음 | 대기 |
| D-f | R2 | 08-stream "런타임 기본값": `ZLINK_ASIO_STREAM_GATHER_THRESHOLD`·`..._TINY_GATHER_THRESHOLD`·`..._DISABLE_GATHER` env가 S-4·R2 이후 어떤 동작에도 영향 없음(STREAM raw 엔진은 gather 불가). 접근자·문서 삭제는 스펙 문장 변경 | 구조(죽은 knob 3개 제거) | 대기 |
| ~~D-g1·D-g2~~ | G-1 | **철회(2026-09-07, 사용자)**: thread-safe 소켓은 zlink의 설계 철학이다. 앱 간·앱↔I/O 잠금을 계약 완화로 없애는 제안은 올리지 않는다. 남은 잠금 ~15쌍(≈ zmq 대비 격차의 대부분)은 그 철학의 대가로 받아들이고, 계약 안에서 "아무것도 지키지 않는 잠금"만 계속 찾아 없앤다(S-1·G-1 방식) | — | 철회 |
| D-MP1 | MP-1 (D-BP12) | socket README §2·part send(:944)·Message :108: thread별 독립 multipart sequence 동시 보관, 같은 record는 같은 thread, FINAL 원자 admission, close 시 전부 폐기, slot 수명은 sequence | 동시 multipart 제출 지원(A 버그), marker·suspend/resume·control lease 제거(규칙 6→3) | **확정(2026-09-07, 사용자 방향 동의·D-B198)** — 스펙 반영, MP-2 구현 |
| D-MP2 | MP-1 | README :440-446·:1312: public `MORE` 조립 buffer는 pipe HWM 밖, 판정은 FINAL의 frame 단위(현행 코드·테스트와 일치, 문장 명료화), total-known 예외 확대 없음 | 계약 모순 제거 | 확정(D-B198) |
| D-MP3 | MP-1 | ZMP :241-265·:543, ROUTER :419: control(FLOW/WEIGHT) 보류를 실제 pipe multipart write 구간으로 한정 — public 조립 buffer만 있는 동안 control 진행(**관찰 동작 변화**: MORE 후 FINAL을 미루는 caller가 control을 막지 않음) | 전역 boundary 상태 제거 | 확정(D-B198) — 사용자 재확인 요망 |
| D-MP4 | MP-1 | README :1071, ZMP :472 "admission 전 payload 미보관": 호출 사이 조립 buffer와 거절된 record 미보관을 구분 | 문장 정합 | 확정(D-B198) |
| D-MP5 | MP-1 | ROUTER :54 family/RID 혼합 금지를 같은 thread의 sequence로 한정 | 문장 정합 | 확정(D-B198) |
| D-W1 | Windows 검증(A, 09-07) | Windows Core DLL의 CRT 링크: `/MD`(현재) + Java 22에서 `msvcp140.dll` access violation vs `/MT`(충돌 없음). 패키징 정책 결정 필요 | Windows Java/Framework 안정성 | **대기** — 권고 `/MT` 배포 또는 두 변형; A Windows 재현으로 확정(D-B230) |
| D-S1 | S-D(sol, 09-08) | `rcvbuf=-1`일 때 STREAM decoder read target max(현재 4 KiB)를 OS default socket buffer 크기까지 올릴지 — CCU별 user-space buffer 메모리 상한 정책이 먼저 필요 | 64 KiB 처리량/latency vs connection당 RSS | **대기** — D-B267; B2(성장 규칙 단순화)는 기존 max 유지라 별개로 진행 가능 |
| D-H1 | CCU-2(09-08) | 06-auto-hwm §2 "연결 증가로 목표 감소 → 새 목표를 즉시 기록": attach 증분 plan 확장(O(N²) 결함 수정)은 새 방향만 즉시, 기존 방향 인하는 debounce(3000 ms)까지 지연(연결 2048개 초과 구간) | CCU 4000 PASS(0→202.8 kops) vs 일시적 admission 느슨함(상한 있음) | **채택(D-B269, 09-08 18:20)** — §2 표 한 행 갱신(ko/en), CCU-2는 0.17.4에 포함 |
| 관찰 | S-A | 64 KiB에서 zlink 서버 앱 스레드 1개가 93 % 포화(I/O 스레드 45 % idle). 벤치 서버 구조(앱 스레드 1개) 문제이며 Core 계약과 무관 — asio 스택은 io 워커 8개에서 read→write 직결 | — | 기록 |

## 7.6 머신 A(bindings 성능 작업)와의 조율

사용자가 다른 머신에서 bindings 라이브러리 성능 작업을 시작한다(2026-09-07). 그쪽은 측정 내내 **같은 Core 라이브러리**를 써야 하므로 main을 따라오지 않고 **한 커밋에 고정**한다.

- 고정 방법: `git worktree add --detach ~/project/zlink-core-base <SHA>` → `JOBS=4 scripts/build-core.sh release --lib-only` → 러너에 `ZLINK_CORE_SOURCE=local` + 그 트리의 `core/build` 경로. with_stream 러너는 기본이 release 다운로드(404)이므로 local 명시 필수.
- 고정 시점: **errno 정정(잘못된 send flags: ENOTSUP → EINVAL, 03-errors §2)이 포함된 커밋 이후**. 관측 가능한 변화라 측정 중간에 바인딩을 고치는 일을 피한다. 감독관이 그 게이트 통과 직후 태그를 만들고 SHA를 알린다.
- 이 캠페인은 `core/` 와 `bindings/c/{perf,bench}` 만 건드리므로 다른 바인딩 디렉터리와 파일이 겹치지 않는다.
- 캠페인 시작(`285f37792d`) 이후 `core/include` · `core/src/libzlink.vers` diff는 버전 매크로 외에 비어 있다(ABI 불변). 버전은 0.17.1(A 고정용, `4cd03b9173`) → **0.17.2**(`dca377aa5e`, tag `core/v0.17.2`, 2026-09-08). A는 D-BP14 절차로 0.17.2에 재고정한다. 동작 변화는 결함 수정 3건뿐: close의 `CLOSE_BUSY` 경합, 비-STREAM drain 중복 디코딩, send flags errno.

### 7.7 동기화 모델 인벤토리와 목표

스펙 [`systems/11-synchronization-model`](../../core/doc/spec/core/systems/11-synchronization-model.ko.md)의 규칙과 현재 코드의 차이. STREAM tcp 1024 B 셀(CCU 20, callgrind)에서 message당 mutex 획득 횟수. 착지할 때마다 갱신한다.

| lock | 스펙 분류 | 두 번째 thread | 2026-09-07 (G-11a 뒤) | 목표 | job |
|---|---|---|---|---|---|
| mailbox 삽입점 `_sync` | §3.3 여러 producer | 여러 thread | 2.7 | 2.7 (구조) | — |
| socket 직렬화: `public_api_sync` + command owner + command마다 `receive.sync` | C2 → turn 하나 | application thread와 command owner | 1.47 | turn의 CAS만 | G-11 2a |
| `read_activated` / `has_in`의 receive partition | C2 | 위와 같은 클러스터 | 1.28 | 0 | G-11 2a |
| session 쪽 `pipe_t::write`/`flush`의 `_out_sync` | §3.2 SPSC + C3 | I/O thread 하나뿐 | ~~2.0~~ → 0 (`5304885197`, seqlock C3 ledger; stream_tcp 셀 mutex 24.05→21.78/msg) | 0 | G-11b(2c) **완료** |
| socket 쪽 `_out_sync` | C2 → turn | application thread | 1.0 | 0 (cold 경로는 유지) | G-11 2b |
| route shard `sync` | C1 | 조회만 hot | 1.0 | 0 (스냅샷 조회) | G-11 2d |
| public poller handle 표 | C1 | 조회만 hot | 0.56 | 0 | G-11 2e |
| boost.asio 내부 | Core 밖 | — | 4.0 | — | — |
| **합계** | | | **15.1** | **≈ 8.3 (Core 소유 10.1 → 3.3)** | |

제거된 "지키는 조건이 없던 lock"의 기록: `activate_read` 처리의 `_out_sync`(S-1), 항상 비어 있던 지연 종료 큐의 context lock과 planned=applied일 때의 registry lock(G-1), PAIR command의 turn 예외(G-11a), `fast_mutex_t` 재귀 mutex → `mutex_t`/`recursive_mutex_t` 분리(S-2). libzmq 비교(socket lock 하나 안에서 command 처리, pipe mutex 0, mailbox 1.7)와 출처 분석은 `c016-worklog/core-rf-G-11-lock-provenance.md`.

## 8. 체크리스트

- [x] Phase 0: Release 빌드, with_stream 기준 표(§7.1), 경량 3셀 기준, hotpath `stream_tcp` 셀 커밋(`6f64e76b51`, D-B142 — harness가 I/O 스레드를 안 세던 결함도 수정)
- [x] Phase 1: S-A 프로파일 표, S-B 경로 비용 표 → 원인별 job 목록(D-B140·D-B141). G-A(공통 경로 프로파일)는 Phase 2G 시작 시 수행
- [x] Phase 2S(종료 2026-09-07 05:15, D-B158): 채택 S-4·S-10(`597f134d68`), S-2·S-9(`e1db6f1f72`), S-1(`baaa68d67b`), S-12(`73e6c54c60`), S-11(`2529709db6`); 기각 S-3·S-5(측정으로 반박). 축소셀 Ir/msg 11,096 → 9,474(−14.6 %), hotpath stream_tcp 15540 → 14623(−5.9 %), idle with_stream zlink 절대 +7.7/+10.2/+7.2 %, zlink/asio 0.835/0.768/0.775 → 0.821/0.823/0.787(목표 0.95 미달 — 남은 격차는 §7.5 D-c 핸드오프 구조와 앱 스레드 1개 관찰)
- [ ] Phase 2G: G-0 idle 재기준(D-B158·159), G-A(D-B166), G-5(`7549a128b1`), **G-2(`749145fded`, 5셀 −1.5~−7.1 %)**, **G-1+G-3(`99f0294377`, 5셀 0.956~0.992)** 착지. **G-11 잠금 출처 분석 채택(D-B178·179)** → G-11a 착지(`1a15660a18`, 명령 드레인 항상 turn; hotpath reqrep −5.2 %, stream −3.0 %) → 진행 G-11b(2c, session 쪽 `_out_sync`) → 2a·2b·2d → step 3, 각 게이트. 스펙: `systems/11-synchronization-model`(ko/en) 신설(`389078a68f`), 착지마다 §7 표 갱신. 진행 S-14(단일 lane 회계 분류 경합, 기존 결함 D-B182). 대기 G-10(clock_gettime)·G-7(eventfd)·R10-B 게이트. 폐기 G-6·G-R1(정책). 재기준: G-11 시리즈 착지 후 idle runs 3(A의 새 러너 + G-5)
- [ ] Phase 2G: 스크린 셀 재측정 표, G-1 … (각 채택/기각, 커밋 해시, 패턴별 전 size 비율)
- [x] Phase 3 apply(2026-09-07 12:30): R1+R2(`cb9139d16d`), R3+R4(`72100c7be3`), R5·R6R8·R9·R7R11(`2753a2d799`) 착지 = **−2,664/+1,001행**; R10-B apply 완료(게이트 대기). 인벤토리 오류 3건을 apply job이 걸러냄(R4 #3a, R6 #2, R7 #6). 보류(설계 job·D): pipe.cpp 개념별 분할(익명 helper 공유 헤더 선행), ws/wss 쌍둥이 병합, lb::sendpipe, route-binding cache(D), `oversize_admission_out_`(D 확인), registry `recursive_mutex_t` 필요성
- [x] 동시 multipart 제출 지원(D-BP12 → D-B197~D-B214, 2026-09-08 03:40 착지): MP-1 설계(A안) → MP-2 구현 → 독립 리뷰 3회(차단 6+6+1건 전부 수정, MP-3/8/9) → MP-4/5 single fast path 복원 → MP-6 D-BP15 테스트 → MP-7 completion drain 결함 수정. 게이트 `gate-mp-summary.md`: ctest 209/209, suite 97×3, mirror 12/12, hotpath reqrep 0.882·stream 0.955(reference 갱신 `aef7015e0f`), with_stream idle ±1 %. 스펙 8 파일 동반 커밋(D-MP1~5, completion pull 명료화, TLS destructor·인계 규칙).
- [x] Phase 4(2026-09-08): hotpath 5셀 PASS(reference 갱신), ctest 전체 209/209, 스펙 문구 정합(`6ef6cfaaf3` + MP 스펙), 버전 **0.17.2** bump `dca377aa5e`, tag `core/v0.17.2`(2026-09-08 03:58, D-B216; 머신 A 재고정 요청). idle 재측정(perf/c 전 size·with_stream 3회)은 bump 뒤 별도 기록(사용자 결정 D-B214: 일정 단축).
- [ ] **다음 캠페인(0.17.5, 메시지당 명령 수 — D-B260 S-C)**: SC-1 단일-part receive 인계 통합, SC-2 send admission 전달 계층 통합, SC-3 Asio completion 표현·allocator 진단, SC-4 output drain 판정 통합(각 3 h; 목표 CCU20 9,041→8,040~8,440 Ir/msg, zlink/zmq 0.94~1.0). 비교 스택 zlink/asio/asio_pull/cppserver/cppserver_pull/zmq(D-B253·254), S-D 결과(D-B267): **B1** `restart_input()` speculative read를 full-read evidence gate와 통합(실패 recvfrom 1/msg 제거), **B2** decoder read target 성장 규칙 단순화(full hit 1회 → 2×, max clamp; 64 KiB frame 2-read 분할·fragment copy 197.8k Ir/msg 해소) 각 2~3 h, SC-1~4보다 앞순위; 계측 app suffix 결함 선수정; D-S1(`rcvbuf=-1` decoder max 정책) 사용자 결정.
- [ ] 0.17.3 이월: ~~ST-1 STREAM packet pump 정체 수정~~ **착지 `de730d4ac5`(D-B229; receive 소유권 프로토콜, D-e 종결)**, single tcp 1024 B PAIR/DD −4~5 % 관찰 재확인(D-B219), G-11 2a/2b/2d(socket 쪽 `_out_sync`·receive partition·route shard, 목표 lock/msg 8.3), backlog(`_slot_sync`, mailbox 예외 경로, `receive_once_guarded`(D-e), pipe.cpp 분할, ws/wss 병합, lb::sendpipe, R7 #4/#7, R11-B), TSan 기존 debt 5건(monitor/ctx lock-order, lb peer-weight).
