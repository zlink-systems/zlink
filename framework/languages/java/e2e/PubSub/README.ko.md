# Java PubSub E2E

이 E2E는 공통 Config 3 Pub/Sub 시나리오를 Java framework public API로 검증한다.

역할은 `.NET` E2E와 같은 의미로 나뉜다.

| 위치 | 역할 |
|------|------|
| `Shared/` | publish message, evidence record, channel 이름을 공유한다. |
| `Server/Publisher/` | public `ZLinkFanoutClient`로 publish를 수행하고 HTTP endpoint로 client trigger를 받는다. |
| `Server/Subscriber/` | publish handler, dispatch error observer, evidence endpoint를 제공한다. |
| `Client/` | PS-A1, PS-A2, PS-A3, PS-A4, PS-B1, PS-B2, PS-C1 scenario를 실행한다. PS-A4와 PS-B2에서는 필요한 subscriber/publisher process를 직접 시작하고 종료한다. |

실행은 아래 명령을 사용한다.

```bash
timeout 420s ./run_e2e.sh
```

`run_e2e.sh`는 Gradle `installDist`를 실행한 뒤 publisher, subscriber, client binary를 각각 시작한다.
publisher와 subscriber는 같은 Redis location store endpoint와 실행별 key prefix를 등록한다.
PS-A4 subscriber reconnect와 PS-B2 publisher restart의 lifecycle 제어는 Client support가 맡는다.
실패하면 `logs/<run-id>/` 아래 role별 stdout, stderr, message flow log를 출력한다.

subscriber가 받은 fanout delivery는 subscriber 역할 server의 bounded `/evidence/wait` endpoint로
확인한다. client는 publisher endpoint로 publish를 트리거한 뒤 반복 snapshot polling 대신 각
subscriber의 실제 handler/observer marker가 나올 때까지 bounded wait를 호출한다. topic filter처럼
부재를 확인해야 하는 조건만 wait 이후 snapshot을 한 번 대조한다.
