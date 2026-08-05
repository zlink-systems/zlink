# Round 202: ready transition coalescing 후보 반려

## 측정한 문제

Spot mailbox가 이미 claim된 동안 새 record를 admission하면 같은 owner와 domain을 ready set에 다시
등록하고 ready handler를 매번 호출한다. Claim을 보유한 consumer만 해당 mailbox를 읽을 수 있으므로,
claim release 전의 반복 wake는 즉시 처리할 수 있는 작업을 추가하지 않는다.

이 동작을 고정하는 진단 test를 먼저 추가했다. 첫 record로 claim을 얻은 뒤 claim을 유지한 상태에서
32개 record를 추가하면 기존 구현은 handler를 총 33회 호출해 `Expected 1 Was 33`으로 실패했다.
후보는 ready set이 absent에서 present로 바뀔 때만 handler를 호출하고, claim 중인 mailbox는 claim
release가 한 번 다시 등록하도록 변경했다. 변경 뒤 진단 test와 다음 focused suite 5개가 통과했다.

- `test_mesh_node_basic`
- `test_mesh_peer_admission`
- `test_mesh_monitor_matrix`
- `test_mesh_stress`
- `test_mesh_lifecycle_contracts`

후보 runtime은 `core/build/lib/libzlink.so.10.6.0`이며 SHA-256은
`b777fb2242b6b0df3bb78434aa1e925f82dd919be7fb53ea9a4bf4a7946d585d`였다. Runner가 같은 경로를
출력했고 Core source보다 오래된 runtime이 없음을 확인했다.

10-peer Pub/Sub `strace`에서는 수신 message 370,463개에 `futex` 708,501회가 기록돼 message당
약 1.91회였다. 이전 약 2.43회보다 21% 감소했으므로 반복 wake를 줄인다는 가설은 계측에서 확인됐다.
다만 `strace`가 처리량과 latency를 크게 바꾸므로 이 실행은 성능 gate로 사용하지 않았다.

## 100-peer paired gate

tcp 64바이트, 100 peer, active 5초, server와 client I/O thread 각각 1개 조건으로 Spot 세 pattern과
matched ROUTER를 한 번씩 비교했다.

| Pattern | Spot throughput | ROUTER throughput | Throughput ratio | Mean ratio | P95 ratio | P99 ratio |
|---|---:|---:|---:|---:|---:|---:|
| PUBSUB | 3,526,916.4 msg/s | 4,137,818.8 msg/s | 85.24% | 1.1893 | 4.9317 | 9.2666 |
| REQREP | 67,865.2 ops/s | 112,962.2 ops/s | 60.08% | 3.7909 | 2.7163 | 4.0560 |
| SENDSEND | 68,799.8 ops/s | 136,308.8 ops/s | 50.47% | 3.1158 | 2.8589 | 3.5625 |

PUBSUB throughput은 이전 79.44% 기준보다 높았지만 90% 목표에 도달하지 못했다. Peer 종료 snapshot에는
application message 225개와 14,400바이트가 남았고 p95와 p99도 기준을 초과했다. REQREP와 SENDSEND는
pending queue와 multicast drop이 0이었지만 처리량과 모든 latency 기준을 통과하지 못했다.

결과:
`bindings/c/perf/results/multi/paired/20260720-202856-s9-p02-round202-ready-coalesce-c100/`

## 판정

후보는 반복 wake와 `futex` 호출을 줄였지만 세 pattern의 공통 성능 개선과 수신 완전성을 증명하지
못했다. 정식 5회 gate로 확장하지 않고 `mesh_runtime.cpp`의 후보 hunk와 진단 test를 원복했다.

원복 뒤 공식 runtime SHA-256은
`a57d91a90ae3c0d67ed7d469df848e70038ff700e93fd5989c889795755b3301`이며 Core source보다 새롭다.
timeout, assertion, version, package와 배포는 변경하지 않았다.
