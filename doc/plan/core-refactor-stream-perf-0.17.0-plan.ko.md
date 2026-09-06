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

목표: 세 크기 모두 zlink/asio ≥ 0.95 (64·1024 B는 zmq도 넘는다). 중간 목표 0.90.

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
- 스펙은 **문구 정합만**(내부 구조·파일 배치 서술). 계약 문장은 손대지 않는다.
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

| 시점 | commit | 64 B zlink/asio | 1024 B zlink/asio | 65536 B zlink/asio | zlink 서버 CPU% (64/1024/64K) | load avg |
|---|---|---|---|---|---|---|
| 참고(A, 09-06 오전) | `core_after_a_release` | 271.3 / 349.0 = 0.78 | 246.5 / 316.1 = 0.78 | 29.1 / 37.2 = 0.78 | 353 / 358 / 228 | — |
| Phase 0 기준 | `285f37792d` | | | | | |
| S-1 뒤 | | | | | | |

### 7.2 perf/c 1024 B 경량 3셀 (tcp, Phase 0 기준 대비 비율)

| 셀 | Phase 0 | S-1 뒤 | R1 뒤 | … |
|---|---|---|---|---|
| ROUTER_ROUTER single | 1.00 | | | |
| ROUTER_ROUTER_SENDSEND multi | 1.00 | | | |
| ROUTER_ROUTER_REQREP multi | 1.00 | | | |

### 7.3 hotpath_gate

| 셀 | reference | 최신 |
|---|---|---|
| stream_tcp (신설) | | |
| router_router_tcp | 2972.88 | |
| dealer_dealer_inproc | 3455.38 | |
| dealer_router_reqrep_inproc | 12054.89 | |
| pair_inproc | 2505.36 | |

### 7.4 perf/c 스크린 셀 (1024 B tcp, Phase 0 기준 대비 비율; Phase 2G 시작·Phase 4 종료 시 전 size로 확장)

| 셀 | Phase 0 | 2S 뒤 | 2G 뒤 | Phase 4 |
|---|---|---|---|---|
| single PAIR / DD / DR / RR / PUBSUB / DR_REQREP / RR_REQREP | 1.00 | | | |
| multi DD / DR_SENDSEND / RR_SENDSEND / DR_REQREP / RR_REQREP / PUBSUB / STREAM | 1.00 | | | |

### 7.5 D(spec gap) 후보 — 사용자 결정 대기

| # | 발견 job | 바꿔야 하는 계약(스펙 절·문장) | 예상 이득 | 결정 |
|---|---|---|---|---|
| | | | | |

## 8. 체크리스트

- [ ] Phase 0: Release 빌드, with_stream 기준 표(§7.1), 경량 3셀 기준, hotpath `stream_tcp` 셀 커밋
- [ ] Phase 1: S-A 프로파일 표, S-B 경로 비용 표, G-A 공통 경로 표 → 원인별 job 목록 S-·G-(D-B141)
- [ ] Phase 2S: S-1 … (각 채택/기각, 커밋 해시, zlink/asio 비율 추이)
- [ ] Phase 2G: 스크린 셀 재측정 표, G-1 … (각 채택/기각, 커밋 해시, 패턴별 전 size 비율)
- [ ] Phase 3: R1 stream · R2 raw/engine · R3 pipe · R4 socket_base · R5 request_reply · R6 session/registry · R7 api/core · R8 sockets 나머지 · R9 transports · R10 core 나머지 · R11 utils (인벤토리 → 적용, 커밋 해시)
- [ ] Phase 4: 최종 표, hotpath 5셀 PASS, ctest 전체, 스펙 문구 정합, 종결 D-B1xx
