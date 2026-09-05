# Rust binding hot-path pass 1

최종 유효 after: 20/20 cells. 변경은 binding 내부 저장·호출 비용과 perf 사전 검사에 한정한다. 공개 API 시그니처·ownership·typed error·측정 의미는 유지한다.

## 원인과 처리

| 위치 | 측정·계약 근거 | 소유 계층 / 조치 |
|---|---|---|
| `src/contracts/messaging/operations.rs:18` | DD 16,380건·DR 3,040건의 Message Vec grow; 기본 2-part마다 한 번 | binding: 기존 first+rest를 inline 2 slots+rest로 변경; builder message에 inline |
| `src/runtime/messaging/operations/send_ops.rs:330` | DD/DR 모두 성공 part마다 errno 1회 | binding: rc가 실패일 때만 errno 조회 |
| `src/runtime/sockets/socket/socket_parts_runtime.rs:63` | basic/sub/router에 recv loop 중복, part마다 임시 init+wrapper move | binding: 기존 private scratch의 initialized Message를 직접 overwrite; FINAL 때만 공개 envelope 교환 |
| `src/runtime/sockets/socket/router.rs:49` | Core socket spec :538 RID view는 다음 data recv 진입 시 무효 | binding: 첫 part 직후 RID를 소유 값으로 복사; multipart 끝까지 borrowed pointer 보관 제거 |
| `perf/multi/src/perf_common.rs:244` | DD Box 1+Arc 1/건, DR 동일 비용에 request_task Box 1 추가 | runner 비용으로 분리; scheduler/drain/fairness 유지 |
| `perf/multi/src/perf_common.rs:327,338` | DD String 약 1/건, DR 약 2/건; 환경변수 조회 | runner 구성 조회 비용으로 분리; 이번 binding 개선 효과에 포함하지 않음 |
| `src/internal/completion_owner.rs:368,442,799` | REQUEST Arc entry·reply Vec·wake Vec | 기존 완료·반환 수명 유지; 공개 Future/entry pool은 가이드 §4에 따라 기각 |

## Callgrind 비교

10 clients, tcp, 64B+empty tail(2 parts), duration 1초, client만 Callgrind, server는 native. 이전 로그의 C와 Rust before 및 이번 final-after를 사용했다. 모든 프로파일 시작 load ≤3. `perf` 없음; `~/.local/bin/valgrind --tool=callgrind` 사용.

분모는 native send/request part 호출 수÷2인 제출 sequence 수다. 초기 handshake·종료와 Core I/O thread Ir도 포함하므로 순수 정상 상태의 binding 비용이 아니다. libc allocation은 Core+runner+Rust 전체이고, Rust allocator는 그 부분집합이다. 두 값을 더하지 않는다.

| Pattern / 버전 | 제출 수 | 총 Ir | Ir/제출 | libc malloc+calloc+realloc/제출 | Rust alloc/제출 |
|---|---:|---:|---:|---:|---:|
| dealer_dealer / C | 22,071 | 164,392,110 | 7,448.3 | 1.154 | 0.000 |
| dealer_dealer / Rust before | 16,385 | 187,599,221 | 11,449.4 | 5.818 | 4.205 |
| dealer_dealer / Rust after | 17,105 | 187,057,292 | 10,935.8 | 4.807 | 3.205 |
| dealer_router_reqrep / C | 6,070 | 126,873,523 | 20,901.7 | 2.804 | 0.000 |
| dealer_router_reqrep / Rust before | 3,040 | 99,255,402 | 32,649.8 | 13.061 | 8.464 |
| dealer_router_reqrep / Rust after | 3,150 | 100,082,643 | 31,772.3 | 11.952 | 7.430 |

| Pattern / 버전 | 전체 application→Core API 호출 | 호출/제출 |
|---|---:|---:|
| c-dealer_dealer / before | 154,678 | 7.008 |
| c-dealer_router_reqrep / before | 74,009 | 12.193 |
| rust-dealer_dealer / before | 280,300 | 17.107 |
| rust-dealer_router_reqrep / before | 91,184 | 29.995 |
| rust-dealer_dealer / final-after | 257,496 | 15.054 |
| rust-dealer_router_reqrep / final-after | 87,326 | 27.723 |

Core 내부의 public symbol 재호출은 제외했다. Rust executable→zlink_* edge, C executable→PLT→zlink_* edge를 Callgrind object/caller 관계로 구분했다. C PLT 호출 수는 executable에서 그 PLT로 들어오는 edge와 대조했다. 초기화·monitor·poll·종료 호출도 포함한다. 원자료: `rust-perf1-ffi-boundary.json`.

DD Ir/제출 −4.49%, DR −2.69%. Message Vec grow는 두 pattern 모두 1/건→0/건. Rust alloc 감소는 각각 4.205→3.205, 8.464→7.430회/제출이다. 5% 이상 처리량 이득과 별도로 구조 개선의 근거로 사용한다.

| 건당 비용(주 경로) | C DD / DR | Rust before DD / DR | Rust after DD / DR |
|---|---|---|---|
| binding part Vec grow | 0 / 0 | 1 / 1 | 0 / 0 |
| runner Box + Arc | 해당 Rust 객체 없음 | 1+1 / 2+1 | 1+1 / 2+1 |
| runner 환경변수 String | getenv의 Rust String 없음 | 약 1 / 약 2 | 약 1 / 약 2 |
| binding REQUEST Arc entry + reply Vec | 해당 Rust 객체 없음 | 0 / 1+1 | 0 / 1+1 |
| 선택 FFI: send/request_part | 2 / 2 | 2 / 2 | 2 / 2 |
| 선택 FFI: zlink_msg_copy | 0 / 0 | 2 / 2 | 2 / 2 |
| 선택 FFI: 성공 zlink_errno | 0 / 0 | 2 / 2 | 0 / 0 |
| 수신 scratch part의 init+wrapper move | C native storage | part마다 | warm-up/growth 뒤 0 (basic/router/sub) |

FFI 표는 주 경로의 선택 symbol 호출이며 전체 native 호출 수로 오해하지 않아야 한다. `zlink_msg_copy`는 Core 공유-copy API 호출 수다. 64B inline body에서는 header 안의 body 복사가 포함되며 큰 body는 refcount 공유다. compiler가 inline한 memcpy의 byte 수는 Callgrind에서 독립 계수하지 않았으므로 추정치를 실측처럼 제시하지 않는다. DD/DR client 프로파일은 변경한 router/sub 수신 재사용 비용 자체를 직접 측정하지 않는다.

## 후보 판정

- 가이드 §2.1: 즉시 성공 SEND에는 이미 lazy completion 등록을 사용한다. 이번 pass는 2-part staging 할당과 성공 errno 호출만 제거했다. close와 경쟁하는 shared submit RwLock은 그대로 필요하므로 전체 경로가 무락이라고 판정하지 않는다.
- §2.2: token/context 조회는 기존 HashMap, 선행 WRITABLE 보류·재생은 기존 owner 한 곳에 있다. 별도 index나 retry 상태를 추가하지 않았다.
- §2.3: 기존 public/runtime owner 전환과 NO_DATA까지 drain을 유지한다. timeout-0 spin, 새 poller, budget/retry 증가는 적용하지 않았다.
- §2.4: 임시 수신 wrapper 생성 대 private scratch 재사용을 비교해 재사용을 선택했다. 공개 wrapper pool을 만들지 않았다.
- snapshot 제거 대 native 공유-copy 유지를 비교했다. Core socket spec :931은 실패에도 part를 소비하므로, Rust 재제출 ownership을 보존하기 위해 copy를 유지한다.
- 공개 Future/entry pool, scheduler 변경, 인위적 in-flight 제한은 기각 목록·요청 범위상 적용하지 않았다.

## 별도 runner 버그

`perf/run_benchmarks{,_multi}.sh`는 worktree checkout mtime을 library build freshness로 해석했다. `core/src/version.rc.in`은 19:36:16, library는 19:11:21이었지만 build 원본과 worktree의 전체 src/include 내용은 같았다. `perf/core_runtime.sh`로 중복 prepare 함수를 통합하고 CMakeCache의 원본과 내용 동일성을 확인한 뒤 원본의 수정 시각을 검사한다. 내용 불일치·stale 원본·파일 누락은 거부한다. 초기 실행은 측정 전 exit 1; Core 재빌드를 실행하지 않았다. 이 변경은 사전 검사에만 적용되며 처리량 효과에 합산하지 않는다.

## 처리량 before / after

단위 Kmsg/s, REQREP는 Kops/s. TCP, 100 clients, 5초, 1 run, sizes 64/256/1024/4096/65536, 기본 I/O 4/4와 2 parts. C와 before는 사용자 지정 19:28~19:31 report. after는 각 셀을 load ≤2.3에서 시작하고 0.5초마다 검사하여 관측 load ≤3인 실행만 채택한다. 각 셀의 duration/runs와 runner 내부 측정 의미는 그대로다.

요청한 전체 matrix 명령은 status complete/20 cases/100 result lines로 끝났으나 실행 중 load 3.76을 관측하여 `perf_rust_multi_linux_20260905_204035.txt`를 유효 after에서 제외했다. 유효 셀 재실행은 높은 load라는 검증 문제 때문이며 결과 변동에 따른 반복이 아니다.

| Pattern | B | C | Before | After | Δ before | Before/C | After/C |
|---|---:|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 1134.4 | 494.7 | 450.4 | -9.0% | 43.6% | 39.7% |
| DEALER_DEALER | 256 | 1066.7 | 498.5 | 561.3 | +12.6% | 46.7% | 52.6% |
| DEALER_DEALER | 1024 | 859.7 | 411.0 | 410.8 | -0.0% | 47.8% | 47.8% |
| DEALER_DEALER | 4096 | 267.8 | 205.3 | 273.0 | +32.9% | 76.7% | 101.9% |
| DEALER_DEALER | 65536 | 56.5 | 56.6 | 73.2 | +29.4% | 100.1% | 129.5% |
| DEALER_ROUTER_REQREP | 64 | 160.1 | 108.6 | 137.5 | +26.6% | 67.8% | 85.9% |
| DEALER_ROUTER_REQREP | 256 | 160.5 | 101.9 | 146.3 | +43.6% | 63.5% | 91.1% |
| DEALER_ROUTER_REQREP | 1024 | 149.6 | 104.7 | 126.9 | +21.2% | 70.0% | 84.8% |
| DEALER_ROUTER_REQREP | 4096 | 140.9 | 92.8 | 125.5 | +35.2% | 65.8% | 89.0% |
| DEALER_ROUTER_REQREP | 65536 | 23.5 | 14.7 | 15.2 | +3.8% | 62.4% | 64.8% |
| ROUTER_ROUTER_REQREP | 64 | 144.8 | 97.4 | 117.2 | +20.2% | 67.3% | 80.9% |
| ROUTER_ROUTER_REQREP | 256 | 136.7 | 90.2 | 89.5 | -0.8% | 66.0% | 65.5% |
| ROUTER_ROUTER_REQREP | 1024 | 135.0 | 89.2 | 111.8 | +25.3% | 66.1% | 82.8% |
| ROUTER_ROUTER_REQREP | 4096 | 116.8 | 81.0 | 102.3 | +26.3% | 69.3% | 87.5% |
| ROUTER_ROUTER_REQREP | 65536 | 22.1 | 15.9 | 15.3 | -3.7% | 72.3% | 69.6% |
| PUBSUB | 64 | 655.3 | 443.9 | 710.3 | +60.0% | 67.7% | 108.4% |
| PUBSUB | 256 | 828.3 | 588.6 | 847.3 | +44.0% | 71.1% | 102.3% |
| PUBSUB | 1024 | 850.3 | 719.7 | 856.1 | +19.0% | 84.6% | 100.7% |
| PUBSUB | 4096 | 671.2 | 584.1 | 682.8 | +16.9% | 87.0% | 101.7% |
| PUBSUB | 65536 | 64.0 | 67.5 | 67.8 | +0.5% | 105.4% | 105.9% |

| Pattern | Before/C 평균 | After/C 평균 | After/C latency 평균 배수 | 목표 | 판정 |
|---|---:|---:|---:|---:|---|
| DEALER_DEALER | 63.0% | 74.3% | 3.01x | 95% | 목표 미달; pass 2 대상 |
| DEALER_ROUTER_REQREP | 65.9% | 83.1% | 1.22x | 85% | 목표 미달; pass 2 대상 |
| ROUTER_ROUTER_REQREP | 68.2% | 77.3% | 1.55x | 85% | 목표 미달; pass 2 대상 |
| PUBSUB | 83.2% | 103.8% | 0.99x | 95% | 처리량 목표 충족; pass 2 review 전 |

기존 before의 report META load에는 3 초과 값도 있다. before/after는 1 run의 서로 다른 시점이므로 차이를 전부 코드 효과로 확정하지 않는다. C report의 core_revision은 `30112c0639`로 브리프의 `a40cb46335`와 다르며, 비교에는 지정된 같은 runtime 경로의 artifact를 사용했다. 이 작업에서 Core 변경·재빌드는 없다. 현재 library SHA256: `d4b95b10ce3f96315740de20d2ea4e1d1d40f3dffd7a50de5530d26dff9a3f00`.

## Gate

- 공식 `ZLINK_CORE_SOURCE=local CARGO_BUILD_JOBS=3 bash bindings/rust/tests/run_tests.sh`: PASS 14/14, samples 7/7 포함 (`rust-perf1-gate.log`).
- receive_failure_tests, send_failure_tests, routed_async_tests, ownership_tests: 각 5회 PASS (`rust-perf1-related-{1..5}.log`).
- `cargo clippy -j3 --all-targets -- -D warnings`: PASS; `cargo fmt --check`: PASS; `git diff --check`: PASS.
- 공유 runtime 검사: 동일 내용의 새 worktree 허용, 다른 내용 거부, 실제 stale 거부, 원본 파일 누락 거부: PASS. shell syntax PASS.
- 변경 source의 공개 선언 줄 비교 및 surface/contract tests PASS. 외부 API 시그니처·ownership·error contract 불변.
- `CARGO_TARGET_DIR=/home/hep7hep7/project/zlink-work/c016/rust-target-perf1`; 직접 cargo `-j3`, 공식 shell 내부 cargo는 같은 의미의 `CARGO_BUILD_JOBS=3`.
- detached 상태 유지. commit/push/reset/checkout/stash 없음. 기존 core/build·build-dev symlink 보존.

## 변경 파일

- `bindings/rust/src/contracts/messaging/operations.rs`
- `bindings/rust/src/runtime/messaging/operations/send_ops.rs`
- `bindings/rust/src/runtime/sockets/socket/{router,socket_parts_runtime}.rs`
- `bindings/rust/tests/receive_failure_tests.rs`
- `bindings/rust/perf/{core_runtime,run_benchmarks,run_benchmarks_multi}.sh`

소유 계층: binding의 part storage·native receive metadata·completion lifetime; runner는 runtime artifact 검증만 담당한다.
Spec 조항: Core socket README :522–543 initialized overwrite/실패 불변/RID view lifetime, :931–967 모든 send part 소비; Rust README :398 socket-local completion owner.
교차언어 대조: C는 native storage, C++/.NET은 inline staging·수신 재사용. .NET pass 1도 실패 ownership 때문에 native snapshot을 유지한다. Rust의 기존 Vec 첫-part 구조와 값 이동 비용을 바꾼 것이며 공통 contract를 다르게 구현한 것이 아니다.
변경 분류: B 기존 비용·RID borrowed lifetime 결함 및 runner mtime 오탐 수정. Framework runtime 변경 없음.
수정 전/후 규칙 수: recv loop 소유 3→1, perf runtime 검사 소유 2→1; completion 제어점 1→1.
Spec gap: 없음. 새 public API·spec 수정 요구 없음.

## BLOCKERS

실행 차단 없음. 유효 after 20/20 complete, 관측 최대 load 2.974. 성능 목표 미달 항목은 위 표에 기록하며 pass 1만으로 최종 승인하지 않는다. Sol read-only review·pass 2는 이번 역할 밖의 다음 단계다. 기존 before load 및 source-revision 메타데이터 차이는 비교 해석의 제한이다.

## 산출물

- 진행 로그: `/home/hep7hep7/project/zlink-work/c016/rust-perf-pass1-progress.md`
- before 프로파일: `/home/hep7hep7/project/zlink-work/c016/rust-profile-before`
- after 프로파일: `/home/hep7hep7/project/zlink-work/c016/rust-profile-final-after`
- 유효 after 목록과 load: `/home/hep7hep7/project/zlink-work/c016/rust-perf1-valid-after.json`
- 유효 report 복사본: `/home/hep7hep7/project/zlink-work/c016/reports/rust_p1_after_*.txt`
- C before: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_192830_p1rust.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_192929_p1rust.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_193023_p1rust.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_193117_p1rust.txt`
- Rust before: `/home/hep7hep7/project/zlink/bindings/rust/perf/results/multi/report/perf_rust_multi_linux_20260905_192857.txt`, `/home/hep7hep7/project/zlink/bindings/rust/perf/results/multi/report/perf_rust_multi_linux_20260905_192956.txt`, `/home/hep7hep7/project/zlink/bindings/rust/perf/results/multi/report/perf_rust_multi_linux_20260905_193050.txt`, `/home/hep7hep7/project/zlink/bindings/rust/perf/results/multi/report/perf_rust_multi_linux_20260905_193144.txt`
