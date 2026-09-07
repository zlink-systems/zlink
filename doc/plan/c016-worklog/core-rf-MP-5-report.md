# Core RF MP-5 결과 보고서

## 결과

MP-4에서 증가해 보였던 single-part hotpath의 `Ir`를 제거했다. `FINAL` 전송은 non-publish
caller slot이 실제로 존재할 때만 multipart helper 경로로 진입하며, helper가 없거나
`send_sequence_count == 0`이면 public entry에서 곧바로 complete-record 전송 경로를 사용한다.

- Release+LTO hotpath gate: **5/5 PASS**
- MP-3 대비 증가했던 세 셀: dealer/dealer **-0.46%**, pair **-0.64%**,
  router/router **-0.31%**
- 동일 dev runner의 single-after-multipart: 평균 **22.207 ms**, main 대비 **-1.26%**
- 기능·ASan+LSan·TSan: 남은 실패 없음
- commit, stash, branch 전환, spec 수정 없음

작업 시작 전 MP-3+MP-4 전체 diff는 지정된
`/tmp/claude-1000/-home-hep7hep7-project-zlink/a5b31a9a-1a3b-4bcb-a080-53988ed569cb/scratchpad/mp4-before-mp5.patch`에
보존했다. SHA-256은
`8be4251267cc8bfa1ebcbd5086b0f5be5f10b17e9e42913812f7c9cacdb89e5a`다.

## 원인

### MP-4 counter가 원인인지 확인

보존된 `mp3-before-mp4.patch`와 작업 시작 시 보존한 MP-4 patch를 각각 깨끗한 HEAD에 적용하고,
같은 Release+LTO library와 정적 `hotpath_bench`를 다시 링크한 뒤 `pair_inproc` 20,000회를
callgrind로 측정했다.

| 항목 (self Ir) | 재빌드 MP-3 | 재빌드 MP-4 | MP-4 - MP-3 |
|---|---:|---:|---:|
| program total | 49,034,133 | 49,015,670 | -18,463 |
| `submit_completion_aware_part()` | 1,520,000 | 1,520,000 | 0 |
| `prepare_send_step_locked()` | 820,000 | 820,000 | 0 |
| 상위 15개 함수 각각 | 동일 | 동일 | 0 |
| Ir/msg | 2,451.707 | 2,450.784 | -0.923 |

따라서 MP-4의 `send_sequence_count` atomic load와 TLS 참조 변경은 helper 미생성 PAIR single
경로에서 실행되지 않았고, 보고된 MP-3의 2,341.905 Ir/msg는 보존된 MP-3 최종 patch로
재현되지 않았다. 과거 MP-3 수치가 최종 소스보다 이전 상태로 링크된 정적 runner에서 나왔다는
것이 이 대조가 지지하는 원인이다. 당시 binary 자체가 보존되지 않아 이 provenance 판단은
소스 patch 재현 결과에 근거한 추론이다.

### 실제 single-part 추가 비용

재빌드한 MP-3와 MP-4 양쪽에서 실제 비용은
`core/src/api/socket/socket_message_send_api.cpp:391`의
`submit_completion_aware_part()`였다. 기존 순서는 다음과 같았다.

1. `core/src/api/socket/socket_message_send_api.cpp:400`에서 blocking `FINAL`의 staging
   public-API scope를 먼저 연다.
2. 그 뒤 `core/src/api/socket/socket_message_send_api.cpp:415`에서
   `prepare_send_step_locked()`를 호출한다.
3. helper/현재 caller sequence가 없다는 `prepare_rc == 1`을 확인한 뒤에야 scope를 닫고
   complete-record 경로로 돌아간다.

즉 helper가 없는 single send도 lifecycle scope, helper/TLS 판정과 mutex 경로를 먼저 거쳤다.
ROUTER는 이 판정보다 RID spec 복사도 먼저 수행했다. helper가 생성된 적은 있지만 현재 caller
slot 수가 0인 single-after-multipart도 같은 늦은 판정 때문에 비용을 지불했다.

## callgrind 함수별 차이

아래는 동일 Release+LTO `pair_inproc` 20,000회에서 MP-4와 최종 MP-5의 self Ir를 비교한
것이다. 0은 함수가 삭제됐다는 뜻이 아니라 이 single-part trace에서 호출되지 않았다는 뜻이다.

| 함수 | MP-4 Ir | MP-5 Ir | 차이 | Ir/msg 차이 |
|---|---:|---:|---:|---:|
| program total | 49,015,670 | 46,523,455 | **-2,492,215** | **-124.611** |
| `submit_completion_aware_part()` | 1,520,000 | 0 | -1,520,000 | -76.000 |
| `prepare_send_step_locked()` | 820,000 | 0 | -820,000 | -41.000 |
| `borrow_send_sequence_state()` | 0 | 160,000 | +160,000 | +8.000 |
| `raw_send()` | 900,000 | 960,000 | +60,000 | +3.000 |
| `submit_public_send_record()` | 740,000 | 760,000 | +20,000 | +1.000 |
| `pthread_mutex_lock` | 721,040 | 721,840 | +800 | +0.040 |
| `pthread_mutex_unlock` | 520,819 | 521,426 | +607 | +0.030 |

원본은 scratchpad의 `MP-5/pair-mp3.callgrind`, `pair-mp4.callgrind`,
`pair-mp5.callgrind`에 보존했다.

## 수정

- `part_helper_internal.hpp:216`, `part_helper_api.cpp:833`
  - `borrow_send_sequence_state()`를 non-publish caller-slot 존재 판정의 단일 소유자로 추가했다.
  - socket의 기존 borrowed helper state와 MP-4 C3 projection인 `send_sequence_count`만 읽는다.
    반환 pointer의 수명은 이미 public entry가 보유한 socket/public-handle pin이 보장한다.
  - `current_send_sequence_active()`도 같은 판정을 재사용한다.
- `socket_message_send_api.cpp:587`, `socket_message_send_api.cpp:674`
  - PAIR/DEALER `zlink_send_part(FINAL)`과 ROUTER `zlink_send_part_rid(FINAL)`가 helper 없음 또는
    count zero를 public entry에서 먼저 확인한다.
  - 이 경우 spec 구성과 staging scope 없이 `submit_public_send_record()`로 직접 보낸다.
- `socket_request_reply_submit_api.cpp:1037`
  - single REQUEST도 같은 borrowed state 판정을 사용한다. 기존 socket pin 아래에서 읽으므로
    `shared_ptr` refcount와 별도의 counter 판정을 중복하지 않는다.

대안 A인 단일 borrowed predicate를 선택했다. 대안 B인 API별 TLS/raw cache는 같은 helper 존재
사실을 여러 상태로 만들고, 대안 C인 `submit_completion_aware_part()`의 단순 분리/noinline은
이미 열린 scope와 늦은 판정을 그대로 둔다. 수정 전에는 “helper 확인”과 “count-zero 확인”이
API별로 늦게 또는 중복 적용되는 두 규칙이었고, 수정 후에는 **caller slot이 있을 때만 helper
경로에 진입한다**는 한 규칙이다. 새 상태·timer·retry·public API는 추가하지 않았다.

## 검증

모든 빌드는 `JOBS=4`, foreground로 실행했고 동시에 실행된 ninja는 최대 1개였다.

| 검증 | 결과 |
|---|---|
| `JOBS=4 scripts/build-core.sh dev` | PASS |
| 직접 영향 5 target | **5/5 PASS**, 15.19초 |
| 관련 regex `--repeat until-fail:2` | **40 target × 2 = 80/80 PASS**, 258.04초 |
| ASan+LSan (`detect_leaks=1`, `halt_on_error=1`, `abort_on_error=1`) | **6/6 PASS**, 16.55초 |
| GCC TSan (`setarch x86_64 -R`, 기존 suppression) | **8/8 PASS**, 23.42초 |
| `git diff --check` | PASS |
| `core/include`, `core/src/libzlink.vers` diff | 없음 |

직접 영향 5 target은 `test_public_inproc_multipart_send`, `test_helper_ownership`,
`test_helper_interleave`, `test_phase3_request_reply_contract`,
`unittest_phase3_request_reply_owners`다. ASan 6개는 앞의 integration 4개와
`unittest_complete_record_admission`, `unittest_phase3_request_reply_owners`, TSan 8개는 이 6개에
`test_single_lane_flow_control_boundary`, `unittest_zmp_contract_edges`를 더했다.

## 성능

### Release+LTO hotpath 5셀

Release library와 정적 runner를 최종 소스로 갱신하고, 다른 ninja가 없는 상태에서 시작 load
average **1.33 / 0.81 / 1.01**, 지정된 `PERF_LOCK` 아래 한 번 실행했다.

| cell | MP-3 Ir/msg | MP-4 Ir/msg | MP-5 Ir/msg | MP-5 - MP-3 | gate |
|---|---:|---:|---:|---:|---|
| `dealer_dealer_inproc` | 3,265.08 | 3,374.92 | **3,249.922** | **-0.46%** | PASS |
| `dealer_router_reqrep_inproc` | 18,531.55 | 18,345.40 | **18,329.474** | **-1.09%** | PASS |
| `pair_inproc` | 2,341.91 | 2,451.51 | **2,326.905** | **-0.64%** | PASS |
| `router_router_tcp` | 2,924.84 | 3,111.84 | **2,915.899** | **-0.31%** | PASS |
| `stream_tcp` | 14,239.10 | 14,228.41 | **14,234.011** | **-0.04%** | PASS |

증가했던 세 single-part 셀은 모두 MP-3 ±1% 안으로 돌아왔다. req/rep은 대칭 ±1% 목표보다
0.09%p 더 낮지만 instruction 감소 방향이며 공식 reference 대비 -1.79%로 gate를 통과했다.

### 동일 runner dev 셀

`/tmp/mp3_multipart_perf` binary를 그대로 사용하고 `LD_LIBRARY_PATH=core/build-dev/lib`만 지정했다.
첫 측정 load average는 **0.57 / 1.06 / 1.33**, single 확인 표본은
**0.44 / 1.01 / 1.31**이었다.

| 셀 | main | MP-4 | MP-5 | 판정 |
|---|---:|---:|---:|---|
| 같은 PAIR에서 2-part 100회 후 single 100,000회 | 22.491 ms | 평균 23.573 ms | 20.983 / 23.431 ms, 평균 **22.207 ms** | main 대비 **-1.26%**, ±5% 이내 |
| 4 thread × 2-part 20,000회 | 20초 timeout | 102.501 ms, 780,483 record/s | **98.513 ms, 812,079 record/s** | MP-4 대비 시간 -3.89%, 처리량 +4.05% |

## 소유권·계약·교차언어·분류

- 소유 계층: Core part helper가 caller slot 존재를 소유하고, public send entry는 그 단일
  projection을 소비해 helper 경로 진입 여부만 결정한다. complete-record scope가 direct send의
  lifecycle과 admission을 계속 소유한다.
- spec 근거: `core/doc/spec/core/systems/11-synchronization-model.ko.md` §2 C3·§8,
  `core/doc/spec/core/socket/README.ko.md`의 Part send·pending admission·Request/reply 조항.
- 교차언어: C++, .NET, Node, Java, Rust, Go binding은 multipart를 Core C API에 위임하므로
  binding별 runtime 변경이 필요 없다.
- 변경 분류: **B — 기존 결함**. MP-3 최종 소스의 늦은 fast-path 판정과 과거 정적 runner의
  provenance 불일치를 바로잡았다.
- spec 변경: 없음.

## MP-5 변경 파일

기존 MP-3+MP-4 patch 위에서 MP-5가 추가로 수정한 파일은 다음 네 개다.

- `core/src/api/socket/part_helper_internal.hpp`
- `core/src/api/socket/part_helper_api.cpp`
- `core/src/api/socket/socket_message_send_api.cpp`
- `core/src/api/socket/socket_request_reply_submit_api.cpp`

worktree에는 MP-3+MP-4를 포함해 기존 25개 tracked 변경이 그대로 남아 있으며 모두 미커밋이다.
