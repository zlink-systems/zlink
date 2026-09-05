# Stage 1 Node 첫 reply 유실 조사와 sender replay 수정

## 결론

현재 설치된 Node binding으로 `user-spot-native-two-process.test.js`를 5회 추적한 결과는
분류 **(b) route not admitted**다. 실패한 첫 시도는 target에 도달하지 않았고 sender의
binding submit이 `SubmitError(result=2, nativeErrno=113)`로 즉시 끝났다. target이 terminal
record를 기록한 뒤 유지 중인 physical pair에서 reply가 사라진 사례와
`RequestError(result=101)`은 5회 중 없었다.

따라서 sender는 새 스펙대로 terminal envelope가 없는 durable lifecycle operation을 같은
`OperationId`로 다시 보낸다. 각 시도에는 남은 deadline 전부를 전달하며 횟수 제한을 두지
않는다. 전체 deadline을 소진하면 typed `DeadlineExceeded`로 끝낸다. 이 함수는 User Spot
create·close와 Actor create 전용이며 application request 경로는 변경하지 않았다.

## 조사 방법

- `285cfa522a`의 절반 분할을 제거하고 각 attempt에 남은 deadline 전부를 전달했다.
- 기존 monitor를 먼저 사용해 `CONNECTION_READY(0x1000)`와 `DISCONNECTED(0x0200)`의
  `connectionId`, transport lane, endpoint를 기록했다.
- Application message-flow tracer는 command 47/48 infrastructure lifecycle operation을
  다루지 않으므로 `flow_id`는 발급되지 않았다. 이 제한은 공통 observability spec의
  application dispatch 범위와 일치한다. 대신 128-bit `OperationId`와 wire correlation을
  같은 흐름의 식별자로 사용했다.
- 부족한 시점 정보는 환경변수로 제한한 임시 로그로 sender attempt, target ingress,
  terminal record settle, reply submit, binding terminal을 기록했다. 임시 로그와 test fixture
  forwarding은 조사 뒤 모두 제거했다.
- Core와 binding을 수정하거나 다시 빌드하지 않았고 local package를 sync하지 않았다.

`ReplyToken`은 public API에서 physical connection ID를 노출하지 않는다. 아래 reply pair는
monitor의 Ready/retire 순서와 “reply는 request를 받은 transport pair에 고정된다”는 Core
계약을 합쳐 판정한 값이다. 즉 connection ID 연결은 명시적으로 **추론**이다.

## 5회 타임라인

모든 시각은 Unix millisecond다. `A/C`는 Application/Completion lane이며 방향은
source outbound → target inbound다. 각 run의 close는 command 48, correlation 2이고 target
실행 횟수는 모두 1이었다.

| run | flow 식별 | 첫 create attempt와 binding terminal | target terminal record / reply | physical pair·HANDOVER | close 결과 |
| --- | --- | --- | --- | --- | --- |
| 1 | `506343160996010:1`, corr `1`; flow_id 없음 | `1788566609729`부터 attempt 1~8이 `SubmitError(2/113)`; target 수신 없음. attempt 9가 target에 도달 | 수신 `1788566609892`, record/reply `1788566609900`, sender reply `1788566609901` | loser: source `A9/C13`, target `A6/C8`, retire `1788566609719/1788566609720`; winner/reply: source `A17` → target `A10` | op `...:2`; record/reply `1788566609914`, sender reply `1788566609914` |
| 2 | `1672236093747571:1`, corr `1`; flow_id 없음 | attempt 1 submit `1788566651563`, terminal envelope 수신 `1788566651574` | 수신 `1788566651566`, record/reply `1788566651573` | HANDOVER 없음; stable/reply pair source `A9` → target `A6` | op `...:2`; record/reply `1788566651586`, sender reply `1788566651588` |
| 3 | `6452363327415890:1`, corr `1`; flow_id 없음 | winner Ready 뒤 attempt 1 submit `1788566659189`, terminal envelope `1788566659202` | 수신 `1788566659191`, record/reply `1788566659201` | loser: source `A9/C13`, target `A6/C8`, retire `1788566658988/1788566658984`; winner/reply: source `A20` → target `A12`, Ready `1788566659177` | op `...:2`; record/reply `1788566659215`, sender reply `1788566659216` |
| 4 | `6466501514314714:1`, corr `1`; flow_id 없음 | attempt 1 submit `1788566663654`, terminal envelope `1788566663666` | 수신 `1788566663656`, record/reply `1788566663664` | HANDOVER 없음; stable/reply pair source `A9` → target `A6` | op `...:2`; record/reply `1788566663682`, sender reply `1788566663682` |
| 5 | `1189427562304106:1`, corr `1`; flow_id 없음 | `1788566670881`부터 attempt 1~7이 `SubmitError(2/113)`; target 수신 없음. attempt 8이 target에 도달 | 수신 `1788566671028`, record/reply `1788566671034`, sender reply `1788566671035` | loser: source `A9/C13`, target `A6/C8`, retire `1788566670867~868/1788566670869`; winner/reply: source `A17` → target `A10`, Ready `1788566671010/1788566671014` | op `...:2`; record/reply `1788566671050`, sender reply `1788566671050` |

Run 1과 5에서 loser pair는 첫 create attempt보다 먼저 retire됐다. 두 run 모두 첫 여러
attempt에 대응하는 target ingress와 terminal record가 없고 binding이 request admission 전에
`NotConnected`로 끝났다. 새 pair가 Ready가 된 뒤 같은 `OperationId`의 attempt만 target에서
실행됐다. Run 2·4는 교체가 없었고, run 3은 loser retire와 winner Ready가 첫 attempt보다 먼저
끝났다. 살아 있는 pair에서 target terminal 뒤 sender reply가 누락된 run은 없다.

이 증거로 현재 재현은 (a)가 아니라 **(b)**다. 동일 operation replay는 실행 중복을 숨기는
retry가 아니라, terminal envelope가 없는 pre-admission 결과를 원래 deadline 안에서 이어 가는
스펙 동작이다.

## 변경

### Node runtime

- `framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts`
  - `remaining / 2` attempt timeout과 관련 주석을 제거했다.
  - local·remote attempt 모두 남은 end-to-end deadline 전부를 사용한다.
  - terminal envelope를 받기 전 예외만 같은 encoded header로 replay한다. Header가 동일하므로
    `OperationId`, correlation과 close fence도 동일하다.
  - deadline 소진은 원래 transport 예외 대신 internal/public `DeadlineExceeded`로 분류한다.

### Regression

- `framework/languages/node/test/m6b/m6b-user-spot-terminal-replay.contract.ts`
  - 첫 reply가 deadline보다 일찍 유실된 경우 두 번째 request의 `OperationId`와 bytes가 같고
    원래 deadline 전에 terminal을 받는지 고정했다.
  - 정상 첫 attempt가 전체 deadline의 50%보다 오래 걸려도 잘리지 않고 request 1회로
    완료되는지 고정했다. 기존 half split에서는 이 test가 첫 attempt를 중단한다.

Assertion, fixture timeout과 runtime budget은 늘리거나 낮추지 않았다.

## 검증 결과

모든 npm/test 명령은 `TMPDIR=/dev/shm/zlink-tmp-node`, `unset ZLINK_LIBRARY_PATH`,
`flock -w7200 /tmp/zlink-node-gate.lock` 조건에서 실행했다.

| 검증 | 결과 |
| --- | --- |
| whole-deadline + monitor/operation 임시 추적 two-process 5회 | 5/5 pass; 위 표의 원인 자료 수집 |
| 임시 추적 제거·최종 코드 two-process 독립 5회 | 5/5 pass; 각 test `747~905 ms` |
| 임시 추적 제거 뒤 `node --test` 신규 regression 파일 | 2/2 pass (`68 ms`, `186 ms`) |
| `npm run verify:m6b-runtime` | 신규 regression 2/2 pass; 전체 108 pass, 2 fail |
| 기존 terminal replay 보존 test 단독 재실행 | pass (`379 ms`); aggregate에서만 나온 `0 !== 101`은 단독 재현 안 됨 |
| 기존 `raw backend dispatches Spot requests and Actor sends through M6B owners` 단독 실행 | fail; `sendActorBoundSession` expected `Ok(0)`, actual `InvalidState(8)` |
| `npm run typecheck` | pass |
| `git diff --check` | pass |

두 번째 M6B 실패는 변경한 `requestUserSpotOperation`과 무관한 bound-session send 경로이며
단독으로 재현된다. 지침에 따라 assertion, fixture와 다른 runtime 경로를 수정하지 않았다.

## C++ parity

C++ `raw_mesh_node_owner.cpp`의 `request_user_spot_create`와
`request_user_spot_close`는 `request_infrastructure`의 단일 request를 사용해 terminal envelope가
없는 결과를 replay하지 않는다. 별도 `infrastructure_request_retry_state_t`는 각 attempt에 남은
deadline 전부를 주는 점은 새 스펙과 같지만 `not_connected`와 `route_unavailable`만 replay하고
`timed_out`은 terminal로 끝낸다. Actor create와 bound-session bind 일부만 이 retry helper를 쓴다.

따라서 C++는 다음 두 점에서 새 sender spec과 다르다.

1. User Spot create·close durable lifecycle operation에 sender replay가 없다.
2. Retry helper를 쓰는 durable operation도 lost reply/handover timeout을 replay하지 않는다.

요청 범위에 따라 C++는 수정하지 않았다.

## BLOCKERS

1. `285cfa522a`가 기록한 historical `RequestError(result=101)` reply-loss 형태는 현재 설치된
   package에서 5회 동안 재현되지 않았다. 현재 증거는 (b)를 확정하지만 과거 binary에서 (a)가
   함께 존재했는지는 확정하지 못한다.
2. 조사 중 B session 소유의 `bindings/node/provenance/core-package-provenance.json`이 외부에서
   `0.15.1` release에서 dirty local `0.17.0` package provenance로 변경돼 있었다. 이 파일과
   Core/binding에는 손대지 않았고 package를 다시 빌드하거나 sync하지 않았다. Historical binary와
   정확히 같은 native revision을 다시 실행하려면 supervisor가 B session과 별도로 고정해야 한다.
3. 위 bound-session M6B 단독 실패가 남아 있다. 이 작업의 replay 변경과 호출 경로가 겹치지
   않으며 범위 밖이라 원인 수정하지 않았다.
