# gate s11 — S-11 (`_in_active`/`_state` → std::atomic, `_state_active` 미러 제거)

- Main HEAD 92038c1bea(+docs c392d3824e) → `git pull --rebase` 후 동일. `git status --short -- core bindings scripts` 초기 빈 상태 확인(doc/plan worklog는 무시, stash로 우회 후 복원).
- Patch: `~/project/zlink-work/s11` diff → `git apply --3way` **충돌 없이 클린 적용**(pipe.cpp +28/-27, pipe.hpp +22/-9 라인 규모). 공개 인터페이스: `git diff --stat -- core/include core/src/libzlink.vers` 비어 있음(확인).
- 헤더 미러 cmp: 8 헤더(zlink.h, zlink_enum.h, zlink_errno.h, zlink/{common,core/api,socket/api,message/api,eventing/api}.h) × 4 미러(c/cpp/rust/go) = 32건 전부 일치.

## 빌드
- `build-dev`: 성공. `build`(release, --lib-only): 성공. `build-gate`(hotpath_bench): 성공.

## ctest (core/build-dev)
- 전체 209개 1회: 208 pass, 1 fail = `hotpath_gate`(dev 트리는 미최적화라 정상, valgrind 5셀은 별도 §hotpath_gate 참조).
- 합집합 패턴 `wake|poll|stream|pipe|mailbox|fq|dealer|router|pair|close|release`(56개) × 5회: 전부 pass.
- lost-wake 세트(`test_wake_invariants`, `test_two_poller_wake`, `test_wake_invariant_hwm_lwm_shrink`, `test_wake_invariant_completion_owner`, `test_stream_packet_progress`, `test_stream_send_blocking_wakeup`) `--repeat until-fail:10`: 전부 pass, 0 fail.
- 기지 간헐 `test_close_completion_poller_release`, `test_single_lane_flow_snapshot_accounting`: 정상 통과(재현 없음).

## hotpath_gate (5셀, valgrind, build-gate)
| 셀 | reference | measured | ratio | verdict |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3423.533 | 3381.089 | 0.9876 | PASS |
| dealer_router_reqrep_inproc | 19682.196 | 19343.166 | 0.9828 | PASS |
| pair_inproc | 2527.834 | 2504.030 | 0.9906 | PASS |
| router_router_tcp | 2972.532 | 2954.597 | 0.9940 | PASS |
| stream_tcp | 14623.471 | 14630.977 | 1.0005 | PASS |

stream_tcp는 예상된 ~4% 개선이 보이지 않음(사실상 동일, +0.05%). reference는 변경하지 않음.

## 성능
### with_stream (zlink,asio, CCU 1000, runs 1)
| size | zlink | asio | ratio | S-1 기준 |
|---|---:|---:|---:|---:|
| 64B | 301.74 | 379.58 | 0.795 | 0.818 |
| 1024B | 272.96 | 337.12 | 0.810 | 0.802 |
| 65536B | 33.23 | 41.50 | 0.801 | 0.824 |
단발 측정(runs 1, 기준 표는 runs 3 median) — 64B/65536B 소폭 하락, 1024B 소폭 개선, 노이즈 범위 가능성.

### perf/c 경량 3셀 (1024B tcp, runs 1)
| 셀 | 값 | Phase 0 기준 | ratio |
|---|---:|---:|---:|
| single ROUTER_ROUTER | 807.33 Kmsg/s | 744.4 | 1.084 |
| multi RR_SENDSEND | 238.33 Kops/s | 111.5 | 2.14 |
| multi RR_REQREP | 187.72 Kops/s | 73.0 | 2.57 |
multi 두 셀의 비율 급증은 S-11 단독 기여로 보기 어려움(환경/부하 차이 가능) — 감독관 재확인 요망.

load average: 측정 구간 0.7–2.4 (조용한 편).

## TSan (test_two_poller_wake)
main `core/build-tsan`는 `ENABLE_TSAN=ON`이 clang 전용 `-mllvm` 플래그를 무조건 주입해 이 머신(GCC 13, clang 미설치)에서 컴파일 실패. s11/s2 `build-tsan` 캐시를 확인한 결과 실제로는 `ENABLE_TSAN=OFF` + 수동 `-fsanitize=thread -fno-omit-frame-pointer`/`-fsanitize=thread`(링커) + `ENABLE_LTO=OFF`로 구성되어 있어 동일하게 재구성 후 `test_two_poller_wake`만 빌드, `setarch $(uname -m) -R`로 실행.
- **결과: TSan 경고 1건**(기대는 0). `receive_once_guarded`(recv_routed) 읽기 vs `notify_receive_progress_locked`(stop_unowned_async_command_processing_at_idle 경로) 쓰기 race. 계획 §7.5 D-e 후보(수신 lease/owner 배제 미결정)와 일치하는 것으로 보임 — S-11 범위 밖, 별도 결정 대기.

## 결론
main working tree는 patch 적용 상태 그대로 유지(커밋 안 함). 코드 판단/수정 없음(TSan 트리 재구성은 빌드 설정만, 소스 변경 없음).
