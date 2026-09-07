# Core RF G-11b-2 결과 — session writer ledger C3 발행

## 결과

G-11b 1차에서 확인한 monitor race를 Core pipe에서 수정했다. Session I/O writer는 `write()`와
`flush()`에서 `_out_sync`를 잡지 않으며, 다른 thread가 읽는 outbound ledger만 C3 atomic으로
발행한다. 최종 TSan 경고는 pristine과 동일한 20건이고, Release/LTO hotpath gate도 모두
통과했다.

공개 header, `libzlink.vers`, 스펙 문서와 test는 수정하지 않았다. commit과 stash도 실행하지
않았다.

## 원인과 소유권

Session endpoint의 queue와 writer가 소유한 local 상태는 connection의 I/O thread가 단독으로 쓴다. 그러나
socket을 소유한 thread의 monitor snapshot은 반대 endpoint의 `_msgs_written`·`_bytes_written`을
읽는다. 기존 getter가 `_out_sync`를 잡아도 session writer가 같은 lock을 생략하면 이 접근은
동기화되지 않는다. G-11b 1차의 신규 TSan 21건이 이 경계를 직접 확인했다.

적용한 경계는 다음과 같다.

- `_msgs_written`·`_bytes_written`, `_peers_msgs_read`·`_peers_bytes_read`, `_out_active`를 atomic으로
  바꾸고 다른 thread의 모든 읽기를 acquire load로 통일했다. 발행은 release store이며
  credit recovery가 `_out_active`를 false→true로 바꾸는 전이는 CAS가 단독 확정한다. Lock을
  소유한 lifecycle·flow 전이는 release store를 사용한다.
- Monitor가 함께 보고하는 message/byte 합계는 각각 읽지 않는다. Writer가 두 64-bit 값의
  범위를 줄이지 않고 sequence로 묶어 발행하고, `get_pending_snapshot()`은 앞뒤의 짝수 sequence가 같은
  경우에만 두 값을 사용한다. Monitor 쪽 `_out_sync`는 pipe topology와 peer-credit 쌍을 한
  구간에서 고정하며 topology 변경과 monitoring에서만 잡는다.
- 같은 monitor reader 경계를 갖는 oversize admission count/max도 atomic으로 발행한다. Count는
  원자적으로 증가시키고 max는 더 큰 값일 때만 CAS로 바꾼다. 정상 message에서는 기존 oversize
  조건이 false이므로 이 연산은 실행되지 않는다.
- Charge 값과 경계는 바꾸지 않았다. Frame write부터 charge를 반영하고 complete message dequeue에서
  반환한다. Hiccup은 새 queue generation을 설정할 때 written/peer-credit을 0으로 발행한다.

회계 불변의 직접 근거는 Auto-HWM §4의 “charge가 queue 회계에 반영되기 시작하는 시점은 frame
write부터다”와 “Core HWM charge의 종료 경계는 complete message를 queue에서 dequeue해 binding에
넘기는 시점이다”이다. Connection memory §3.1도 “charge는 frame이 queue에 있는 동안만
connection의 memory 비용”이라고 같은 보유 기간을 규정한다. 이번 변경은 이 산식·증가·반환
지점을 건드리지 않고 ledger의 thread 간 발행 방식만 바꿨다.

근거 코드는 worktree의 `pipe.cpp:778-875`(ledger 발행·snapshot), `pipe.cpp:1023-1053`
(oversize metric), `pipe.cpp:2154-2164`와 `2798-2802`(session write/flush),
`pipe.cpp:2850-2899`(credit CAS), `socket_base_monitor.cpp:62-89`(단일 pending snapshot)이다.

## 설계 비교

| 대안 | 판정 |
|---|---|
| message/byte를 서로 독립된 atomic으로만 발행 | Data race는 없지만 monitor가 서로 다른 시점의 두 값을 조합할 수 있어 Monitoring §6.3의 pipe 합계 field 군 일관성을 만족하지 못한다. |
| 두 64-bit 값을 하나의 128-bit atomic으로 묶음 | lock-free 보장이 없고 기존 full-width 계약을 유지하면서 hot path lock 제거를 보장할 수 없다. |
| full-width atomic 두 개와 단일-writer sequence | 선택. 두 값의 범위를 유지하고 monitor만 stable snapshot을 다시 읽는다. |

수정 전/후 규칙 수: 같은 outbound 사실에 적용하던 `write` lock, `flush` lock, 개별 getter lock의
세 규칙을 **C3 발행과 monitor 단일 snapshot의 두 규칙**으로 줄였다. Session writer의 소유자는 I/O
thread 하나이고, 다른 thread에는 발행된 값 하나만 존재한다.

## 검증

모든 build는 `JOBS=4`로 실행했다. Valgrind와 benchmark는 `pgrep -x ninja`가 비어 있을 때 지정된
`PERF_LOCK`의 `flock` 아래에서 foreground로 실행했다.

| 검증 | 결과 |
|---|---|
| pristine/최종 dev build | PASS / PASS |
| 관련 suite 77개 × 5회 | **385/385 PASS** |
| lost-wake 5종 `until-fail:20` | **100/100 PASS**, 647.51 s |
| `test_close_completion_poller_release` `until-fail:50` | **50/50 PASS**, 46.33 s |
| 최종 monitor/wake/accounting 묶음 | **24/24 PASS** |
| 수동 TSan, LTO OFF, 4 target | pristine 20 → 최종 20, signature 차이 0 |
| Release/LTO `hotpath_gate` | **5/5 PASS** |
| with_stream, local Core, zlink/asio × all size | **6/6 완료, mismatch 0** |
| `git diff --check` | PASS |

TSan target별 경고는 전후 모두 10+1+9+0이다. Signature도
`ypipe_t<command_t>::check_read()` 17건과 기존 receive guard 3건으로 동일하다. G-11b 1차의
`get_msgs_written()`·`get_bytes_written()` 신규 경고 21건은 0건이 됐다.

## 성능

축소 `stream_tcp` cell은 20,000 message를 사용했다.

| 지표 | pristine | 최종 | 변화 |
|---|---:|---:|---:|
| `pthread_mutex_lock` calls/msg | 24.0528 | 22.0566 | **-1.9962** (반올림 -2.0) |
| 전체 Ir/msg | 15,744.753 | 15,673.668 | **-71.085 (-0.45%)** |

최종 Release/LTO hotpath cell의 `측정/reference`는
`dealer_dealer_inproc` 3254.134/3230.922(1.0072),
`dealer_router_reqrep_inproc` 18567.558/18663.506(0.9949),
`pair_inproc` 2308.784/2348.457(0.9831), `router_router_tcp`
2895.848/2972.532(0.9742), `stream_tcp` 14238.221/14623.471(0.9737)이다.

최종 with_stream zlink throughput은 64/1024/65536 byte에서 각각
271.74/244.63/31.57 kops였고 mismatch는 모두 0이다. Asio 비교값은
364.36/327.55/41.84 kops다. 결과는 worktree의
`bindings/c/bench/with_stream/results/20260907_150832/`에 있다.

## 변경 파일과 판정

- `core/src/runtime/core/pipe.hpp`: C3 field와 coherent ledger snapshot 선언
- `core/src/runtime/core/pipe.cpp`: release/acquire publication, CAS, session hot path lock 생략
- `core/src/runtime/sockets/common/socket_base_monitor.cpp`: pipe 합계 단일 snapshot 사용

- 소유 계층: Core `pipe_t`; session I/O thread가 outbound 값을 쓰고 socket/monitor thread에는 C3로 발행한다.
- 스펙 조항: synchronization model §3.2·§3.5, 검증 §6; charge 값과 보유 기간은 Auto-HWM §4와 Connection memory §3.1을 유지한다.
- 교차언어 대조: 모든 binding은 같은 Core C ABI와 `pipe_t`를 사용하므로 언어별 runtime 변경이 없다.
- 변경 분류: **B — 기존 결함 수정.** 승인된 lock 제거에서 드러난 monitor ledger race를 소유 모듈에서 수정했다.

남은 신규 실패는 없다. TSan의 기존 20건은 이 변경의 범위 밖이며 pristine과 동일하다.
