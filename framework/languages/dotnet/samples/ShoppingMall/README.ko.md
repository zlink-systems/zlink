# ShoppingMall 샘플

`ShoppingMall`은 주문 생성과 주문 workflow 상태 전이를 보여주는 .NET Framework
샘플이다. Commerce API 역할은 주문 요청을 받고, OrderWorkflow 역할은 주문별 상태를
진행시키며, 클라이언트는 성공, 실패, 재시도, projection rebuild 흐름을 검증한다.

## 실행

Linux 또는 WSL에서 전체 시나리오를 실행한다.

```bash
./run_sample.sh
```

Windows PowerShell에서는 다음 명령을 사용한다.

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\run_sample.ps1
```

## 구성

- `Shared/`는 클라이언트와 서버가 공유하는 주문 요청, 응답, 상태 계약을 담는다.
- `Client/`는 주문 workflow self-check 시나리오를 실행한다.
- `Server/CommerceApi/`는 외부 주문 요청을 받고 Order ID를 global SpotId로 사용해
  `OrderWorkflowSpot`에 직접 요청한다. 첫 요청은
  `InstanceSpot("shoppingmall.order-workflow")`를 명시하며, Framework가 owner node를 선택한다.
- `Server/OrderWorkflow/`는 actor-free Instance Spot factory를 제공하고 주문 상태 전이,
  실패 처리, projection rebuild를 담당한다.
- `Server/Shared/`는 서버 역할 사이에서 공유하는 저장소와 도메인 코드를 담는다.
- 서버 프로세스들은 registry 없이 공유 location store(Redis)에 위치를 등록하고 자동 연결한다.
- Commerce API는 `IZLinkSpotManager.GetOrCreate`나 특정 NodeRid를 사용하지 않는다. 같은 Order ID의
  이후 요청은 Location Store에 공개된 current owner로 전달된다.
- 주문 이벤트 스트림, 조회 모델, 장바구니·재고·결제·멱등 상태도 같은 Redis 안에 저장한다.
  Redis key는 역할별 임시 config 파일에 기록한 실행 전용 key prefix 아래에만 만들어진다.
- `run_sample.sh`와 `run_sample.ps1`는 실행마다 전용 Redis Docker 컨테이너를 직접 시작한다.
  외부 Redis endpoint 재사용 mode는 제공하지 않는다. 컨테이너 이름, Docker host port,
  Redis key prefix, log directory가 모두 실행별로 달라서 동시에 실행되는 다른 테스트와 섞이지 않는다.

## 성공 조건

클라이언트 시나리오는 정상 주문, 멱등성, 같은 멱등 키의 동시 시작 경쟁, pending 상태,
`InventoryReserved` 이후 명시 재개, 재고 실패, 결제 실패, projection rebuild, 일관성,
scale-out 경로를 검증한다. `run_sample.sh`는 client log에서 `shoppingmall=completed`를
확인하고, 서버 evidence 확인이 끝나면 `shoppingmall-server-evidence=completed`를 출력한다.
