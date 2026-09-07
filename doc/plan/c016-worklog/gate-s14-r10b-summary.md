# gate s14-r10b 요약

## 적용

- S-14: `core/tests/integration/test_dealer_router_single_lane_contract.cpp`의 snapshot 시험에 기존 공개 `POLLCOMPLETION` 소유자 setup/teardown을 추가했다.
- R10-B-redo: `ctx_socket_registry_t`의 두 동일 wait loop를 private `wait_until` 템플릿으로 통합하고, `_slot_sync` 및 pending 옵션 주석을 보강했다.
- 두 patch 모두 `git apply --3way` clean 적용, 기계적 충돌 없음. public header/export diff 없음.

## 빌드·테스트

| 항목 | 결과 |
|---|---|
| dev build (`JOBS=4`) | PASS |
| 전체 `ctest -j2` | 207/208 PASS; `hotpath_gate`만 FAIL (측정값이 reference보다 7.4~25.6% 좋아져 +5% 상한) |
| 지정 suite 74 tests × 5 | 5/5 PASS |
| `stream|pipe` 23 tests, `-j4` × 10 | 10/10 PASS |
| S-14 solo `test_single_lane_flow_snapshot_accounting` | repeat-until-fail: 1 PASS 후 1 FAIL (line 2972); 이후 독립 3회 3/3 PASS — **간헐 실패, 기대 30/30 미달** |
| release lib (`JOBS=4`) | PASS |

## ABI·품질

- `git diff --stat -- core/include core/src/libzlink.vers`: 비어 있음.
- mirror cmp: 현재 Core의 3개 `zlink*.h` × C/C++/Go/Rust 4 mirror = 12/12 PASS. `scripts/gate/README.md`에는 명시된 8-header 절차가 없어 fallback 검색을 사용했다.
- `git diff --check`: PASS.

## hotpath gate (load average 7.31/5.09/4.96)

| cell | reference | measured | ratio | 판정 |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3423.533 | 3271.193 | 0.9555 | PASS |
| dealer_router_reqrep_inproc | 18663.506 | 18639.585 | 0.9987 | PASS |
| pair_inproc | 2348.457 | 2330.888 | 0.9925 | PASS |
| router_router_tcp | 2972.532 | 2916.264 | 0.9811 | PASS |
| stream_tcp | 14623.471 | 14195.343 | 0.9707 | PASS |

## 성능

with_stream (`ZLINK_CORE_SOURCE=local`, CCU 1000, runs 1; load 5.02/4.73/4.84), Phase-0 대비 ratio:

| size | zlink kops (ratio) | asio kops (ratio) |
|---|---:|---:|
| 64 | 288.30 (1.072) | 375.60 (1.167) |
| 1024 | 293.26 (1.207) | 347.45 (1.098) |
| 65536 | 35.51 (1.168) | 44.47 (1.134) |

perf/c 1024 B tcp (local runtime): single ROUTER_ROUTER 746.394 Kmsg/s (Phase-0 744.4, 1.003; load 3.15/4.24/4.67); multi RR_SENDSEND 243.072 Kops/s (Phase-0 111.5, 2.180) 및 RR_REQREP 180.852 Kops/s (Phase-0 73.0, 2.478; load 10.07/6.22/5.33).

perf/c single runner의 stale-runtime 자동 rebuild는 명령 앞에 `JOBS=4`를 명시했지만 로그는 내부 `build-core ... jobs=16`을 출력했다. runner가 전달값을 덮어쓴 것으로 보이며, 이탈 사실만 기록한다.

## 판정

- S-14의 solo 안정성 기준(30/30)이 실패했으므로 이 묶음은 **채택 불가**다. 게이트 권한상 코드·기대값은 수정하지 않았다.
- R10-B는 동작 불변 중복 제거/주석이며, S-14는 기존 계약 테스트 전제의 결함 수정이라는 각 job의 B 분류를 유지한다. 수정 전/후 규칙 수: R10-B wait-loop 규칙 2개→1개, S-14 runtime 규칙 변화 0개.
- main working tree는 적용 상태로 남겼고 커밋·stash·스펙 문서 변경은 하지 않았다.
