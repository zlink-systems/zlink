# C++ Single PAIR / ws — local Core 0.13.2

- Core/runtime: local `0.13.2`, baseline revision `30ee3845f5e16a07332e38e8bdedb43557b87caf`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 3`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- smoke: C `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_170844_cpp-pair-ws-local0132-smoke-c-20260825.txt`,
  C++ `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_170854_cpp-pair-ws-local0132-smoke-cpp-20260825.txt`.
- C baseline: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_170906_cpp-pair-ws-local0132-baseline-c-20260825.txt`
- C++ baseline: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_171045_cpp-pair-ws-local0132-baseline-cpp-20260825.txt`

| Throughput ratio (64B→262144B) | Aggregate | Latency median | 판정 |
|---|---:|---:|---|
| 79.11%, 98.36%, 95.70%, 91.40%, 93.31%, 102.85% | **93.46%** | **1.03x** | 미달 |

## 후보·POSDDD no-go

- Public send hot path에는 이미 single-part direct native submit, bounded 8MiB large-message
  pool(128KiB–1MiB), thread-local operation-state reuse가 적용돼 있다. 동일 변경을 다시
  candidate로 측정하는 것은 A/B가 아니다.
- 64KiB부터 pool을 사용하는 후보는 앞선 PAIR/tcp에서 25/30 result lines만 남기고 timeout되어
  폐기됐다. 같은 global pool 정책을 WebSocket에만 다시 적용할 근거가 없고, 64B gap도 해결하지
  못한다.
- 64B의 고정비를 줄이려 outbound attempt mutex나 callback-state weak lifetime을 제거하는 것은
  concurrency/close 계약을 바꾸므로 금지다. pool cap 확대도 fan-out 메모리 경계를 바꾸므로 금지다.
- 따라서 새 source 후보는 만들지 않았다. POSDDD 관점에서도 이미 분리된 message·operation state
  책임을 다시 합치거나 lifecycle guard를 없애면 복잡성·계약 위험만 증가한다.

목표는 C++ one-way strict 95.00%이며 최종 상태는 `미달(93.46%)`다. latency gate는 통과했고,
paired report와 no-go 근거를 남긴 뒤 다음 항목으로 이동한다.
