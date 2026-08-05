# ShoppingMall Kotlin 샘플 포팅 인벤토리

이 문서는 `framework/doc/framework/common/sample/event/shoppingmall.ko.md`와
`framework/languages/dotnet/samples/ShoppingMall`을 기준으로 Kotlin 샘플의 반영 상태를
점검한다. 기준은 주문 시작, 멱등 처리, 이벤트 소싱, 조회 모델 재생성, 명시 재개, 수평 확장
self-check가 실행 코드와 runner에서 실제로 검증되는지이다.

## 기준 문서와 기준 구현

- 공통 시나리오: `framework/doc/framework/common/sample/event/shoppingmall.ko.md`
- .NET 기준 구현: `framework/languages/dotnet/samples/ShoppingMall`
- Kotlin 구현: `framework/languages/java/samples/kotlin/ShoppingMall`

## 구현 매핑

| 항목 | Kotlin 위치 | 상태 | 근거 |
|------|-------------|------|------|
| 공유 주문 계약 | `Shared/src/main/kotlin/.../Messages.kt` | 완료 | 주문 시작, 상태 조회, projection 삭제·재생성, pending 매핑, 명시 재개, 서버 assertion 메시지를 둔다. |
| CommerceApi 역할 | `Server/CommerceApi/src/main/kotlin/...` | 완료 | 주문 요청 검증, 멱등 매핑 조회·예약, workflow 라우팅, projection 조회, self-check relay를 담당한다. |
| OrderWorkflow 역할 | `Server/OrderWorkflow/src/main/kotlin/...` | 완료 | 이벤트 스트림 재생, 기대 버전 append, 상태 전이, projection fold, 명시 재개를 담당한다. |
| 업무 상태 저장소 | `Server/Configuration/src/main/kotlin/.../CommerceStore.kt` | 완료 | 장바구니, 재고, 결제, idempotency, event stream, read model을 실행별 store에 둔다. |
| client self-check | `Client/src/main/kotlin/.../ShoppingMallClientScenario.kt` | 완료 | 성공 주문, 중복 시작, 동시 시작 경쟁, pending 복구, 명시 재개, 재고 실패, 결제 실패 보상, projection rebuild, 조회 일관성, scale-out을 검증한다. |
| runner | `run_sample.sh`, `run_sample.ps1` | 완료 | 실행별 Redis Docker 컨테이너, 임시 endpoint, key prefix, log directory를 만들고 client/server evidence marker를 확인한다. |
| sample-local README | `README.md` | 완료 | 실행 방법, 역할 구성, 성공 marker를 설명한다. |

## 시나리오별 상태

| 공통 요구 | Kotlin 상태 | 확인 위치 |
|-----------|-------------|-----------|
| `StartOrderReq`가 `Created`를 즉시 돌려주고 background workflow가 `Confirmed`까지 진행 | 완료 | `ShoppingMallClientScenario.runSuccessfulOrder()` |
| 같은 `IdempotencyKey` 재전송이 같은 `OrderId`를 반환 | 완료 | `runDuplicateIdempotency()` |
| 같은 `IdempotencyKey`를 두 `CommerceApi`에 동시에 보내도 하나의 `OrderId`로 수렴 | 완료 | `runConcurrentIdempotency()` |
| pending 매핑만 있는 주문이 owning `CommerceApi`로 전달되어 같은 `OrderId`에서 재개 | 완료 | `runPendingRecovery()` |
| `InventoryReserved` 이후 명시 재개가 결제와 확정을 이어 감 | 완료 | `runExplicitResume()` |
| 재고 부족은 실패 상태와 재고 실패 사유를 남김 | 완료 | `runInventoryFailure()` |
| 결제 실패는 예약 해제 보상 이벤트와 실패 상태를 남김 | 완료 | `runPaymentFailure()` |
| 조회 모델 삭제 후 event stream replay로 projection 복원 | 완료 | `runProjectionRebuild()` |
| 다른 `CommerceApi`에서 조회해도 같은 최종 상태 확인 | 완료 | `runQueryConsistency()` |
| `CommerceApi x2`, `OrderWorkflow x2` scale-out 실행 | 완료 | `runScaleOut()`, `run_sample.sh`, `run_sample.ps1` |
| 서버 쪽 evidence가 event sequence와 보상·멱등 count를 검증 | 완료 | `ServerAssertionHandler` |

## 검증 명령

아래 명령으로 현재 샘플을 검증한다.

```bash
cd framework/languages/java/samples/kotlin/ShoppingMall
../../gradlew --settings-file standalone.settings.gradle.kts --no-daemon --no-parallel --max-workers=1 classes --quiet
nice -n 10 timeout 600s ./run_sample.sh
pwsh -NoProfile -ExecutionPolicy Bypass -File ./run_sample.ps1
```

2026-07-07 현재 checkout에서 `nice -n 10 timeout 600s ./run_sample.sh`가 통과했다. runner는
`CommerceApi` 2개, `OrderWorkflow` 2개, client, 실행별 Redis를 띄웠고, client log에서
`shoppingmall-concurrent=completed`, `shoppingmall-pending=completed`, `shoppingmall-resume=completed`,
`shoppingmall-inventory-failure=completed`, `shoppingmall-payment-failure=completed`,
`shoppingmall-rebuild=completed`, `shoppingmall-consistency=completed`, `shoppingmall-scaleout=completed`,
`shoppingmall-server-evidence=completed`, `shoppingmall=completed`를 확인했다. 증거 파일은
`build/sample-logs/client.log`, `build/sample-logs/api-a.log`, `build/sample-logs/api-b.log`,
`build/sample-logs/workflow-a.log`, `build/sample-logs/workflow-b.log`, `logs/flow-*.log`이다. Bash와
PowerShell runner는 모두 Gradle 병렬 실행을 끄고 worker를 1개로 제한한다.

## 남은 gap

없음. Kotlin ShoppingMall 샘플은 공통 ShoppingMall 시나리오와 .NET 기준 self-check 항목을 모두
실행 코드와 runner에 반영한다.
