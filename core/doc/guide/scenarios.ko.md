---
title: "Core 사용 시나리오"
---

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: Core 용어](glossary.ko.md)
<!-- zlink-nav:end -->

# Core 사용 시나리오

> **이 장이 답하는 것** — 흔한 요구 사항을 socket pattern과 핵심 API로 바로 찾을 수
> 있는 조회표다. 각 pattern의 정확한 계약은 해당 socket 스펙이 소유한다.

| 요구 사항 | Socket pattern | 핵심 API |
|---|---|---|
| Thread 사이 일대일 통신 | PAIR | `zlink_send`, `zlink_recv` |
| Topic 기반 단방향 배포 | PUB/SUB | `zlink_publish`, `zlink_subscribe` |
| 비동기 routed request/reply | DEALER/ROUTER | `zlink_request`, `zlink_completion_recv`, `zlink_reply` |
| 여러 peer에 routing id로 송신 | ROUTER | `zlink_send_rid`, `zlink_router_recv` |
| 외부 TCP 계열 client 연동 | STREAM | `zlink_stream_recv_packet` |
| Readiness 통합 대기 | poller | `zlink_poller_add`, `zlink_poller_wait` |
| 연결 상태 관찰 | socket monitor | `zlink_socket_monitor_open`, `zlink_socket_monitor_recv` |
| 주기 작업 알림 | generic timer | `zlink_timer_start`, `zlink_timer_recv` |

Application topology와 stateful object scenario는 Framework 언어별 가이드와 E2E 문서에서 다룬다.
