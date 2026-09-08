# Node heartbeat pending admission과 Core 0.17.3

## 판정

이 실패는 Core 0.17.3의 admission 계약 변경이나 Framework runtime 결함이 아니다. 테스트가
`completed < 256`과 50 ms 경과를 Core send queue가 현재 가득 찼다는 증거로 사용한 timing
경합이다. 판정은 **A(기존 byte-HWM 계약에 맞춘 test fixture 적응)** 이다.

Core는 이미 queue에 들어간 frame의 순서를 보존하지만, HWM 때문에 아직 admission되지 않은
서로 다른 비동기 submit 사이의 FIFO를 보장하지 않는다. 따라서 큰 data frame이 기다리는 동안
Core queue가 잠시 비면 작은 heartbeat frame이 먼저 admission될 수 있다. Stream connector
스펙도 heartbeat의 형식과 주기만 정하며, pending application submit보다 뒤에 admission돼야 한다는
계약은 두지 않는다
(`framework/doc/framework/common/spec/stream-connector/32-stream-connector.ko.md:174-190`,
`:378-383`).

## 원인과 계약 근거

기존 fixture의 원인은
`framework/languages/node/test/contract/fixtures/stream-heartbeat-shutdown-process.js`의 기존
25-41행이다. 256개 Promise 가운데 완료된 수는 과거의 누적 admission 수다. 그 값이 256보다
작아도 현재 Core queue의 accounted byte가 0일 수 있다. 9월 8일 gate에서는 이 구간에
heartbeat가 25 ms 안에 완료됐다
(`zlink-work/gates/rel1/node-npmtest-rerun4.log:10681` 부근).

Core 계약은 다음 동작을 요구한다.

- `core/doc/spec/core/socket/README.ko.md:434-458`: HWM은 실제 queue byte를 제한한다. 일반
  frame charge는 payload와 `sizeof(zlink_msg_t)`의 합이며, 빈 pipe에는 HWM보다 큰 complete
  message 한 건을 허용할 수 있다.
- `core/doc/spec/core/systems/06-auto-hwm.ko.md:374-401`: admission은 대상 physical queue의
  미반환 byte와 후보 frame charge를 더해 판정한다.
- 같은 문서 `:423-438`: `outstanding + candidate > HWM`이면 거절한다.

이 계약은 `447f41a9f2f`(2026-08-22)에 문서화됐고 socket 공개 스펙은 `b94d8ae839f`
(2026-08-24)에 같은 내용을 반영했다. 둘 다 9월 6일 `core/build-dev`보다 앞선다.

`git log core/v0.17.2..core/v0.17.3 -- core/src` 결과는 다음과 같다.

| 커밋 | 변경 | admission/HWM 영향 |
|---|---|---|
| `de730d4ac5` | receive ownership protocol과 STREAM receive stall 수정 | 없음. 커밋이 규칙과 public interface가 바뀌지 않았다고 명시한다. |
| `f5d7cccde2` | Windows entropy와 context 종료의 spurious wake 수정 | 없음. |

따라서 0.17.3에 admission 의미를 바꾼 Core 커밋은 없다. `core/build-dev` 뒤와 0.17.2 사이의
`5304885197`은 session-side pipe write/flush의 lock과 accounting publication을 최적화했다.
Transport drain 시점을 바꿀 수는 있지만 public admission 의미는 바꾸지 않았다고 명시한다.
중간 Core를 재빌드하지 않는 작업 제약 때문에 이 커밋 하나를 timing 변화의 원인으로 단정하지
않는다.

## 측정

진단은 설치된 Node binding 0.17.3을 그대로 사용하고 process map에서 실제로 로드된 Core
library를 확인했다. 두 실행 모두 socket option readback과 monitor의 planned/applied SNDHWM이
65,536 byte였다. `autoHwmEnabled=true`였지만 수동 `sendHwm`을 덮어쓰지 않았다.

기존 65,536-byte data payload의 Core charge는 65,600 byte였다. 빈 pipe oversize 예외로
admission된 frame이 kernel socket buffer로 빠지다가, peer receive window가 닫힌 뒤 한 frame이
Core queue에 남았다. Heartbeat의 encoded frame은 32 byte이고 최소 Core message charge 64 byte를
더한 admission charge는 96 byte다.

표의 값은 `완료 Promise 수 / sndPendingBytes`다. 목표 0 ms 표본은 256개 submit 호출을 만든 직후라
실제 관찰 시각이 각각 6.441 ms와 7.507 ms였다. Heartbeat는 50 ms 표본 뒤 제출했다.

| Runtime | 실제 load 경로 | 0 ms | 25 ms | 50 ms | 200 ms | Heartbeat admission |
|---|---|---:|---:|---:|---:|---|
| Core 0.17.0 dev | `core/build-dev/lib/libzlink.so.0.17.0` | 0 / 0 | 251 / 65,600 | 251 / 65,600 | 251 / 65,600 | 200 ms까지 pending; close에서 terminal |
| Core 0.17.3 | `/home/hep7/.cache/zlink/core/0.17.3/linux-x64/lib/libzlink.so.0.17.3` | 0 / 0 | 251 / 65,600 | 251 / 65,600 | 251 / 65,600 | 200 ms까지 pending; close에서 terminal |

0.17.3 반복 진단 10회도 모두 25/50/200 ms에서 `251 / 65,600`이었고 heartbeat는 200 ms까지
pending이었다. 반면 제공된 gate 실패와, 첫 수정에서 full snapshot을 한 번만 확인한 티켓은
heartbeat가 25 ms 안에 admission됐다. 같은 0.17.3에서 두 결과가 나온 사실과 두 runtime의 같은
byte snapshot은 계약 변경이 아니라 kernel buffer drain과 Node completion 처리의 scheduling
경합임을 뒷받침한다.

## 수정

수정 파일은
`framework/languages/node/test/contract/fixtures/stream-heartbeat-shutdown-process.js` 하나다.

- Monitor가 보고한 `minimumCoreMessageChargeBytes`를 payload에서 빼 data frame charge를
  applied SNDHWM 65,536 byte와 정확히 맞췄다. Oversize 예외에 의존하지 않는다.
- 누적 `completed` 수와 고정 50 ms만으로 pending을 추정하지 않는다. `sndPendingBytes >=
  autoHwmAppliedSndHwmBytes`이고 완료 수가 25 ms 동안 변하지 않는 상태를 확인한다. 첫 full
  snapshot 뒤에도 kernel buffer로 drain될 수 있는 transient를 제외한다.
- 기존 테스트 목적은 유지했다. Heartbeat Promise가 pending인 동안 parent가 SIGINT를 보내고,
  child의 `closed` message에서 `heartbeatCompleted=true`를 확인한다.

규칙 수는 수정 전 `누적 완료 수`와 `50 ms 경과`라는 간접 조건 두 개를 pending으로 해석했지만,
수정 후에는 `현재 queue charge가 applied HWM에 도달해 안정적으로 유지됨`이라는 Core 소유 상태
하나를 사용한다.

Framework runtime source는 바꾸지 않았다. 다른 언어에는 이 Node process SIGINT fixture와 같은
real-binding test가 없으며, 공통 Core byte-HWM 계약은 언어와 무관하다.

## 티켓 결과

| 티켓 | 내용 | rc |
|---|---|---:|
| `1-1788864998-76840-codex-heartbeat-Node_heartbeat_admission_diagnosis_with_` | Core 0.17.3 진단 | 0 |
| `1-1788865273-87831-codex-heartbeat-Node_heartbeat_admission_diagnosis_with_` | Core build-dev 비교 진단 | 0 |
| `1-1788865510-2368-codex-heartbeat-Repeat_Node_heartbeat_admission_timing_1` | Core 0.17.3 진단 10회 | 0 |
| `1-1788865632-7952-codex-heartbeat-Node_heartbeat_shutdown_focused_test_Cor` | 첫 full snapshot만 확인한 수정안 검증 | 1 |
| `1-1788865684-10629-codex-heartbeat-Node_heartbeat_shutdown_focused_test_Cor` | 최종 fixture 1/3 | 0 |
| `1-1788865698-11949-codex-heartbeat-Node_heartbeat_shutdown_focused_test_Cor` | 최종 fixture 2/3 | 0 |
| `1-1788865711-13751-codex-heartbeat-Node_heartbeat_shutdown_focused_test_Cor` | 최종 fixture 3/3 | 0 |

최종 세 실행은 모두 test 1개 통과, 실패 0개이며 실행 시간은 199.3-201.0 ms였다.
