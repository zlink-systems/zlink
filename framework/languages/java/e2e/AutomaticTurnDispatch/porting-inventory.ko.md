# Java AutomaticTurnDispatch 구현 inventory

이 inventory는 구형 `AwaitDispatch` fixture를 Config 8 정식 계약으로 교체한 결과를 기록한다.

| 항목 | 이전 | 현재 |
|---|---|---|
| fixture/selector | 구형 Config 8 이름과 `YD-*` | `AutomaticTurnDispatch`, `ATD-A1`~`ATD-E5` |
| request 완료 | blocking `await`와 `await` 선택 | `submit(Class<T>)`이 반환하는 `CompletionStage` 하나 |
| worker 완료 | `await()` | `submit()`이 반환하는 `CompletionStage` |
| actor join 완료 | `await`/`await` | `submit()`이 반환하는 `CompletionStage` |
| cancellation | framework `CancellationToken` | Java `CompletionStage` cancellation |
| Spot 대상 | 공개 `SpotRef` 생성 | location subsystem의 `SpotHandleResolver`가 제공하는 opaque `SpotHandle` |
| session handler | packet name 반복과 동기 handler | message type descriptor와 `CompletionStage<Void>` handler |
| evidence | `await-*`, 일부 `YD-*` | 공통 `await-*`, 19개 `ATD-*` report |

구형 public API를 유지하는 compatibility helper는 추가하지 않았다. 구현과 검증에 사용되지 않는
구형 fixture source와 runner는 삭제 대상이며, generated build/log 파일은 계약 산출물로 보지 않는다.
