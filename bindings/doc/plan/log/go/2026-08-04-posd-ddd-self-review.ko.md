# Go binding POSD·DDD 자체 설계 검토 기록

이 기록은 2026-08-04에 Go binding의 Core 0.9.0 raw 경계와 runtime lifecycle을 다시 읽고 적용한
자체 검토 결과다. 구현자가 수행한 검토이므로 독립 frontier review나 최종 `CLEAN` 판정을 대신하지
않는다.

## 검토 범위

다음 경로를 실제 호출 순서와 함께 확인했다.

- `bindings/go/internal/native/request_progress.go`
- `bindings/go/internal/native/socket_core.go`
- `bindings/go/internal/native/dealer_router_request.go`
- `bindings/go/internal/native/socket_direct.go`
- `bindings/go/internal/native/socket_send_ready.go`
- `bindings/go/internal/native/socket_routed.go`
- `bindings/go/internal/native/socket_types.go`
- `bindings/go/tests/hot-path-cost-inventory.json`

검토 기준은 `socketCore`가 native handle, callback handle과 request progress의 lifecycle을 소유하는지,
호출자가 cgo pointer·callback userdata·poller 상세를 조립하지 않는지, request마다 불필요한 poller와
worker를 만들지 않는지다.

## 확인한 위험 신호

첫째, 내부 request progress pump를 native handle key의 전역 `sync.Map`에 보관하고 있었다. map entry를
삭제하는 lifecycle이 없어서 socket을 닫은 뒤에도 pump와 raw handle이 process 전역 map에 남을 수 있었다.
외부 `Poller`가 같은 native handle을 참조하는 동안 내부 pump를 생략해야 하므로 외부 progress 참조
registry까지 제거할 수는 없지만, 내부 pump의 수명은 외부 객체와 분리되어야 했다.

둘째, pump의 `activeCount` 감소와 worker 종료 표시가 별도의 atomic 연산이었다. worker가 종료 조건을
읽은 직후 새 request가 count를 증가시키면 새 request는 이미 실행 중인 worker를 보았고, 기존 worker는
종료하는 경로가 생길 수 있었다. 이 경우 active request가 있어도 다음 worker가 시작되지 않을 수 있다.

셋째, callback registration과 `Close`가 callback handle slot을 각각 읽고 썼다. callback dispatcher를
닫는 작업은 callback 내부에서 socket을 닫는 경우를 고려해 mutex 밖에서 수행해야 하지만, slot 교체와
socket close의 순서는 한 owner가 직렬화해야 했다. 이 경계는 `go test -race ./...`에서 `OnPacket`과
`Close`를 동시에 관찰했을 때 실제 data race로 재현됐다.

넷째, request completion이 끝난 뒤에도 progress worker가 native poller에서 socket handle을 관찰하는
짧은 구간이 있었다. socket을 먼저 닫으면 poller wait가 이미 해제된 native mutex를 참조할 수 있고, 실제로
multipart test와 async request test를 함께 race 반복할 때 native abort로 재현됐다.

## 대안 비교와 선택

request progress에는 다음 두 대안을 비교했다.

| 대안 | 장점 | 문제 |
|------|------|------|
| native handle 전역 map을 유지하고 완료 시 entry를 삭제 | 변경 범위가 작다 | socket close, 마지막 completion, handle 재사용 사이의 삭제 순서를 전역 map이 알아야 한다 |
| `socketCore`가 handle 단위 pump를 보관 | native handle과 내부 progress의 owner가 같고 socket close에서 참조를 끊을 수 있다 | socketCore에 lifecycle mutex와 pump field가 추가된다 |

두 번째 대안을 선택했다. `Poller`와 socket wrapper가 서로 다른 Go object일 수 있다는 사실 때문에
`externalProgressRefs`는 native handle 기준 전역 registry로 유지했다. 반면 binding 내부 request pump는
`socketCore.requestProgress`가 유일하게 만들고, `Close`가 그 참조를 끊는다. 따라서 caller surface나
Core C ABI를 늘리지 않고 ownership을 한 계층으로 모은다.

worker lifecycle은 `activeCount`, `workerOn`과 attach/stop 판단을 `progressPump.mu` 아래에서 처리한다.
worker가 native poller를 해제하는 시점에 active request가 다시 생기면 worker slot을 유지하고 새 worker를
시작한다. request마다 poller를 만들지 않는 기존 handle 단위 비용은 유지한다.

socket close 순서에는 다음 두 대안을 비교했다.

| 대안 | 장점 | 문제 |
|------|------|------|
| socket을 먼저 닫고 worker가 뒤에서 종료 | close 구현의 변경이 작다 | worker가 이미 파괴된 native handle을 계속 poll할 수 있다 |
| worker를 중지하고 종료를 기다린 뒤 socket을 닫음 | native poller와 socket handle의 owner 수명이 순서대로 끝난다 | close가 worker의 최대 poll interval까지 기다릴 수 있다 |

두 번째 대안을 선택했다. `Close` 실패가 socket을 계속 사용할 수 있는 상태를 남기는 경우에는 pump를 다시
실행하도록 `resume`해 실패 경로에서도 request progress를 잃지 않게 했다.

callback registration에는 field별 lock을 반복하지 않고 `socketCore.replaceCallback`을 두었다. 이 helper는
등록 native call과 handle slot 교체를 owner mutex 아래에서 수행하고, 이전 dispatcher를 닫는 작업은 mutex
밖에서 실행한다. `Close`도 native socket close와 callback slot 분리를 직렬화한 뒤 dispatcher를 해제한다.
이 구조는 callback handler가 다시 `Close`를 호출할 수 있는 계약을 유지하면서 lifecycle deadlock을 만들지
않는다.

## 적용 결과

- `socketProgressPumps` 전역 map을 제거했다.
- request progress pump를 `socketCore`가 만들고 보관하며 `Close`에서 참조를 해제하도록 바꿨다.
- socket `Close`가 progress worker의 native poller 종료를 기다린 뒤 native socket을 닫도록 바꿨다. native
  close가 실패하면 기존 pump를 복구한다.
- pump worker의 attach/detach/재시작 판단을 mutex로 보호했다.
- receive, send-ready와 STREAM packet callback의 handle 등록·교체·해제를 같은 owner 경계로 모았다.
- `routedSocket.Recv`의 callback busy 확인은 `socketCore`가 관리하는 atomic 상태를 읽어 receive hot path에
  mutex contention을 추가하지 않는다.
- public Go API, Core C ABI, submit 반환값과 package module path는 바꾸지 않았다.

## 검증과 판정

현재 source worktree에서 다음 검증을 통과했다.

```text
go test ./...                                  PASS
go test -race ./... -count=3                   PASS
go vet ./...                                   PASS
go test ./internal/native -run 'Test(OptimizationGuard|RawCore11Allowlist|HotPathCostInventory)' -count=1  PASS
./tests/run_tests.sh                            PASS (samples: 7/7)
scripts/local-package/go/build-wsl.sh ...      PASS (clean consumer)
```

이 결과는 구현자 자체 검토와 Linux x86_64 source runtime의 증거다. 독립 frontier reviewer의 fresh
read-only review, 공통 submit 반환 draft 승인, Linux arm64와 macOS native package consumer는 이 기록에서
닫지 않는다.

판정: `PARTIAL / NOT CLEAN`.
