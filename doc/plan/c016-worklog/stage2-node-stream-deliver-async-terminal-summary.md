# STAGE 2 Node STREAM deliver async terminal 결과

## 결과

Node bound-session STREAM push가 binding의 비동기 terminal인 `submit()`을 await하도록
수정했다. payload decode와 message builder 구성은 transport catch 밖에 있으며, catch는
`SubmitError.result`만 분류한다.

- `Backpressured`와 `NotAdmitted`는 session target을 유지한 채 typed submit failure로 전달한다.
- `NotConnected`, `NotFound`, `Terminated`는 `disconnectRid()`로 기존 STREAM monitor 기반
  close/unbind lifecycle을 요청하고 typed submit failure를 전달한다.
- 그 밖의 typed result와 일반 오류는 그대로 다시 던진다.
- local fast path는 typed submit result를 보존한다. remote command 36은 one-way transport
  admission 뒤 별도 client delivery ack를 만들지 않는다.
- retained outbound payload는 async delivery가 끝나기 전에 settle하지 않는다.

## Diff

- `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts`
  - `RawStreamSessionService.deliver()`를 awaitable로 변경하고 `submit_sync()`를
    `await submit.submit()`으로 교체했다.
  - typed capacity/close terminal만 Framework backend result로 변환했다.
  - close terminal에서 직접 `sessionTargets`를 삭제하지 않고 `disconnectRid()`를 호출한다.
- `framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts`
  - session delivery 계약과 retained/direct 소비 경로가 async terminal을 await한다.
  - local ingress는 typed submit result를 반환하고, remote one-way ingress는 사후 ack를
    생성하지 않는다.
- `framework/languages/node/test/m6b/m6b-runtime.contract.ts`
  - fake STREAM이 async pending과 WRITABLE 해제를 모델링한다.
  - typed backpressure 뒤 동일 session 재사용, 세 close terminal의 lifecycle 요청,
    unknown error 동일 객체 전파를 검증한다.
  - sync terminal이 호출되면 즉시 실패하도록 회귀 guard를 추가했다.

## 필수 판정 네 줄

- **Owner:** DONTWAIT와 exact WRITABLE 재제출은 Node binding async terminal이 소유하며,
  close 감지와 unbind 수렴은 Framework STREAM monitor/session lifecycle이 소유한다.
- **Spec clause:** `01-submit-and-completion` 100-179행과 434-459행의 async HWM terminal 및
  typed failure 계약, `02-session-actor-binding` 186-218행의 command 36 one-way delivery,
  `05-transport-liveness` 226-248행의 monitor 기반 close 수렴을 따른다.
- **Parity:** C++ `stream_host_service.cpp`, .NET `ZLinkBackendStreamSocketWrapper.SendAsync()`,
  Java `submitOwnedStreamFrameAsync()`와 같이 bound-session push에서 binding async terminal을
  사용한다. Node의 sync terminal/catch-all만 기존에 달랐다.
- **Class:** **B — 기존 Framework 구현 결함**. 공개 계약 변경이나 우회는 없다.

## 검증

- focused `raw backend dispatches Spot requests and Actor sends through M6B owners` 3회:
  통과 (`26.864 ms`, `25.518 ms`, `26.949 ms`).
- `npm run verify:m6b-runtime`: 통과 (`112 passed`, `0 failed`).
- 회귀 항목: async backpressure 회복과 session 유지, typed capacity result 유지,
  `NotConnected`/`NotFound`/`Terminated` close 요청, unknown error 전파 모두 통과.
- `npm run typecheck`: 통과.
- touched runtime files ESLint: 통과. M6B TypeScript test는 ESLint 설정 대상이 아니어서
  ignored warning만 발생했다.
- `bash samples/run_samples.sh SupportChat.Ts`: 통과.
- `bash samples/run_samples.sh TicTacToe.Ts`: 통과.
- 최초 SupportChat 실행은 Playwright Chromium 미설치로 중단됐다. 저장소 표준
  `npm run browser:install`을 실행한 뒤 재검증해 통과했다.
- Core/local package는 재빌드하지 않았다. commit도 생성하지 않았다.

## BLOCKERS

없음.
