# gate g11a 결과

## 결론

G-11a patch는 main에 적용 가능하다. 기능·반복·mirror 검증은 통과했으나, hotpath gate는
`dealer_router_reqrep_inproc`가 기준보다 5.18% 개선되어 정책상 FAIL이다. 기준 갱신 여부는
감독관 판단이 필요하다.

## 포팅

- 원본: `~/project/zlink-work/g11a` (`08da256f1e`), 추출 patch:
  `<scratchpad>/G-11a.patch`.
- `git apply --3way`로 두 hunk 모두 clean 적용, 충돌 없음.
- 변경: `core/src/runtime/sockets/common/socket_base_lifecycle.cpp`,
  `core/tests/unittest/unittest_receive_transaction.cpp`.
- PAIR activation command도 socket turn을 획득하도록 내부 unittest 기대값을 갱신한 것은
  G-11a 보고서 §8 근거에 따라 허용했다. public contract test는 변경하지 않았다.
- `git diff --stat -- core/include core/src/libzlink.vers`: 빈 결과. `git diff --check`: PASS.

## 빌드와 테스트

| 항목 | 결과 |
|---|---|
| dev (`JOBS=4`) | PASS |
| 전체 ctest (`-j2`) | 207 PASS, `hotpath_gate` 1 FAIL (dev 트리에 없는 예상 실패) |
| 지정 suite `pair|wake|poll|stream|pipe|mailbox|send|recv|router|dealer|close|release` | 61/61 × 5 PASS |
| lost-wake: wake 4개 | 각 10회 PASS |
| lost-wake: `test_stream_packet_progress`, `test_stream_send_blocking_wakeup` | 각 10회 PASS |
| `test_close_completion_poller_release` | 50회 PASS |
| `stream|pipe` (`-j4`) | 23/23 PASS |
| release lib (`JOBS=4`) | PASS |

알려진 간헐인 `test_single_lane_flow_snapshot_accounting`은 전체 ctest에서 PASS,
`test_stream_socket_recv_multiclient_ready_regression`은 suite 5회 및 stream/pipe에서 PASS였다.
`unittest_request_timeout_scheduler`는 이 변경 suite에 포함되지 않았다.

`scripts/gate/README.md`에는 명시한 8×4 절차가 없어 `zlink.h`·`zlink_enum.h`를 c/cpp/go/rust
mirror와 `cmp`했다(8 comparisons PASS).

## hotpath gate (callgrind Ir/msg)

| 셀 | reference | measured | ratio | 결과 |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3423.533 | 3271.285 | 0.9555 | PASS |
| dealer_router_reqrep_inproc | 19682.196 | 18663.506 | 0.9482 | FAIL (+5% 개선 경계 초과) |
| pair_inproc | 2348.457 | 2331.650 | 0.9928 | PASS |
| router_router_tcp | 2972.532 | 2916.169 | 0.9810 | PASS |
| stream_tcp | 14623.471 | 14183.290 | 0.9699 | PASS |

valgrind 시작 시 `pgrep -x ninja`는 비어 있었다. load average: 8.27 / 4.11 / 3.74.

## 성능

| 측정 | 값 | 기준 대비 |
|---|---:|---:|
| with_stream 64 B zlink/asio | 298.39 / 391.92 = 0.761 | Phase 0 0.835 대비 0.912× |
| with_stream 1024 B zlink/asio | 279.95 / 325.47 = 0.860 | Phase 0 0.823 대비 1.045× |
| with_stream 64 KiB zlink/asio | 31.96 / 41.79 = 0.765 | Phase 0 0.787 대비 0.972× |
| perf/c single ROUTER_ROUTER tcp 1024 | 813.414 Kmsg/s | 744.4 대비 1.093× |
| perf/c multi RR_SENDSEND tcp 1024 | 230.417 Kops/s | 111.5 대비 2.066× |
| perf/c multi RR_REQREP tcp 1024 | 166.302 Kops/s | 73.0 대비 2.278× |

with_stream load average는 3.96 / 3.58 / 3.58, perf/c는 2.84 / 3.38 / 3.51이었다.
기본 with_stream 실행은 release download 404로 실패했고, 동일 release local lib를 명시한
`ZLINK_CORE_SOURCE=local` 재실행에서 위 결과를 얻었다. perf/c runner가 stale local runtime을
감지해 release lib를 자동 재빌드했다. 이 자동 경로는 runner 기본값으로 `JOBS=16`을 사용했다.
게이트가 지정한 `JOBS=4` 제약을 벗어난 3-object 증분 빌드였으며, 이 절차 이탈을 명시적으로
기록한다.

## 주의 사항

시작 시 `git pull --rebase -q`는 다른 job의 `doc/plan` progress 변경 때문에 Git이 거절했다.
그러나 `HEAD`와 `origin/main`은 모두 `a2b087ff0c`였고 origin 선행 커밋은 없었다. main에는
포팅 patch와 worklog만 남겼으며, stash·commit·spec 문서 변경은 하지 않았다.

종료 직전에는 시작 시 없던 `core/doc/spec/core/socket/08-stream.en.md`와
`08-stream.ko.md`의 수정이 작업 트리에 관측됐다. g11a는 이 파일을 읽거나 수정하지 않았으며,
동시 작업의 변경으로 보존했다.
