# ABI regression tests summary

- .NET: `native_monitor_event_layout_matches_c_abi` — `bindings/dotnet/tests/Zlink.Tests/test_monitor_contract.cs`
  - `ZlinkMonitorEvent` size 800, offsets `ConnectionId=784`, `TransportLane=792`, `Flags=796` 고정.
- Rust: `monitor_event_layout_matches_c_abi` — `bindings/rust/src/runtime/native/ffi.rs`
  - 실제 private FFI mirror의 size 800과 offsets 784/792/796 고정. `memoffset` 없이 `MaybeUninit` + `addr_of!` 사용.
- Python: `test_ffi_layouts_are_the_core_0_17_layouts` 확장 — `bindings/python/tests/test_native_contract.py`
  - 기존 size/alignment assertion에 `connection_id`, `transport_lane`, `flags` offsets 784/792/796 추가.

## 실행 결과

- .NET 공식 `bindings/dotnet/tests/run_tests.sh` (새 테스트 filter): test 1/1 PASS, samples 7/7 PASS.
- Rust 공식 `bindings/rust/tests/run_tests.sh`: suites/samples 14/14 PASS. `CARGO_TARGET_DIR=/home/hep7hep7/project/zlink-work/c016/rust-target-abi`, jobs=2, stable Rust 1.97.1.
- Python 공식 `bindings/python/tests/run_tests.sh` (해당 ABI test): test 1/1 PASS, samples 7/7 PASS. 임시 venv와 in-place extension으로 검증 후 source-tree 생성 산출물 제거.
- `git diff --check`: PASS.

## BLOCKERS

- 없음.
