# Java ShoppingMall Sample

`ShoppingMall`은 주문 생성과 주문 workflow 상태 전이를 보여주는 Java Framework 샘플이다.
Commerce API 역할은 HTTP로 주문 요청을 받고, OrderWorkflow 역할은 ZLink channel과 Redis-backed
상태 저장소를 통해 주문을 진행한다. 클라이언트는 정상 주문, 실패 보상, 멱등성, projection rebuild,
scale-out 경로를 self-check로 검증한다.

## 역할

| project | 책임 |
|---------|------|
| `Shared` | 클라이언트와 서버가 공유하는 주문 요청, 응답, 상태 계약을 담는다. |
| `Client` | API A/B를 호출해 주문 workflow self-check 시나리오를 실행한다. |
| `Server/Configuration` | role들이 공유하는 이름, endpoint, Redis location store 설정을 제공한다. |
| `Server/Shared` | 주문 domain event, projection fold, Redis-backed store를 제공한다. |
| `Server/CommerceApi` | HTTP 주문 API와 self-check endpoint를 열고 workflow channel로 요청을 보낸다. |
| `Server/OrderWorkflow` | MeshNode의 workflow ChannelName과 order Spot을 실행하고 주문 상태 전이를 처리한다. |

## 실행

Linux 또는 WSL에서는 아래 명령을 사용한다.

```bash
./run_sample.sh
```

Windows PowerShell에서는 아래 명령을 사용한다.

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\run_sample.ps1
```

runner는 실행마다 Docker가 할당한 loopback port에 전용 Redis container를 만들고, 실행별 Redis key
prefix를 설정한다. 외부 Redis endpoint는 재사용하지 않으며, 성공하거나 실패하면 자신이 만든
container를 제거한다.

## 성공 조건

runner는 CommerceApi 2개, OrderWorkflow 2개, Client 1개를 실행한다. 모든 endpoint가 열리면
`topology=ready`를 출력하고 client self-check를 실행한다.

성공하면 아래 marker가 출력된다.

```text
shoppingmall-server-evidence=completed
shoppingmall=completed
shoppingmall full client/server self-check completed
```

실패하면 `build/sample-logs/` 아래 role 로그를 출력한다. message flow log는 기본적으로 `logs/`
아래에 남는다.
