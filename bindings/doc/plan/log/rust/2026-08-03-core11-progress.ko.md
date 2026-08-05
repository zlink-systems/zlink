# Rust binding Core 0.9.0 최신화 진행 기록

이 기록은 2026-08-03에 Rust binding에서 수행한 Core 0.9.0 raw migration, candidate package 검증과 process
smoke 결과를 남긴다. Linux x86_64에서 확인한 결과이며, Go·Rust 공통 submit 계약 승인, 모든 지원 platform과
independent frontier review까지 완료했다는 뜻은 아니다.

## 현재 판정

Rust source, candidate-bound crate와 Linux x86_64 consumer·sample·source test·perf smoke는 통과했다.
그러나 현재 작업은 `PARTIAL / NOT CLEAN`이다.

- `Go·Rust submit 반환 초안`은 PGR-COMMON-03에서 승인되지 않았다. 따라서 현재 구현의 `bool`/`Result<bool,
  SubmitError>` 반환을 임의로 `error`/`Result<(), SubmitError>`로 바꾸지 않았다.
- V11-R2 evidence의 `independent` 값이 `false`이므로 구현자와 분리된 frontier review가 없다.
- Candidate-bound package는 현재 Linux x86_64만 포함한다. Linux arm64와 Darwin의 Core 0.9.0 runtime·native
  consumer evidence는 없다.

## 공통 candidate

| 항목 | 값 |
|------|-----|
| Candidate manifest | `.artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-reply-match-completion-hwm-20260801.json` |
| Candidate manifest SHA-256 | `d318525a4cf8496b6bef5d900c9a88330ea6d7e10ed4120ac0fd9f19d23f6765` |
| Candidate aggregate SHA-256 | `327587596195a162374498b630f51a043977dd392eb556061af615bf05186703` |
| V11-R2 review SHA-256 | `171a9cc8f7203500de08050dcb74ecd36b4c9ce55a75a14ce1bee283705c9e04` |
| Core runtime | `/home/hep7/project/kairos/zlink/.artifacts/wsl/install/zlink-core/11.1.0/lib/libzlink.so.0.1.0` |
| Core runtime SHA-256 | `b6fadc481c649b50637a9c0eb01d15a016e6ba4cd5bab967bdb6da4497a3c0c4` |
| Core provenance SHA-256 | `46f7bd17c0be3987fed14ca3cb594139e3edb778d3996248d23b6d2d6b53f693` |

## 구현과 설계 검토

Core 0.9.0 header를 Rust FFI의 유일한 입력으로 고정하고 service, actor, Spot projection과 Core 0.9.0에 없는
legacy symbol을 source·public re-export·sample·perf에서 제거했다. Generic operation은 raw socket 작업별
state로 나누고, request routing ID와 callback을 operation이 소유하도록 했다. 255-byte routing ID는
operation enum의 inline 크기를 불필요하게 키우지 않도록 한 번만 heap allocation한다. Callback은
`FnOnce + Send + 'static` 경계와 progress guard로 한 번만 완료한다.

이 선택은 호출부에 raw buffer, transport detail 또는 private FFI를 노출하지 않고, 메시지·native handle·callback의
수명과 소유권을 binding 내부 operation과 resource 경계가 관리하게 하기 위한 것이다. 필요한 allocation은 ownership과
Core ABI의 수명 조건으로 분류했으며, 불필요한 forwarding layer와 service compatibility surface는 삭제했다.

주요 checkpoint commit은 `2c68dd77bf`(raw Core 0.9.0 boundary), `bcb3346e9d`(operation state POSD/DDD refactor),
`ec8191ea40`(candidate-bound crate gate), `c8a9a63890`(candidate runtime resolver와 perf runner),
`a42e39faca`(candidate runtime source test runner)다.

## Candidate package checkpoint

실행 명령은 다음과 같다.

```bash
scripts/local-package/rust/build-wsl.sh \
  --platforms linux-x86_64 \
  --core-candidate-manifest /home/hep7/project/kairos/zlink/.artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-reply-match-completion-hwm-20260801.json \
  --core-package-evidence /home/hep7/project/kairos/zlink/.artifacts/v11/evidence/V11-M3-CORE-VERIFY/core-package-20260801.json \
  --output-root /home/hep7/project/kairos/zlink/.artifacts/wsl/rust-candidate-final
```

이 표는 `a42e39faca52cd02edb9f6ed9e7079b78e819e95` source revision에서 실행한 package gate checkpoint다.
이후 log와 README를 commit하면 source revision이 바뀌므로, 최종 gate는 같은 command로 다시 실행해야 한다.
최종 판정에서는 아래 경로의 현재 JSON이 source revision, package hash와 verification 결과의 권위 있는 입력이다.

`/home/hep7/project/kairos/zlink/.artifacts/wsl/rust-candidate-final/rust-package-11.1.0.json`

| 항목 | 값 |
|------|-----|
| Crate | `zlink-11.1.0` |
| Crate SHA-256 | `34b903fbeb03315d763fb610818778b9ed21ad9d38f3bfe620ea1abfa67ee11b` |
| Source manifest SHA-256 | `eccfee2d5ac1f7e60d0368d4b3677d07910a7ebd786477c384e3d714d120c3f4` |
| Source aggregate SHA-256 | `9fd6e41a8d418c22b8fbabb920595569455087e828023012d35c33c5b97ccd9d` |
| Platform | `linux-x86_64` |
| `cargo package` | `pass` |
| `cargo test --workspace --all-targets` | `pass` |
| `cargo clippy --all-targets -- -D warnings` | `pass` |
| Samples | `pass`, 7/7 |
| Clean consumer | `pass`, path dependency 없음, `LD_LIBRARY_PATH=unset` |
| Clean consumer linker condition | `package-derived-rustflags` |

Clean consumer가 RPATH를 package source에서 계산한 `RUSTFLAGS`로 주입한 사실은 evidence에 그대로 남긴다.
Cargo build script의 linker argument가 dependency consumer에 자동 전달된다고 해석하지 않는다.

## Source test와 perf smoke

Candidate runtime resolver를 통해 source test도 같은 Core runtime을 사용했다.

```bash
ZLINK_RUST_PACKAGE_EVIDENCE=/home/hep7/project/kairos/zlink/.artifacts/wsl/rust-candidate-final/rust-package-11.1.0.json \
  bindings/rust/tests/run_tests.sh
```

결과는 `Total: 11`, `Pass: 11`, `Fail: 0`이다. Perf smoke는 공식 report를 만들지 않도록 `--smoke`를 사용했다.
각 실행 뒤 지정한 임시 results directory에 file이 생성되지 않았다.

```bash
bindings/rust/perf/run_benchmarks.sh \
  --smoke --pattern PAIR --duration 1 --msg-sizes 64 --transports inproc --runs 1 \
  --rust-package-evidence /home/hep7/project/kairos/zlink/.artifacts/wsl/rust-candidate-final/rust-package-11.1.0.json

bindings/rust/perf/run_benchmarks_multi.sh \
  --smoke --pattern MULTI_DEALER_ROUTER --duration 1 --msg-sizes 64 --transports tcp --runs 1 --clients 1 \
  --rust-package-evidence /home/hep7/project/kairos/zlink/.artifacts/wsl/rust-candidate-final/rust-package-11.1.0.json
```

두 실행 모두 `SMOKE PASS`, exit code `0`이며 candidate runtime SHA-256
`b6fadc481c649b50637a9c0eb01d15a016e6ba4cd5bab967bdb6da4497a3c0c4`를 출력했다.

## 남은 조건

다음 조건이 충족되기 전에는 Rust 또는 Go·Rust 공통 최신화를 완료로 표시하지 않는다.

1. PGR-COMMON-03에서 submit 반환과 비동기 completion 정책을 승인하고, 승인된 public contract에 맞춘 두
   언어 구현·contract test·정식 문서를 같은 parity inventory에 반영한다.
2. Rust source와 package 전체 diff를 구현자와 분리된 frontier reviewer가 `contract`, `POSD`, `DDD`,
   `performance-cost`, `dead-code`, `test/evidence` category로 검토하고 `CLEAN`을 판정한다.
3. 지원 대상 각 platform에서 같은 Core candidate identity의 runtime provenance, package contents와 native
   consumer load를 확인한다. 현재 evidence는 Linux x86_64 한 platform만 포함한다.
