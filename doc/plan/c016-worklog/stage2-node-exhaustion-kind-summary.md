# Stage 2 Node durable operation exhaustion kind

## Diff

- `framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts`
  - durable lifecycle operation별로 admitted attempt 존재 여부를 누적한다.
  - binding request의 typed `RequestResult.TimedOut`만 admitted 증거로 사용한다.
  - 전체 deadline 소진 시 admitted 이력이 없으면 내부 `RouteNotConnected`를 통해 공개
    `Unavailable`, 있으면 내부·공개 `DeadlineExceeded`로 완료한다.
- `framework/languages/node/test/m6b/m6b-user-spot-terminal-replay.contract.ts`
  - deadline 내내 `RequestResult.NotConnected`인 route-absent 회귀를 추가했다.
  - admitted request가 reply 없이 `RequestResult.TimedOut`으로 끝나는 회귀를 추가했다.
  - 기존 동일 operation ID replay와 whole-remaining-deadline 검증을 유지했다.

## Runtime 변경 판정

- Owner: Framework durable lifecycle operation sender가 operation 단위 replay와 exhaustion kind를 소유한다.
- Spec clause: `03-spot-actor/04-actor-model.ko.md` sender bullets 676-680 및 `07-framework-error-model.ko.md` 75-85 — never admitted 소진은 `Unavailable`, admitted reply 미수신 소진은 `DeadlineExceeded`다.
- Parity: C++ Stage 2도 raw owner의 `route_unavailable`을 `Unavailable`, `timed_out`을 `DeadlineExceeded`로 매핑한다.
- Class: A — 수정된 공통 계약에 대한 기존 Node sender replay 경로의 계약 적응이다.

## Results

- m6b replay contract: PASS, 4/4
  - `node node_modules/typescript/bin/tsc -p tsconfig.m6b-runtime.json`
  - `node --test build/m6b-runtime/languages/node/test/m6b/m6b-user-spot-terminal-replay.contract.js`
- native two-process: PASS, 3/3
  - `node --test test/contract/user-spot-native-two-process.test.js`를 3회 실행했다.
- typecheck: PASS
  - `npm run typecheck`
- touched runtime lint: PASS
  - `node node_modules/eslint/bin/eslint.js packages/framework/src/runtime/foundation/service-stateful-runtime.ts`
- 모든 npm/test 호출은 `TMPDIR=/dev/shm/zlink-tmp-node`, `unset ZLINK_LIBRARY_PATH`,
  `flock -w7200 /tmp/zlink-node-gate.lock` 환경에서 실행했다.

## BLOCKERS

- 없음.
