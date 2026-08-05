---
title: "Raw messaging 신뢰성"
---

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: Socket pattern 선택](03-0-socket-patterns.ko.md) | [다음: 설계 근거](design-rationale.ko.md)
<!-- zlink-nav:end -->

# Raw messaging 신뢰성

> **이 장이 답하는 것** — Core가 보장하는 전달 규칙과, 보장하지 않아 application이나
> Framework가 책임져야 하는 것(재시도·중복 제거·durable storage)을 가른다.

Core는 transport와 socket pattern의 전달 규칙을 제공하지만 application-level delivery 보장을 만들지
않는다. 재시도, deduplication, durable storage와 업무 transaction은 application 또는 Framework가
담당한다.

## Queue와 backpressure

HWM에 도달하면 send가 backpressure 결과를 반환할 수 있다. Blocking timeout을 설정해도 전달 완료나
remote 처리 완료를 뜻하지 않는다. Send-ready 알림 뒤에도 다른 sender가 queue를 먼저 사용할 수 있으므로
재시도 결과를 확인한다.

## PUB/SUB

Subscriber가 subscription과 connection을 설정하기 전에 발행한 message는 받을 수 없다. 느린 subscriber의
처리는 HWM과 publisher option에 따라 제한된다. 중요한 event에는 별도의 동기화나 재전송 경로를 설계한다.

## Request/reply

Request timeout은 reply가 제한 시간 안에 도착하지 않았다는 뜻이다. Remote가 request를 처리하지 않았다는
증거가 아니다. 부작용이 있는 request를 재시도하려면 application request id와 deduplication 규칙을 둔다.

## Connection 전환

Reconnect 중에는 transport 상태와 route availability가 바뀐다. `zlink_connect()`의 성공은 연결 intent가
접수됐다는 뜻이며 즉시 전송 가능하다는 뜻이 아니다. Poller, send-ready와 socket monitor를 사용해 실제
상태를 관찰한다.
