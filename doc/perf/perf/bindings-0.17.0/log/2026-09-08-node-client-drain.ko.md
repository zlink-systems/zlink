# Node SENDSEND client echo drain 수정과 검증

## 결과와 범위

Node DR/RR SENDSEND client는 active 구간이 끝난 뒤 public send admission만
정리하고 socket을 닫았다. Relay가 이미 받아들인 요청의 echo가 남아 있어도 client
종료 조건에는 반영되지 않았다.
`bindings/node/perf/multi/perf_multi_routed_sendsend.ts:271-290`이 이 종료 조건을
소유한다.

공유 client helper에 `EchoReplyDrain`을 추가했다. 제출 직전에 미수신 수를 늘리고,
admission 거절 또는 run·phase·크기가 맞는 echo 수신에서 줄인다. Active 구간에는 이
수를 읽지 않으며, active 종료 뒤 기존 `sendDrainStopNs` 안에서 admission과 echo가
모두 정리될 때까지 같은 poller로 진행한다. Deadline 뒤 echo는 미수신 수만 줄이고
collector와 latency 표본 처리를 건너뛴다.

- 소유 계층: Node multi perf SENDSEND client의 측정 종료와 socket 수명 관리.
- 정책 근거: `doc/perf/PERF_MULTI_TEST_POLICY.md` §1.2의 echo 비연동 연속 제출,
  §3.5와 §4.1의 active 구간 밖 집계 제외.
- 교차언어 대조: C client는 retained send와 미수신 echo가 모두 없어야 닫는다.
  C++ `33f63ae89d`의 `echo_reply_drain_t`와 작업 시작 시점의 .NET
  `PerfMultiEchoReplyDrain`도 제출 전 증가, 거절·정상 echo 수신에서 감소,
  teardown 전용 drain을 사용한다.
- 변경 분류: **B — Node perf client의 기존 종료 결함**. Core와 Node binding
  library의 계약 위반을 확인한 것이 아니다.

수정 파일은 `bindings/node/perf/multi/perf_multi_routed_sendsend.ts`다.
`perf_multi_dealer_router_client.ts`와 `perf_multi_router_router_client.ts`는 이 helper를
이미 공유하므로 바꾸지 않았다. `npm run build:incremental`로 runner가 실행하는
`bindings/node/dist-tools/perf/multi/perf_multi_routed_sendsend.js`를 갱신했다.
Relay server 부분, Core, Framework, 다른 언어와 Node binding library는 수정하지 않았다.

수정 전/후 규칙 수: echo client의 종료 기준 **2종 → 1종**
(Node는 admission까지만, C/C++/.NET은 admitted echo 수신까지 → 모두 admitted echo
수신까지). 새 counter는 active 지표 counter와 역할이 다르며 teardown에서만 종료
여부를 결정한다.

## C++ helper 대응

| C++ `echo_reply_drain_t` | Node `EchoReplyDrain` |
|---|---|
| submit 전에 `submitted()` | submit 호출 전에 `submitted()` |
| admission 예외에서 `finished()` | 동기·비동기 admission 거절에서 `admissionRejected()` |
| expected echo에서 `finished()` | multipart와 run·phase·크기가 맞는 echo에서 `received()` |
| 기존 `drain_deadline`까지 같은 poller로 dispatch | 기존 `sendDrainStopNs`까지 같은 poller로 receive |
| deadline 뒤 count·latency 생략 | `receivedAtNs >= activeStopNs`이면 collector 생략 |

Promise continuation보다 receive dispatch가 먼저 실행될 수 있으므로 counter는 admission
완료 뒤가 아니라 submit 전에 늘린다. Node counter는 client event loop에서만 갱신된다.
미수신 수가 0일 때 echo 또는 admission 거절을 더 받으면 오류로 처리한다.

## 불변 확인

| 불변 | 코드 근거 |
|---|---|
| Active 송신을 gate하지 않음 | active loop 조건은 `failure`와 `activeStopNs`뿐이다. `replyDrain.pending`은 teardown loop에서만 읽는다. |
| Deadline을 늘리지 않음 | 기존 `activeStopNs + PERF_MULTI_SEND_DRAIN_TIMEOUT_MS` 계산을 유지하고, poll timeout도 `sendDrainStopNs - now`만 사용한다. |
| Deadline 뒤 echo를 지표에서 제외 | echo counter를 줄인 직후 `receivedAtNs >= activeStopNs`이면 `collector.recordPayload()`와 latency batch 처리를 건너뛴다. |
| 상한·timeout·sleep·client 수·크기와 오류 처리 유지 | 기존 값은 바꾸지 않았다. Counter underflow와 drain 미완료는 오류이며 예외를 삼키지 않는다. |

## 고정 Core와 성능 검증

모든 perf 실행은 다음 Core를 사용했으며 `perf-ticket.sh submit -p 2 -o codex`로만
제출했다.

```bash
export ZLINK_CORE_SOURCE=release
export ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.2
```

최종 후보 측정 명령은 tcp DR/RR SENDSEND, 64·256·1024·4096·65536 B,
duration 5, runs 3이다. 결과는
`bindings/node/perf/results/multi/report/perf_node_multi_linux_20260908_121356_node_client_drain_tcp_3run_final2.txt`에
있다.
고정 Core 경로는 report의 `META,runtime_libzlink`에서 확인했다.

결과는 **partial**이다. DR 4096 B 세 run이 기존 5초 deadline에서 각각 미수신 echo
113,343개, 76,130개, 83,655개를 남겨 실패했다. 나머지는 27/30 run, 9/10
configuration, RESULT 45/50이 완료됐다. Drain 실패를 성공으로 바꾸거나 오류를
무시하지 않았다.

| Pattern | 크기 | `r10node` 1-run | 최종 3-run | 편차 |
|---|---:|---:|---:|---:|
| DR SENDSEND | 64 | 169,952.4 | 167,358.2 | -1.53% |
| DR SENDSEND | 256 | 151,719.6 | 153,400.2 | +1.11% |
| DR SENDSEND | 1024 | 130,581.2 | 128,258.8 | -1.78% |
| DR SENDSEND | 4096 | 100,547.6 | 실패 | 판정 불가 |
| DR SENDSEND | 65536 | 30,158.6 | 30,772.2 | +2.03% |
| RR SENDSEND | 64 | 179,809.6 | 171,410.8 | -4.67% |
| RR SENDSEND | 256 | 161,997.0 | 154,419.8 | -4.68% |
| RR SENDSEND | 1024 | 138,291.4 | 134,246.4 | -2.92% |
| RR SENDSEND | 4096 | 106,486.6 | 101,190.6 | -4.97% |
| RR SENDSEND | 65536 | 28,998.0 | 25,924.4 | -10.60% |

DR 기준 report는 `perf_node_multi_linux_20260908_113734_r10node.txt`, RR 기준
report는 `perf_node_multi_linux_20260908_111649_r10node.txt`다. 성공한 case 가운데
RR 65536 B는 ±5% 조건을 벗어났다. 따라서 요청한 30/30 complete와 전 크기 ±5%
조건은 달성하지 못했다.

Deadline 진단은 DR/tcp/4096 B/duration 5/runs 1로 실행했다. Active 종료 직후
미수신 echo는 523,800개, admission은 100개였고 기존 deadline 잔여 시간은
4,999,933,567 ns였다. Deadline 안에 약 472,000개를 더 받았으나 51,672개가 남았다.
결과는 `perf_node_multi_linux_20260908_121707_node_client_drain_diag_4096.txt`다.
진단 출력은 최종 코드에서 제거했다.

작업 도중 main에 들어온 `537c6eec93`은 C++/.NET/Python/Rust의 SENDSEND teardown
창을 C처럼 `max(PERF_MULTI_SEND_DRAIN_TIMEOUT_MS, active duration × 3000)`으로
바꾸고 정책에도 같은 값을 기록했다. Duration 5에서는 15초다. 이번 요청은 deadline과
timeout 변경을 금지하므로 Node에는 적용하지 않았다. Active 송신 gate, relay server
변경 또는 deadline 증가 없이 위 backlog를 5초 안에 모두 정리하는 방법은 확인하지 못했다.

## Node 테스트

- `npm run build:incremental`: 통과. TypeScript와 `dist-tools`를 갱신했다.
- `node --test dist-tools/tests/perf_multi_routed_sendsend_contract.test.js`: 10/10 통과.
- `npm test`: 실패. 이번 변경과 무관한
  `dist-tools/tests/multipart.test.js`의 `native thread stress mixes single-part,
  multipart, and close races`에서 `counts.rejected_einval > 0n` assertion이
  실패했다. 해당 테스트 파일만 다시 실행해도 같은 실패가 재현됐다.

Commit과 push는 수행하지 않았다.
