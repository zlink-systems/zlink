---
title: "Thread safety"
---

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: Message API와 ownership](09-message-api.ko.md) | [다음: Routing ID](08-routing-id.ko.md)
<!-- zlink-nav:end -->

# Thread safety

> **이 장이 답하는 것** — 어느 API를 여러 thread에서 동시에 불러도 되고, 어느 API는
> 호출을 직렬화해야 하는지 정리한다.

Core는 같은 handle에 단계별 concurrency 계약을 적용한다.

## Data 경로

지원하는 handle에서는 여러 thread의 send를 허용한다. 개념적인
`send`/`publish`/`send_rid` hot path는 typed `*_part` API에 대응한다. 성공한 multipart sequence는
연속성을 유지하지만 하나의 논리적 multipart sequence를 여러 thread로 나누면 안 된다.

별도 계약이 없으면 receive는 single-consumer다. 같은 socket에서 receive를 동시에 실행하지 않는다.
Receive가 반환한 routing-id view는 socket이 소유한다 — 같은 socket의 다음 data-recv 호출(성공·실패
모두)이나 close 뒤에는 이 view를 쓰지 않는다. 값을 보관하려면 반환 즉시 복사한다.

## Control 경로

설정과 endpoint operation은 필요한 경우 내부에서 직렬화된다. 직렬화는 data race를 막지만 서로 충돌하는
lifecycle 변경에 의미를 부여하지 않는다. 가능하면 traffic을 시작하기 전에 option을 설정한다.

## Close

Close는 더 엄격한 lifecycle gate를 사용한다. 다른 thread가 같은 handle에서 API를 실행 중이면
`ZLINK_CLOSE_BUSY`(`EBUSY`)를 반환한다. Close가 접수된 뒤 새 API 진입은 `ESHUTDOWN`을 반환한다.

## Pull 모델 — 콜백이 없다

Core는 application 콜백을 등록받지 않는다. socket data, completion, monitor event, timer fire는 모두
application thread가 poller로 readiness를 기다린 뒤 `*_recv_part()`·whole-message
`zlink_recv()`/`zlink_router_recv()`·`zlink_completion_recv()`·`zlink_socket_monitor_recv()`·
`zlink_timer_recv()`로 직접 꺼낸다. 따라서 "콜백을 짧게 유지한다" 같은 규칙은 없고, 어느 thread에서
받을지는 application이 정한다 — 한 socket의 receive 소비자는 하나로 유지한다. whole-message 수신도
같은 single-consumer 계약을 따르며 이 모델을 바꾸지 않는다.
