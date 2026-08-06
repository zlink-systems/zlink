# .NET graceful drain의 bounded teardown

이 문서는 .NET Framework host가 정상 종료와 deadline 초과 종료를 처리하는
책임 경계를 설명한다. 목적은 server restart 중에도 graceful drain이 정해진
시간 안에 terminal 상태에 도달하고, receive loop가 cancellation을 무시하더라도
socket과 poller의 소유권을 회수하는 방법을 고정하는 데 있다.

## 1. 공통 시나리오와의 관계

`RL-A3 Many Clients Reconnect After A Server Restart`는 100개 client가 baseline
request를 완료한 뒤 server가 정상 종료되고, replacement가 ready 상태가 되면
각 client가 한 번씩 request를 완료하는지 확인한다. Application은 별도의 반복
reconnect loop를 호출하지 않는다.

이 시나리오에서 provider 종료는 storm client의 request 수가 아니라 Framework의
shutdown contract를 검증해야 한다. 따라서 E2E runner는 process나 Redis를
공유하여 시간을 줄이지 않고, 각 실행을 별도 process와 별도 store namespace로
구성한다.

## 2. 종료 경로

```text
+----------------------+    +----------------------+    +----------------------+
| Host stop signal      | -> | Orderly drain        | -> | Runtime teardown     |
+----------------------+    +----------------------+    +----------------------+
                                      | deadline exceeded
                                      v
                              +----------------------+
                              | Bounded force stop   |
                              +----------------------+
```

정상 경로는 application admission을 닫고, draining 상태와 serving weight를
전파한 뒤, 전파 bound 안에서 accepted operation을 기다린다. 이 경로가 완료되면
runtime resource를 정상 순서로 해제한다.

deadline이 만료되면 `DeadlineExceeded`를 원인으로 보존한 채 force-stop으로
전환한다. 이미 orderly deadline을 모두 사용했으므로 force-stop에는 전체
shutdown deadline을 다시 배정하지 않는다. 독립된 최소 teardown budget을
사용하여 runtime, auto-connect, owner resource를 bounded하게 해제한다.

다른 단계에서 발생한 teardown failure는 해당 단계에 필요한 deadline을 사용할
수 있지만, deadline 만료 경로는 항상 최소 force-stop budget을 사용한다. 이
구분으로 orderly drain과 force teardown의 시간이 합쳐져 process 종료 관찰
시간을 초과하는 문제를 방지한다.

## 3. receive loop와 native resource 소유권

`ZLinkManagedMeshNode`의 force-stop은 먼저 receive loop의 종료를 기다리지만,
그 대기는 force-stop cancellation token으로 제한된다. receive loop가 token을
관찰하지 않아도 대기가 끝나면 다음 순서로 node가 소유한 inbound operation,
poller, socket을 해제한다.

1. receive loop cancellation을 요청한다.
2. receive loop 종료를 shutdown token으로 bounded wait한다.
3. inbound operation admission을 닫고 남은 operation을 bounded wait한다.
4. pending operation과 mailbox를 정리한다.
5. poller와 socket을 해제한다.

이 순서는 receive loop의 정상 종료를 기다리는 동안 native resource 해제가
무기한 지연되는 것을 막는다. cancellation 이후에도 receive loop가 계속
동작할 수 있으므로, receive loop와 socket을 동시에 사용하지 않도록 node의
기존 stop state와 socket ownership 규칙을 먼저 적용한다.

## 4. 성능과 동시성 기준

이 경로는 message hot path에 추가 allocation이나 lock을 넣지 않는다. deadline
source와 teardown cancellation source는 operation/lifecycle 경로에서만
만들어진다. 정상 request 처리에는 영향을 주지 않으며, 100개 client의
concurrent request는 각 process의 public Framework API를 통해 수행한다.

Framework는 binding의 private member나 reflection을 사용하지 않는다. socket,
poller, receive storage의 ownership과 lifecycle 변환은 binding public API와
semantic runtime integration 경계 안에서 처리한다.

## 5. 검증 기준

다음 검증은 구현이 공통 E2E scenario와 같은 책임 그래프를 사용하는지 확인한다.

- RL-A3: 100개 client의 baseline과 replacement 이후 100개 unique reply
- RL-A4: rolling 또는 blue-green 교체 중 serving target이 0이 되지 않는지
- `DrainCoordinatorTests`: deadline 만료 뒤 원래 원인을 보존하고 독립된
  bounded force-stop budget을 사용하는지
- `Zlink.Framework.UnitTests`: graceful drain, runtime teardown, ownership
  regression

E2E startup 중 Redis owner lease가 local readiness window 안에 확보되지 않는
실패는 시나리오 assertion과 분리하여 판단한다. 이 경우 로그의 store
operation과 process readiness를 먼저 확인하고, 시나리오가 요구하는 격리
실행 조건을 변경하지 않는다.
