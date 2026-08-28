# C++ Single — ROUTER_ROUTER_REQREP / tls (local Core 0.13.2)

> **재검증 중 — 이 문서의 기존 paired 수치와 후보 판정은 성능 결론으로 사용하지 않는다.**
> C 기준은 Core callback에서 완료를 처리하지만 C++ single은 요청당 async consumer를 생성해
> completion을 소비한다. 이 consumer/workload 동등성 검증 없이 계산한 비율은 binding 성능 판정에
> 부적합하다. 2026-08-26의 진단에서 coroutine observer를 public callback으로 대체해도 64B TLS
> throughput이 97.79→99.59 Kops/s(+1.8%)로만 변했고, server lazy single-part materialization
> 제거도 96.50 Kops/s로 개선되지 않았다. 즉 coroutine만이 주원인은 아니며, 실제 binding request/
> reply 경로의 allocation·state·lifetime 비용을 재검증한다.

## 결론

- 최종 상태: **보류(미달 71.00%)** — Sol 최종 승인
- clean-source secure 5-run paired 결과: throughput aggregate mean **71.00%**, latency median ratio **1.59x**
- request/reply 기준(throughput 85% 이상, latency 2.0x 이하)에서 latency는 통과했지만 throughput은 미달이다.

## 최종 측정

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260826_005621_cpp-router-router-reqrep-tls-local0132-final-clean-paired5-c-20260826.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260826_005903_cpp-router-router-reqrep-tls-local0132-final-clean-paired5-cpp-20260826.txt`
- Core source: local `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2` (0.13.2)

| Size | C msg/s | C++ msg/s | Throughput ratio | C latency ms | C++ latency ms | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 187,069.6 | 96,061.4 | 51.35% | 0.287 | 0.598 | 2.08x |
| 256 | 172,652.0 | 85,653.8 | 49.61% | 0.300 | 0.865 | 2.88x |
| 1,024 | 92,202.2 | 42,533.6 | 46.13% | 0.660 | 1.509 | 2.29x |
| 65,536 | 5,828.6 | 5,255.0 | 90.16% | 2.056 | 2.264 | 1.10x |
| 131,072 | 3,554.6 | 3,296.0 | 92.72% | 1.685 | 1.797 | 1.07x |
| 262,144 | 1,938.0 | 1,860.8 | 96.02% | 1.545 | 1.570 | 1.02x |

## 개선 pass와 계약 gate

1. C0 — current self-anchor/co-allocated completion state를 historical 71.43%와 독립 측정했다.
   - **74.52%, latency 1.52x**로 +3.09%p 개선했고 contract 5/5를 통과했다. Sol은 이것만 유지했다.
2. C1 — Core terminal completion과 self-anchor release를 같은 mutex 구간에서 결합했다.
   - contract 5/5지만 **70.78%, 1.71x**로 회귀해 source를 되돌렸다.
3. C2 — 수신 native reply frame을 no-init storage에 직접 adopt했다.
   - contract 5/5지만 **70.74%, 1.54x**로 throughput 회귀해 source를 되돌렸다.
4. C3 — scheduler가 없을 때 state-owned inline resume slot을 사용했다.
   - contract 5/5지만 **68.79%, 1.75x**로 회귀해 source를 되돌렸다.
5. exact target 선택 생략·routing-id cache·lvalue direct move·liveness/outbound mutex 제거는 target/no-reroute,
   ownership, close/cancel/concurrency 계약을 바꾸거나 비용 대비 효과가 없어 Sol no-go다.

Sol 최종 검토는 C0 외 안전 후보가 소진됐음을 승인했다. clean final의 64B·256B·1KiB latency는 각각
2x를 넘지만, 이 계획의 request/reply latency gate는 median **1.59x**라 통과다. throughput 85% gate는
**71.00%**로 미달하므로 보류로 확정한다.
