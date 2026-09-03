# Phase 5 perf 회귀 — 감독관 직접 진단 (2026-09-03)

## 3자 비교 (single, 1024B, 1-run, throughput ratio)
worktree: baseline=core/v0.15.1, prelane=39e7467be5(pull-completion bb66e85376 + send-path 04ecca54d1, **lane 이전**), candidate=main(lane 포함)

| cell | base | prelane | cand | pre/base | cand/pre |
|---|--:|--:|--:|--:|--:|
| DEALER_DEALER tcp | 992k | 746k | 658k | 0.752 | 0.882 |
| DEALER_DEALER inproc | 986k | 675k | 588k | 0.685 | 0.870 |
| DEALER_ROUTER_REQREP tcp | 465k | 364k | 322k | 0.783 | 0.886 |
| DEALER_ROUTER_REQREP inproc | 618k | 409k | 342k | 0.662 | 0.836 |
| ROUTER_ROUTER tcp | 935k | 940k | 881k | 1.006 | 0.938 |

**결론: 회귀는 두 겹이다.**
1. **주범 = pull-completion send path (bb66e85376, lane 이전·이미 커밋됨)**: pre/base 0.66~0.78 (약 25~34% 저하). DEALER/one-way·REQREP 전반. R/R one-way는 pre/base≈1.0 (send path가 completion-aware가 아닌 direct라 영향 적음).
2. **lane 증분 (8b40b3feb2)**: cand/pre 0.84~0.94 (약 6~16% 추가 저하). 모든 pattern.

## callgrind 근거 (2s, inproc DEALER_DEALER)
- baseline send: `submit_simple_part` 경로.
- prelane/candidate send: `submit_completion_aware_part → send_completion_submit_blocking → try_admit_send_parts_scoped` (pull-completion). 훨씬 무거움 = 1번 회귀의 코드 근거.
- candidate 신규 self-cost (lane 증분): `transport_pair_application_ready`(매 dealer send, `_transport_pairs_sync` mutex+std::map find), `snapshot_attached_pipes`(send마다 vector 스냅샷), `routed_submit_candidate`/`request_submit_candidate` filter, 수신측 `end_public_part_receive_delivery_hold`(inclusive 3.16%)·`reclassify_transport_pair_application_head`. `mutex_t::lock` 호출수 증가.

## R/R inproc "ERROR"(runner non_zero_exit_1) — 별건, 데이터 손실 아님
- 최소재현: ROUTER↔ROUTER inproc에서 recv(DONTWAIT)는 **0ms 즉시 성공**(delivery 정상). 그러나 monitor를 open→close한 뒤 `zlink_poll`은 첫 메시지에 POLLIN edge를 timeout까지 안 올림(레벨 재검사로 timeout 시점에야 readable 보고). prelane은 정상.
- 영향: bench 수신 루프가 poll로 대기하는 구간에서 edge 유실 → handshake/active phase 지연·실패. **poll readiness 신호(probe가 `_in_active=false`로 만든 뒤 writer flush의 read-activate 명령 유실 가능성, 힌트 (d))** 를 봐야 함.
- 재현 바이너리: c016/rr6.cpp(즉시 delivery 확인), rr_inproc_repro4/5.cpp(poll edge 지연 확인).

## 수정 방향(계약 유지)
- lane 증분: pair readiness/lane-count/peer-type을 pipe_t atomic 캐시로 두고 hot path(send의 transport_pair_application_ready, recv의 reclassify 게이트)에서 lock·map 제거. reclassify는 count-1 pair에 completion source가 실제로 열릴 수 있을 때만. snapshot_attached_pipes를 send마다 돌리지 않기.
- pull-completion 주범: send path에서 completion 레코드가 실제로 필요한 경우(DONTWAIT pending·request)만 completion-aware 비용을 지불하고, 즉시 admit되는 blocking NONE send는 경량 경로로. **단 이건 0.16 설계 핵심이라 계약 확인 필요.**
- poll edge: probe/reclassify가 read-activate 신호를 삼키지 않도록.
