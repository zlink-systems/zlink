# Rust binding review pass 2

기준 detached `fa7136cf1f9864e4029c96bbb5ea0a62c089d7c5`; 변경 범위 `bindings/rust/**`. **완료: 공식 gate 14/14·관련 5회·clippy/fmt/diff PASS, 유효 after 20/20(최대 load 1.916). DD/DR 목표 미달, RR/PUBSUB는 지정 C 평균 대비 처리량 목표 충족.**

## 구현 전 read-only 후보 판정

| 후보 | 근거·예상 효과 | 계약 보존 | 가이드 §4 | 판정 |
|---|---|---|---|---|
| (a) SEND snapshot 제거 | socket spec :931은 실패에도 part를 소비하므로 재시도 원본 필요 | 제거하면 ownership 위반 | public 계약 제약 | no-go |
| (a) sequence당 scratch init/close 한 번 | 현재 2-part DD FFI 15.038회/제출; 2회 감소 가능. Native 실제 PAIR DONTWAIT+recv mini rate -1.25/+1.70/+4.95%(64/1024/65536B) | initialized empty descriptor 재사용 가능 | 해당 없음 | 5% 미만, no-go |
| (b) RwLock → atomic closed 확인 | DD with_submit inclusive 차이 956250 Ir / 전체 203200930 = 0.471%; 해당 profile에서 lock 대기 호출 없음 | load→close→FFI 경쟁으로 handle 수명 보장 상실. 안전한 reader count+close wait는 RwLock 정책 중복 | 직접 항목은 아니나 새 수명 규칙 금지 | no-go |
| (c) REQREP pending 번들 | CompletionEntry는 이미 Arc 1개 안에 Mutex<EntryState>·Condvar·target 보유. 분리된 C++ resume slot이 없음 | Future와 registry가 공유하는 entry를 Future 안으로 이동하거나 재사용하면 late completion 수명 변경 | entry/Future identity pool 금지 | no-go |
| (d) 일반 수신 wrapper 재사용 | recv_part_sequence는 pass 1부터 private scratch를 직접 overwrite. 추가 공개 wrapper pool은 중복 | pool은 공개 소유권·수명 침해 | public wrapper pool 금지 | no-go |
| (d) REQUEST reply를 Vec 원소에 직접 adopt | take_completion_parts :799–813 (변경 전)의 part당 init+move+wrapper 값 이동; C++ completion_owner.cpp:439는 이미 direct adopt. Native mini rate +53.5/+54.5/+57.8% | Core message spec §zlink_msg_adopt :288–310, caller-owned uninitialized destination; source empty, completion_close와 Message drop이 각 소유권 정리 | pool 아님, 해당 없음 | 적용 후보 |

미니벤치는 native helper 비용의 비교이며 TCP 전체 처리량 5% 개선을 뜻하지 않는다. 크기별 ABABAB 3쌍 중앙값을 사용했고 모든 결과를 보존한다.

현재 같은 조건 Callgrind는 C DD 7.008 FFI/제출, DR 12.177이며 브리프의 C 13회는 같은 계수 범위에서 재현되지 않았다. Core 내부 public symbol 재호출은 제외한다.

## 변경·계약

- `bindings/rust/src/internal/completion_owner.rs`: REQUEST reply part를 최종 Vec 원소의 미초기화 native field에 직접 adopt한다. 인수 성공 뒤에만 Vec 길이를 늘린다. 실패 시 기존 RequestError::InternalError 도메인으로 errno를 전달하고 인수한 prefix와 completion 소유 나머지를 각 owner가 정리한다. 일반적인 성공·timeout·WRITABLE·detach·close 경로는 그대로다.
- `bindings/rust/src/runtime/native/ffi.rs`: Core에 이미 존재하는 `zlink_msg_adopt`의 비공개 FFI 선언을 추가했다. `mod ffi`는 비공개다.
- 테스트는 빈 reply, 빈 tail, inline·large payload, completion source 해제 뒤 reply 수명, shared source refcount, 부분 인수 실패의 해제를 검증한다.

소유 계층: binding CompletionOwner의 native reply 인수·Rust Message 소유권. completion drain·재시도·close 제어는 기존 owner에 남는다.
Spec 조항: `core/doc/spec/core/02-message.ko.md:288–310`(adopt의 destination/source 수명), `core/doc/spec/core/socket/README.ko.md:931–967`(snapshot 필요), `bindings/doc/spec/rust/README.ko.md:398`(socket-local completion owner·drop detach).
교차언어 대조: C++ `completion_owner.cpp:439`의 최종 vector 직접 adopt를 사용했다. Rust entry는 이미 단일 Arc bundle이며 C++의 별도 resume-slot allocation이 없어 그 변경은 적용 대상이 아니다. Go는 callback/context ownership과 cgo 경계가 달라 번들·pool 변경을 포팅하지 않았다. Framework runtime 변경 없음.
변경 분류: B 기존 reply 인수 경로의 중복 초기화·값 이동 비용 제거.
수정 전/후 규칙 수: reply 인수 단계 `init → move → wrapper push` 3→`최종 원소 adopt` 1; completion 제어 owner 1→1. 새 pool·index·timer·retry 규칙 없음.
Spec gap: 없음. 공개 API·ownership·typed error 도메인·측정 의미 유지. 공개 contract/export/include/Cargo.toml 및 모든 runner diff 0(`rust-perf2-api-scope.json`).

## Callgrind

10 clients, TCP, 64B + 빈 tail, 1초, I/O 2/2. Client만 Callgrind, server native. C·Rust 순차 실행, 시작 `uptime` load≤3. Core artifact SHA-256는 모든 단계에서 `d4b95b10ce3f96315740de20d2ea4e1d1d40f3dffd7a50de5530d26dff9a3f00`. 전체 Ir에는 setup·drain·Core I/O thread 비용이 포함돼 순수 정상 상태의 binding 비용이 아니다. 제출 분모는 send/request part 호출÷2이며 deadline 근처 실패/정리도 포함한다.

| Pattern | 버전 | 제출 | Ir/제출 | application→Core FFI/제출 |
|---|---|---:|---:|---:|
| dealer_dealer | c before | 22899 | 7261.0 | 7.008 |
| dealer_router_reqrep | c before | 6420 | 20274.8 | 12.177 |
| dealer_dealer | rust before | 18745 | 10840.3 | 15.038 |
| dealer_router_reqrep | rust before | 3020 | 32081.2 | 27.721 |
| dealer_dealer | rust after | 12945 | 11077.3 | 15.042 |
| dealer_router_reqrep | rust after | 3010 | 31661.8 | 25.759 |

REQREP Ir/제출 −1.31%. 선택한 reply 경로는 init 2→0, move 2→0, adopt 0→2회/제출로 FFI 2회를 제거했다. 이 변경이 TCP 전체 처리량을 독립적으로 5% 올렸다는 증거는 아니며, 채택 근거는 native helper의 5% 이상 효과와 소유권 인수 단계 감소다. 일반 SEND 경로는 바꾸지 않았고 DD FFI는 15회로 같다. Entry Arc allocation과 공개 reply Vec allocation은 각각 1회/REQUEST로 유지된다. Before의 register_request allocator 3020/3020, reply allocator 3020/3020으로 확인했다. 별도 resume slot·per-entry map node allocation은 없으며, HashMap insert/remove 비용 합계도 전체 DR Ir의 약 0.98%다.

FFI 계수는 Rust executable→Core, C executable→PLT→Core edge로 한정한다. Core 내부에서 public symbol을 다시 호출하는 edge는 제외한다. 모든 symbol별 계수는 `rust-perf2-profile-metrics.json`, parser·raw profile은 `rust-perf2-profile-analysis.py`와 `rust-profile-pass2-{before,after}/`에 있다.

## Gate

- 공식 `CARGO_TARGET_DIR=... ZLINK_CORE_SOURCE=local CARGO_BUILD_JOBS=3 bash bindings/rust/tests/run_tests.sh`: **PASS 14/14**, samples 7/7 포함.
- `cargo test -j3 --lib --test routed_async_tests --test receive_failure_tests --test send_failure_tests --test ownership_tests -- --test-threads=1`: **5회 PASS**. 매회 lib 16, ownership 9, receive_failure 6, routed_async 18, send_failure 9 테스트.
- `cargo clippy -j3 --all-targets -- -D warnings`: **PASS**.
- `cargo fmt --check`, `git diff --check`: **PASS**.
- 모든 Cargo build/test/clippy는 지정된 `rust-target-perf2`, 직접 명령 `-j3` 또는 공식 shell의 `CARGO_BUILD_JOBS=3`를 사용했다.
- 최초 build는 PATH의 Cargo 1.75가 Edition 2024를 지원하지 않아 실패했다. 테스트 러너와 같은 `~/.cargo/env` 적용 후 통과했으며 Cargo.toml을 바꾸지 않았다. 변경 직후 fmt가 지적한 해당 파일의 형식만 수정했고 최종 gate는 모두 통과했다. 남은 기능 테스트 실패 없음.
- Core configure/build/clean과 git commit/push/reset/checkout/stash 실행 없음. 기존 untracked `core/build`, `core/build-dev` 보존.

## 공식 after 비교 조건

100 clients, TCP, sizes 64/256/1024/4096/65536, 5초, 1 run, 기본 I/O 4/4·2 parts·auto-HWM·scheduler·drain·fairness 유지. 요청한 전체 matrix 명령은 22:39:57 load 0.90에서 시작했으나 22:40:42 load 3.0337을 관측해 중단(exit −15). 러너는 exit 1이고 complete report는 없으므로 판정에서 제외했다.

동일 셀의 공식 runner를 `--reuse-build --output <artifact>`로 실행하며 각 셀 시작은 load≤1.3에서 기다리고 실행 중 0.5초마다 검사한다. 셀을 실행 중 정지·재개하거나 duration을 늘리지 않는다. 재실행은 위 load 위반 때문이다. 유효 셀은 각각 runs 1만 채택하고 낮은 결과를 이유로 반복하지 않는다. 유효 report는 `reports/rust_p2_after_*.txt`, 전체 load 표본은 같은 tag의 `*-load.json`, 목록은 `rust-perf2-valid-after.json`에 보존한다.

C 기준은 요청한 `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/*_p1rust-r3q2.txt` 4개다. 작업 worktree에는 ignored report가 없어 원본 checkout에서 읽었다. Report의 `RESULT`는 median이므로 **run 1/2/3 표의 산술평균을 다시 계산**했다(표의 k/s는 소수 3자리 정밀도). Rust before도 같은 시점의 `213747`, `214045`, `214337`, `214629` report의 run별 평균이다.

브리프의 64.7/66.1/69.1/93.8%는 median/median 기준이고, 요청한 mean/mean으로 재계산한 before는 **62.98/66.01/68.62/92.74%**다. 이 통계 차이를 코드 효과에 합산하지 않는다. 또한 지정 C report의 load META는 **3.86/4.70/4.61/5.04**, Rust DD before META는 5.03으로 기록되어 있다. 기존 값을 교체하지 않지만 저부하 paired before라고 확정하지 않으며, after/C 및 before 대비 차이는 이 부하 차이의 영향을 포함한다. 과거 C report에는 runtime 경로·revision은 있지만 SHA-256이 없어, 과거 artifact byte 동일성을 이 report만으로 재확인할 수 없다.

## Native helper 미니벤치

Core 공개 C API만 사용하는 별도 진단 프로그램(`rust-perf2-native-mini.c`)을 `cc -O3`로 빌드했다. Binding·공식 runner는 이 프로그램을 호출하지 않는다. 각 size는 before/after를 번갈아 3회 실행한 중앙값이고 원시 CSV를 보존했다. Reply 인수는 동일 template의 shared copy→인수→close를 500,000회, SEND 후보는 실제 PAIR inproc 2-part DONTWAIT submit→recv를 100,000회 실행했다. SEND는 blocking warmup 뒤 모든 timed submit 성공을 assert한다. 후자는 네트워크 TCP throughput 측정이 아니라 scratch lifecycle 후보의 상한 확인이다.

| 후보 | B | Before ns/record | After ns/record | 작업률 변화 |
|---|---:|---:|---:|---:|
| reply_adopt | 64 | 64.198 | 41.826 | +53.49% |
| reply_adopt | 1024 | 64.432 | 41.702 | +54.51% |
| reply_adopt | 65536 | 64.755 | 41.030 | +57.82% |
| shared_submit | 64 | 647.540 | 655.745 | -1.25% |
| shared_submit | 1024 | 676.556 | 665.258 | +1.70% |
| shared_submit | 65536 | 777.212 | 740.543 | +4.95% |

## 공식 after 결과

단위 Kmsg/s, REQREP는 Kops/s. Latency는 각 report의 ms 값을 사용한 after/C 비율이다. 모든 size 비율의 산술평균으로 aggregate를 계산했다.

| Pattern | B | C mean k/s | Before mean k/s | After k/s | Δ before | After/C | Latency After/C |
|---|---:|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 962.802 | 450.294 | 456.584 | +1.4% | 47.4% | 1701.463x |
| DEALER_DEALER | 256 | 914.290 | 387.471 | 519.806 | +34.2% | 56.9% | 0.318x |
| DEALER_DEALER | 1024 | 783.023 | 364.149 | 455.572 | +25.1% | 58.2% | 0.002x |
| DEALER_DEALER | 4096 | 308.888 | 199.876 | 297.998 | +49.1% | 96.5% | 0.533x |
| DEALER_DEALER | 65536 | 61.689 | 70.672 | 108.809 | +54.0% | 176.4% | 0.646x |
| DEALER_ROUTER_REQREP | 64 | 190.341 | 128.556 | 148.071 | +15.2% | 77.8% | 1.048x |
| DEALER_ROUTER_REQREP | 256 | 176.716 | 117.314 | 143.053 | +21.9% | 81.0% | 0.926x |
| DEALER_ROUTER_REQREP | 1024 | 154.602 | 100.159 | 133.716 | +33.5% | 86.5% | 0.931x |
| DEALER_ROUTER_REQREP | 4096 | 137.433 | 95.841 | 128.039 | +33.6% | 93.2% | 0.778x |
| DEALER_ROUTER_REQREP | 65536 | 22.930 | 14.122 | 17.035 | +20.6% | 74.3% | 2.176x |
| ROUTER_ROUTER_REQREP | 64 | 159.658 | 106.747 | 119.309 | +11.8% | 74.7% | 1.088x |
| ROUTER_ROUTER_REQREP | 256 | 130.028 | 92.693 | 120.965 | +30.5% | 93.0% | 1.026x |
| ROUTER_ROUTER_REQREP | 1024 | 126.283 | 85.911 | 121.521 | +41.4% | 96.2% | 0.973x |
| ROUTER_ROUTER_REQREP | 4096 | 110.437 | 81.036 | 111.698 | +37.8% | 101.1% | 0.990x |
| ROUTER_ROUTER_REQREP | 65536 | 20.840 | 13.243 | 17.007 | +28.4% | 81.6% | 1.943x |
| PUBSUB | 64 | 687.455 | 592.679 | 763.603 | +28.8% | 111.1% | 1.186x |
| PUBSUB | 256 | 695.452 | 651.856 | 938.484 | +44.0% | 134.9% | 1.057x |
| PUBSUB | 1024 | 759.683 | 720.659 | 968.099 | +34.3% | 127.4% | 0.711x |
| PUBSUB | 4096 | 609.026 | 587.339 | 792.309 | +34.9% | 130.1% | 0.634x |
| PUBSUB | 65536 | 62.843 | 58.104 | 67.103 | +15.5% | 106.8% | 0.723x |

| Pattern | Before/C means | After/C mean | Δ before mean | Latency ratio mean |
|---|---:|---:|---:|---:|
| DEALER_DEALER | 63.0% | 87.1% | +32.7% | 340.592x |
| DEALER_ROUTER_REQREP | 66.0% | 82.5% | +25.0% | 1.172x |
| ROUTER_ROUTER_REQREP | 68.6% | 89.3% | +30.0% | 1.204x |
| PUBSUB | 92.7% | 122.1% | +31.5% | 0.862x |

유효 셀 20/20, RESULT 100행, 모든 셀 success 1/fail 0/status complete. 최대 관측 load **1.916**, 각 셀 첫 실행(a1)만 사용했다. 전체 matrix의 load 위반 실행을 제외한 뒤 유효 셀을 다시 고른 반복은 없다.

## 최종 판정·남은 미달

- **DEALER_DEALER 보류**: aggregate 87.1% < 목표 95%. 64/256/1024B는 47.4/56.9/58.2%로 개별 최소 85% 미달이다. 64B 지연은 698.734ms / C 반복 평균 0.410667ms = 1701.46x이며 전체 latency ratio 평균은 340.59x다. 64B 지연을 제외하거나 재측정하지 않았다.
- **DEALER_ROUTER_REQREP 보류**: aggregate 82.5% < 목표 85%. 개별 최소 70%는 모두 충족한다. 65536B latency ratio 2.176x를 남기며 aggregate latency ratio는 1.172x다.
- **ROUTER_ROUTER_REQREP**: aggregate 89.3% ≥ 목표 85%, 개별 최소 70% 충족. Latency ratio 평균 1.204x.
- **PUBSUB**: aggregate 122.1% ≥ 목표 95%, 개별 최소 85% 충족. Latency ratio 평균 0.862x.
- 전체 before 대비 차이는 DD +32.7%, DR +25.0%, RR +30.0%, PUBSUB +31.5%다. **변경하지 않은 DD·PUBSUB도 크게 상승했고 기존 baseline load가 높으므로 이를 binding 변경의 독립적인 처리량 효과로 확정하지 않는다.** 검증된 변경 효과는 native helper 작업률 +53.5~57.8%, REQUEST당 FFI 2회 감소, Callgrind 전체 Ir/제출 −1.31%다.
- wrapper/entry/Future pool, atomic-only close, 추가 map index, runner 수정은 적용하지 않았다. 추가 계약 보존 후보의 5% 이상 근거가 없어 본 pass를 종료한다. 성능 미달은 기능 실패와 구분한다. 남은 기능 테스트 실패·spec gap 없음.

## 산출물

- 요약: `/home/hep7hep7/project/zlink-work/c016/rust-perf-pass2-summary.md`
- 진행: `/home/hep7hep7/project/zlink-work/c016/rust-perf-pass2-progress.md`
- after 원본 복사: `/home/hep7hep7/project/zlink-work/c016/reports/rust_p2_after_*.txt`
- after 합본(원본 셀별 metadata 유지): `/home/hep7hep7/project/zlink-work/c016/reports/rust-perf-pass2-after.txt`
- baseline 평균·반복값: `/home/hep7hep7/project/zlink-work/c016/rust-perf2-baseline-means.json`
- 전체 비교: `/home/hep7hep7/project/zlink-work/c016/rust-perf2-comparison.json`
- profile 계수: `/home/hep7hep7/project/zlink-work/c016/rust-perf2-profile-metrics.json`
- profile 원자료: `/home/hep7hep7/project/zlink-work/c016/rust-profile-pass2-before`, `/home/hep7hep7/project/zlink-work/c016/rust-profile-pass2-after`
- mini 원자료: `/home/hep7hep7/project/zlink-work/c016/rust-perf2-native-mini.csv`
- gate 로그: `/home/hep7hep7/project/zlink-work/c016/rust-perf2-gate-*.log`; 종료 코드 `/home/hep7hep7/project/zlink-work/c016/rust-perf2-gates.json`
- 공개 API·범위 확인: `/home/hep7hep7/project/zlink-work/c016/rust-perf2-api-scope.json`
- 최종 report/load 검증: `/home/hep7hep7/project/zlink-work/c016/rust-perf2-final-validation.json`

작업 종료 코드: `EXIT:0` (리뷰·구현·측정·gate 완료, 성능 목표 미달은 위 보류 항목에 기록).
