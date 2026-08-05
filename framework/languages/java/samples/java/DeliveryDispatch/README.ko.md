# Java DeliveryDispatch Sample

이 샘플은 배송 생성, 배송원 제안, timeout 재배정, 상태 추적, 고객 stream push를 하나의
실행 흐름으로 검증한다. 공통 시나리오 기준은
`framework/doc/framework/common/sample/deliverydispatch/README.ko.md`다.

## 역할

| project | 책임 |
|---------|------|
| `Server/Configuration` | role들이 공유하는 이름, endpoint, Redis location store 설정을 제공한다. |
| `Server/Tracking` | 배송 상태 event를 기록하고 고객 actor로 알림을 보낸다. |
| `Server/CustomerGateway` | 고객 stream session, customer actor, entry spot을 제공한다. |
| `Server/CourierSession` | 배송원 stream session을 받고 courier actor와 session을 bind한다. |
| `Server/CourierSpotNode` | node 1/2의 MeshNode에서 courier actor와 Entry Spot을 실행한다. 디렉터리 이름은 현재 source 위치를 그대로 적는다. |
| `Server/Dispatch` | HTTP API와 dispatch worker를 실행한다. |
| `Client` | HTTP 요청, stream subscription, offer decision, marker 검증을 수행한다. |

## 실행

```bash
./run_sample.sh
```

runner는 사용 가능한 local port와 격리된 Redis location store를 준비한 뒤 각 role을 별도 process로
시작한다. 모든 endpoint가 열리면 `topology=ready`를 출력하고 client self-check를 실행한다.

성공하면 아래 marker가 출력된다.

```text
deliverydispatch-reassignment=completed
deliverydispatch-server-evidence=completed
deliverydispatch=completed
deliverydispatch full client/server self-check completed
```

실패하면 `build/sample-logs/` 아래 role 로그를 출력한다. message flow log는 기본적으로 `logs/`
아래에 남는다.
