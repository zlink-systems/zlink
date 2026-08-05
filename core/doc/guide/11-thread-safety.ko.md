---
title: "Thread safety"
---

[English](11-thread-safety.en.md)

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
Receive가 반환한 routing-id와 topic view는 thread-local이며 같은 thread의 다음 receive 계열 호출에서
무효화될 수 있다.

## Control 경로

설정과 endpoint operation은 필요한 경우 내부에서 직렬화된다. 직렬화는 data race를 막지만 서로 충돌하는
lifecycle 변경에 의미를 부여하지 않는다. 가능하면 traffic을 시작하기 전에 option을 설정한다.

## Close

Close는 더 엄격한 lifecycle gate를 사용한다. 다른 operation이나 callback 때문에 안전하게 제거할 수 없으면
busy를 반환한다. Close가 접수된 뒤 새 API 진입은 shutdown을 반환한다. STREAM raw callback에서는 자기
handle을 닫을 수 없다.

## Callback

Socket receive callback은 해당 socket의 I/O thread에서 실행된다. Monitor와 generic timer callback은 Core
control runtime에서 실행된다. Callback을 짧게 유지하고 blocking application 작업은 application이 소유한
queue로 넘긴다.
