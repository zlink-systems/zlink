# gate v2 C++ Framework — rebuilt 0.17.0 packages

## Result

The C++ gate did not pass.  The full build succeeded, and the focused M6
binaries are green on direct repetition.  CTest completed **63/69** tests;
six failures are classified below.

| Suite | Pass | Fail | Result |
|---|---:|---:|---|
| CMake preset refresh | 1 | 0 | pass |
| Full Debug build | 1 | 0 | pass (246 Ninja actions) |
| Full CTest (`-j2`) | 63 | 6 | fail |
| `test_cpp_framework_m6a_runtime` direct (3 runs) | 3 | 0 | pass |
| `test_cpp_framework_m6b_runtime` direct (3 runs) | 3 | 0 | pass |
| Focused recheck of the four connector/package failures | 0 | 4 | fail; all reproduced |

No source, test, Core, binding, package, or protected-document file was
modified. This task changed only this worklog summary.

## Failure classification

| CTest case | Bucket | Exact assertion/error and location | Evidence and classification |
|---|---|---|---|
| `test_cpp_framework_m6b_runtime` | C — known pre-existing | `send () == zlink::submit_result_t::not_found`, [`test_cpp_framework_m6b_runtime.cpp:1909`](../../../framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp#L1909) | Exact same assertion predates this gate at [`bucketB-cpp-m6.log:14763`](../../../zlink-work/c016/logs/bucketB-cpp-m6.log#L14763). This gate's direct binary runs passed 3/3, so the parallel-CTest occurrence is not classified as a D-B85 regression. |
| `test_cpp_framework_common_e2e_inventory` | C — known pre-existing | `FAIL: 278 required inventory conditions are open`, [`verify_common_inventory.sh:161`](../../../framework/languages/cpp/e2e/verify_common_inventory.sh#L161) | Explicitly supplied as known; prior record labels the C++ inventory 278 as known at [`c-cross-language-e2e.log:55`](../../../zlink-work/c016/logs/c-cross-language-e2e.log#L55). The current result is exactly 278. |
| `test_cpp_framework_install_consumer` | F — newly exposed in this D-B85 gate | Consumer build requires missing `install/lib/liblz4.so`; terminal assertion [`install_consumer.cmake:257`](../../../framework/languages/cpp/tests/Zlink.Framework.PackageTests/install_consumer.cmake#L257) | Reproduces in the focused recheck: `No rule to make target .../install/lib/liblz4.so`; no supplied older result records this failure. It is a package-install contract failure, not DONTWAIT/backpressure or terminal request classification. |
| `test_cpp_stream_connector_install_consumer` | F — newly exposed in this D-B85 gate | `stream connector component is missing: .../install/lib/liblz4.a`, [`stream_connector_consumer.cmake:67`](../../../framework/languages/cpp/tests/Zlink.Framework.PackageTests/stream_connector_consumer.cmake#L67) | Reproduces in the focused recheck. No supplied older result records it. |
| `test_cpp_stream_connector` | B — terminal/error classification | `zlink::bind_error_t: Unknown error 501 (errno=22)`; test registration [`CMakeLists.txt:1702`](../../../framework/languages/cpp/CMakeLists.txt#L1702) | Reproduces directly under focused CTest. This is an explicit terminal bind error, not a DONTWAIT/backpressure outcome. |
| `connector_perf_smoke` | B — terminal/error classification | `Unknown error 501 (errno=22)`; invoked command [`CMakeLists.txt:1753`](../../../framework/languages/cpp/CMakeLists.txt#L1753) | Reproduces directly under focused CTest before the perf workload begins. This is an explicit terminal bind error, not a DONTWAIT/backpressure outcome. |

No failure fell in A (DONTWAIT/backpressure) or D (environment).  The two F
entries mean new in the supplied comparison set; this execution does not
establish that the D-B85 REQUEST port caused the package-install defects.

## Commands

Run from `framework/languages/cpp` unless stated otherwise:

```bash
cmake --preset linux-ninja-debug
cmake --build build/linux-ninja-debug -j12
ctest --test-dir build/linux-ninja-debug --output-on-failure -j2

for run in 1 2 3; do
  ./build/linux-ninja-debug/test_cpp_framework_m6a_runtime
done
for run in 1 2 3; do
  ./build/linux-ninja-debug/test_cpp_framework_m6b_runtime
done

ctest --test-dir build/linux-ninja-debug --output-on-failure -j1 \
  -R '^(test_cpp_framework_install_consumer|test_cpp_stream_connector_install_consumer|test_cpp_stream_connector|connector_perf_smoke)$'
```

## Logs

- `zlink-work/c016/logs/gate-v2-cpp-configure.log`
- `zlink-work/c016/logs/gate-v2-cpp-build.log`
- `zlink-work/c016/logs/gate-v2-cpp-ctest.log`
- `zlink-work/c016/logs/gate-v2-cpp-m6a-3x.log`
- `zlink-work/c016/logs/gate-v2-cpp-m6b-3x.log`
- `zlink-work/c016/logs/gate-v2-cpp-failure-rerun.log`
