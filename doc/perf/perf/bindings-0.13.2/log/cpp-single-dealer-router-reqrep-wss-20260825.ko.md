# C++ Single DEALER_ROUTER_REQREP / wss — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `3cea9390edaf5f310ce66cdd351680ccc9380240`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- final manifest: clean C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- focused contract smoke after reverting the last candidate: `test_cpp_contract_request_reply` passed (1/1).
- final C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_213254_cpp-dealer-router-reqrep-wss-local0132-final-clean-paired5-c-20260825.txt`
- final C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_213548_cpp-dealer-router-reqrep-wss-local0132-final-clean-paired5-cpp-20260825.txt`
- both reports: `status=complete`, expected/actual result lines `30/30`.

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 43.78% | 2.55x |
| 256B | 38.22% | 2.79x |
| 1024B | 38.62% | 2.63x |
| 65536B | 89.15% | 1.19x |
| 131072B | 92.75% | 1.06x |
| 262144B | 96.56% | 0.86x |

## 개선 pass와 Sol reviewer 최종 판정

- 후보 1: bridge와 completion state co-allocation. contract 5/5를 통과했으나 paired aggregate 67.72%로
  기준선 대비 재현 가능한 개선이 아니어서 폐기했다.
- 후보 2: scheduler가 없는 await에서 inline resume slot을 사용. cancellation contract 회귀를 먼저 찾아
  수정한 뒤 contract 5/5와 paired 측정을 통과했지만 67.07%로 회귀해 폐기했다.
- 후보 3: native reply-part direct adopt으로 default-init/move/close/reinit을 제거. contract 5/5를
  통과했지만 68.37%(+0.68%p)는 control drift 범위이고, 고정비 가설과 달리 64B가 악화되어 폐기했다.
- Sol reviewer 결론: 세 후보 모두 실질 개선이 아니며, 남은 비용은 public exact-target 선택, async result
  consumer, vector reply ownership, close/liveness와 callback context 계약에 있다. 이를 줄이는 target cache·일반
  dealer 우회·callback harness 대체·API/ownership 변경은 spec gap이므로 이 셀의 안전한 후보는 소진됐다.
- clean-source final pair aggregate는 **66.51%**, latency median은 **1.87x**다. latency gate는 통과하지만
  request/reply throughput 목표 85%에 미달하므로 최종 상태는 **`보류(미달 66.51%)`**다.
