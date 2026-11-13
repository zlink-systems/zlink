감독자 리뷰: 변경 요청 2건.

1. **round-robin 순번을 쥔 채 용량을 기다린다 (회귀).** `runLoop`가 `roundRobin.tryAcquire(receiveOwner)`로 순번(`active`)을 얻은 뒤 `await rotation.acquire(capacitySignal)`에서 application queue 용량을 기다린다. 용량이 다 찬 동안 순번은 풀리지 않는다. 그러면 다른 ready owner는 `tryAcquire`가 계속 false라 `waitReceiveLoopTurn()`(setImmediate) 루프를 돌며, 포화 구간 내내 CPU를 계속 쓴다. 수정 전에는 `acquirePermit`를 `tryAcquire`보다 먼저 기다렸으므로 순번을 쥔 owner가 대기 중일 수 없었다. 용량 예약을 순번 획득 앞으로 옮기고, 순번을 못 얻으면 `rotation.releaseUnused()`로 돌려주는 순서가 맞다. 수정 전 실패 시험: owner 둘, 용량 0에서 `tryAcquire` 호출 수가 setImmediate 회전에 비례해 늘지 않음(또는 대기 중 순번이 비어 있음)을 assert.
2. `channel-receive-rotation.test.js` 첫 시험의 `console.log(JSON.stringify(...))`를 지운다(시험 출력 잡음).

나머지(count 예약 경로 2→1, `releaseUnqueued`로 미큐 lease 반환을 finally 한 곳에 모은 것, `drainWaiters`의 요청 수 반영)는 확인했다.

