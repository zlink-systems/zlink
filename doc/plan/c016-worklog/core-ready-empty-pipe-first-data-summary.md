# D-118 Core — READY 이후 빈 pipe의 첫 DATA admission

기존 credit admission helper를 transport-pair hold 해제에도 적용했다. 공개 C 회귀 테스트 10회, integration 132개, Release+LTO hotpath 4개 cell과 감독의 공개 C 재현이 모두 통과했다. BLOCKERS는 없다.

## 변경 범위와 원인

- 작업 tree: `/home/hep7/project/zlink-core-gate`, detached HEAD `c169e29eac71cfa7ed910b9bf9acb5faff367ad0` (`origin/main` fetch 후 checkout).
- Core 코드와 build는 위 worktree에서만 변경·실행했다. Main tree에는 요청받은 이 보고서만 작성했다. Commit은 생성하지 않았다.
- 원인: 기준 HEAD의 `core/src/runtime/core/pipe.cpp:1880`에서 `release_writes_for_transport_pair()`가 `check_hwm_unlocked()`만 호출했다. 80-byte RID preamble의 read credit이 sender cache에 반영되지 않으면 HWM 1에서 `_out_active=false`를 유지하면서 credit waiter도 등록하지 않았다. Peer가 먼저 읽은 경우에는 published snapshot을 놓치고, 나중에 읽은 경우에는 waiter가 없어 activation을 보내지 않았다.
- 관련 소유 코드: `pipe.cpp:1794`의 `hwm_credit_ready_unlocked()`, 기준 HEAD `pipe.cpp:3898`의 read-credit publication과 `pipe.cpp:3956`의 waiter 조건부 `activate_write` 전송.

| 변경 파일 | 변경 내용 |
|---|---|
| `core/src/runtime/core/pipe.cpp:1883` | Cached HWM 검사 대신 기존 `hwm_credit_ready_unlocked(NULL)` 사용 |
| `core/tests/integration/test_ready_empty_pipe_first_data.cpp:145` | 공개 C API로 첫 DATA admission·credit wake·재제출·수신 검증 |
| `core/tests/CMakeLists.txt:81` | 이웃 integration 테스트와 동일하게 등록 (`integration;serial`, timeout 10초) |

Hold 해제는 기존 helper의 snapshot → waiter 등록 → 재검사를 따른다. Credit이 이미 반환됐으면 즉시 write를 활성화하고, 아직 반환되지 않았으면 기존 waiter가 후속 read의 activation을 받는다. Inactive pipe의 조기 반환, remote PAUSED 검사, 빈 queue의 complete-message oversize 예외는 기존 소유 코드에 유지된다. 새 state, timer, retry 또는 reference 변경은 없다.

- 소유 계층: Core `pipe_t`의 write admission과 read-credit progression.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md` §6 “Part send와 pending admission”의 자원 회복·WRITABLE 1건 계약(986–1003행), `core/doc/spec/core/systems/06-auto-hwm.ko.md` §5의 빈 queue complete-message oversize admission(572행).
- 교차언어 대조: 감독의 Go 진단에 대응하는 동일 공개 C 재현을 원본 Core에서 실패, 수정 Core에서 성공으로 확인했다. Binding·Framework 변경은 없으며 Go suite 자체는 이 작업에서 실행하지 않았다.
- 변경 분류: **B — 기존 Core 결함**.
- 수정 전/후 규칙 수: **2 → 1**. 일반 admission의 snapshot/waiter 규칙과 hold 해제의 cached-only 규칙을 기존 helper 하나로 통합했다.

## 회귀 검증

테스트는 auto-HWM을 끄고 양쪽 SNDHWM·RCVHWM을 1로 설정한다. DEALER→ROUTER와 ROUTER→ROUTER 각각을 inproc·TCP에서 검증하며, 첫 DATA 전에는 application message를 보내지 않는다. READY edge가 monitor 개설 전에 발생한 경우에는 공개 monitor status의 READY snapshot으로 확인한다.

Inproc에서 reader-first는 sender의 connect-before-bind 후 ROUTER 수신을 진행하고 sender monitor를 나중에 열어, preamble credit이 hold 해제 전에 게시되는 순서를 만든다. Writer-first는 endpoint 역할을 뒤집어 ROUTER connector가 metadata로 RID를 채택하게 하고, 첫 송신이 거절된 뒤 public receive로 남은 preamble을 읽는다. TCP는 같은 public API 절차에서 binder·connector 양쪽 송신의 초기 admission을 검증한다.

첫 DATA는 즉시 admission되거나 nonzero token을 반환해야 한다. Token이면 새 송신 없이 receiver를 진행한 뒤 WRITABLE의 ID·context·RID·ADMITTED 결과를 검사하고 completion queue를 비운다. 같은 payload의 DONTWAIT 재제출은 성공해야 하며 ROUTER가 payload·source RID·FINAL·reply token 0을 정확히 받아야 한다. 추가 DATA와 중복 completion도 검사한다.

두 credit 순서를 실제로 구분하는지 다음 대조 구현으로 확인했다. 대조 구현은 모두 제거했으며 최종 patch에는 기존 helper 재사용만 남아 있다.

| Hold 해제 구현 | Inproc reader-first, DR/RR | Inproc writer-first, DR/RR | TCP 4개 |
|---|---|---|---|
| 원본 cached-only | 모두 WRITABLE 미발생으로 실패 | 모두 WRITABLE 미발생으로 실패 | 통과 |
| Snapshot만 사용, waiter 없음 | 통과 | 모두 WRITABLE 미발생으로 실패 | 통과 |
| Waiter만 등록, snapshot 없음 | 모두 WRITABLE 미발생으로 실패 | 통과 | 통과 |
| 최종 기존 helper 재사용 | 통과 | 통과 | 통과 |

원본·대조 로그: `/tmp/zlink-core-d118/regression-baseline.log`, `regression-snapshot-only-control.log`, `regression-waiter-only-control.log`.

## Build와 gate 결과

```bash
cmake -S core -B core/build-gate -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DWITH_TLS=ON -DBUILD_BENCHMARKS=ON
cmake --build core/build-gate -j 8
ctest --test-dir core/build-gate -R '^test_ready_empty_pipe_first_data$' --repeat until-fail:10 --output-on-failure
ctest --test-dir core/build-gate -R '^unittest_pipe_byte_charge$' --output-on-failure
bash core/tests/run_test_lanes.sh --build-dir core/build-gate --lanes integration
ctest --test-dir core/build-gate -R '^hotpath_gate$' -V
```

| 검증 | 결과 | 로그 (`/tmp/zlink-core-d118/`) |
|---|---|---|
| Release 전체 build, TLS·benchmarks·tests ON | exit 0, Core object와 hotpath executable에 `-O3 -DNDEBUG -flto=auto` 확인 | `configure.log`, `build-final.log` |
| 새 회귀 테스트 | 10/10 통과, 총 80개 조합 실행, 2.23초 | `regression-10x.log` |
| `unittest_pipe_byte_charge` | 통과 | `pipe-byte-charge.log` |
| Integration lane | **132/132 통과**, skip·failure 없음, 208.95초 | `integration.log` |
| `hotpath_gate` | **4/4 cell 통과**, 4.47초 | `hotpath.log` |
| 감독의 공개 C 재현 | **exit 0** | `repro-fixed.log`, `repro-ldd.log` |
| Patch 검증 | `git diff --check`, 새 파일 whitespace 검사, `git apply --check --cached` 및 reverse check 통과 | `/tmp/zlink-core-d118/fix.patch` |

Integration에는 `test_request_writable_contract`, `test_router_mandatory_hwm`, `test_flow_state_paired`, `test_flow_state_c_api`와 wake-invariant 테스트가 포함되며 모두 통과했다.

Hotpath 단위는 instruction/message이고 판정 범위는 기존 reference 대비 ±5%다.

| Cell | Reference | 측정 | 측정/reference | 판정 |
|---|---:|---:|---:|---|
| `dealer_dealer_inproc` | 3455.381000 | 3436.960900 | 0.9947 | PASS |
| `dealer_router_reqrep_inproc` | 12054.894800 | 12177.361200 | 1.0102 | PASS |
| `pair_inproc` | 2681.956600 | 2688.446000 | 1.0024 | PASS |
| `router_router_tcp` | 2972.881700 | 2971.164100 | 0.9994 | PASS |

`core/tests/perf/hotpath_reference.json`은 변경하지 않았다. SHA-256: `278c8ea5539ea96100f8d97a631536a1627dac8066624186df44ed1f33cbf5c0`.

## 공개 C 재현과 patch

Worktree root에서 다음 명령으로 요청된 executable을 다시 빌드·실행했다.

```bash
cc -std=c11 -Wall -Wextra -I core/include /tmp/zlink-go-prime/request-prime.c -L core/build-gate/lib -Wl,-rpath,/home/hep7/project/zlink-core-gate/core/build-gate/lib -lzlink -o /tmp/zlink-go-prime/request-prime
LD_LIBRARY_PATH=/home/hep7/project/zlink-core-gate/core/build-gate/lib timeout 10 /tmp/zlink-go-prime/request-prime
```

원본은 양쪽 READY 뒤 `SEND result=1 errno=11 id=1`, WRITABLE 없음으로 exit 1이었다(`repro-baseline.log`). 수정 후에는 양쪽 READY edge를 받은 뒤 `SEND result=0 errno=0 id=0`, ROUTER `RECV_OK`·payload 11 bytes, 최종 `PASS`로 exit 0이었다. `ldd`로 `/home/hep7/project/zlink-core-gate/core/build-gate/lib/libzlink.so.0` 사용을 확인했다.

Patch: `/tmp/zlink-core-d118/fix.patch` — 코드·테스트 3개 파일, 295 insertions / 1 deletion. SHA-256: `bc25062c6dbd5fa90805f212cb95c38caea3d63531eab1c12a87ac25af6f5d83`.

**BLOCKERS: 없음.** 요청된 검증은 모두 통과했으며, 남은 실패와 미완료 작업은 없다. 모든 `doc/spec/**`와 hotpath reference는 변경하지 않았다.
