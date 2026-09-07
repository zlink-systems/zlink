# 게이트 r3-r4 요약

## 1. 포팅
- R3 (~/project/zlink-work/r3, 15 files: pipe.*, session_base.*, session_base_pipe_io.cpp, ctx.hpp, socket_base_endpoint.cpp, unittest 9개) → main(3586f0eb17)에 `git apply --3way` 클린 적용, 충돌 없음.
- R4-AB (~/project/zlink-work/r4, 9 files: socket_base.hpp/_monitor.cpp/_msg.cpp, routed_submit_target.hpp, socket_close_ops.*(삭제), monitor_api.cpp, core/CMakeLists.txt, socket_send_complete.cpp) → R3 적용 뒤 `git apply --3way` 클린 적용, 충돌 없음.
- `git diff --stat -- core/include core/src/libzlink.vers` 두 patch 적용 후 비어 있음 확인(공개 인터페이스 불변).

## 2. 빌드
- `JOBS=4 scripts/build-core.sh dev`: 성공.
- `JOBS=4 scripts/build-core.sh release --lib-only`: 성공 (core/build/lib/libzlink.so.0.17.0).
- `cmake --build core/build-gate --target hotpath_bench -j4`: 성공.

## 3. ctest (core/build-dev, -j2)
- 전체 1회: 209개 중 207 pass, 2 "fail"
  - `test_single_lane_flow_snapshot_accounting`: 브리프의 known intermittent. 단독 재실행 3/3 pass → 간헐 확인.
  - `hotpath_gate`(dev 트리 내장): 5셀 모두 ratio 1.13~1.30로 "FAIL" — 브리프에 명시된 대로 dev 트리는 RelWithDebInfo/LTO OFF라 release 기준과 비교 무의미(정상, 4절 release 측정으로 대체).
- 브리프 union 패턴(`pipe|wake|poll|stream|hwm|flow|credit|router|dealer|pair|session|monitor|close|send|submit`, 94개) 5회: 4/5 100% pass, 1/5에서 `test_single_lane_flow_snapshot_accounting` 1건 실패 → 간헐과 일치, 고정 실패 없음.
- lost-wake 세트(`test_stream_send_blocking_wakeup`, `test_wake_invariants`, `test_two_poller_wake`, `test_wake_invariant_hwm_lwm_shrink`, `test_wake_invariant_completion_owner`) `--repeat until-fail:10`: 5개 전부 100% pass, 0 fail.

## 4. 공개 인터페이스 확인
- `git diff --stat -- core/include core/src/libzlink.vers` 재확인: 비어 있음.
- mirror cmp: `core/include/{zlink.h,zlink_enum.h,zlink_errno.h}` vs `bindings/{cpp,go,c,rust}/include/*` 12/12 바이트 동일(OK).

## 5. hotpath_gate 5셀 (release, `core/build-gate`, valgrind 명령 카운트, `flock` 하)
| cell | reference | measured | ratio | verdict |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3423.533 | 3382.216 | 0.9879 | PASS |
| dealer_router_reqrep_inproc | 19682.196 | 19337.182 | 0.9825 | PASS |
| pair_inproc | 2527.834 | 2504.546 | 0.9908 | PASS |
| router_router_tcp | 2972.532 | 2957.859 | 0.9951 | PASS |
| stream_tcp | 14623.471 | 14801.671 | 1.0122 | PASS |

전 셀 5% 이내, reference 갱신 불필요.

## 6. 성능 측정 (`flock` 하, 모두 `ZLINK_CORE_SOURCE=local` core/build release lib)

### with_stream (CCU 1000, size all, runs 1, --reuse-build)
| size | zlink kops | asio kops | z/a | Phase 2S idle 기준 z/a | zlink 대비 기준 | asio 대비 기준 |
|---|---:|---:|---:|---:|---:|---:|
| 64 B | 301.93 | 368.95 | 0.818 | 0.821 (289.7/352.8) | +4.2% | +4.6% |
| 1024 B | 274.52 | 355.14 | 0.773 | 0.823 (267.8/325.2) | +2.5% | +9.2% |
| 65536 B | 33.60 | 41.97 | 0.801 | 0.787 (32.6/41.4) | +3.1% | +1.4% |

zlink 자체는 기준 대비 전 size 개선. 1024 B에서 asio 비교군이 기준보다 9.2% 더 빨라져 z/a 비율이 낮아 보이나, zlink 절대 처리량 자체는 저하 없음.

### perf/c 경량 3셀 (1024 B tcp, runs 1, --reuse-build), Phase 2G idle 기준(§7.4) 대비
| cell | Phase 2G 기준 | 측정 | ratio |
|---|---:|---:|---:|
| single ROUTER_ROUTER | 732.2 Kmsg/s | 541.28 Kmsg/s | 0.739 |
| multi ROUTER_ROUTER_SENDSEND | 242.5 Kops/s | 140.946 Kops/s | 0.581 |
| multi ROUTER_ROUTER_REQREP | 170.1 Kops/s | 99.196 Kops/s | 0.583 |

**경고: 이 3셀은 신뢰하기 어렵다.** 측정 시각(09:32경) load average 2.5~3.2였고, 동시에 다른 job의 worktree(zlink-work/r7r11, zlink-work/r9)에서 무관한 ctest가 실행 중이었음을 확인함(ps aux로 확인, PERF_LOCK은 이 job 내부만 직렬화하고 다른 job의 ctest는 잠그지 않음). hotpath_gate(명령 카운트 기반, 부하 무관)는 5셀 모두 기준 이내인 반면 perf/c(벽시계 처리량 기반, multi는 100 클라이언트 소켓 경합)만 균일하게 40% 안팟으로 떨어진 패턴은 코드 회귀보다 부하 오염과 부합한다(계획 §7.4의 "Phase 0 multi 값은 부하 오염이라 폐기" 사례와 동일 패턴). **감독관 판단 필요: 머신이 유휴일 때 이 3셀만 재측정 권고.**

## 7. load average
- hotpath_gate 측정 시작: 7.59 6.38 6.31
- with_stream 측정 시작: 4.55 5.75 6.10 (재확인 2.81)
- perf/c single 측정 시작: 2.53 4.59 5.63
- perf/c multi 측정 시작: 3.21 4.62 5.63 (multi 측정 중 다른 job ctest 동시 실행 확인)

## 8. 상태
- main 워킹트리는 R3+R4 patch 적용 상태로 유지(커밋 안 함). `git diff --stat -- core/include core/src/libzlink.vers`는 비어 있음.
- 코드 판단/수정 없음(포팅 충돌 없어 기계적 해결도 없었음).
