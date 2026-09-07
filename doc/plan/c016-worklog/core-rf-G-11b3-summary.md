# Core RF G-11b-3 결과 — G-11b-2 리뷰 수정

## 결과

G-11b-2의 session writer `_out_sync` 제거와 C3 ledger 발행을 유지하면서 독립 리뷰의 차단 항목
3건을 수정했다. Seqlock reader의 홀수 sequence 재시도는 초기화되지 않은 값을 읽지 않는 표준
순서로 바꿨고, local `msgs_read`·`bytes_read`도 같은 coherent snapshot에 포함했다. 이 두
방향은 하나의 공통 seqlock helper를 사용한다. `_out_active` 코드는 바꾸지 않았으며 아래에서
CAS와 store의 실제 범위를 바로잡아 서술한다.

정확성 검증에서 신규 TSan 경고는 0이고 lost-wake는 100/100 통과했다. 다만 요청된
with_stream 3회 비교에서 zlink/asio 비율이 1024 B와 64 KiB에서 pristine보다 각각 12.85%,
16.55% 나빴다. 최종 측정의 system CPU가 pristine의 약 37%보다 높은 52~61%였지만, 지시대로
재측정으로 덮지 않고 **채택 판단을 감독관에게 넘긴다**.

공개 header, `core/src/libzlink.vers`, 스펙 문서와 test는 수정하지 않았다. commit과 stash도
실행하지 않았다.

## 리뷰 수정 3건

1. **Seqlock reader UB 제거.** `pipe.cpp:798-823`은 `seq1`을 acquire load하고 홀수이면 값에
   접근하지 않은 채 재시도한다. 짝수일 때만 두 atomic 값을 읽고 `seq2`를 acquire load한 뒤
   `seq1 == seq2`인 경우에만 local 결과를 output에 채택한다. 모든 비교 피연산자가 초기화되어
   G-11b-2의 `do/while` 첫 odd 경로 UB가 없다.
2. **Local read pair 일관성.** `_inbound_ledger_sequence`를 추가하고 complete-message read와
   hiccup reset을 `odd → msgs/bytes → even`으로 발행한다(`pipe.cpp:825-839,3309-3314,
   3903-3919`). `get_pending_snapshot()`은 peer written pair와 local read pair를 각각 stable
   sequence로 읽은 뒤 receive 합계를 만든다(`pipe.cpp:848-894`).
3. **`_out_active` 서술 정정.** 경쟁 가능한 async credit recovery인
   `refresh_write_credit`, decoder reservation recovery와 `process_activate_write`만 false→true
   CAS로 단독 확정한다(`pipe.cpp:1075-1093,1459-1473,2896-2911`). Owner-local
   arm→recheck(`:1863-1876`), transport release(`:1930-1951`), flow resume(`:2138-2155`),
   hiccup/lifecycle(`:2961-2971,3213-3214`)은 owner 또는 `_out_sync` 아래 release store다.

## 소유권과 스펙 대조

소유 계층은 Core `pipe_t`다. Socket 쪽 끝은 socket turn이, session 쪽 끝은 connection I/O
thread가 단독 소유한다. 반대편과 monitor가 읽는 ledger만 C3 atomic으로 발행한다. Queue,
credit 값·산식, wake 조건과 topology 수명은 바꾸지 않았다.

Synchronization model §3.2(`11-synchronization-model.ko.md:94-115`)의 “그 끝을 소유한
thread만 쓴다”, “반대쪽 끝이 읽어야 하는 값 … 만 C3 atomic으로 발행한다”, release store /
acquire load 규칙을 따른다. §6(`:192-208`)이 요구하는 lock 수, wake 불변, TSan delta,
lost-wake와 회계 값 검증을 아래에 기록했다.

Monitoring §6.3의 적용 문장은 다음과 같다(`06-monitoring.ko.md:176-180`).

> `snd_pending_msgs`, `rcv_pending_msgs`, `snd_pending_bytes`, `rcv_pending_bytes`,
> `snd_bytes_in_flight`, `rcv_bytes_in_flight`로 구성된 pipe 합계 field 군은 하나의 lock
> 안에서 읽으므로 그 field 군 안에서 일관된다.

여기서 “하나의 lock”은 socket-side `_out_sync`가 topology와 local peer-credit을 고정하고,
그 lock 밖 single writer의 두 directional ledger는 stable even sequence만 채택하는 C3 snapshot을
통해 같은 일관성 경계에 들어온다는 뜻이다. Auto-HWM과 flow counter의 교차 일관성은 스펙대로
별도다.

Auto-HWM §4의 “charge가 queue 회계에 반영되기 시작하는 시점은 frame write부터다”와 “Core
HWM charge의 종료 경계는 complete message를 queue에서 dequeue해 binding에 넘기는 시점이다”를
재확인했다(`06-auto-hwm.ko.md:434-435,467`). Connection memory §3.1도 charge가 frame이 queue에
있는 동안만 connection 비용이라고 규정한다(`05-connection-memory.ko.md:57`). 산식·증가·반환
지점은 변경하지 않고 발행 일관성만 바꿨으므로 **어느 문장도 다른 동작이 되지 않았다**.

교차언어 대조: C/C++/.NET/Java/Kotlin/Node/Rust/Python binding은 모두 같은 Core C ABI와 이
`pipe_t` 구현을 사용한다. 언어별 독립 pipe/accounting runtime이 없어 한 언어에만 별도 보상
경로나 상태를 추가하지 않았다.

## 설계 비교

| 대안 | 판정 |
|---|---|
| Monitor가 source socket의 receive turn을 획득 | 기각. Monitor가 application hot-path 소유권에 들어가고 cold snapshot 때문에 receive 진행을 기다리는 새 규칙이 생긴다. |
| `msgs_read`·`bytes_read`를 독립 atomic으로만 유지 | 기각. Data race는 없지만 서로 다른 complete-message 경계를 조합할 수 있어 Monitoring §6.3을 충족하지 못한다. |
| 두 full-width atomic과 single-writer sequence | 선택. 64-bit 범위를 줄이지 않고 monitor reader만 stable snapshot을 재시도하며 outbound와 동일한 C3 규칙을 재사용한다. |

3차 수정 전/후 규칙 수: **outbound seqlock + inbound 독립 발행의 2개 규칙 → 두 방향 모두 공통
seqlock 발행/조회 1개 규칙**. Writer가 odd 상태에서 deschedule되면 reader가 기다릴 수 있으나
writer 구간에는 blocking/callback이 없고, 현재 spec에는 snapshot progress 상한이 없다.

## 검증

모든 build는 `JOBS=4`로 실행했다. Valgrind와 benchmark는 `pgrep -x ninja`가 비어 있을 때만
지정 `PERF_LOCK`의 `flock` 아래 foreground로 실행했다.

| 검증 | 결과 |
|---|---|
| pristine/최종 Release LTO local Core, 최종 dev build | PASS / PASS / PASS |
| 지정 정규식 suite(현재 목록 105개) ×5 | **525/525 PASS**, 633.41 s |
| lost-wake 5종 `until-fail:20` | **100/100 PASS**, 619.62 s |
| `test_close_completion_poller_release` `until-fail:50` | 후반 1회 10.02 s timeout; 즉시 단독 재실행 PASS. 기존 문서화된 동일 간헐과 일치 |
| monitor/accounting 18종 `until-fail:10` | 17종 10/10 PASS; 기지 간헐 `test_single_lane_flow_snapshot_accounting`은 8 PASS 뒤 1 FAIL(5 s wait), 단독 재실행 PASS |
| 수동 TSan, LTO OFF, `setarch -R`, 4 target | pristine **10+1+9+0 = 20**, 최종 **10+1+9+0 = 20**, signature delta 0 |
| Release/LTO `hotpath_gate` | **5/5 PASS** |
| with_stream zlink/asio × all size ×3 | 18/18 run 완료, mismatch 0 |
| `git diff --check`; 보호 경로 diff | PASS; `core/include`, `libzlink.vers`, `core/doc/spec` 변경 없음 |

TSan signature는 전후 모두 `ypipe_t<command_t>::check_read()` 17건과 기존 receive guard 3건이다.
새 ledger/seqlock 관련 signature는 0건이다. Close timeout은 with-monitor case 완료 뒤
without-monitor case에서 누적되었고, `test_single_lane_flow_snapshot_accounting` 실패는 기존과 같은
line 2842의 5초 accounting wait 만료였다. 둘 다 단독 즉시 실행은 0.02초에 통과했으며 이번
3차 변경에 맞춘 timeout·재시도·expectation 변경은 하지 않았다.

## Lock/명령어 수와 hotpath 5셀

축소 dev `stream_tcp` cell은 20,000 message다.

| 지표 | pristine | 3차 최종 | 변화 |
|---|---:|---:|---:|
| `pthread_mutex_lock` calls/msg | 24.0528 | 21.7794 | **-2.2734** |
| 전체 Ir/msg | 15,744.753 | 15,628.428 | **-116.325 (-0.74%)** |

Release/LTO 5셀은 다음과 같다.

| cell | reference | measured | ratio | 판정 |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3230.922 | 3271.091 | 1.0124 | PASS |
| dealer_router_reqrep_inproc | 18663.506 | 18656.468 | 0.9996 | PASS |
| pair_inproc | 2348.457 | 2330.784 | 0.9925 | PASS |
| router_router_tcp | 2972.532 | 2913.024 | 0.9800 | PASS |
| stream_tcp | 14623.471 | 14274.368 | 0.9761 | PASS |

## Locked RMW 우려 — pristine 대비 with_stream 비율

두 측정은 동일 detached HEAD에서 patch를 내린 pristine과 3차 최종을 각각 Release/LTO로
빌드하고, `--stack zlink,asio --size all --ccu 1000 --runs 3 --reuse-build`로 실행했다.
결과 디렉터리는 각각
`bindings/c/bench/with_stream/results/G-11b3-pristine-dfe6ec-runs3/`와
`bindings/c/bench/with_stream/results/G-11b3-after-runs3/`이다.

| size | pristine zlink/asio (kops) | pristine 비율 | 최종 zlink/asio (kops) | 최종 비율 | 비율 변화 |
|---:|---:|---:|---:|---:|---:|
| 64 B | 297.852 / 366.667 | 0.812322 | 175.182 / 195.266 | 0.897145 | **+10.44%** |
| 1024 B | 273.137 / 335.835 | 0.813308 | 129.760 / 183.070 | 0.708801 | **-12.85%** |
| 64 KiB | 33.557 / 40.946 | 0.819557 | 17.970 / 26.275 | 0.683918 | **-16.55%** |

Pristine 시작 load average는 `2.30 1.46 1.49`, 최종은 `3.11 2.08 1.30`이었다. 결과 CSV의
median system CPU도 pristine 약 37%에서 최종 52~61%로 높아 절대 throughput 하락을 patch에
단독 귀속할 수 없다. 그러나 asio로 정규화한 비율도 1024 B와 64 KiB에서 pristine보다
나빠졌으므로 locked RMW cycle 우려가 해소됐다고 판정하지 않는다. **성능 조건상 채택 여부는
감독관 판단 사항이다.**

## 변경 파일과 판정

- `core/src/runtime/core/pipe.hpp`: inbound sequence와 공통 coherent ledger helper 선언
- `core/src/runtime/core/pipe.cpp`: 안전한 standard reader 순서, 양방향 공통 발행, local read pair snapshot
- `core/src/runtime/sockets/common/socket_base_monitor.cpp`: G-11b-2의 pipe 합계 단일 snapshot 사용 유지

- 소유 계층: Core `pipe_t`; 각 pipe 끝의 single writer와 monitor reader 사이 C3 발행.
- 스펙 조항: Synchronization model §3.2·§6, Monitoring §6.3; Auto-HWM §4와 Connection memory §3.1의 charge 경계 유지.
- 교차언어 대조: 모든 binding이 같은 Core C ABI/`pipe_t`를 사용하므로 언어별 runtime 변경 없음.
- 변경 분류: **B — 기존 결함 수정.** G-11b-2 reader UB와 불완전한 monitor snapshot을 소유 모듈에서 수정.
- 멈춘 지점: correctness 구현·검증은 완료. 1024 B/64 KiB zlink/asio 비율 악화 때문에 최종 채택 결정은 감독관에게 넘김.
